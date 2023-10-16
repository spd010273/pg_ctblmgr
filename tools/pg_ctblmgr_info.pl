#!/usr/bin/perl

use strict;
use warnings;
use utf8;

use Params::Validate qw( :all );
use Carp;
use Readonly;
use English qw( -no_match_vars );

use Getopt::Std;
use IPC::Shareable qw( :lock );
use Data::Dumper;
use Text::Table;
use DBI;

use FindBin;
use lib "$FindBin::Bin/../service/lib";

use Util;
Readonly my $TCP_KEEPALIVE          => 60;
Readonly my $TCP_KEEPALIVE_INTERVAL => 5;
Readonly my $TCP_KEEPALIVE_COUNT    => 200;
Readonly my $TCP_USER_TIMEOUT       => 1000 * 60 * 5;
Readonly::Scalar my $EXTENSION_NAME => 'pg_ctblmgr';
Readonly::Scalar my $SCHEMA_NAME    => 'pgctblmgr';
Readonly::Scalar my $USAGE          => <<USAGE;
USAGE:
 $0 [-C command -v cache_table] [ -d database -h host -U user -p port ]
    -C command: issue a command to pg_ctblmgr. Available commands are:
        rebuild - Rebuild a cache table
        check - Check the validity of a cache table
                The check command requires database connection parameters
    -v cache_table: The cache table the command applies to
USAGE

Readonly::Scalar my $CACHE_TABLE_COLUMNS => <<'END_SQL';
    SELECT a.attname::VARCHAR AS column_name
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_attribute a
        ON a.attrelid = c.oid
       AND a.attnum > 0
       AND a.attisdropped IS FALSE
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
     WHERE c.relname::VARCHAR = ?
  ORDER BY a.attnum ASC
END_SQL

Readonly::Scalar my $CACHE_TABLE_UNIQUE => <<'END_SQL';
    SELECT array_agg( a.attname::VARCHAR ) AS unique_keys
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
INNER JOIN pg_catalog.pg_constraint co
        ON co.contype = 'u'
       AND co.conrelid = c.oid
INNER JOIN pg_catalog.pg_attribute a
        ON a.attrelid = c.oid
       AND a.attnum = ANY( co.conkey )
       AND a.attnum > 0
       AND a.attisdropped IS FALSE
     WHERE c.relname::VARCHAR = ?
  GROUP BY co.oid
     UNION
    SELECT array_agg( a.attname::VARCHAR ) AS unique_keys
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
INNER JOIN pg_catalog.pg_index i
        ON i.indisunique IS TRUE
       AND i.indislive IS TRUE
       AND i.indisready IS TRUE
       AND i.indrelid = c.oid
INNER JOIN pg_catalog.pg_attribute a
        ON a.attrelid = c.oid
       AND a.attnum > 0
       AND a.attisdropped IS FALSE
       AND a.attnum = ANY( i.indkey )
INNER JOIN pg_catalog.pg_class ci
        ON ci.oid = i.indexrelid
 LEFT JOIN pg_catalog.pg_constraint co
        ON co.contype = 'u'
       AND co.conrelid = c.oid
       AND co.conindid = ci.oid
     WHERE c.relname::VARCHAR = ?
       AND co.oid IS NULL
  GROUP BY ci.oid
END_SQL

sub print_usage(;$)
{
    my( $message ) = validate_pos(
        @_,
        { type => SCALAR, optional => 1 },
    );

    if( $message )
    {
        print "$message\n";
    }

    print $USAGE;
    exit 1;
}

sub HELP_MESSAGE()
{
    print_usage();
}

