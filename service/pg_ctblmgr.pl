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
use Carp;

use FindBin;
use lib "$FindBin::Bin/lib";

use Util;
use DB;
use QueryParser;

# DEV NOTES:
# - This can read queries but is relatively untested against all the possible
#   variations and expressiveness of SQL. Therefore, the simpler and less
#   deeply nested a query can be, the better. There are safety checks to
#   prevent bad queries from executing.
# - This requires, like matviews, that a unique expression exists on the table,
#   though this can support multiple unique indicies.
# NOTE: IPC::Shareable keys seeem to be extremely short (4-8 chars) and may collide!

Readonly my $SLEEP_TIMER => 1; # seconds for main loop

our $OUTPUT_AUTOFLUSH = 1;
our $|=1;

## GLOBAL VARIABLES
$PARENT_PID  = $PROCESS_ID;
$SLOT_NAME   = '__pg_ctblmgr';
$LOG_FILE    = '';
$LOG_FH      = undef;
$DAEMONIZE   = 0;

sub _terminate_sigint()
{
    # Wrapper to mask errors
    print "Caught sigint $PROCESS_ID\n";
    _terminate();
}

sub _terminate(;$$$)
{
    my( $package, $file, $line ) = validate_pos(
        @_,
        { type => SCALAR | UNDEF, optional => 1 },
        { type => SCALAR | UNDEF, optional => 1 },
        { type => SCALAR | UNDEF, optional => 1 },
    );

    if( $PROCESS_ID == $PARENT_PID )
    {
        #this is crucial to prevent running out of shm after crashes / terminations
        &shm_cleanup();
    }

    if( @_ )
    {
        CORE::die( @_ );
    }

    exit( 0 );
}

$SIG{INT} = \&_terminate_sigint;
$SIG{__DIE__} = \&_terminate;

sub shm_cleanup()
{
    # Note: This cleans up shared memory and semaphore arrays. These
    # will not be automatically be cleaned up by the kernel. This can be
    # done manually with ipcs / ipcrm
    return unless( $PROCESS_ID != $PARENT_PID );
    my $WORKER_FILTER_TABLES;
    my $WORKER_STATUSES;

    tie(
        $WORKER_FILTER_TABLES,
        'IPC::Shareable',
        { key => 'WORKER_FILTER_TABLES' }
    );
    tie(
        $WORKER_STATUSES,
        'IPC::Shareable',
        { key => 'STATUSES' }
    );

    tied( $WORKER_FILTER_TABLES )->clean_up_all();
    tied( $WORKER_STATUSES )->clean_up_all();

    my $sigwarn = $SIG{__WARN__};
    local $SIG{__WARN__} = sub {};

    $SIG{__WARN__} = $sigwarn;
    return;
}

sub get_distinct_filter_tables()
{
    my $WORKER_FILTER_TABLES;
    tie( $WORKER_FILTER_TABLES, 'IPC::Shareable', { key => 'WORKER_FILTER_TABLES' } );

    my $DISTINCT_FILTER_TABLES = [];
    tied( $WORKER_FILTER_TABLES )->shlock( LOCK_EX );
    foreach my $pid( keys %$WORKER_FILTER_TABLES )
    {
        foreach my $filter_table( keys %{$WORKER_FILTER_TABLES->{$pid}} )
        {
            unless( grep /^$filter_table$/, @$DISTINCT_FILTER_TABLES )
            {
                push( @$DISTINCT_FILTER_TABLES, $filter_table );
            }
        }
    }
    tied( $WORKER_FILTER_TABLES )->shunlock();

    return $DISTINCT_FILTER_TABLES;
}

