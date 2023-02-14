#!/usr/bin/perl

use strict;
use warnings;
use utf8;

use DBI;
use Params::Validate qw( :all );
use Carp;
use Readonly;
use English qw( -no_match_vars );

use JSON;
use IO::Select;
use IO::Handle;
use Getopt::Std;
use Time::HiRes qw( gettimeofday tv_interval );
use POSIX qw( strftime setsid :sys_wait_h );
use Cwd qw( abs_path );
use IPC::Shareable qw( :lock );
use IO::Interactive qw( is_interactive );
use Data::Dumper;

# NOTE: IPC::Shareable keys seeem to be extremely short (4-8 chars) and may collide!
$OUTPUT_AUTOFLUSH = 1;

## STATIC GLOBAL VARIABLES
Readonly::Scalar my $DEBUG                  => 1;
Readonly::Scalar my $CLEAN_UP               => 0; # Emergency shm cleanup
Readonly::Scalar my $LOG_LEVEL_FATAL        => 5;
Readonly::Scalar my $LOG_LEVEL_ERROR        => 4;
Readonly::Scalar my $LOG_LEVEL_WARNING      => 3;
Readonly::Scalar my $LOG_LEVEL_INFO         => 2;
Readonly::Scalar my $LOG_LEVEL_DEBUG        => 1;
Readonly::Scalar my $WORKER_STATUS_STARTUP  => 1;
Readonly::Scalar my $WORKER_STATUS_RUNNING  => 2;
Readonly::Scalar my $WORKER_STATUS_UPDATING => 3;
Readonly::Scalar my $WORKER_STATUS_EXITED   => 4;
Readonly::Scalar my $EXTENSION_NAME         => 'pg_ctblmgr';
Readonly::Scalar my $SCHEMA_NAME            => 'pgctblmgr';
Readonly::Scalar my $SQL_STATE_ADMIN_TERM   => '57P01';
Readonly::Scalar my $SQL_STATE_ADMIN_CANC   => '57014';
Readonly::Scalar my $MAX_QUERY_RETRIES      => 5;
Readonly::Scalar my $USAGE                  => <<"USAGE";
    Usage:
    $0
        -U <database user>
        -h <database host name>
        -d <database name>
        [ -p <database port> ]
        [ -D ( do not daemonize ) ]
USAGE

Readonly::Scalar my $REPLICATION_PEEK_QUERY => <<END_SQL;
    SELECT lsn,
           xid,
           data::JSONB AS data
      FROM pg_catalog.pg_logical_slot_peek_changes(
               ?::NAME,
               NULL::PG_LSN,
               NULL::INTEGER,
               'wal-level'::VARCHAR,
               ?::VARCHAR,
               'filter-tables'::VARCHAR,
               ?::VARCHAR,
               'include-transaction'::VARCHAR,
               'TRUE'::VARCHAR
           )
     WHERE ?::PG_LSN IS NULL OR lsn > ?::PG_LSN
  ORDER BY lsn ASC
END_SQL

Readonly::Scalar my $REPLICATION_SEEK_QUERY => <<END_SQL;
    SELECT lsn,
           xid,
           data::JSONB AS data
      FROM pg_catalog.pg_logical_slot_get_changes(
               ?,
               ?,
               NULL,
               'wal-level',
               ?,
               'filter-tables',
               ?,
               'include-transaction',
               TRUE
           )
  ORDER BY lsn ASC
END_SQL

Readonly::Scalar my $FILTER_TABLE_OID_CACHE => <<END_SQL;
    SELECT n.nspname::VARCHAR || '.' || c.relname::VARCHAR AS name,
           c.oid
      FROM pg_class c
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
     WHERE n.nspname::VARCHAR || '.' || c.relname::VARCHAR = ANY( ARRAY[ __BINDPOINTS__ ]::VARCHAR[] )
END_SQL

Readonly::Scalar my $CREATE_TEST_VIEW => <<END_SQL;
    CREATE TEMPORARY VIEW _pgctblmgr_test AS
    (
        __DEFINITION__
    )
END_SQL

Readonly::Scalar my $GET_TEST_VIEW_PARSE_TREE => <<"END_SQL";
    SELECT ${SCHEMA_NAME}.fn_get_parse_tree( r.ev_action )::JSONB AS tree
      FROM pg_catalog.pg_rewrite r
     WHERE r.rulename = '_RETURN'
       AND r.ev_class = '_pgctblmgr_test'::REGCLASS::OID