sub parse_worker_status($)
{
    my( $worker_status ) = validate_pos(
        @_,
        { type => SCALAR },
    );

    my $status = 'unknown';
    $status = 'startup'               if( $worker_status == $WORKER_STATUS_STARTUP     );
    $status = 'running'               if( $worker_status == $WORKER_STATUS_RUNNING     );
    $status = 'idle'                  if( $worker_status == $WORKER_STATUS_IDLE        );
    $status = 'exited'                if( $worker_status == $WORKER_STATUS_EXITED      );
    $status = 'inserting'             if( $worker_status == $WORKER_STATUS_INSERT      );
    $status = 'fast deleting'         if( $worker_status == $WORKER_STATUS_FAST_DELETE );
    $status = 'slow deleting'         if( $worker_status == $WORKER_STATUS_SLOW_DELETE );
    $status = 'updating'              if( $worker_status == $WORKER_STATUS_UPDATE      );
    $status = 'generating temp table' if( $worker_status == $WORKER_STATUS_TEMP_TABLE  );
    $status = 'parsing query'         if( $worker_status == $WORKER_STATUS_QUERY_PARSE );
    $status = 'rebuild cache table'   if( $worker_status == $WORKER_STATUS_REPLACE     );
    return $status;
}

sub parse_cache_table($$$)
{
    my( $cache_table, $schema, $name ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => SCALARREF },
        { type => SCALARREF },
    );

    my @relation = split( /\./, $cache_table );

    if( scalar( @relation ) > 1 )
    {
        $$schema = $relation[0];
        $$name   = $relation[1];
    }
    else
    {
        $$name   = $relation[0];
        $$schema = undef;
    }

    return;
}

sub command_rebuild($)
{
    my( $cache_table ) = validate_pos(
        @_,
        { type => SCALAR },
    );

    my $WORKER_STATUSES = {};

    eval { tie( $WORKER_STATUSES, 'IPC::Shareable', { key => 'STATUSES' } ); };

    if( $OS_ERROR )
    {
        carp( "Failed to attach to shared memory - is $EXTENSION_NAME running?\n" );
        return undef;
    }

    tied( $WORKER_STATUSES )->shlock( LOCK_SH | LOCK_NB );
    my $found = 0;
    foreach my $child_pid( keys %$WORKER_STATUSES )
    {
        if( defined( $WORKER_STATUSES->{$child_pid} ) && defined( $WORKER_STATUSES->{$child_pid}->{name} ) )
        {
            if( $WORKER_STATUSES->{$child_pid}->{name} eq $cache_table )
            {
                $found = $child_pid;
            }
        }
    }

    if( $found )
    {
        tied( $WORKER_STATUSES )->shlock( LOCK_EX );
        $WORKER_STATUSES->{$found}->{replace} = 1;
        tied( $WORKER_STATUSES )->shunlock();
        return $found;
    }
    else
    {
        carp( "There doesn't seem to be a worker handling $cache_table\n" );
    }

    tied( $WORKER_STATUSES )->shunlock();

    return undef;
}

sub get_cache_table_definition($$$$$)
{
    my( $handle, $cache_table, $def, $columns, $uniques ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALARREF },
        { type => ARRAYREF },
        { type => ARRAYREF },
    );

    my $name;
    my $namespace;

    parse_cache_table( $cache_table, \$namespace, \$name );

    my $get_def_query = <<END_SQL;
    SELECT definition
      FROM $SCHEMA_NAME.tb_maintenance_object
     WHERE ( ?::VARCHAR IS NULL OR ?::VARCHAR = namespace::VARCHAR )
       AND ?::VARCHAR = name::VARCHAR
END_SQL

    my $sth = $handle->prepare( $get_def_query );

    return undef unless( $sth );

    $sth->bind_param( 1, $namespace );
    $sth->bind_param( 2, $namespace );
    $sth->bind_param( 3, $name );

    return undef unless( $sth->execute() );

    if( $sth->rows() == 0 )
    {
        $sth->finish();
        return undef;
    }

    my $row = $sth->fetchrow_hashref();

    $sth->finish();

    $$def = $row->{definition};
    $sth = $handle->prepare( $CACHE_TABLE_COLUMNS );

	return undef unless( $sth );

	$sth->bind_param( 1, $namespace );
    $sth->bind_param( 2, $name );

    unless( $sth->execute() )
    {
        $sth->finish();
        return undef;
    }

    while( $row = $sth->fetchrow_hashref() )
    {
        push( @$columns, $row->{column_name} );
    }

    $sth->finish();
    $sth = $handle->prepare( $CACHE_TABLE_UNIQUE );

    return undef unless( $sth );
    $sth->bind_param( 1, $namespace );
    $sth->bind_param( 2, $name );
    $sth->bind_param( 3, $namespace );
    $sth->bind_param( 4, $name );

    unless( $sth->execute() )
    {
        $sth->finish();
        return undef;
    }

    while( $row = $sth->fetchrow_hashref() )
    {
        push( @$uniques, $row->{unique_keys} );
    }

    $sth->finish();

    return 1;
}