sub populate_worker_data($$)
{
    my( $handle, $WORKER_DATA ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => HASHREF | UNDEF },
    );

    my $worker_data = get_worker_list( $handle ); 
    
    if( $worker_data )
    {
        foreach my $worker_entry( @$worker_data )
        {
            my $pk_maintenance_object = $worker_entry->{maintenance_object};
            my $filter_tables         = $worker_entry->{filter_tables};
            my $ct_hash               = &get_ct_digest( $handle, $pk_maintenance_object ); 

            unless( $ct_hash )
            {
                _log( $LOG_LEVEL_ERROR, "Failed to get digest for cache table $pk_maintenance_object" );
                next;
            }

            $WORKER_DATA->{$pk_maintenance_object} = $ct_hash;
        }
    }
    else
    {
        _log( $LOG_LEVEL_ERROR, 'Failed to get updated worker list' );
    }

    return $WORKER_DATA;
}

sub check_for_new_cache_tables($$$)
{
    my( $handle, $current_workers, $new_workers ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => HASHREF },
        { type => HASHREF },
    );

    my $diff = {
        new    => {},
        change => {},
        old    => {},
    };

    foreach my $pk_maintenance_object( keys %$new_workers )
    {
        if( defined( $current_workers->{$pk_maintenance_object} ) )
        {
            next if( $current_workers->{$pk_maintenance_object} eq $new_workers->{$pk_maintenance_object} );
            #indicate a change to a CT
            $diff->{change}->{$pk_maintenance_object} = $new_workers->{$pk_maintenance_object};
            $current_workers->{$pk_maintenance_object} = $new_workers->{$pk_maintenance_object};
        }
        else
        {
            #indicate a new CT has been added
            $diff->{new}->{$pk_maintenance_object} = $new_workers->{$pk_maintenance_object};
            $current_workers->{$pk_maintenance_object} = $new_workers->{$pk_maintenance_object};
        }
    }

    foreach my $pk_maintenance_object( keys %$current_workers )
    {
        next if( defined( $new_workers->{$pk_maintenance_object} ) );
        #indicate a removed CT
        $diff->{old}->{$pk_maintenance_object} = $current_workers->{$pk_maintenance_object};
    }
   
    foreach my $pk_maintenance_object( keys %{$diff->{old}} )
    {
        delete( $current_workers->{$pk_maintenance_object} );    
    }

    return $diff;
}