END_SQL

Readonly::Scalar my $DROP_TEST_VIEW => <<END_SQL;
    DROP VIEW IF EXISTS _pgctblmgr_test
END_SQL

Readonly::Scalar my $CHECK_CACHE_TABLE_EXISTS => <<END_SQL;
    SELECT c.oid
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
     WHERE c.relkind = 'r'
       AND c.relname::VARCHAR = ?
END_SQL

Readonly::Scalar my $CREATE_CACHE_TABLE => <<END_SQL;
CREATE TABLE IF NOT EXISTS __TABLE__ AS
(
    __DEFINITION__
);
END_SQL

Readonly::Scalar my $GET_CACHE_TABLE_DEFINITION => <<"END_SQL";
    SELECT d.name AS driver,
           mo.namespace,
           mo.name,
           mo.definition
      FROM ${SCHEMA_NAME}.tb_driver d
INNER JOIN ${SCHEMA_NAME}.tb_maintenance_object mo
        ON mo.driver = d.driver
     WHERE mo.maintenance_object = ?
END_SQL

Readonly::Scalar my $EXTENSION_CHECK_QUERY => <<END_SQL;
    SELECT n.oid
      FROM pg_namespace n
     WHERE n.nspname = ?
END_SQL

Readonly::Scalar my $GET_WORKER_LIST => <<"END_SQL";
    SELECT rs.maintenance_channel,
           rs.filter,
           mg.wal_level,
           mo.maintenance_object
      FROM ${SCHEMA_NAME}.__pgctblmgr_repl_slot rs
INNER JOIN ${SCHEMA_NAME}.tb_maintenance_object mo
        ON mo.maintenance_object = rs.id
INNER JOIN ${SCHEMA_NAME}.tb_maintenance_group mg
        ON mg.maintenance_group = mo.maintenance_group
END_SQL

Readonly::Scalar my $CHECK_EXTENSION_RUNNING_QUERY => <<"END_SQL";
    SELECT pg_try_advisory_lock(
               c.oid::BIGINT
           ) AS lock_acquired
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
     WHERE c.relname::VARCHAR = ?
       AND c.relkind = 'r'
END_SQL

## GLOBAL VARIABLES
my $CONNECTION_MAP = {};
my $PARENT_PID     = $PROCESS_ID;
my $SLOT_NAME      = '__pg_ctblmgr';
my $LOG_FILE       = '';
my $LOG_FH         = undef;
my $DAEMONIZE      = 0;
my $CHILDREN       = [];
my $PINS           = [];

sub _terminate()
{
    if( $PROCESS_ID == $PARENT_PID )
    {
        #this is crucial to prevent running out of shm after crashes / terminations
        shm_cleanup();
    }
    exit( 0 );
}

$SIG{INT} = \&_terminate;

sub _log($$)
{
    my( $log_level, $message ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => SCALAR },
    );

    my $message_prefix = '';
    my $log_level_name = '';

    if( !is_interactive() && $DAEMONIZE )
    {
        unless( defined( $LOG_FH ) )
        {
            #attempt to open log file
        }
    }

    return if( $log_level == $LOG_LEVEL_DEBUG && !$DEBUG );

    if( $log_level == $LOG_LEVEL_DEBUG )
    {
        $log_level_name = 'DEBUG';
    }
    elsif( $log_level == $LOG_LEVEL_INFO )
    {
        $log_level_name = 'INFO';
    }
    elsif( $log_level == $LOG_LEVEL_WARNING )
    {
        $log_level_name = 'WARNING';
    }
    elsif( $log_level == $LOG_LEVEL_ERROR )
    {
        $log_level_name = 'ERROR';
    }
    elsif( $log_level == $LOG_LEVEL_FATAL )
    {
        $log_level_name = 'FATAL';
    }
    else
    {
        return;
    }

    my $pid  = $PROCESS_ID;
    my ( $sec, $min, $hour, $mday, $mon, $year, $wday, $yday, $is_dst ) = localtime( time );
    my $time_stamp = sprintf(
        '%04d-%02d-%02d %02d:%02d:%02d',
        $year + 1900,
        $mon + 1,
        $mday,
        $hour,
        $min,
        $sec
    );

    my $log_message = "$time_stamp [$pid] $log_level_name: $message\n";

    if( is_interactive() || !$DAEMONIZE )
    {
        if( $log_level >= $LOG_LEVEL_ERROR )
        {
            print( STDERR $log_message );
        }
        else
        {
            print( STDOUT $log_message );
        }
    }
    else
    {
        print( $LOG_FH $log_message );
    }

    if( $log_level == $LOG_LEVEL_FATAL )
    {
        _terminate();
    }

    return;
}