sub command_check($$)
{
    my( $cache_table, $connection_map ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => HASHREF },
    );

    my $handle = DBI->connect( $connection_map->{connection_string}, $connection_map->{username}, undef );

    unless( $handle )
    {
        carp( "Failed to connect to database\n" );
        return undef;
    }

    my $def;
    my $columns = [];
    my $uniques = [];

    unless( get_cache_table_definition( $handle, $cache_table, \$def, $columns, $uniques ) )
    {
        carp( "Cache table '$cache_table' appears to no exist\n" );
        return undef;
    }

    if( !defined( $def ) )
    {
        carp( "Received empty definition for cache table '$cache_table'\n" );
        return undef;
    }

    $handle->do( 'BEGIN' );
    $handle->do( "SET application_name = '$EXTENSION_NAME validate $cache_table'" );
    $handle->do( "SET tcp_keepalives_idle = $TCP_KEEPALIVE" );
    $handle->do( "SET tcp_keepalives_interval = $TCP_KEEPALIVE_INTERVAL" );
    $handle->do( "SET tcp_keepalives_count = $TCP_KEEPALIVE_COUNT" );
    $handle->do( "SET tcp_user_timeout = $TCP_USER_TIMEOUT" );

    unless( $handle->do( "CREATE TEMP TABLE tt_ct AS( SELECT * FROM $cache_table )" ) )
    {
        carp( "Failed to store contents of cache table '$cache_table'\n" );
        $handle->do( 'ROLLBACK' );
        return undef;
    }

    unless( $handle->do( "CREATE TEMP TABLE tt_current AS( $def )" ) )
    {
        carp( "Failed to store contents of cache table definition for '$cache_table'\n" );
        $handle->do( 'ROLLBACK' );
        return undef;
    }

    my $index = 0;

    foreach my $unique_columns( @$uniques )
    {
        my $cols = join( ',', @$unique_columns );
        $handle->do( "CREATE UNIQUE INDEX ix_foo${index} ON tt_ct( $cols )" );
        $index++;
        $handle->do( "CREATE UNIQUE INDEX ix_foo${index} on tt_current( $cols )" );
        $index++;
    }

    my $create_ind_ct = 'CREATE INDEX ix_allcols_ct ON tt_ct(';
    my $create_ind_cur = 'CREATE INDEX ix_allcols_cur ON tt_current(';
    my $col_type_q = <<"END_SQL";
    SELECT t.typname
      FROM pg_class c
      JOIN pg_attribute a
        ON a.attrelid = c.oid
       AND a.attnum > 0
       AND a.attisdropped IS FALSE
      JOIN pg_type t
        ON t.oid = a.atttypid
      JOIN pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR ILIKE 'pg_temp%'
     WHERE c.relname::VARCHAR = ?
       AND a.attname::VARCHAR = ?