sub parent_loop($$$)
{
    my( $WORKER_STATUSES, $WORKER_FILTER_TABLES, $worker_mapping ) = validate_pos(
        @_,
        { type => HASHREF }, # shm status hash
        { type => HASHREF }, # shm WAL hash
        { type => HASHREF }, # local mapping of pk_maint_obj -> pid
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
        _log(
            $LOG_LEVEL_FATAL,
            'Failed to obtain lock on database - another '
          . "$EXTENSION_NAME instance seems to be running"
         );
    }

    my $last_lsn_applied;
    my $last_peeked_lsn;
    my $max_peeked_lsn;
    my $max_idle_lsn;

    my $last_last_lsn_applied;
    my $last_max_idle_lsn;
    my $filter_table_lsns = {};

    # There are two interlocks here:
    #   - We only ack the 'least' LSN applied by all workers
    #   - We need to continuously ack LSNs that don't apply to any workers
    #       - Handles the case where the primary is busy but there is no
    #         activity on the base tables which 'drive' our cache tables.

    my $DISTINCT_FILTER_TABLES = get_distinct_filter_tables();
    my $all_filter_tables = join( ',', @$DISTINCT_FILTER_TABLES );
    print "Filtering for all:\n'$all_filter_tables'\n";
    my $WORKER_DATA = {};
    $WORKER_DATA = populate_worker_data( $handle, $WORKER_DATA );
    my $wal_level = 'M';
    while( 1 )
    {
        ## CACHE TABLE MANAGEMENT
        my $tmp_worker_data = {};
        $tmp_worker_data = populate_worker_data( $handle, $tmp_worker_data );
        my $diff = check_for_new_cache_tables( $handle, $WORKER_DATA, $tmp_worker_data );
        if(
               scalar( keys %{$diff->{new}}    ) > 0
            || scalar( keys %{$diff->{change}} ) > 0
            || scalar( keys %{$diff->{old}}    ) > 0
          )
        {
            # Cache table changes detected
            _log( $LOG_LEVEL_INFO, 'Detected changes to cache table definitions' );
            # Remove old children
            foreach my $pk_maintenance_object( keys %{$diff->{old}} )
            {
                my $target_pid = $worker_mapping->{$pk_maintenance_object};
                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                _log( $LOG_LEVEL_DEBUG, "Parent terminating child $target_pid" );
                $WORKER_STATUSES->{$target_pid}->{shutdown} = 1;
                tied( $WORKER_STATUSES )->shunlock();
                # Unlock, wait for child to exit
                my $kid;
                
                do
                {
                    sleep( 1 );
                    _log( $LOG_LEVEL_DEBUG, "Waiting on child $target_pid to exit..." );
                    $kid = waitpid( $target_pid, WNOHANG );
                } while( $kid > 0 );
               
                waitpid( $target_pid, 0 );  # reap child
                _log( $LOG_LEVEL_DEBUG, "Child $target_pid exited!" ); 
                delete( $worker_mapping->{$pk_maintenance_object} );
                tied( $WORKER_FILTER_TABLES )->shlock( LOCK_EX );
                delete( $WORKER_FILTER_TABLES->{$target_pid} );
                tied( $WORKER_FILTER_TABLES )->shunlock();
            }

            # Add new children
            foreach my $pk_maintenance_object( keys %{$diff->{new}} )
            {
                print( "Adding new worker for pk $pk_maintenance_object\n" );
                # XXX new worker code - NEED TO ADD FT changes to WFT
                my $worker_data = get_worker_list( $handle, $pk_maintenance_object );
                unless( $worker_data )
                {
                    _log( $LOG_LEVEL_ERROR, 'Need to spin up new child but could not locate maintenance object' );
                    next;
                }

                $worker_data            = $worker_data->[0];
                my $wal_level           = $worker_data->{wal_level};
                my $filter_tables       = $worker_data->{filter_tables};
                my $maintenance_channel = $worker_data->{maintenance_channel};
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
                    $worker_mapping->{$pk_maintenance_object} = $child_pid;
                    tied( $WORKER_FILTER_TABLES )->shlock( LOCK_EX );

                    foreach my $filter_table( @$filter_tables )
                    {
                        if( !defined( $WORKER_FILTER_TABLES->{$child_pid}->{$filter_table} ) )
                        {
                            $WORKER_FILTER_TABLES->{$child_pid}->{$filter_table} = [];
                        }
                        # Setup the pins (parsed XLOG queue) for each filter table
                        # changes relevent to said changes will be pushed into this queue
                        # by the parent and popped later by the workers
                    }
                    tied( $WORKER_FILTER_TABLES )->shunlock();

                    $WORKER_STATUSES->{$child_pid}->{status}   = $WORKER_STATUS_STARTUP;
                    $WORKER_STATUSES->{$child_pid}->{shutdown} = 0;
                    $WORKER_STATUSES->{$child_pid}->{last_lsn} = undef;
                    _log( $LOG_LEVEL_DEBUG, "Parent created child $child_pid" );
                }
                else
                {
                    _log( $LOG_LEVEL_FATAL, "Failed to fork worker process" );
                }
            }
        }
        ## CHANGE MANAGEMENT
        my $num_in_flight_changes = 0; # number of changes we're queueing
        my $num_outstanding_changes = 0; # number of changes we've queued previously
        #print "Peeking uneeded changes\n";
        #&replication_slot_peek_unneeded_changes( $handle, \$max_idle_lsn, $all_filter_tables );
        #print "Uneeded changes peeked\n";
        my $WT_LOCKED = 0;

        ### LSN / Change Management
        # Here we peek changes (get them but do not change the slot's LSN). These changes are then passed to child processes and,
        # after the relevent change is acknowledged, we 'seek' these changes, in that we acknowledge them with respect to
        # the replication slot.
        print "Peeking from '$last_peeked_lsn'\n";
        my $data = &replication_peek(
            $handle,
            $all_filter_tables,
            $wal_level,
            \$last_peeked_lsn
        );
        print "Peeded to '$last_peeked_lsn'\n";

        if( $data )
        {
            # iterate over each change in outer loop - one change may go to one or more workers
            foreach my $change( @$data )
            {
                $num_in_flight_changes++;
                print Dumper( $change );
                if( !$WT_LOCKED )
                {
                    print "Attempting to lock FT\n";
                    tied( $WORKER_FILTER_TABLES )->shlock( LOCK_EX );
                    $WT_LOCKED = 1;
                }

                foreach my $pid( keys %{$WORKER_FILTER_TABLES} )
                {
                    my $filter_table = $change->{data}->{schema_name} . '.' . $change->{data}->{table_name};

                    if(
                            defined( $WORKER_FILTER_TABLES->{$pid}->{$filter_table} )
                         && ref( $WORKER_FILTER_TABLES->{$pid}->{$filter_table} ) eq 'ARRAY'
                      )
                    {

                        push( @{$WORKER_FILTER_TABLES->{$pid}->{$filter_table}}, $change );
                        my $change_lsn = $change->{begin_lsn};
                        print "Parsed out change lsn '$change_lsn'\n";
                        if(
                               !defined( $filter_table_lsns->{$filter_table} )
                            || lsn_cmp( $filter_table_lsns->{$filter_table}, $change_lsn ) < 0
                          )
                        {
                            $filter_table_lsns->{$filter_table} = $change_lsn;
                        }
                    }
                }
            }

            # These are changes that are still considered in-flight
            foreach my $filter_table( @$DISTINCT_FILTER_TABLES )
            {
                my $lsn = $filter_table_lsns->{$filter_table};

                next unless( defined( $lsn ) );

                if(
                      !defined( $max_idle_lsn )
                   || lsn_cmp( $lsn, $max_idle_lsn ) < 0
                  )
                {
                    $max_idle_lsn = $lsn;
                }
            }
        }
        
        print "last_peeked_lsn: '$last_peeked_lsn', max_idle_lsn: '$max_idle_lsn'\n";
        ## XXX
#        foreach my $filter_table( @$DISTINCT_FILTER_TABLES )
#        {
#            $last_peeked_lsn = $filter_table_lsns->{$filter_table};
#            my $data = &replication_peek(
#                $handle,
#                $filter_table,
#                $wal_level,
#                \$last_peeked_lsn
#            );
#            $filter_table_lsns->{$filter_table} = $last_peeked_lsn;
#            next unless( $data );
#
#            if( !$WT_LOCKED )
#            {
#                print "Attempting to lock FT\n";
#                tied( $WORKER_FILTER_TABLES )->shlock( LOCK_EX );
#                $WT_LOCKED = 1;
#            }

#            foreach my $pid( keys %{$WORKER_FILTER_TABLES} )
#            {
#                if(
#                        defined( $WORKER_FILTER_TABLES->{$pid}->{$filter_table} )
#                     && ref( $WORKER_FILTER_TABLES->{$pid}->{$filter_table} ) eq 'ARRAY'
#                  )
#                {
#                    foreach my $change( @$data )
#                    {
#                        push( @{$WORKER_FILTER_TABLES->{$pid}->{$filter_table}}, $change );
#                    }
#                }
#            }
#        }

        # Idle WT check
        if( !$WT_LOCKED )
        {
            tied( $WORKER_FILTER_TABLES )->shlock( LOCK_SH );
            $WT_LOCKED = 1;
        }

        foreach my $pid( keys %$WORKER_FILTER_TABLES )
        {
            foreach my $filter_table( keys %{$WORKER_FILTER_TABLES->{$pid}} )
            {
                if( 
                       defined( $WORKER_FILTER_TABLES->{$pid}->{$filter_table} )
                    && ref( $WORKER_FILTER_TABLES->{$pid}->{$filter_table} ) eq 'ARRAY'
                  )
                {
                    $num_outstanding_changes += scalar( @{$WORKER_FILTER_TABLES->{$pid}->{$filter_table}} );
                }
            }
        }

		if( $WT_LOCKED )
		{
            tied( $WORKER_FILTER_TABLES )->shunlock();
            $WT_LOCKED = 0;
        }
        # Get worker applied LSNs and ack up to the smallest LSN

        my $worker_lsns = {};
        tied( $WORKER_STATUSES )->shlock( LOCK_SH | LOCK_NB );
        foreach my $pid( keys( %$WORKER_STATUSES ) )
        {
            if(
                   defined( $WORKER_STATUSES->{$pid} )
                && defined( $WORKER_STATUSES->{$pid}->{last_lsn} )
              )
            {
                $worker_lsns->{$pid} = $WORKER_STATUSES->{$pid}->{last_lsn};
            }
        }
        tied( $WORKER_STATUSES )->shunlock();

        print "Current LSN stats: outstanding: $num_outstanding_changes, in_flight: $num_in_flight_changes\n";
        print "Worker LSNs:\n";
        foreach my $pid( keys %$worker_lsns )
        {
            my $last_lsn = $worker_lsns->{$pid};
            print "$pid  -  '$last_lsn'\n";
            unless( $last_lsn )
            {
                # Note - we WILL NOT ack any LSNs iff a worker hasn't
                # completed anything here
                undef( $last_lsn_applied );
            }

            if( !defined( $last_lsn_applied ) && defined( $last_lsn ) )
            {
                $last_lsn_applied = $last_lsn;
            }
            elsif( defined( $last_lsn_applied ) && defined( $last_lsn ) )
            {
                if( &lsn_cmp( $last_lsn_applied, $last_lsn ) < 0 )
                {
                    $last_lsn_applied = $last_lsn;
                }
            }
        }

        if(
                defined( $last_lsn_applied )
             && (
                  (
                      defined( $last_last_lsn_applied )
                   && &lsn_cmp( $last_lsn_applied, $last_last_lsn_applied ) > 0
                  )
               || ( !defined( $last_last_lsn_applied ) )
                )
          )
        {
            print "Seeking changes to '$last_lsn_applied'\n";
            if( &replication_seek( $handle, $last_lsn_applied, $all_filter_tables ) )
            {
                $last_last_lsn_applied = $last_lsn_applied;
            }
        }

        if( $num_in_flight_changes == 0 && $num_outstanding_changes == 0 )
        {
            if( !defined( $max_idle_lsn ) )
            {
                $max_idle_lsn = $last_peeked_lsn;
            }
        }

        if(
              (
                   $num_in_flight_changes == 0
                && defined( $max_idle_lsn )
                && defined( $last_max_idle_lsn )
                && &lsn_cmp( $last_max_idle_lsn, $max_idle_lsn ) < 0
              )
           || (
                  $num_in_flight_changes == 0
               && $num_outstanding_changes == 0
               && defined( $max_idle_lsn )
               && !defined( $last_lsn_applied )
              )
          )
        {
            _log( $LOG_LEVEL_DEBUG, "Seeking changes to $max_idle_lsn" );
            &replication_seek( $handle, $max_idle_lsn, $all_filter_tables );
            $last_lsn_applied = $max_idle_lsn;
        }

        $last_max_idle_lsn = $max_idle_lsn;
        sleep( $SLEEP_TIMER );
        
        ## WORKER HEALTH CHECKS
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
    $cache_hash->{parse_tree} = &find_table_aliases(
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

    if(
          !defined( $cache_hash->{cache_table_uniques} )
       || ref( $cache_hash->{cache_table_uniques} ) ne 'ARRAY'
       || scalar( @{$cache_hash->{cache_table_uniques}} ) == 0
      )
    {
        print "Generating unique index\n";
        unless( create_cache_table_unique( $handle, $cache_hash ) )
        {
            _log(
                $LOG_LEVEL_ERROR,
                "Failed to generate cache table unique index"
            );
        }
    }

    # Last pre-flight check - validate TABLE_MAPPING against filter tables
    foreach my $schema( keys %{$cache_hash->{table_mapping}->{RELS}} )
    {
        foreach my $table( keys %{$cache_hash->{table_mapping}->{RELS}->{$schema}} )
        {
            my $relname = "${schema}.${table}";

            unless( grep( /^$relname$/, @$filter_tables ) )
            {
                # This is mainly for debugging, but we could add this relation
                # to the filter tables array rather than complaining
                _log(
                    $LOG_LEVEL_ERROR,
                    "Relation $relname is not present in filter tables "
                  . 'provided by parent! Updates may be missed.'
                );
                return;
            }
        }
    }

    return;
}

sub worker_entrypoint($$$$)
{
    my(
        $wal_level,
        $filter_tables,
        $maintenance_channel,
        $pk_maintenance_object
      ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => ARRAYREF },
        { type => SCALAR },
        { type => SCALAR },
    );

    my $CACHE_HASH           = {};
    my $WORKER_FILTER_TABLES = {};
    my $WAL_DATA;
    my $WORKER_STATUSES      = {};

    my $worker_pid  = $PROCESS_ID;

    tie(
        $WORKER_STATUSES,
        'IPC::Shareable',
        { key => 'STATUSES' }
    );
    tie(
        $WORKER_FILTER_TABLES,
        'IPC::Shareable',
        { key => 'WORKER_FILTER_TABLES' }
    );

    my $count = 0;
    my $lim = 15;
    until( tied( $WORKER_STATUSES )->shlock( LOCK_SH | LOCK_NB ) )
    {
        if( $count > $lim )
        {
            _log( $LOG_LEVEL_FATAL, "Parent took > $lim seconds to start!" );
        }
        _log(
            $LOG_LEVEL_DEBUG,
            "Worker $worker_pid waiting to enter running state"
        );
        sleep( 1 );
        $count++;
    }

    tied( $WORKER_STATUSES )->shunlock();

    tied( $WORKER_STATUSES )->shlock( LOCK_EX );
    $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_RUNNING;
    tied( $WORKER_STATUSES )->shunlock();

    my $handle = DBI->connect(
        $CONNECTION_MAP->{connection_string},
        $CONNECTION_MAP->{user_name},
        undef
    );

    _log( $LOG_LEVEL_FATAL, 'Worker failed to connect to DB' ) unless( $handle );

    unless( &get_ct_definition( $handle, $pk_maintenance_object, $CACHE_HASH ) )
    {
        _log(
            $LOG_LEVEL_FATAL,
            "Failed to look up CT '$pk_maintenance_object' definition"
        );
    }

    # Table mapping and parse tree are (relatively) static and only change if
    # our query changes underneath us
    # TODO: Add detection and correction for the above
    _log( $LOG_LEVEL_DEBUG, "Worker $worker_pid running" );

    if( $CACHE_HASH->{driver} eq 'postgresql' )
    {
        &check_ct_exists(
            $handle,
            $CACHE_HASH
        );

        # Main worker loop
        &worker_cache_refresh(
            $handle,
            $pk_maintenance_object,
            $filter_tables,
            $CACHE_HASH
        );

        while( 1 )
        {
            # Check for commanded exit
            my $exit = 0;
            tied( $WORKER_STATUSES )->shlock( LOCK_SH );
            if( defined( $WORKER_STATUSES ) && defined( $WORKER_STATUSES->{$worker_pid} ) )
            {
                $exit = $WORKER_STATUSES->{$worker_pid}->{shutdown} if( defined( $WORKER_STATUSES->{$worker_pid}->{shutdown} ) );
            }
            tied( $WORKER_STATUSES )->shunlock();

            if( defined $exit && $exit == 1 )
            {
                _log( $LOG_LEVEL_INFO, "PID $worker_pid commanded to shutdown" );
                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_EXITED;
                tied( $WORKER_STATUSES )->shunlock();

                my $dct = try_query( $handle, "DROP TABLE $CACHE_HASH->{schema}.$CACHE_HASH->{name}" );
                unless( $dct )
                {
                    _log( $LOG_LEVEL_ERROR, "Failed to drop cache table $CACHE_HASH->{schema}.$CACHE_HASH->{name}" );
                }
                exit( 0 );
            }

            # check to see if definition has changed
            my $max_peeked_lsn;
            my $max_applied_lsn;
            my $test_hash = &get_ct_digest( $handle, $pk_maintenance_object );

            if( !defined $test_hash )
            {
                _log(
                    $LOG_LEVEL_ERROR,
                    'Failed to check maintenance object '
                  . 'for definition change (SHA256)'
                );
            }
            else
            {
                if( $test_hash ne $CACHE_HASH->{digest} )
                {
                    _log(
                        $LOG_LEVEL_INFO,
                        'Cache table definition has changed, replacing the '
                      . 'cache table'
                    );

                    &worker_cache_refresh(
                        $handle,
                        $pk_maintenance_object,
                        $filter_tables,
                        $CACHE_HASH
                    );
                    &replace_cache_table( $handle, $pk_maintenance_object );
                }
            }

            # Process changes
            my $changes = {};
            my $WAL_DATA = {};

            # Quickly dequeue items to hold ex lock for minimum time
            tied( $WORKER_FILTER_TABLES )->shlock( LOCK_EX );
            if( !defined $WORKER_FILTER_TABLES || ref( $WORKER_FILTER_TABLES ) ne 'HASH' )
            {
                _log( $LOG_LEVEL_ERROR, 'WFT is not defined or not a hash!' );
                tied( $WORKER_FILTER_TABLES )->shunlock();
                sleep( 5 );

                tied( $WORKER_FILTER_TABLES )->shlock( LOCK_EX );
                if( !defined $WORKER_FILTER_TABLES || ref( $WORKER_FILTER_TABLES ) ne 'HASH' )
                {
                    tied( $WORKER_FILTER_TABLES )->shunlock();
                    exit( 1 );
                }
            }

            foreach my $filter_table( keys %{$WORKER_FILTER_TABLES->{$worker_pid}} )
            {
                $WAL_DATA->{$filter_table} = [];
                if(
                       defined( $WORKER_FILTER_TABLES->{$worker_pid}->{$filter_table} )
                    && ref( $WORKER_FILTER_TABLES->{$worker_pid}->{$filter_table} ) eq 'ARRAY'
                    && scalar( @{$WORKER_FILTER_TABLES->{$worker_pid}->{$filter_table}} ) > 0
                  )
                {
                    while( scalar( @{$WORKER_FILTER_TABLES->{$worker_pid}->{$filter_table}} ) > 0 )
                    {
                        my $change = pop( @{$WORKER_FILTER_TABLES->{$worker_pid}->{$filter_table}} );
                        
                        push( @{$WAL_DATA->{$filter_table}}, $change );
                    }
                }
            }

            tied( $WORKER_FILTER_TABLES )->shunlock();
            
            foreach my $filter_table( keys %$WAL_DATA )
            {
                my $change;

                while( scalar( @{$WAL_DATA->{$filter_table}} ) > 0 )
                {
                    $change = pop( @{$WAL_DATA->{$filter_table}} );
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
                                push(
                                    @{$changes->{$schema}->{$table}->{$key}},
                                    $val
                                );
                            }
                        }

                        if(
                                !defined( $max_peeked_lsn )
                             || &lsn_cmp( $max_peeked_lsn, $change->{commit_lsn} ) < 0
                          )
                        {
                            $max_peeked_lsn = $change->{commit_lsn};
                        }
                    }
                }

                # Digest changes for this filter table
            }

            if( scalar( keys %$changes ) > 0 )
            {
                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                $WORKER_STATUSES->{$worker_pid}->{statuses} = $WORKER_STATUS_UPDATING;
                tied( $WORKER_STATUSES )->shunlock();

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
                        'Failed to apply filters to query for cache '
                      . "table '$CACHE_HASH->{name}'"
                    );
                    next;
                }

                # At this point we're ready to execute the table into a temp
                # table
                my $temp_table = &generate_temp_table( $handle, $query );

                if( !defined( $temp_table ) )
                {
                    _log(
                        $LOG_LEVEL_ERROR,
                        'Failed to generate temp table for updating cache '
                      . "table '$CACHE_HASH->{name}'"
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
                        "Deleting entries from $CACHE_HASH->{schema}."
                      . "$CACHE_HASH->{name} failed"
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
                        "Updating entries in $CACHE_HASH->{schema}."
                      . "$CACHE_HASH->{name} failed"
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
                        "Inserting entries into $CACHE_HASH->{schema}."
                      . "$CACHE_HASH->{name} failed"
                    );
                    next;
                }

                # If we make it here we can signal that we've applied up to
                # $max_peeked_lsn changes Check here to see if the table
                # definition has changed
                unless( &drop_temp_table( $handle, $temp_table ) )
                {
                    _log(
                        $LOG_LEVEL_ERROR,
                        'Failed to drop temporary table used to maintain cache '
                      . "table $CACHE_HASH->{schema}.$CACHE_HASH->{name}"
                    );
                    next;
                }

                print "Applied $max_peeked_lsn\n";
                $max_applied_lsn = $max_peeked_lsn;
                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                $WORKER_STATUSES->{$worker_pid}->{statuses} = $WORKER_STATUS_RUNNING;
                $WORKER_STATUSES->{$worker_pid}->{last_lsn} = $max_applied_lsn;
                print Dumper( $WORKER_STATUSES );
                tied( $WORKER_STATUSES )->shunlock();
            }

            sleep( $SLEEP_TIMER );
        } # postgres driver main loop
    }
    else
    {
        _log(
            $LOG_LEVEL_FATAL,
            "Worker cannot proceed. Driver $CACHE_HASH->{driver} not implemented"
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

if( defined( $port ) && ( $port !~ /^\d+$/ || $port < 1 || $port > 65535 ) )
{
    usage( 'Invalid port' );
}

unless( defined( $dbname ) && length( $dbname ) > 0 )
{
    usage( 'Invalid database name' );
}

unless( defined( $user ) && length( $user ) > 0 )
{
    usage( 'Invalid username' );
}

unless( defined( $host ) && length( $host ) > 0 )
{
    usage( 'Invalid host name' );
}

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
    croak(
        'There appears to be another instance of '
      . "$EXTENSION_NAME running on this database\n"
    );
}

if( !create_replication_slot( $handle ) )
{
    croak(
        'Failed to create replication slot'
    );
}

my $worker_data = get_worker_list( $handle );

$handle->disconnect();
undef( $handle );

## GLOBAL SHM VARIABLES
my $WORKER_FILTER_TABLES = {};
my $WORKER_STATUSES      = {};

tie(
    $WORKER_FILTER_TABLES,
    'IPC::Shareable',
    {
        key     => 'WORKER_FILTER_TABLES',
        create  => 1,
        destroy => 1
    }
);
tie(
    $WORKER_STATUSES,
    'IPC::Shareable',
    {
        key     => 'STATUSES',
        create  => 1,
        destroy => 1
    }
);

if( $CLEAN_UP )
{
    shm_cleanup();
    exit( 0 );
}

# Wipe and start fresh if we crashed previously
my $worker_mapping = {};
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
        $worker_mapping->{$pk_maintenance_object} = $child_pid;
        tied( $WORKER_FILTER_TABLES )->shlock( LOCK_EX );

        foreach my $filter_table( @$filter_tables )
        {
            if( !defined( $WORKER_FILTER_TABLES->{$child_pid}->{$filter_table} ) )
            {
                $WORKER_FILTER_TABLES->{$child_pid}->{$filter_table} = [];
            }
            # Setup the pins (parsed XLOG queue) for each filter table
            # changes relevent to said changes will be pushed into this queue
            # by the parent and popped later by the workers
        }
        tied( $WORKER_FILTER_TABLES )->shunlock();

        $WORKER_STATUSES->{$child_pid}->{status}   = $WORKER_STATUS_STARTUP;
        $WORKER_STATUSES->{$child_pid}->{shutdown} = 0;
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
parent_loop( $WORKER_STATUSES, $WORKER_FILTER_TABLES, $worker_mapping );
_log( $LOG_LEVEL_ERROR, "Parent exited main loop" );
shm_cleanup();
exit( 0 );