sub usage($)
{
    my( $message ) = validate_pos(
        @_,
        { type => SCALAR },
    );

    print "$message\n" if( $message );
    print $USAGE;

    exit( 1 );
}

# lsn_cmp( A, B ):
#  Compares two LSNs (Log Sequence Number) and determines which is greater
#  Returns:
#   - -1 if( A < B )
#   - 0 if( A == B )
#   - 1 if( A > B )
#  LSNs are a 64-bit integer represented as two 32-bit values (expressed in hex),
#  separated by a slash IE:
#  XXXXXXXX/YYYYYYYY
sub lsn_cmp($$)
{
    my( $a_lsn, $b_lsn ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => SCALAR },
    );

    my $a_ms = $a_lsn;
    $a_ms =~ s/\/.*$//;
    $a_ms = 0 unless( $a_ms );
    my $b_ms = $b_lsn;
    $b_ms =~ s/\/.*$//;
    $b_ms = 0 unless( $b_ms );
    my $a_ls = $a_lsn;
    $a_ls =~ s/^[0-9a-fA-F]*\///;
    $a_ls = 0 unless( $a_ls );
    my $b_ls = $b_lsn;
    $b_ls =~ s/^[0-9a-fA-F]*\///;
    $b_ls = 0 unless( $b_ls );
    my $a_ms_int = hex( $a_ms );
    my $a_ls_int = hex( $a_ls );
    my $b_ms_int = hex( $b_ms );
    my $b_ls_int = hex( $b_ls );

    if( $a_ms_int == $b_ms_int )
    {
        return 1 if( $a_ls_int > $b_ls_int );
        return 0 if( $a_ls_int == $b_ls_int );
        return -1;
    }
    elsif( $a_ms_int > $b_ms_int )
    {
        return 1;
    }

    return -1;
}

sub try_query($$;$)
{
    my( $handle, $query, $params ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => ARRAYREF | UNDEF, optional => 1 },
    );

    my $sth;
    my $retry_counter     = 0;
    my $last_backoff_time = 0;
    my $last_sql_state    = '';
    my $sleep_backoff     = 1;
    my $try_count         = 0;

    RETRY_CONN:
    $retry_counter++;
    return undef if( $retry_counter > $MAX_QUERY_RETRIES );

    until( defined( $handle ) && $handle->pg_ping > 0 )
    {
        _log( $LOG_LEVEL_INFO, 'Not connected to DB, attempting to reconnect...' ) if( $DEBUG );
        $try_count++;
        sleep( $sleep_backoff );
        $handle = DBI->connect(
            $CONNECTION_MAP->{connection_string},
            $CONNECTION_MAP->{user_name},
            undef
        );

        $last_backoff_time = $sleep_backoff;
        $sleep_backoff    += int( rand( 2 ** $try_count - 1 ) );

        if( $try_count > 5 )
        {
            ## XXX Check if DB was dropped
        }
    }

    # We're connected to the DB at this point
    if( $PARENT_PID == $PROCESS_ID )
    {
        unless( check_extension_running( $handle ) )
        {
            _log( $LOG_LEVEL_FATAL, "Failed to acquire lock after reconnecting to database" );
        }
    }

    until( $handle->pg_ping > 0 && ( $sth = $handle->prepare( $query ) ) )
    {
        goto RETRY_CONN;
    }

    goto RETRY_CONN unless( defined( $sth ) );

    if(
          defined( $params )
       && ref( $params ) eq 'ARRAY'
       && scalar( @$params ) > 0
      )
    {
        # Bind Params
        my $bind_index = 1;

        foreach my $param( @$params )
        {
            $sth->bind_param( $bind_index, $param );
            $bind_index++;
        }
    }

    $sleep_backoff     = 1;
    $try_count         = 0;
    $last_backoff_time = 0;

    until( $sth->execute() )
    {
        _log( $LOG_LEVEL_ERROR, 'Failed to execute statement, retrying...' ) if( $DEBUG );

        $try_count++;
        goto RETRY_CONN if( $handle->pg_ping <= 0 );
        my $query_state = $handle->state;

        if( $query_state eq $SQL_STATE_ADMIN_CANC )
        {
            _log( $LOG_LEVEL_ERROR, 'Query canceled by administrator. Retrying...' );
        }
        elsif( $query_state eq $SQL_STATE_ADMIN_TERM )
        {
            _log( $LOG_LEVEL_ERROR, 'Query terminated by administrator. Retrying...' );
        }

        sleep( $sleep_backoff );

        goto RETRY_CONN if( $handle->pg_ping <= 0 );
        $last_backoff_time = $sleep_backoff;
        $sleep_backoff    += int( rand( 2 ** $try_count - 1 ) );

        return undef if( $try_count >= $MAX_QUERY_RETRIES );
    }

    return $sth;
}

