#!/usr/bin/perl

use strict;
use warnings;
use utf8;

use DBI;
use Params::Validate qw( :all );
use Carp;
use Readonly;
use English qw( -no_match_vars );

use JSON::XS;
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

# DEV NOTES:
# - This can read queries but is relatively untested against all the possible variations and expressiveness of SQL
#   therefore, the simpler and less deeply nested a query can be, the better. There are safety checks to prevent bad
#   queries from executing
# - This requires, like matviews, that a unique expression exists on the table, though this can support multiple
# NOTE: IPC::Shareable keys seeem to be extremely short (4-8 chars) and may collide!

$OUTPUT_AUTOFLUSH = 1;

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
    my $filter_table_lsns = {};

    while( 1 )
    {
        tied( $WORKER_FILTER_TABLES )->shlock( LOCK_SH );
        foreach my $filter_table( keys %$WORKER_FILTER_TABLES )
        {
            print "PArent checking replication slot for table $filter_table\n";
            $last_peeked_lsn = $filter_table_lsns->{$filter_table};
            my $data = replication_peek( $handle, $filter_table, \$last_peeked_lsn );
            $filter_table_lsns->{$filter_table} = $last_peeked_lsn;
            next unless( $data );
            print Dumper( $data );
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
        # Get worker applied LSNs and ack up to the smallest LSN

        my $worker_lsns = {};
        tied( $WORKER_STATUSES )->shlock( LOCK_SH );
        foreach my $pid( keys %$WORKER_STATUSES )
        {
            $worker_lsns->{$pid} = $WORKER_STATUSES->{$pid}->{last_lsn};
        }
        tied( $WORKER_STATUSES )->shunlock();

        foreach my $pid( keys %$worker_lsns )
        {
            my $last_lsn = $worker_lsns->{$pid};

            unless( $last_lsn )
            {
                # Note - we WILL NOT ack any LSNs iff a worker hasn't completed anything here
                undef( $last_lsn_applied );
                last;
            }

            if( !defined( $last_lsn_applied ) )
            {
                $last_lsn_applied = $last_lsn;
            }
            else
            {
                if( lsn_cmp( $last_lsn_applied, $last_lsn ) < 0 )
                {
                    $last_lsn_applied = $last_lsn;
                }
            }
        }

        if( defined( $last_lsn_applied ) )
        {
            if( &replication_seek( $handle, $last_lsn_applied ) )
            {
                print "Parent seeked changes to $last_lsn_applied\n";
            }
        }

        undef( $last_lsn_applied );
        sleep( 5 );

    }

    return;
}

