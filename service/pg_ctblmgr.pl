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
use Data::Dumper;

use FindBin;
use lib "$FindBin::Bin/lib";

use Util;
use DB;
use QueryParser;

# NOTE: IPC::Shareable keys seeem to be extremely short (4-8 chars) and may collide!
$OUTPUT_AUTOFLUSH = 1;

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

## GLOBAL VARIABLES
$PARENT_PID  = $PROCESS_ID;
$SLOT_NAME   = '__pg_ctblmgr';
$LOG_FILE    = '';
$LOG_FH      = undef;
$DAEMONIZE   = 0;
my $CHILDREN = [];
my $PINS     = [];

sub _terminate()
{
    if( $PROCESS_ID == $PARENT_PID )
    {
        #this is crucial to prevent running out of shm after crashes / terminations
        &shm_cleanup();
    }
    exit( 0 );
}

$SIG{INT} = \&_terminate;

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

    # Table mapping and parse tree are (relatively) static and only change if our query changes underneath us
    # TODO: Add detection and correction for the above
    if( $cache_table_driver eq 'postgresql' )
    {
        check_ct_exists( $handle, $cache_table_schema, $cache_table_name, $cache_table_definition );
        my $relcache = get_relcache( $handle );
        my $TABLE_MAPPING = {};
        my $PARSE_TREE = find_table_aliases( $handle, $relcache, $cache_table_definition, $filter_tables, $TABLE_MAPPING );
    }
    else
    {
        _log( $LOG_LEVEL_FATAL, "Worker cannot proceed. Driver $cache_table_driver not implemented\n" );
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