sub get_relcache($$)
{
    my( $handle, $filter_tables ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => ARRAYREF },
    );

    my $query = $FILTER_TABLE_OID_CACHE;
    my $bindpoints = '?,' x ( scalar( @$filter_tables ) - 1 );
    $bindpoints .= '?';

    $query =~ s/__BINDPOINTS__/$bindpoints/;

    my $sth = try_query( $handle, $query, $filter_tables );

    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to get relcache for cache table" );
    }

    my $cache = {};
    while( my $row = $sth->fetchrow_hashref() )
    {
        my $name = $row->{name};
        my $oid  = $row->{oid};
        $cache->{$name} = $oid;
    }

    return $cache;
}

sub check_extension($)
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $sth = try_query(
        $handle,
        $EXTENSION_CHECK_QUERY,
        [ $SCHEMA_NAME ]
    );

    return 0 unless( $sth );

    if( $sth->rows() > 0 )
    {
        $sth->finish();
        return 1;
    }

    $sth->finish();
    return 0;
}

sub check_extension_running($)
{
    # This subroutine needs to use DBI methods to avoid deep recursion
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $sth = $handle->prepare(
        $CHECK_EXTENSION_RUNNING_QUERY
    );

    return 0 unless( $sth );

    $sth->bind_param( 1, $SCHEMA_NAME );
    $sth->bind_param( 2, '__pgctblmgr_repl_slot' );

    return 0 unless( $sth->execute() );

    if( $sth->rows() > 0 )
    {
        my $row    = $sth->fetchrow_hashref();
        my $result = $row->{lock_acquired};
        $sth->finish();
        return $result;
    }

    $sth->finish();
    return 0;
}

sub replication_peek($$$)
{
    my( $handle, $filter_tables, $max_lsn ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALARREF },
        { type => SCALAR | UNDEF, optional => 1 },
    );

    my $sth = try_query(
        $handle,
        $REPLICATION_PEEK_QUERY,
        [ $SLOT_NAME, 'F', $filter_tables, $$max_lsn, $$max_lsn ]
    );

    unless( $sth )
    {
        return 0;
    }

    if( $sth->rows() > 0 )
    {
        my $intermediate_data = {};
        my $xids              = [];

        while( my $row = $sth->fetchrow_hashref() )
        {
            my $lsn  = $row->{lsn};

            if( !defined $$max_lsn )
            {
                $$max_lsn = $lsn;
            }
            else
            {
                if( lsn_cmp( $$max_lsn, $lsn ) < 0 )
                {
                    $$max_lsn = $lsn;
                }
            }

            my $xid  = $row->{xid};
            my $data = from_json( $row->{data} );
            my $out  = { lsn => $lsn, xid => $xid, data => $data };

            unless( $xid ~~ @$xids )
            {
                push( @$xids, $xid );
            }

            if(
                  $data->{type} ne 'COMMIT'
               && $data->{type} ne 'BEGIN'
               && $data->{type} ne 'ROLLBACK'
              )
            {
                push( @{$intermediate_data->{$xid}->{DML}}, $out );
            }
            else
            {
                # Assume transaction demarcation
                $intermediate_data->{$xid}->{$data->{type}} = $out->{lsn};
            }
        }
        
        $sth->finish();

        my $out_data = [];
        foreach my $xid( @$xids )
        {
            if(
                    exists( $intermediate_data->{$xid}->{COMMIT} )
                 && exists( $intermediate_data->{$xid}->{BEGIN} )
              )
            {
                if(
                      exists( $intermediate_data->{$xid}->{DML} )
                   && scalar( @{$intermediate_data->{$xid}->{DML}} )
                  )
                {
                    foreach my $dml( @{$intermediate_data->{$xid}->{DML}} )
                    {
                        $dml->{commit_lsn} = $intermediate_data->{$xid}->{COMMIT};
                        push( @$out_data, $dml )
                    }
                }
            }
        }

        return if( scalar( @$out_data ) == 0 );
        return $out_data;
    }

    $sth->finish();
    return 0;
}