END_SQL

    my $col_type_sth = $handle->prepare( $col_type_q );
    unless( $col_type_sth )
    {
        carp( "Could not prep column type query" );
        return undef;
    }

    my $cast_cols = [];
    my $ind_cols = []; 

    foreach my $column( @$columns )
    {
        $col_type_sth->bind_param( 1, 'tt_current' );
        $col_type_sth->bind_param( 2, $column );
        unless( $col_type_sth->execute() )
        {
            carp( "Type check of column $column failed" );
            return undef;
        }
   
        my $type_row = $col_type_sth->fetchrow_hashref(); 
        my $type = $type_row->{typname};

        if( $type eq 'json' || $type eq 'jsonb' )
        {
            push( @$cast_cols, "${column}::TEXT" );
            push( @$ind_cols, "( ${column}::TEXT )" );
        }
        else
        {
            push( @$cast_cols, $column );
            push( @$ind_cols, $column );
        }
    }
 
    $create_ind_ct .= join( ',', @$ind_cols ) . ')';
    $create_ind_cur .= join( ',', @$ind_cols ) . ')';
    $handle->do( $create_ind_ct );
    $handle->do( $create_ind_cur );
    my $join_predicate = '( ( '
                       . join(
                             ' ) AND ( ',
                             map { "( ttes.$_ IS NULL AND ttcs.$_ IS NULL ) OR ( ttes.$_ = ttcs.$_ )" } @$cast_cols
                         )
                       . ') ) ';
    my $cs_uniques = [];
    my $es_uniques = [];

    foreach my $unique_columns( @$uniques )
    {
        push(
            @$cs_uniques,
            '( '
          . join(
                ' ) AND ( ',
                map { "ttcs.$_ IS NULL" } @$unique_columns
            )
          . ' )'
        );

        push(
            @$es_uniques,
            '( '
          . join(
                ' ) AND ( ',
                map { "ttes.$_ IS NULL" } @$unique_columns
            )
          . ' )'
        );
    }

    my $cs_where = '( ' . join( ' ) AND ( ', @$cs_uniques ) . ' )';
    my $es_where = '( ' . join( ' ) AND ( ', @$es_uniques ) . ' )';
    my $check_query_left = <<"END_SQL";
    CREATE TEMP TABLE tt_validation_left AS
    (
        SELECT ttcs.*
          FROM tt_ct ttes
     LEFT JOIN tt_current ttcs
            ON $join_predicate
         WHERE $cs_where
    );
END_SQL
    my $check_query_right = <<"END_SQL";
    CREATE TEMP TABLE tt_validation_right AS
    (
        SELECT ttes.*
          FROM tt_current ttcs
     LEFT JOIN tt_ct ttes
            ON $join_predicate
         WHERE $es_where
    )
END_SQL
    print "$check_query_left\n";
    print "$check_query_right\n";
    print "Checking table validity, this may take some time.\n";
    unless( $handle->do( $check_query_left ) )
    {
        carp( "Failed to create validation table for '$cache_table'\n" );
        $handle->do( 'ROLLBACK' );
        return undef;
    }

    unless( $handle->do( $check_query_right ) )
    {
        carp( "Failed to create validation table for '$cache_table'\n" );
        $handle->do( 'ROLLBACK' );
        return undef;
    }

    my $count_sth = $handle->prepare( 'SELECT COUNT(*) AS count FROM tt_validation_left' );

    unless( $count_sth )
    {
        carp( "Failed to prep left count query" );
        $handle->do( 'ROLLBACK' );
        return undef;
    }

    unless( $count_sth->execute() )
    {
        $count_sth->finish();
        $handle->do( 'ROLLBACK' );
        return undef;
    }

    my $count_row = $count_sth->fetchrow_hashref();

    my $count_left = $count_row->{count};
    $count_sth->finish();
    $count_sth = $handle->prepare( 'SELECT COUNT(*) AS COUNT FROM tt_validation_right' );
    return undef unless( $count_sth );

    unless( $count_sth->execute() )
    {
        $count_sth->finish();
        $handle->do( 'ROLLBACK' );
        return undef;
    }

    $count_row = $count_sth->fetchrow_hashref();

    my $count_right = $count_row->{count};
    $count_sth->finish();
    $handle->do( 'ROLLBACK' );
    if( $count_left > 0 || $count_right > 0 )
    {
        carp( "Failed: L: $count_left, R: $count_right" );
        return 0;
    }

    return 1;
}

sub parse_command($$;$)
{
    my( $command, $cache_table, $connection_map ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => SCALAR },
        { type => HASHREF | UNDEF, optional => 1 },
    );

    if( $command eq 'rebuild' )
    {
        my $pid = command_rebuild( $cache_table );
        if( $pid  )
        {
            print "Successfully commanded refresh of $cache_table to $pid\n";
            return;
        }
    }
    elsif( $command eq 'check' )
    {
        if( !defined( $connection_map ) || scalar( keys %$connection_map ) == 0 )
        {
            print_usage( 'Need database connection parameters for this command' );
        }

        my $result = command_check( $cache_table, $connection_map );

        if( defined( $result ) )
        {
            if( $result )
            {
                print "Table '$cache_table' passed validation\n";
            }
            else
            {
                print "Table '$cache_table' FAILED validation\n";
            }
        }
    }
    else
    {
        print_usage( "Invalid command $command" );
    }

    return;
}