# This sub consolidates caching function to a CACHE_HASH output which is
# further accessed by subroutines in the main worker loop
sub worker_cache_refresh($$$$)
{
    my(
        $handle,
        $pk_maintenance_object,
        $filter_tables,
        $cache_hash
    ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => ARRAYREF },
        { type => HASHREF | UNDEF },
    );

    unless( &get_ct_definition( $handle, $pk_maintenance_object, $cache_hash ) )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to get cache table definition" );
    }

    $cache_hash->{relcache}      = &get_relcache( $handle );
    $cache_hash->{table_mapping} = {};
    $cache_hash->{parse_tree}    = &find_table_aliases(
        $handle,
        $cache_hash->{relcache},
        $cache_hash->{definition},
        $filter_tables,
        $cache_hash->{table_mapping}
    );

    # Columns in order of appearance on table
    $cache_hash->{cache_table_columns} = &get_cache_table_columns(
        $handle,
        $cache_hash->{schema},
        $cache_hash->{name}
    );
    # Array of uniques (index/constraints) containing array of constrained cols
    $cache_hash->{cache_table_uniques} = &get_cache_table_unique(
        $handle,
        $cache_hash->{schema},
        $cache_hash->{name}
    );

    # Last pre-flight check - validate TABLE_MAPPING against filter tables
    foreach my $schema( keys %{$cache_hash->{table_mapping}->{RELS}} )
    {
        foreach my $table( keys %{$cache_hash->{table_mapping}->{RELS}->{$schema}} )
        {
            my $relname = "${schema}.${table}";

            unless( grep( /^$relname$/, @$filter_tables ) )
            {
                # This is mainly for debugging, but we could add this relation to the filter tables array
                # rather than complaining
                _log(
                    $LOG_LEVEL_ERROR,
                    "Relation $relname is not present in filter tables provided by parent! Updates may be missed."
                );
                return;
            }
        }
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

    my $CACHE_HASH = {};
    my $WORKER_FILTER_TABLES = {};
    my $WAL_DATA;
    my $WORKER_STATUSES = {};

    my $worker_pid  = $PROCESS_ID;
    my $array_index = 0;
    my $index_found = 0;

    tie( $WORKER_STATUSES, 'IPC::Shareable', { key => 'STATUSES' } );
    tie( $WORKER_FILTER_TABLES, 'IPC::Shareable', { key => 'WORKER_FILTER_TABLES' } );

    foreach my $filter_table( @$filter_tables )
    {
        # CRITICAL Section - check main filter_tables structure and find our index
        tied( $WORKER_FILTER_TABLES )->shlock( LOCK_SH );

        if(
               !defined( $WORKER_FILTER_TABLES->{$filter_table} )
            || !defined( $WORKER_FILTER_TABLES->{$filter_table}->{pins} )
          )
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
                sleep( 1 ) if( $OS_ERROR );
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
    sleep( 1 );
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

    unless( &get_ct_definition( $handle, $pk_maintenance_object, $CACHE_HASH ) )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to look up CT '$pk_maintenance_object' definition" );
    }

    # Table mapping and parse tree are (relatively) static and only change if our query changes underneath us
    # TODO: Add detection and correction for the above
    _log( $LOG_LEVEL_DEBUG, "Worker $worker_pid running" );

    if( $CACHE_HASH->{driver} eq 'postgresql' )
    {
        &check_ct_exists(
            $handle,
            $CACHE_HASH->{schema},
            $CACHE_HASH->{name},
            $CACHE_HASH->{definition}
        );

        # Main worker loop
        &worker_cache_refresh( $handle, $pk_maintenance_object, $filter_tables, $CACHE_HASH );

        while( 1 )
        {
            # check to see if definition has changed
            my $max_peeked_lsn;
            my $max_applied_lsn;
            my $test_hash = &get_ct_digest( $handle, $pk_maintenance_object );
            if( !defined $test_hash )
            {
                _log( $LOG_LEVEL_FATAL, "Failed to check maintenance object for definition change (SHA256)" );
            }

            if( $test_hash ne $CACHE_HASH->{digest} )
            {
                _log( $LOG_LEVEL_INFO, "Cache table definition has changed, replacing the cache table" );
                &worker_cache_refresh( $handle, $pk_maintenance_object, $filter_tables, $CACHE_HASH );
                &replace_cache_table( $handle, $pk_maintenance_object );
            }

            # Process changes
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
                        defined( $WAL_DATA )
                     && defined( $WAL_DATA->{$filter_table} )
                     && defined( $WAL_DATA->{$filter_table}->{pin} )
                  )
                {
                    while( scalar( @{$WAL_DATA->{$filter_table}->{pin}} ) > 0 )
                    {
                        $change = pop( @{$WAL_DATA->{$filter_table}->{pin}} );
                        print "Worker got change: \n";
                        print Dumper( $change );
                        if( $change )
                        {
                            my $schema = $change->{data}->{schema_name};
                            my $table  = $change->{data}->{table_name};

                            foreach my $key( keys %{$change->{data}->{key}} )
                            {
                                my $val = $change->{data}->{key}->{$key};

                                if( !defined( $changes->{$schema}->{$table}->{$key} ) )
                                {
                                    $changes->{$schema}->{$table}->{$key} = [ $val ];
                                }
                                else
                                {
                                    push( @{$changes->{$schema}->{$table}->{$key}}, $val );
                                }
                            }

                            if( !defined( $max_peeked_lsn ) || lsn_cmp( $max_peeked_lsn, $change->{commit_lsn} ) < 0 )
                            {
                                $max_peeked_lsn = $change->{commit_lsn};
                            }
                        }

                    }
                }

                tied( $WAL_DATA->{$filter_table}->{pin} )->shunlock();
                # Digest changes for this filter table
            }

            if( scalar( keys %$changes ) > 0 )
            {
                _log( $LOG_LEVEL_DEBUG, "Applying changes" );
                # now lets apply changes from the array after pop
                my $query = &apply_filters(
                    $handle,
                    $CACHE_HASH->{parse_tree},
                    $CACHE_HASH->{table_mapping},
                    $CACHE_HASH->{definition},
                    $changes
                );

                if( !&test_query( $handle, $query ) )
                {
                    _log(
                        $LOG_LEVEL_ERROR,
                        "Failed to apply filters to query for cache table '$CACHE_HASH->{name}'"
                    );
                    next;
                }

                # At this point we're ready to execute the table into a temp table
                my $temp_table = &generate_temp_table( $handle, $query );

                if( !defined( $temp_table ) )
                {
                    _log(
                        $LOG_LEVEL_ERROR,
                        "Failed to generate temp table for updating cache table '$CACHE_HASH->{name}'"
                    );
                    next;
                }

                my $delete_result = generate_delete_statement(
                    $handle,
                    $CACHE_HASH->{definition},
                    $CACHE_HASH->{schema},
                    $CACHE_HASH->{name},
                    $CACHE_HASH->{cache_table_columns},
                    $CACHE_HASH->{cache_table_uniques}
                );

                unless( $delete_result )
                {
                    _log(
                        $LOG_LEVEL_ERROR,
                        "Deleting entries from $CACHE_HASH->{schema}.$CACHE_HASH->{name} failed"
                    );
                    next;
                }

                my $update_result = generate_update_statement(
                    $handle,
                    $temp_table,
                    $CACHE_HASH->{schema},
                    $CACHE_HASH->{name},
                    $CACHE_HASH->{cache_table_columns},
                    $CACHE_HASH->{cache_table_uniques}
                );

                unless( $update_result )
                {
                    _log(
                        $LOG_LEVEL_ERROR,
                        "Updating entries in $CACHE_HASH->{schema}.$CACHE_HASH->{name} failed"
                    );
                    next;
                }

                my $insert_result = generate_insert_statement(
                    $handle,
                    $temp_table,
                    $CACHE_HASH->{schema},
                    $CACHE_HASH->{name},
                    $CACHE_HASH->{cache_table_columns},
                    $CACHE_HASH->{cache_table_uniques}
                );

                unless( $insert_result )
                {
                    _log(
                        $LOG_LEVEL_ERROR,
                        "Inserting entries into $CACHE_HASH->{schema}.$CACHE_HASH->{name} failed"
                    );
                    next;
                }

                # If we make it here we can signal that we've applied up to $max_peeked_lsn changes
                # Check here to see if the table definition has changed
                &drop_temp_table( $handle, $temp_table );

                print "Applied $max_peeked_lsn\n";
                $max_applied_lsn = $max_peeked_lsn;
                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                $WORKER_STATUSES->{$worker_pid}->{last_lsn} = $max_applied_lsn;
                tied( $WORKER_STATUSES )->shunlock();
            }

            sleep( 1 );
        } # postgres driver main loop
    }
    else
    {
        _log(
            $LOG_LEVEL_FATAL,
            "Worker cannot proceed. Driver $CACHE_HASH->{driver} not implemented\n"
        );
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
usage( 'Invalid port'          ) if( defined( $port ) and ( $port !~ /^\d+$/ or $port < 1 or $port > 65535 ) );
usage( 'Invalid database name' ) unless( defined( $dbname ) && length( $dbname ) > 0 );
usage( 'Invalid username'      ) unless( defined( $user ) && length( $user ) > 0 );
usage( 'Invalid host name'     ) unless( defined( $host ) && length( $host ) > 0 );

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
        &worker_entrypoint(
            $wal_level,
            $filter_tables,
            $maintenance_channel,
            $pk_maintenance_object
        );
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