sub replication_seek($$$)
{
    my( $handle, $filter_tables, $lsn ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALAR },
    );

    my $sth = try_query(
        $handle,
        $REPLICATION_SEEK_QUERY,
        [ $SLOT_NAME, $lsn, 'F', $filter_tables ]
    );

    unless( $sth )
    {
        return 0;
    }

    if( $sth->rows() > 0 )
    {
        while( my $row = $sth->fetchrow_hashref() )
        {
            my $lsn = $row->{lsn};
            my $xid = $row->{xid};
            my $data = $row->{data};
        }

        $sth->finish();
        return;
    }

    $sth->finish();
    return 0;
}

sub check_ct_exists($$$$)
{
    my( $handle, $schema, $name, $definition ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALAR },
        { type => SCALAR },
    );

    my $sth = try_query( $handle, $CHECK_CACHE_TABLE_EXISTS, [ $schema, $name ] );
    
    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to verify that $schema.$name exists" );
    }

    if( $sth->rows() > 0 )
    {
        $sth->finish();
        _log( $LOG_LEVEL_DEBUG, "Cache Table $schema.$name already exists" );
        return;
    }

    my $create_query = $CREATE_CACHE_TABLE;
    $create_query =~ s/__TABLE__/${schema}.${name}/;
    $create_query =~ s/__DEFINITION__/$definition/;

    $sth = try_query( $handle, $create_query, undef );

    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to create cache table $schema.$name" );
    }

    _log( $LOG_LEVEL_DEBUG, "Cache Table $schema.$name created" );
    $sth->finish();
    return;
}

sub get_query_parsetree($$)
{
    my( $handle, $definition ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
    );

    my $get_parse_tree_q = $GET_TEST_VIEW_PARSE_TREE;
    $get_parse_tree_q =~ s/__DEFINITION__/$definition/;

    my $sth = try_query( $handle, $get_parse_tree_q, undef );

    my $defrow = $sth->fetchrow_hashref();
    my $query_tree = $defrow->{tree};
    $sth->finish();
    my $parse_tree_obj = from_json( $query_tree );

    return unless( $parse_tree_obj );
    return $parse_tree_obj;
}

sub find_table_aliases($$$$)
{
    my( $handle, $relcache, $definition, $filter_tables ) = validate_pos(
        @_,
        { type => OBJECT   },
        { type => HASHREF  },
        { type => SCALAR   },
        { type => ARRAYREF },
    );

    my $parse_tree_obj = get_query_parsetree( $handle, $definition );

    return unless( defined $parse_tree_obj );
    # XXX
}

sub get_worker_list($)
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $sth = try_query( $handle, $GET_WORKER_LIST );

    unless( $sth )
    {
        return undef;
    }

    if( $sth->rows() > 0 )
    {
        my $worker_data = [];

        while( my $row = $sth->fetchrow_hashref() )
        {
            my $maintenance_channel = $row->{maintenance_channel};
            my $filter_tables       = $row->{filter};
            my $wal_level           = $row->{wal_level};
            my $maintenance_object  = $row->{maintenance_object};

            push(
                @$worker_data,
                {
                    maintenance_channel => $maintenance_channel,
                    filter_tables       => $filter_tables,
                    wal_level           => $wal_level,
                    maintenance_object  => $maintenance_object,
                }
            );
        }

        $sth->finish();
        return $worker_data;
    }

    $sth->finish();
    return undef;
}

sub shm_cleanup()
{
    my $WORKER_FILTER_TABLES;
    my $WORKER_STATUSES;
    tie( $WORKER_FILTER_TABLES, 'IPC::Shareable', { key => 'WORKER_FILTER_TABLES' } );
    tie( $WORKER_STATUSES, 'IPC::Shareable', { key => 'STATUSES' } );
    
    tied( $WORKER_FILTER_TABLES )->clean_up_all();
    tied( $WORKER_STATUSES )->clean_up_all();

    my $sigwarn = $SIG{__WARN__}; 
    local $SIG{__WARN__} = sub {};
    my $test;
    for( my $i = 0; $i < 1000; $i++ )
    {
        my $key = "P$i";
        eval { tie( $test, 'IPC::Shareable', { key => $key } ) };
        if( !$OS_ERROR )
        {
            if( tied( $test ) )
            {
                if( tied( $test )->clean_up_all() )
                {
                    print "PIN key $key removed\n";
                }
            }
        }
    }

    $SIG{__WARN__} = $sigwarn;
    exit( 0 );
}