sub read_xid_map()
{
    my $XID_MAP = [];
    my $old_warn = $SIG{__WARN__};
    $SIG{__WARN__} = sub { };
    eval { tie( $XID_MAP, 'IPC::Shareable', { key => 'XID' } ); };
    $SIG{__WARN__} = $old_warn;

    if( $OS_ERROR )
    {
        carp( "Could not tie XID_MAP - is $EXTENSION_NAME running?\n" );
        return undef;
    }

    tied( $XID_MAP )->shlock( LOCK_SH | LOCK_NB );
    my $xid_map = {};
    foreach my $elem( @$XID_MAP )
    {
        next unless( defined( $elem->{xid} ) );
        $xid_map->{$elem->{xid}} = {
            in_use   => [],
            snapshot => $elem->{snapshot},
        };

        foreach my $pid( @{$elem->{in_use}} )
        {
            push( @{$xid_map->{$elem->{xid}}->{in_use}}, $pid );
        }
    }

    tied( $XID_MAP )->shunlock();
    return $xid_map;
}

sub read_worker_statuses()
{
    my $WORKER_STATUSES = {};

    eval{ tie( $WORKER_STATUSES, 'IPC::Shareable', { key => 'STATUSES' } ); };
    if( $OS_ERROR )
    {
        carp( "Could not tie WORKER_STATUSES - is $EXTENSION_NAME running?\n" );
        return undef;
    }

    tied( $WORKER_STATUSES )->shlock( LOCK_SH | LOCK_NB );
    my $worker_statuses = {};

    foreach my $pid( keys %$WORKER_STATUSES )
    {
        my $status                = $WORKER_STATUSES->{$pid}->{status};
        my $shutdown              = $WORKER_STATUSES->{$pid}->{shutdown};
        my $replace               = $WORKER_STATUSES->{$pid}->{replace};
        my $last_lsn              = $WORKER_STATUSES->{$pid}->{last_lsn};
        my $pk_maintenance_object = $WORKER_STATUSES->{$pid}->{maintenance_object};
        my $name                  = $WORKER_STATUSES->{$pid}->{name};
        $worker_statuses->{$pid}  = {
            status => $status,
            shutdown => $shutdown,
            replace  => $replace,
            last_lsn => $last_lsn,
            maintenance_object => $pk_maintenance_object,
            name               => $name,
        };
    }

    tied( $WORKER_STATUSES )->shunlock();
    return $worker_statuses;
}

sub read_worker_filter_tables()
{
    my $WORKER_FILTER_TABLES = {};

    eval { tie( $WORKER_FILTER_TABLES, 'IPC::Shareable', { key => 'WORKER_FILTER_TABLES' } ); };
    if( $OS_ERROR )
    {
        carp( "Could not tie WORKER_FILTER_TABLES - is $EXTENSION_NAME running?\n" );
        return undef;
    }

    tied( $WORKER_FILTER_TABLES )->shlock( LOCK_SH | LOCK_NB );
    my $worker_filter_tables = {};
    foreach my $pid( keys %$WORKER_FILTER_TABLES )
    {
        foreach my $filter_table( keys %{$WORKER_FILTER_TABLES->{$pid}} )
        {
            next if( $filter_table eq '__ACTIVE_CHANGES__' );
            # We could likely read the queue data but we'd need the WAL level fed in from $WORKER_STATUSES
            my $queued_change_count = scalar( @{$WORKER_FILTER_TABLES->{$pid}->{$filter_table}} );
            $worker_filter_tables->{$pid}->{$filter_table} = $queued_change_count;
        }
    }

    tied( $WORKER_FILTER_TABLES )->shunlock();
    return $worker_filter_tables;
}

