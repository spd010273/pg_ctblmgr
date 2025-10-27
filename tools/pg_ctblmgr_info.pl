#!/usr/bin/perl

use strict;
use warnings;
use utf8;

use Params::Validate qw( :all );
use Readonly;
use English qw( -no_match_vars );

use Getopt::Std;
use IPC::SysV;
use Data::Dumper;
use Text::Table;
use DBI;

use FindBin;
use lib "$FindBin::Bin/../service/lib";

use Util;
use Shm;
Readonly my $TCP_KEEPALIVE          => 60;
Readonly my $TCP_KEEPALIVE_INTERVAL => 5;
Readonly my $TCP_KEEPALIVE_COUNT    => 200;
Readonly my $TCP_USER_TIMEOUT       => 1000 * 60 * 5;
Readonly::Scalar my $EXTENSION_NAME => 'pg_ctblmgr';
Readonly::Scalar my $SCHEMA_NAME    => 'pgctblmgr';
Readonly my $WS_KEY  => 17783313 + 1;

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

    unless( get_or_create_shm( $WS_KEY ) )
    {
        warn( "Failed to attach to shared memory - is $EXTENSION_NAME running?\n" );
        return undef;
    }

    do_lock( $WS_KEY, $READ_LOCK );
    $WORKER_STATUSES = readmem( $WS_KEY );
    do_lock( $WS_KEY, $READ_UNLOCK );
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
        do_lock( $WS_KEY, $WRITE_LOCK );
        $WORKER_STATUSES = readmem( $WS_KEY );
        $WORKER_STATUSES->{$found}->{replace} = 1 if( defined( $WORKER_STATUSES->{$found} ) );
        writemem( $WS_KEY, $WORKER_STATUSES );
        do_lock( $WS_KEY, $WRITE_UNLOCK );
        system( "kill -1 $found" );
        return $found;
    }
    else
    {
        warn( "There doesn't seem to be a worker handling $cache_table\n" );
    }

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
    SELECT definition,
           namespace
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

    $$def      = $row->{definition};
    $namespace = $row->{namespace};
    $sth       = $handle->prepare( $CACHE_TABLE_COLUMNS );

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
        warn( "Failed to connect to database\n" );
        return undef;
    }

    my $def;
    my $columns = [];
    my $uniques = [];

    unless( get_cache_table_definition( $handle, $cache_table, \$def, $columns, $uniques ) )
    {
        warn( "Cache table '$cache_table' appears to not exist\n" );
        return undef;
    }

    if( !defined( $def ) )
    {
        warn( "Received empty definition for cache table '$cache_table'\n" );
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
        warn( "Failed to store contents of cache table '$cache_table'\n" );
        $handle->do( 'ROLLBACK' );
        return undef;
    }

    unless( $handle->do( "CREATE TEMP TABLE tt_current AS( $def )" ) )
    {
        warn( "Failed to store contents of cache table definition for '$cache_table'\n" );
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
        warn( "Could not prep column type query" );
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
            warn( "Type check of column $column failed" );
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
    $handle->do( $create_ind_ct ) if( scalar( @$ind_cols ) > 0 && scalar( @$ind_cols ) <= 32 );
    $handle->do( $create_ind_cur ) if( scalar( @$ind_cols ) > 0 && scalar( @$ind_cols ) <= 32 );
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
    #print "$check_query_left\n";
    #print "$check_query_right\n";
    print "Checking table validity, this may take some time.\n";
    unless( $handle->do( $check_query_left ) )
    {
        warn( "Failed to create validation table for '$cache_table'\n" );
        $handle->do( 'ROLLBACK' );
        return undef;
    }

    unless( $handle->do( $check_query_right ) )
    {
        warn( "Failed to create validation table for '$cache_table'\n" );
        $handle->do( 'ROLLBACK' );
        return undef;
    }

    my $count_sth = $handle->prepare( 'SELECT COUNT(*) AS count FROM tt_validation_left' );

    unless( $count_sth )
    {
        warn( "Failed to prep left count query" );
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
        warn( "Failed: L: $count_left, R: $count_right" );
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

sub read_worker_statuses()
{
    my $WORKER_STATUSES = {};

    unless( get_or_create_shm( $WS_KEY ) )
    {
        warn( "Could not tie WORKER_STATUSES - is $EXTENSION_NAME running?\n" );
        return undef;
    }

    do_lock( $WS_KEY, $READ_LOCK );
    $WORKER_STATUSES = readmem( $WS_KEY );
    do_lock( $WS_KEY, $READ_UNLOCK );
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

    return $worker_statuses;
}

sub print_worker_table()
{
    my $worker_statuses = read_worker_statuses();

    if( !defined( $worker_statuses ) )
    {
        return;
    }
    my $table = Text::Table->new(
        "PID\n---", "|\n|",
        "Cache Table\n-----------", "|\n|",
        "Status\n------", "|\n|",
        "Last LSN\n--------",
    );

    foreach my $pid( sort { $a <=> $b } keys %$worker_statuses )
    {
        my $status                = $worker_statuses->{$pid}->{status};
        my $shutdown_bit          = $worker_statuses->{$pid}->{shutdown};
        my $pk_maintenance_object = $worker_statuses->{$pid}->{maintenance_object};
        my $last_lsn              = $worker_statuses->{$pid}->{last_lsn};
        my $ct_name               = $worker_statuses->{$pid}->{name};
        my $status_text           = parse_worker_status( $status );

        $table->add(
            $pid, '|',
            $ct_name, '|',
            $status_text, '|',
            $last_lsn,
        );

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