sub parent_loop($$$)
{
    my( $WORKER_STATUSES, $PINS, $WORKER_FILTER_TABLES ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => ARRAYREF },
        { type => HASHREF },
    );

    #print Dumper( $WORKER_STATUSES );
    #print Dumper( $WORKER_FILTER_TABLES );
    my $handle = DBI->connect(
        $CONNECTION_MAP->{connection_string},
        $CONNECTION_MAP->{user_name},
        undef
    );

    if( !tied( $WORKER_FILTER_TABLES ) )
    {
        _log( $LOG_LEVEL_DEBUG, "SHM not tied" );
        tie( $WORKER_FILTER_TABLES, 'IPC::Shareable', { key => 'WORKER_FILTER_TABLES' } );
    }

    if( !tied( $WORKER_STATUSES ) )
    {
        _log( $LOG_LEVEL_DEBUG, "STATs not tied" );
        tie( $WORKER_STATUSES, 'IPC::Shareable', { key => 'STATUSES' } );
    }

    unless( $handle )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to connect to database" );
    }

    unless( check_extension_running( $handle ) )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to obtain lock on database - another $EXTENSION_NAME instance seems to be running" );
    }

    my $last_lsn_applied;
    my $last_peeked_lsn;

    while( 1 )
    {
        tied( $WORKER_FILTER_TABLES )->shlock( LOCK_SH );
        foreach my $filter_table( keys %$WORKER_FILTER_TABLES )
        {
            my $data = replication_peek( $handle, $filter_table, \$last_peeked_lsn );
            next unless( $data );
            my $index = 0;
 
            foreach my $PIN( @{$WORKER_FILTER_TABLES->{$filter_table}->{pins}} )
            {
                my $pid = $WORKER_FILTER_TABLES->{$filter_table}->{pids}->[$index];
                my $keyname = "P${pid}${filter_table}";

                tie( $PIN, 'IPC::Shareable', { key => $keyname } );
                tied( $PIN )->shlock( LOCK_EX );

                foreach my $change( @$data )
                {
                    push( @$PIN, $change );
                }

                tied( $PIN )->shunlock();
                untie( $PIN );

                $index++;
            }
        }

        tied( $WORKER_FILTER_TABLES )->shunlock();
        sleep( 5 );

    }

    return;
}