sub print_worker_table()
{
    my $xid_map = read_xid_map();
    my $worker_filter_tables = read_worker_filter_tables();
    my $worker_statuses = read_worker_statuses();

    if( !defined $xid_map || !defined( $worker_filter_tables ) || !defined( $worker_statuses ) )
    {
        return;
    }
    my $table = Text::Table->new(
        "PID\n---", "|\n|",
        "Cache Table\n-----------", "|\n|",
        "Status\n------", "|\n|",
        "Last LSN\n--------", "|\n|",
        "Filter Tables\n-------------", "|\n|",
        "Total Queued\n-------------", "|\n|",
        "Snapshot\n--------", "|\n|",
        "XID\n---"
    );

    my @keys = sort { $a <=> $b } keys( %$xid_map );
    my $min_xid = shift( @keys );
    my $min_snapshot = $xid_map->{$min_xid}->{snapshot};
    my $max_xid = pop( @keys );
    my $max_snapshot = $xid_map->{$max_xid}->{snapshot};

    print "XID Mapping ranges:\n";
    print "Min: $min_xid ( $min_snapshot )\n";
    print "Max: $max_xid ( $max_snapshot )\n";

    foreach my $pid( sort { $a <=> $b } keys %$worker_statuses )
    {
        my $status                = $worker_statuses->{$pid}->{status};
        my $shutdown_bit          = $worker_statuses->{$pid}->{shutdown};
        my $pk_maintenance_object = $worker_statuses->{$pid}->{maintenance_object};
        my $last_lsn              = $worker_statuses->{$pid}->{last_lsn};
        my $ct_name               = $worker_statuses->{$pid}->{name};
        my $status_text           = parse_worker_status( $status );

        # Find held XIDs
        my $held_xid;
        my $held_snapshot;

        foreach my $xid( keys %$xid_map )
        {
            if( grep /^$pid$/, @{$xid_map->{$xid}->{in_use}} )
            {
                $held_xid      = $xid;
                $held_snapshot = $xid_map->{$xid}->{snapshot};
                last;
            }
        }

        # Find queue depth for filter_tables
        my $queue = {};
        my $total_queued = 0;
        my $filter_tables = 0;
        foreach my $filter_table( keys %{$worker_filter_tables->{$pid}} )
        {
            $filter_tables++;
            if( $worker_filter_tables->{$pid}->{$filter_table} > 0 )
            {
                $queue->{$filter_table} = $worker_filter_tables->{$pid}->{$filter_table};
                $total_queued += $worker_filter_tables->{$pid}->{$filter_table};
            }
        }

        $table->add(
            $pid, '|',
            $ct_name, '|',
            $status_text, '|',
            $last_lsn, '|',
            $filter_tables, '|',
            $total_queued, '|',
            $held_snapshot, '|',
            $held_xid
        );

        if( scalar( keys %$queue ) > 0 )
        {
            foreach my $filter_table( sort { $a cmp $b } keys %$queue )
            {
                my $count = $queue->{$filter_table};
                $table->add(
                    undef, '|',
                    undef, '|',
                    undef, '|',
                    undef, '|',
                    $filter_table, '|',
                    $count, '|',
                    undef, '|',
                    undef
                );
            }
        }
    }

    print $table;
}

## MAIN PROGRAM
our( $opt_C, $opt_v, $opt_d, $opt_U, $opt_p, $opt_h );

print_usage( 'Invalid arguments' ) unless( getopts( 'C:v:d:U:h:p:' ) );

my $command  = $opt_C;
my $ct       = $opt_v;
my $dbname   = $opt_d;
my $username = $opt_U;
my $port     = $opt_p;
my $hostname = $opt_h;

if(
      ( defined( $command ) && !defined( $ct ) )
   || ( defined( $ct ) && !defined( $command ) )
  )
{
    print_usage( 'Must specify -v and -C together' );
}

my $connection_map = {};
if( defined( $dbname ) )
{
    $port = 5432 unless( defined( $port ) );
    print_usage( 'Invalid port' ) unless( $port =~ m/^\d+$/ && $port <= 65535 );
    print_usage( 'Invalid username' ) unless( defined( $username ) && length( $username ) > 0 );
    print_usage( 'Invalid hostname' ) unless( defined( $hostname ) && length( $hostname ) > 0 );
    $connection_map->{connection_string} = "dbi:Pg:dbname=$dbname;host=$hostname;port=$port";
    $connection_map->{username} = $username;
}

if( !defined( $command ) && !defined( $ct ) )
{
    print_worker_table();
    exit 0;
}

if( defined( $command ) && defined( $ct ) )
{
    parse_command( $command, $ct, $connection_map );
}