sub worker_entrypoint($$$$)
{
    my( $wal_level, $filter_tables, $maintenance_channel, $pk_maintenance_object ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => ARRAYREF },
        { type => SCALAR },
        { type => SCALAR },
    );

    my $WORKER_FILTER_TABLES = {};
    my $WAL_DATA;
    my $WORKER_STATUSES = {};
    
    my $worker_pid = $PROCESS_ID;
    my $array_index = 0;
    my $index_found = 0;

    tie( $WORKER_STATUSES, 'IPC::Shareable', { key => 'STATUSES' } );
    tie( $WORKER_FILTER_TABLES, 'IPC::Shareable', { key => 'WORKER_FILTER_TABLES' } );

    foreach my $filter_table( @$filter_tables )
    {
        # CRITICAL Section - check main filter_tables structure and find our index
        tied( $WORKER_FILTER_TABLES )->shlock( LOCK_SH );

        if( !defined( $WORKER_FILTER_TABLES->{$filter_table} ) || !defined( $WORKER_FILTER_TABLES->{$filter_table}->{pins} ) )
        {
            tied( $WORKER_FILTER_TABLES )->shunlock();
            _log( $LOG_LEVEL_FATAL, "Shared memory doesn't appear to be mapped" );
            exit( 1 );
        }

        unless( $index_found )
        {
            foreach my $item( @{$WORKER_FILTER_TABLES->{$filter_table}->{pids}} )
            {
                if( $item == $worker_pid )
                {
                    $index_found = 1;
                    last;
                }

                $array_index++ if( !$index_found );
            }
        }

        tied( $WORKER_FILTER_TABLES )->shunlock();
        # End critical section

        if( !tied( $WAL_DATA->{$filter_table}->{pin} ) )
        {
            my $PIN = [];
            until( tied( $PIN ) )
            {
                eval{ tie( $PIN, 'IPC::Shareable', { key => "P${worker_pid}${filter_table}" } ) };
                if( $OS_ERROR )
                {
                    sleep( 1 );
                }
            }
            $WAL_DATA->{$filter_table}->{pin} = $PIN;
        }
    }

    _log( $LOG_LEVEL_DEBUG, "Got worker index $array_index" );

    until( tied( $WORKER_STATUSES )->shlock( LOCK_SH | LOCK_NB ) )
    {
        _log( $LOG_LEVEL_DEBUG, "Worker $worker_pid waiting to enter running state" );
        sleep( 1 );
    }

    tied( $WORKER_STATUSES )->shlock( LOCK_EX );
    $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_RUNNING;
    tied( $WORKER_STATUSES )->shunlock();
    my $handle = DBI->connect(
        $CONNECTION_MAP->{connection_string},
        $CONNECTION_MAP->{user_name},
        undef
    );

    unless( $handle )
    {
        _log( $LOG_LEVEL_FATAL, "Worker failed to connect to DB" );
    }

    my $table_sth = try_query( $handle, $GET_CACHE_TABLE_DEFINITION, [ $pk_maintenance_object ] );

    unless( $table_sth )
    {
        _log( $LOG_LEVEL_FATAL, "Worker failed to retreive cache table definition" );
    }

    my $mo_row = $table_sth->fetchrow_hashref();
    my $cache_table_definition = $mo_row->{definition};
    my $cache_table_schema     = $mo_row->{namespace};
    my $cache_table_name       = $mo_row->{name};
    my $cache_table_driver     = $mo_row->{driver};
    $table_sth->finish();

    if( $cache_table_driver eq 'postgresql' )
    {
        check_ct_exists( $handle, $cache_table_schema, $cache_table_name, $cache_table_definition );
        my $relcache = get_relcache( $handle, $filter_tables );
        print Dumper( $relcache );
        my $parse_tree = find_table_aliases( $handle, $relcache, $cache_table_definition, $filter_tables );

        print Dumper( $parse_tree );
    }
    _log( $LOG_LEVEL_DEBUG, "Worker $worker_pid running" );

    # Main worker loop
    my $max_peeked_lsn = '';
    my $max_applied_lsn = '';

    while ( 1 )
    {
        my $changes = {};
        foreach my $filter_table( keys %$WAL_DATA )
        {
            my $pin_keyname = "P${worker_pid}${filter_table}";
            if( !tied( $WAL_DATA->{$filter_table}->{pin} ) )
            {
                unless( tie( $WAL_DATA->{$filter_table}->{pin}, 'IPC::Shareable', { key => $pin_keyname } ) )
                {
                    _log( $LOG_LEVEL_WARNING, "Failed to tie shared memory $pin_keyname" );
                }
            }

            tied( $WAL_DATA->{$filter_table}->{pin} )->shlock( LOCK_SH );
            my $change;
            if(
                    defined $WAL_DATA
                 && defined( $WAL_DATA->{$filter_table} )
                 && defined( $WAL_DATA->{$filter_table}->{pin} )
              )
            {
                while( scalar( @{$WAL_DATA->{$filter_table}->{pin}} ) > 0 )
                {
                    $change = pop( @{$WAL_DATA->{$filter_table}->{pin}} );
                    
                    if( $change )
                    {
                        push( @{$changes->{filter_table}}, $change );
                        if( lsn_cmp( $max_peeked_lsn, $change->{commit_lsn} ) < 0 )
                        {
                            $max_peeked_lsn = $change->{commit_lsn};
                        }
                    }

                }
            }

            print "Worker max peeked lsn: $max_peeked_lsn\n";
            tied( $WAL_DATA->{$filter_table}->{pin} )->shunlock();

            # now lets apply changes from the array after pop

        }
    
        sleep( 1 );
    }

    return;
}

## MAIN PROGRAM

# Parse and validate arguments
our( $opt_D, $opt_d, $opt_U, $opt_h, $opt_p );
my @original_argv = @ARGV;

usage( 'Invalid arguments' ) unless( getopts( 'd:U:h:p:D' ) );

my $dbname = $opt_d;
my $host   = $opt_h;
my $port   = $opt_p;
my $user   = $opt_U;
$DAEMONIZE = $opt_D;

$port = 5432 unless( defined( $port ) );
usage( 'Invalid port' ) if( defined( $port ) and ( $port !~ /^\d+$/ or $port < 1 or $port > 65535 ) );
usage( 'Invalid database name' ) unless( defined( $dbname ) && length( $dbname ) > 0 );
usage( 'Invalid username' ) unless( defined( $user ) && length( $user ) > 0 );
usage( 'Invalid host name' ) unless( defined( $host ) && length( $host ) > 0 );

my $conn_string = "dbi:Pg:dbname=${dbname};host=${host};port=${port}";
$CONNECTION_MAP->{connection_string} = $conn_string;
$CONNECTION_MAP->{user_name}         = $user;
$CONNECTION_MAP->{dbname}            = $dbname;

# Pre-flight checks
my $handle = DBI->connect( $conn_string, $user, undef );

croak( 'Could not connect to the database' ) unless( $handle );

unless( check_extension( $handle ) )
{
    croak( "$EXTENSION_NAME doesn't seem to be installed" );
}

if( !check_extension_running( $handle ) )
{
    croak( "There appears to be another instance of $EXTENSION_NAME running on this database" );
}

my $worker_data = get_worker_list( $handle );
$handle->disconnect();
undef( $handle );

## GLOBAL SHM VARIABLES
my $WORKER_FILTER_TABLES = {};
my $WORKER_STATUSES      = {};

tie( $WORKER_FILTER_TABLES, 'IPC::Shareable', { key => 'WORKER_FILTER_TABLES', create => 1, destroy => 1 } );
tie( $WORKER_STATUSES,      'IPC::Shareable', { key => 'STATUSES', create => 1, destroy => 1 } );

shm_cleanup() if( $CLEAN_UP );

# Wipe and start fresh if we crashed previously
$WORKER_STATUSES = {};
$WORKER_FILTER_TABLES = {};
# Time to fork workers
# Lock status struct to pause workers while we wait to start everything
tied( $WORKER_STATUSES )->shlock( LOCK_EX );

foreach my $worker_entry( @$worker_data )
{
    my $filter_tables         = $worker_entry->{filter_tables};
    my $wal_level             = $worker_entry->{wal_level};
    my $maintenance_channel   = $worker_entry->{maintenance_channel};
    my $pk_maintenance_object = $worker_entry->{maintenance_object};

    foreach my $filter_table( @$filter_tables )
    {
        if( !defined( $WORKER_FILTER_TABLES->{$filter_table} ) )
        {
            $WORKER_FILTER_TABLES->{$filter_table} = {
                pins     => [],
                pids     => [],
            };
        }
    }

    my $child_pid = fork();

    if( defined( $child_pid ) and $child_pid == 0 )
    {
        worker_entrypoint( $wal_level, $filter_tables, $maintenance_channel, $pk_maintenance_object );
        exit( 0 );
    }
    elsif( defined( $child_pid ) and $child_pid > 0 )
    {
        push( @$CHILDREN, $child_pid );

        foreach my $filter_table( @$filter_tables )
        {
            # Setup the pins (parsed XLOG queue) for each filter table
            # changes relevent to said changes will be pushed into this queue
            # by the parent and popped later by the workers
            my $PIN = [];
            tied( $WORKER_FILTER_TABLES )->shlock( LOCK_EX );
            push( @{$WORKER_FILTER_TABLES->{$filter_table}->{pids}}, $child_pid );
            tie( $PIN, 'IPC::Shareable', { key => "P${child_pid}${filter_table}", create => 1, destroy => 1 } );
            push( @{$WORKER_FILTER_TABLES->{$filter_table}->{pins}}, $PIN );
            tied( $WORKER_FILTER_TABLES )->shunlock();
            push( @$PINS, $PIN );
        }

        $WORKER_STATUSES->{$child_pid}->{status}   = $WORKER_STATUS_STARTUP;
        $WORKER_STATUSES->{$child_pid}->{last_lsn} = undef;
        _log( $LOG_LEVEL_DEBUG, "Parent created child $child_pid" );
    }
    else
    {
        _log( $LOG_LEVEL_FATAL, "Failed to fork worker process" );
    }
}

# We've started workers, lets start processing WAL
_log( $LOG_LEVEL_DEBUG, "All workers started" );
tied( $WORKER_STATUSES )->shunlock();
parent_loop( $WORKER_STATUSES, $PINS, $WORKER_FILTER_TABLES );
_log( $LOG_LEVEL_ERROR, "Parent exited main loop" );

foreach my $pin( @$PINS )
{
    next unless( defined( $pin ) );
    tied( $pin )->clean_up_all;
}

tied( $WORKER_FILTER_TABLES )->clean_up_all;
exit( 0 );
