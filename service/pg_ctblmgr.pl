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
# - This can read queries but is relatively untested against all the possible
#   variations and expressiveness of SQL. Therefore, the simpler and less
#   deeply nested a query can be, the better. There are safety checks to
#   prevent bad queries from executing.
# - This requires, like matviews, that a unique expression exists on the table,
#   though this can support multiple unique indicies.
# - Due to IPC::Shareable limitations / the way perl handles data structures
#   under the hood, the shared structures, while cumbersome, prevent memory
#   leaks by, for instance, by eschewing delete() calls and overwriting data
#   in-place.
# - Because of the above, and some weirdness surrounding refs - we need to use
#   cumbersome methods to manipulate arrayrefs. this involves some convoluted
#   code around push/pop/shift/unshift
# NOTE:
# IPC::Shareable keys seeem to be extremely short (4 chars) and may collide!

# enables holding past transactions open for a trailing XID chain we can use
# to lookup historic data

Readonly my $REFRESH_ON_START   => 1;
Readonly my $XID_IDLE_TIMEOUT   => 1000 * 3600; # 1 hour
Readonly my $SLEEP_TIMER        => 0.5; # seconds for main loop
Readonly my $DEFAULT_WFT_SIZE   => 1024 * 1024;

our $OUTPUT_AUTOFLUSH = 1;
our $|                = 1;

## GLOBAL VARIABLES
$PARENT_PID  = $PROCESS_ID;
$LOG_FILE    = '';
$LOG_FH      = undef;
$DAEMONIZE   = 0;

END {
    _terminate();
}

sub _terminate_sigint()
{
    # Wrapper to mask errors
    if( $PROCESS_ID == $PARENT_PID )
    {
        _log( $LOG_LEVEL_INFO, 'Parent process shutting down' );
    }
    else
    {
        _log( $LOG_LEVEL_INFO, 'Worker process shutting down' );
    }

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
        my $handle = &db_connect();

        if( $handle )
        {
            my $ret =  &drop_replication_slot( $handle );
            if( $ret > 0 )
            {
                _log(
                    $LOG_LEVEL_INFO,
                    "Replication slot '$SLOT_NAME' has been dropped"
                );
            }
            elsif( $ret < 0 )
            {
                _log(
                    $LOG_LEVEL_ERROR,
                    'Failed to drop replication slot - you will need to drop '
                  . "'$SLOT_NAME' manually"
                );
            }
        }
        else
        {
            _log(
                $LOG_LEVEL_ERROR,
                'Failed to connect to database - you will need to drop '
              . "'$SLOT_NAME' manually"
            );
        }
        #this is crucial to prevent running out of shm after crashes / term
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
    my @XID_MAP;

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

    if( $ENABLE_FAST_DELETE )
    {
        tie(
            @XID_MAP,
            'IPC::Shareable',
            { key => 'XID' }
        );
    }

    tied( $WORKER_FILTER_TABLES )->clean_up_all();
    tied( $WORKER_STATUSES      )->clean_up_all();
    tied( @XID_MAP              )->clean_up_all() if( $ENABLE_FAST_DELETE );

    return;
}

sub shm_pre_cleanup()
{
    foreach my $key( split( "\n", `ipcs -m | grep -v -E 'postgres' | grep \$(whoami) | grep '0x' | awk '{print \$1}'` ) )
    {
        return 0 unless( system( "ipcrm --shmem-key $key" ) == 0 );
    }

    foreach my $key( split( "\n", `ipcs -s | grep -v -E 'postgres' | grep \$(whoami) | grep '0x' | awk '{print \$1}'` ) )
    {
        return 0 unless( system( "ipcrm --semaphore-key $key" ) == 0 );
    }

    return 1;
}

sub get_distinct_filter_tables()
{
    my $WORKER_FILTER_TABLES;

    tie(
        $WORKER_FILTER_TABLES,
        'IPC::Shareable',
        { key => 'WORKER_FILTER_TABLES' }
    );

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
            my $ct_hash               = &get_ct_digest(
                $handle,
                $pk_maintenance_object
            );

            unless( $ct_hash )
            {
                _log(
                    $LOG_LEVEL_ERROR,
                    'Failed to get digest for cache table '
                  . "$pk_maintenance_object"
                );
                next;
            }

            $WORKER_DATA->{$pk_maintenance_object} = $ct_hash;
        }
    }
    else
    {
        #_log( $LOG_LEVEL_ERROR, 'Failed to get updated worker list or no workers exist' );
        return undef;
    }

    return $WORKER_DATA;
}

sub check_for_new_cache_tables($$$)
{
    my( $handle, $current_workers, $new_workers ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => HASHREF | UNDEF },
        { type => HASHREF },
    );

    my $diff = {
        new    => {},
        change => {},
        old    => {},
    };

    foreach my $pk_mo( keys %$new_workers )
    {
        if( defined( $current_workers->{$pk_mo} ) )
        {
            next if( $current_workers->{$pk_mo} eq $new_workers->{$pk_mo} );

            #indicate a change to a CT
            $diff->{change}->{$pk_mo}  = $new_workers->{$pk_mo};
            $current_workers->{$pk_mo} = $new_workers->{$pk_mo};
        }
        else
        {
            #indicate a new CT has been added
            $diff->{new}->{$pk_mo}     = $new_workers->{$pk_mo};
            $current_workers->{$pk_mo} = $new_workers->{$pk_mo};
        }
    }

    foreach my $pk_mo( keys %$current_workers )
    {
        next if( defined( $new_workers->{$pk_mo} ) );
        #indicate a removed CT
        $diff->{old}->{$pk_mo} = $current_workers->{$pk_mo};
    }

    foreach my $pk_mo( keys %{$diff->{old}} )
    {
        delete( $current_workers->{$pk_mo} );
    }

    return $diff;
}

sub _rollback_and_disconnect($)
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    $handle->do( 'ROLLBACK' );
    $handle->disconnect();
    return;
}

# XXX
sub new_xid_placeholder($$$)
{
    my( $new_handle, $new_xid, $new_snapshot ) = validate_pos(
        @_,
        { type => SCALARREF },
        { type => SCALARREF },
        { type => SCALARREF },
    );

    $$new_handle = &db_connect( $$new_handle );

    return 0 unless( $$new_handle );

    $$new_handle->do( "SET idle_session_timeout = $XID_IDLE_TIMEOUT" );
    $$new_handle->do( 'BEGIN' );

    my $sth = $$new_handle->prepare( 'SELECT txid_current() AS xid' );

    unless( $sth )
    {
        _rollback_and_disconnect( $$new_handle );
        return 0;
    }

    unless( $sth->execute() )
    {
        _rollback_and_disconnect( $$new_handle );
        return 0;
    }

    my $row = $sth->fetchrow_hashref();
    $$new_xid = $row->{xid};
    $sth->finish();

    if( $$new_xid =~ m/^\d+$/ )
    {
        $sth = $$new_handle->prepare(
            'SELECT pg_export_snapshot() AS snapshot'
        );

        unless( $sth )
        {
            _rollback_and_disconnect( $$new_handle );
            return 0;
        }

        unless( $sth->execute() )
        {
            _rollback_and_disconnect( $$new_handle );
            return 0;
        }

        $row           = $sth->fetchrow_hashref();
        $$new_snapshot = $row->{snapshot};
        $sth->finish();
        $$new_handle->do(
            "SET application_name = '$EXTENSION_NAME snapshot for $$new_xid'"
        );
        $$new_handle->do( 'SELECT 1' );
        return 1;
    }

    _rollback_and_disconnect( $$new_handle );
    return 0;
}

## PARENT
sub parent_loop($$$)
{
    my( $WORKER_STATUSES, $WORKER_FILTER_TABLES, $worker_mapping ) = validate_pos(
        @_,
        { type => HASHREF }, # shm status hash
        { type => HASHREF }, # shm WAL hash
        { type => HASHREF }, # local mapping of pk_maint_obj -> pid
    );

    my $XID_MAP = [];
    my $handle = &db_connect();

    if( !check_extension_running( $handle ) )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to secure advisory lock in parent process" );
    }

    if( !tied( $WORKER_FILTER_TABLES ) )
    {
        tie( $WORKER_FILTER_TABLES, 'IPC::Shareable', { key => 'WORKER_FILTER_TABLES' } );
    }

    if( !tied( $WORKER_STATUSES ) )
    {
        tie( $WORKER_STATUSES, 'IPC::Shareable', { key => 'STATUSES' } );
    }

    my $local_xid_map = {};

    if( $ENABLE_FAST_DELETE && !tied( $XID_MAP ) )
    {
        tie( $XID_MAP, 'IPC::Shareable', { key => 'XID' } );
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

    my $last_current_lsn;
    my $last_peeked_lsn;
    my $max_peeked_lsn;
    my $max_idle_lsn;

    my $last_seeked_lsn;
    my $filter_table_lsns = {}; # contains the BEGIN lsn for each filter - the max() of all of these
    # if the 'latest' we can safely seek to

    # There are two interlocks here:
    #   - We only ack the 'least' LSN applied by all workers
    #   - We need to continuously ack LSNs that don't apply to any workers
    #       - Handles the case where the primary is busy but there is no
    #         activity on the base tables which 'drive' our cache tables.

    my $DISTINCT_FILTER_TABLES = get_distinct_filter_tables();
    my $all_filter_tables      = join( ',', @$DISTINCT_FILTER_TABLES );
    my $WORKER_DATA            = {};
    $WORKER_DATA               = populate_worker_data( $handle, $WORKER_DATA );
    my $wal_level              = 'M';
    my $dispatched_changes     = {};

    while( 1 )
    {
        # Each loop we determine what LSN we can seek to, if any, and seek to that point

        my $seekable_lsn;
        ## CACHE TABLE MANAGEMENT
        my $tmp_worker_data = {};
        $tmp_worker_data    = populate_worker_data(
            $handle,
            $tmp_worker_data
        );

        # handle edge case startup with 0 workers
        unless( defined $tmp_worker_data )
        {
            # Idle until we have workers to start
            _log(
                $LOG_LEVEL_DEBUG,
                'It appears there are no workers to create, idling until they exist'
            );
            sleep( 4 );
        }
        else
        {
            if( !defined( $WORKER_DATA ) )
            {
                $WORKER_DATA = populate_worker_data( $handle, $WORKER_DATA );
            }
        }

        my $diff = {};

        # Worker management - handle new / changed / removed definitions
        # Note that workers themselves will handle changes in definitions
        # TODO: Verify filter tables is getting set correctly.
        if( defined( $WORKER_DATA ) && defined( $tmp_worker_data ) )
        {
            $diff = check_for_new_cache_tables(
                $handle,
                $WORKER_DATA,
                $tmp_worker_data
            );

            if(
                   scalar( keys %{$diff->{new}}    ) > 0
                || scalar( keys %{$diff->{change}} ) > 0
                || scalar( keys %{$diff->{old}}    ) > 0
              )
            {
                # Cache table changes detected
                _log(
                    $LOG_LEVEL_INFO,
                    'Detected changes to cache table definitions'
                );

                # we don't want forking or termination of children to tamper with our handle
                # so we undef it for when execve clones the memory space.
                $handle->disconnect();
                undef( $handle );
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
                    delete( $WORKER_FILTER_TABLES->{$target_pid} ); ## this may leak shm
                    tied( $WORKER_FILTER_TABLES )->shunlock();
                }

                # Add new children
                foreach my $pk_maintenance_object( keys %{$diff->{new}} )
                {
                    _log(
                        $LOG_LEVEL_DEBUG,
                        "Adding new worker for pk $pk_maintenance_object"
                    );

                    $handle = &db_connect( $handle );
                    my $worker_data = get_worker_list(
                        $handle,
                        $pk_maintenance_object
                    );
                    $handle->disconnect();
                    undef( $handle );
                    unless( $worker_data )
                    {
                        _log(
                            $LOG_LEVEL_ERROR,
                            'Need to spin up new child but could not locate maintenance object'
                        );
                        next;
                    }

                    $worker_data            = $worker_data->[0];
                    my $wal_level           = $worker_data->{wal_level};
                    my $filter_tables       = $worker_data->{filter_tables};
                    my $maintenance_channel = $worker_data->{maintenance_channel};
                    my $ct_name             = $worker_data->{name};
                    my $child_pid           = fork();

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

                        $WORKER_STATUSES->{$child_pid}->{status}             = $WORKER_STATUS_STARTUP;
                        $WORKER_STATUSES->{$child_pid}->{shutdown}           = 0;
                        $WORKER_STATUSES->{$child_pid}->{last_lsn}           = undef;
                        $WORKER_STATUSES->{$child_pid}->{maintenance_object} = $pk_maintenance_object;
                        $WORKER_STATUSES->{$child_pid}->{name}               = $ct_name;
                        _log( $LOG_LEVEL_DEBUG, "Parent created child $child_pid" );
                    }
                    else
                    {
                        _log( $LOG_LEVEL_FATAL, 'Failed to fork worker process' );
                    }
                }

                # We've likely disconnected to prevent execve sillyness - reconnect now
                $handle = &db_connect( $handle );
            }
        }


        ## CHANGE MANAGEMENT
        ##==================

        my $num_in_flight_changes   = 0; # number of changes we're queueing
        my $num_outstanding_changes = 0; # number of changes we've queued previously
        my $WT_LOCKED               = 0;

        ### LSN / Change Management
        ###========================

        # Here we peek changes (get them but do not change the slot's LSN).
        # These changes are then passed to child processes and, after the
        # relevent change is acknowledged, we 'seek' these changes, in that
        # we acknowledge them with respect to the replication slot.

        my $data;

        # Always get the "last_current_lsn" before peeking - that way we can never
        # miss changes on a presumably idle system that could have happened between
        # calls to get_current_lsn and replication_peek()

        $last_current_lsn = &get_current_lsn( $handle );

        $data = &replication_peek(
            $handle,
            $all_filter_tables,
            $wal_level,
            \$last_peeked_lsn
        );

        #_log( $LOG_LEVEL_DEBUG, "Peeking done - last $last_peeked_lsn" ) if( $last_peeked_lsn );

        if( $data )
        {
            # iterate over each change in outer loop - one change may go to one or more workers
            _log( $LOG_LEVEL_DEBUG, 'Distributing ' . scalar( @$data ) . ' changes' );
            foreach my $change( @$data )
            {
                $num_in_flight_changes++;
                if( !$WT_LOCKED )
                {
                    my $lock_time = [ gettimeofday() ];
                    tied( $WORKER_FILTER_TABLES )->shlock( LOCK_EX );
                    $WT_LOCKED = 1;
                    my $lock_delta = tv_interval( $lock_time, [ gettimeofday() ] );
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

                        my $commit_lsn = $change->{commit_lsn};
                        my $change_lsn = $change->{begin_lsn};

                        if(
                               !defined( $filter_table_lsns->{$filter_table} )
                            || lsn_cmp( $filter_table_lsns->{$filter_table}, $change_lsn ) < 0
                          )
                        {
                            $filter_table_lsns->{$filter_table} = $change_lsn;
                        }

                        if( !defined( $dispatched_changes->{$pid} ) )
                        {
                            $dispatched_changes->{$pid} = [];
                        }

                        unless( grep( /^$commit_lsn$/, @{$dispatched_changes->{$pid}} ) )
                        {
                            push( @{$dispatched_changes->{$pid}}, $commit_lsn );
                        }
                    }
                }
            }

            _log( $LOG_LEVEL_DEBUG, "All changes dispatched" );

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

        # maintain dispatched_changes list relative to last_lsn reported by each worker.
        # Post this loop, dispatched_changes will reflect outstanding lsn changes for each worker
        # meaning that we cannot seek past the youngest lsn

        my $youngest_in_flight_lsn;

        foreach my $pid( keys %$worker_lsns )
        {
            my $last_lsn = $worker_lsns->{$pid};

            # here we will maintain the local diaptched_changes versus the global applied lsns
            # if we find a dispatched change for this PID that is <= the PID's last lsn, we remove it
            # such that dispatched changes contains a list of outstanding (in-flight) LSNs

            if(
                   defined( $dispatched_changes->{$pid} )
                && scalar( @{$dispatched_changes->{$pid}} ) > 0
              )
            {
                my @ordered_changes = sort lsn_cmp @{$dispatched_changes->{$pid}};
                my $remove_lsns = [];
                foreach my $dispatched_lsn( @ordered_changes )
                {
                    if( lsn_cmp( $dispatched_lsn, $last_lsn ) <= 0 )
                    {
                        push( @$remove_lsns, $dispatched_lsn );
                    }
                }

                foreach my $remove_lsn( @$remove_lsns )
                {
                    my $index = 0;
                    $index++ until( $dispatched_changes->{$pid}->[$index] eq $remove_lsn );
                    if(
                           defined( $dispatched_changes->{$pid}->[$index] )
                        && $dispatched_changes->{$pid}->[$index] eq $remove_lsn
                      )
                    {
                        splice( @{$dispatched_changes->{$pid}}, $index, 1 );
                    }
                }
            }
        }

        foreach my $pid( keys %$dispatched_changes )
        {
            if( defined $dispatched_changes->{$pid} && scalar( @{$dispatched_changes->{$pid}} ) > 0 )
            {
                if(
                    !defined( $youngest_in_flight_lsn )
                 || (
                        defined( $dispatched_changes->{$pid}->[0] )
                     && lsn_cmp( $youngest_in_flight_lsn, $dispatched_changes->{$pid}->[0] ) > 0
                    )
                  )
                {
                    $youngest_in_flight_lsn = $dispatched_changes->{$pid}->[0];
                    $num_outstanding_changes++;
                }
            }
        }

        ## LSN increment logic
        ##====================

        # max_idle_lsn contains the LSN of the first BEGIN change preceeding any
        # change we're actually concerned about. IFF no changes have happened,
        # we set it to last_peeked_lsn so that we have a consistent LSN to seek
        # to during idle times.

        if( $num_in_flight_changes == 0 && $num_outstanding_changes == 0 )
        {
            if( defined $max_idle_lsn && $max_idle_lsn eq $last_peeked_lsn )
            {
                _log(
                    $LOG_LEVEL_DEBUG,
                    "System appears idle, advancing slot to current lsn $last_current_lsn"
                );
                $max_idle_lsn = $last_current_lsn;
            }
            else
            {
                $max_idle_lsn = $last_peeked_lsn;
            }
        }

        #_log(
        #    $LOG_LEVEL_DEBUG,
        #    "In-flight: $num_in_flight_changes, "
        #  . "Outstanding: $num_outstanding_changes"
        #);

        $seekable_lsn = $max_idle_lsn;

        #if( $last_seeked_lsn )
        #{
        #    _log( $LOG_LEVEL_DEBUG, "Last SEEK: $last_seeked_lsn" );
        #}

        #if( $max_idle_lsn )
        #{
        #    _log( $LOG_LEVEL_DEBUG, "Max IDLE: $max_idle_lsn" );
        #}

        # Safety check - CANNOT seek past any in-flight change
        if(
               defined( $youngest_in_flight_lsn )
            && lsn_cmp( $youngest_in_flight_lsn, $max_idle_lsn ) < 0
          )
        {
            $seekable_lsn = $youngest_in_flight_lsn;
        }

        if(
             defined( $seekable_lsn )
         && (
                ( defined( $last_seeked_lsn ) && lsn_cmp( $seekable_lsn, $last_seeked_lsn ) > 0 )
             || !defined( $last_seeked_lsn )
            )
          )
        {
            my $rows = replication_seek( $handle, $seekable_lsn );
            if( $rows < 0 )
            {
                _log( $LOG_LEVEL_DEBUG, "Logical seek to $seekable_lsn failed" );
            }
            else
            {
                $last_seeked_lsn = $seekable_lsn;
            }
        }

        select( undef, undef, undef, $SLEEP_TIMER );

        ## WORKER HEALTH CHECKS
        ##=====================

        # TODO

        ## XID CHAIN MANAGEMENT
        ##=====================

        if( $ENABLE_FAST_DELETE )
        {
            tied( $XID_MAP )->shlock( LOCK_SH | LOCK_NB );

            if( !defined( $XID_MAP ) || scalar( @$XID_MAP ) < $MAX_XID_LENGTH )
            {
                my $new_handle;
                my $new_xid;
                my $new_snapshot;

                if( !new_xid_placeholder( \$new_handle, \$new_xid, \$new_snapshot ) )
                {
                    tied( $XID_MAP )->shunlock();
                    _log( $LOG_LEVEL_DEBUG, "Could not generate new XID chain member" );
                    next;
                }

                tied( $XID_MAP )->shlock( LOCK_EX );
                $local_xid_map->{$new_xid} = $new_handle;
                push(
                    @$XID_MAP,
                    {
                        xid      => $new_xid,
                        snapshot => $new_snapshot,
                        in_use   => []
                    }
                );
            }
            else
            {
                # Replace oldest chain member
                my $candidate_replace;
                my $candidate_replace_ind;
                my $replace_ind = 0;

                foreach my $elem( @$XID_MAP )
                {
                    if(
                           scalar( @{$elem->{in_use}} ) == 0
                        && (
                                !defined( $candidate_replace )
                             || $elem->{xid} < $candidate_replace
                           )
                      )
                    {
                        $candidate_replace     = $elem->{xid};
                        $candidate_replace_ind = $replace_ind;
                    }

                    $replace_ind++;
                }

                if( !defined( $candidate_replace ) )
                {
                    _log( $LOG_LEVEL_DEBUG, "No XID replacement candidate" );
                    tied( $XID_MAP )->shunlock();
                    next;
                }

                tied( $XID_MAP )->shlock( LOCK_EX );
                my $handle = $local_xid_map->{$candidate_replace};

                if( !defined( $handle ) )
                {
                    _log( $LOG_LEVEL_DEBUG, "No handle to remove" );
                    tied( $XID_MAP )->shunlock();
                    next;
                }

                if( $handle->ping() > 0 && $handle->pg_ping() > 0 )
                {
                    $handle->do( 'ROLLBACK' );
                }

                $handle->disconnect();
                undef( $handle );

                my $snapshot;
                my $new_xid;

                delete( $local_xid_map->{$candidate_replace} );

                if( !new_xid_placeholder( \$handle, \$new_xid, \$snapshot ) )
                {
                    _log( $LOG_LEVEL_DEBUG, "Failed to generate replacement xid member" );
                    tied( $XID_MAP )->shunlock();
                    next;
                }

                if( $XID_MAP->[$candidate_replace_ind]->{xid} != $candidate_replace )
                {
                    _log( $LOG_LEVEL_ERROR, "XID Chain replacement invalid - index $candidate_replace_ind is invalid for XID" );
                    tied( $XID_MAP )->shunlock();
                    next;
                }

                # note: this code is duplicated to handle updating XID_MAP in-place. Due to oddities in
                # how IPC::Shareable handles arrayrefs, the elements must be manipulated using push/pop
                # /shift/unshift. Futher complicating matters, this needs to share scope with the knots
                # created with tie() and tied(). Without this - cases where LOCK_SH get upgraded to LOCK_EX
                # will result in a deadlock on the same PID, unless scope of LOCK_SH call and LOCK_EX call
                # are the same.
                my $ind = 0;
                my @backup;

                while( $ind != $candidate_replace_ind )
                {
                    push( @backup, shift( @$XID_MAP ) );
                    $ind++;
                }

                shift( @$XID_MAP ); # throw away from sh,
                unshift(
                    @$XID_MAP,
                    {
                        snapshot => $snapshot,
                        xid      => $new_xid,
                        in_use   => [],
                    }
                ); # replace tossed element in-place

                while( scalar( @backup ) > 0 )
                {
                    unshift( @$XID_MAP, pop( @backup ) );
                }

                $local_xid_map->{$new_xid} = $handle;
            }

            tied( $XID_MAP )->shunlock();
        }
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

## WORKER
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

    &set_program_name( undef, "worker startup" );
    my $CACHE_HASH           = {};
    my $WORKER_FILTER_TABLES = {};
    my $WAL_DATA;
    my $WORKER_STATUSES      = {};
    my $XID_MAP = [];

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

    if( $ENABLE_FAST_DELETE )
    {
        tie(
            $XID_MAP,
            'IPC::Shareable',
            { key => 'XID' }
        );
    }

    my $count = 0;
    my $lim   = 15;

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

    my $handle = &db_connect();

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
    &set_program_name( $handle, "idle $CACHE_HASH->{name}" );

    if( $CACHE_HASH->{driver} eq 'postgresql' )
    {
        my $ct_check_start = [ gettimeofday() ];
        &check_ct_exists(
            $handle,
            $CACHE_HASH
        );
        my $ct_check_delta = tv_interval( $ct_check_start, [ gettimeofday() ] );
        _log( $LOG_LEVEL_DEBUG, "CT startup validation took $ct_check_delta seconds" );

        # Main worker loop
        &worker_cache_refresh(
            $handle,
            $pk_maintenance_object,
            $filter_tables,
            $CACHE_HASH
        );

        # Check state of the cache table prior to entry - we may have started after a partial table build!
        unless( $REFRESH_ON_START )
        {
            my $count_check_start = [ gettimeofday() ];
            &set_program_name( $handle, "size check: $CACHE_HASH->{name}" );
            my $desired_count = get_def_count( $handle, $CACHE_HASH->{definition} );
            my $current_count = get_table_count( $handle, $CACHE_HASH->{schema} . '.' . $CACHE_HASH->{name} );
            my $count_delta   = tv_interval( $count_check_start, [ gettimeofday() ] );
            _log( $LOG_LEVEL_DEBUG, "CT count check took $count_delta seconds" );

            if( $current_count != $desired_count )
            {
                _log( $LOG_LEVEL_INFO, "Out of date cache table detected on worker startup, initiating rebuild." );
                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                $WORKER_STATUSES->{$worker_pid}->{replace} = 1;
                tied( $WORKER_STATUSES )->shunlock();
            }
        }
        else
        {
            tied( $WORKER_STATUSES )->shlock( LOCK_EX );
            $WORKER_STATUSES->{$worker_pid}->{replace} = 1;
            tied( $WORKER_STATUSES )->shunlock();
        }

        # MAIN LOOP
        while( 1 )
        {
            # Check for commanded exit or replacement
            my $exit    = 0;
            my $replace = 0;

            tied( $WORKER_STATUSES )->shlock( LOCK_SH );

            if( defined( $WORKER_STATUSES ) && defined( $WORKER_STATUSES->{$worker_pid} ) )
            {
                if( defined( $WORKER_STATUSES->{$worker_pid}->{shutdown} ) )
                {
                    $exit    = $WORKER_STATUSES->{$worker_pid}->{shutdown};
                }

                if( defined( $WORKER_STATUSES->{$worker_pid}->{replace} ) )
                {
                    $replace = $WORKER_STATUSES->{$worker_pid}->{replace};
                }
            }

            tied( $WORKER_STATUSES )->shlock( LOCK_EX );
            $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_IDLE;
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
                    _log(
                        $LOG_LEVEL_ERROR,
                        "Failed to drop cache table $CACHE_HASH->{schema}.$CACHE_HASH->{name}"
                    );
                }

                exit( 0 );
            }

            if( defined $replace && $replace == 1 )
            {
                _log( $LOG_LEVEL_DEBUG, "Commanded to replace $CACHE_HASH->{name}" );

                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_REPLACE;
                tied( $WORKER_STATUSES )->shunlock();

                unless( &replace_cache_table( $handle, $pk_maintenance_object ) )
                {
                    _log(
                        $LOG_LEVEL_FATAL,
                        "Replacement of $CACHE_HASH->{name} failed after command to replace"
                    );
                }

                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                $WORKER_STATUSES->{$worker_pid}->{status}  = $WORKER_STATUS_IDLE;
                $WORKER_STATUSES->{$worker_pid}->{replace} = 0;
                tied( $WORKER_STATUSES )->shunlock();
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

                    tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                    $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_REPLACE;
                    tied( $WORKER_STATUSES )->shunlock();

                    unless( &replace_cache_table( $handle, $pk_maintenance_object ) )
                    {
                        _log( $LOG_LEVEL_FATAL, "Replacement of $CACHE_HASH->{name} failed" );
                    }

                    tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                    $WORKER_STATUSES->{$worker_pid}->{status}  = $WORKER_STATUS_IDLE;
                    $WORKER_STATUSES->{$worker_pid}->{replacE} = 0;
                    tied( $WORKER_STATUSES )->shunlock();
                }
            }

            # Process changes
            my $changes  = {};
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
            my $youngest_xid;
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
                        if( !defined $youngest_xid || $change->{xid} < $youngest_xid )
                        {
                            $youngest_xid = $change->{xid};
                        }

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

            my $aged_handle;
            my $aged_snapshot;

            if( scalar( keys %$changes ) > 0 )
            {
                # Timing variables
                my $query_parse_time;
                my $temp_table_time;
                my $fast_delete_time;
                my $slow_delete_time;
                my $update_time;
                my $insert_time;

                # Fast delete variables / flags
                my $can_fast_delete = 0;
                my $tried_fast_delete = 0;
                my $using_xid;
                my $using_xid_ind;

                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_QUERY_PARSE;
                tied( $WORKER_STATUSES )->shunlock();
                _log( $LOG_LEVEL_DEBUG, "Applying changes" );
                my $query_parse_start = [ gettimeofday() ];

                my $query = &apply_filters(
                    $handle,
                    $CACHE_HASH->{parse_tree},
                    $CACHE_HASH->{table_mapping},
                    $CACHE_HASH->{definition},
                    $CACHE_HASH->{relcache},
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

                $query_parse_time = tv_interval( $query_parse_start, [ gettimeofday() ] );
                _log( $LOG_LEVEL_DEBUG, "Query parse took $query_parse_time seconds" );

                if( $ENABLE_FAST_DELETE )
                {
                    # search XID_MAP for suitable XID
                    tied( $XID_MAP )->shlock( LOCK_SH | LOCK_NB );
                    my $best_candidate;
                    my $best_candidate_ind;
                    my $ind = 0;

                    foreach my $elem( @$XID_MAP )
                    {
                        if( defined( $elem->{xid} ) && $elem->{xid} <= $youngest_xid )
                        {
                            $best_candidate = $XID_MAP->[$ind]->{xid};
                            $best_candidate_ind = $ind;
                        }

                        $ind++;
                    }

                    # Add our PID to the list of PIDS using this XID/snapshot combo
                    if( defined( $best_candidate ) )
                    {
                        tied( $XID_MAP )->shlock( LOCK_EX );
                        unless( grep( /^$worker_pid$/, @{$XID_MAP->[$best_candidate_ind]->{in_use}} ) )
                        {
                            my $mod_hr = $XID_MAP->[$best_candidate_ind];
                            push( @{$mod_hr->{in_use}}, $worker_pid );
                            $ind = 0;
                            my @backup;

                            while( $ind != $best_candidate_ind )
                            {
                                push( @backup, shift( @$XID_MAP ) );
                                $ind++;
                            }

                            shift( @$XID_MAP ); # throw away from sh,
                            unshift( @$XID_MAP, $mod_hr ); # replace tossed element in-place

                            while( scalar( @backup ) > 0 )
                            {
                                unshift( @$XID_MAP, pop( @backup ) );
                            }

                            $aged_snapshot   = $XID_MAP->[$best_candidate_ind]->{snapshot};
                            $using_xid       = $best_candidate;
                            $using_xid_ind   = $best_candidate_ind;
                            $can_fast_delete = 1;
                            _log( $LOG_LEVEL_DEBUG, 'Found candidate XID for fast delete' );
                        }
                    }
                    else
                    {
                        _log( $LOG_LEVEL_DEBUG, 'Could not find candidate XID for fast delete' );
                    }

                    tied( $XID_MAP )->shunlock();
                }

                # Generate temp table containing state of rows relevent to the keys that have changed
                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_TEMP_TABLE;
                tied( $WORKER_STATUSES )->shunlock();
                &set_program_name( $handle, "temp table $CACHE_HASH->{name}" );
                my $temp_table_start = [ gettimeofday() ];
                my $temp_table       = &generate_temp_table( $handle, $query, $CACHE_HASH );

                if( !defined( $temp_table ) )
                {
                    _log(
                        $LOG_LEVEL_ERROR,
                        'Failed to generate temp table for updating cache '
                      . "table '$CACHE_HASH->{name}'"
                    );
                    next;
                }

                $temp_table_time = tv_interval( $temp_table_start, [ gettimeofday() ] );
                _log( $LOG_LEVEL_DEBUG, "Temp table generation took $temp_table_time seconds" );

                # Setup aged handle and lock-in snapshot for looking back in time to see
                # the state of the output relative to the changed keys.
                if( $can_fast_delete )
                {
                    _log( $LOG_LEVEL_DEBUG, "Using fast delete" );
                    # TODO, create aged_handle and SET TRANSACTION to aged_snapshot
                    $aged_handle = &db_connect();

                    $tried_fast_delete = 1;
                    unless( $aged_handle )
                    {
                        $can_fast_delete = 0;
                        _log( $LOG_LEVEL_DEBUG, 'Fast delete failed - could not connect aged handle' );
                        goto FD_FALLBACK;
                    }

                    $aged_handle->do( "SET application_name = '$EXTENSION_NAME historic $CACHE_HASH->{name}'" );

                    unless( $aged_handle->do( 'BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ' ) )
                    {
                        $can_fast_delete = 0;
                        _log( $LOG_LEVEL_DEBUG, 'Fast delete failed - could not begin repeatable read transaction' );
                        goto FD_FALLBACK;
                    }

                    unless( $aged_handle->do( "SET idle_session_timeout = $XID_IDLE_TIMEOUT" ) )
                    {
                        $can_fast_delete = 0;
                        _log( $LOG_LEVEL_DEBUG, 'Fast delete failed - could not set idle session timeout' );
                        goto FD_FALLBACK;
                    }

                    unless( $aged_handle->do( "SET TRANSACTION SNAPSHOT '$aged_snapshot'" ) )
                    {
                        $can_fast_delete = 0;
                        _log( $LOG_LEVEL_DEBUG, 'Fast delete failed - could not import aged snapshot' );
                        goto FD_FALLBACK;
                    }

                    _log(
                        $LOG_LEVEL_DEBUG,
                        "Established aged handle at snapshot $aged_snapshot "
                      . "with XID $using_xid, target $youngest_xid"
                    );
                }

FD_FALLBACK:
                if( !$can_fast_delete && $tried_fast_delete )
                {
                    if( defined $aged_handle && $aged_handle->ping() > 0 )
                    {
                        $aged_handle->do( 'ROLLBACK' );
                        $aged_handle->disconnect();
                        undef( $aged_handle );
                    }

                    tied( $XID_MAP )->shlock( LOCK_EX );
                    my $mod_hr = $XID_MAP->[$using_xid_ind];
                    @{$mod_hr->{in_use}} = grep { $_ ne $worker_pid } @{$mod_hr->{in_use}};
                    my $ind     = 0;
                    my @backup;

                    while( $ind != $using_xid_ind )
                    {
                        push( @backup, shift( @$XID_MAP ) );
                        $ind++;
                    }

                    shift( @$XID_MAP ); # throw away from sh,
                    unshift( @$XID_MAP, $mod_hr ); # replace tossed element in-place

                    while( scalar( @backup ) > 0 )
                    {
                        unshift( @$XID_MAP, pop( @backup ) );
                    }
                    tied( $XID_MAP )->shunlock();
                }

                if( $can_fast_delete && $tried_fast_delete && defined( $aged_handle ) )
                {
                    tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                    $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_FAST_DELETE;
                    tied( $WORKER_STATUSES )->shunlock();
                    &set_program_name( $handle, "fast delete $CACHE_HASH->{name}" );
                    &set_program_name( $aged_handle, "fast delete $CACHE_HASH->{name}" );
                    _log( $LOG_LEVEL_DEBUG, "Using fast delete" );
                    # Create temp table in aged handle && perform fast delete
                    my $aged_temp_table = generate_temp_table( $aged_handle, $query, $CACHE_HASH );

                    unless( $aged_temp_table )
                    {
                        _log( $LOG_LEVEL_ERROR, "Fast delete failed - could not create aged temp table" );
                        $can_fast_delete   = 0;
                        $tried_fast_delete = 1;
                        $aged_handle->do( 'ROLLBACK' );
                        $aged_handle->disconnect();
                        goto FD_FALLBACK;
                    }

                    my $fast_delete_start = [ gettimeofday() ];
                    unless(
                        &generate_aged_delete_statement(
                            $aged_handle,
                            $handle,
                            $aged_temp_table,
                            $temp_table,
                            $CACHE_HASH
                        )
                          )
                    {
                        _log( $LOG_LEVEL_ERROR, "Fast delete failed, falling back to slow delete" );
                        $can_fast_delete   = 0;
                        $tried_fast_delete = 1;

                        if( $aged_handle->ping() > 0 )
                        {
                            $aged_handle->do( 'ROLLBACK' );
                            $aged_handle->disconnect();
                        }

                        undef( $aged_handle );
                        goto FD_FALLBACK;
                    }

                    # delete finished, free resources
                    if( $aged_handle && $aged_handle->ping > 0 )
                    {
                        $aged_handle->do( 'ROLLBACK' );
                        $aged_handle->disconnect();
                        undef( $aged_handle );
                    }

                    $fast_delete_time = tv_interval( $fast_delete_start, [ gettimeofday() ] );
                    _log( $LOG_LEVEL_DEBUG, "Fast delete took $fast_delete_time seconds" );
                    tied( $XID_MAP )->shlock( LOCK_EX );
                    my $mod_hr = $XID_MAP->[$using_xid_ind];
                    @{$mod_hr->{in_use}} = grep { $_ ne $worker_pid } @{$mod_hr->{in_use}};
                    my $ind     = 0;
                    my @backup;

                    while( $ind != $using_xid_ind )
                    {
                        push( @backup, shift( @$XID_MAP ) );
                        $ind++;
                    }

                    shift( @$XID_MAP ); # throw away from sh,
                    unshift( @$XID_MAP, $mod_hr ); # replace tossed element in-place

                    while( scalar( @backup ) > 0 )
                    {
                        unshift( @$XID_MAP, pop( @backup ) );
                    }
                    tied( $XID_MAP )->shunlock();
                    _log( $LOG_LEVEL_DEBUG, "Worker released snapshot $aged_snapshot" );
                }

                if( !$can_fast_delete )
                {
                    tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                    $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_SLOW_DELETE;
                    tied( $WORKER_STATUSES )->shunlock();

                    &set_program_name( $handle, "slow delete $CACHE_HASH->{name}" );
                    _log( $LOG_LEVEL_DEBUG, "Using slow delete" );
                    my $slow_delete_start = [ gettimeofday() ];
                    my $delete_result = generate_delete_statement(
                        $handle,
                        $CACHE_HASH
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
                    $slow_delete_time = tv_interval( $slow_delete_start, [ gettimeofday() ] );
                    _log( $LOG_LEVEL_DEBUG, "Slow delete took $slow_delete_time seconds" );
                }

                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_UPDATE;
                tied( $WORKER_STATUSES )->shunlock();
                &set_program_name( $handle, "update $CACHE_HASH->{name}" );
                my $update_start = [ gettimeofday() ];
                my $update_result = generate_update_statement(
                    $handle,
                    $temp_table,
                    $CACHE_HASH
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
                $update_time = tv_interval( $update_start, [ gettimeofday() ] );
                _log( $LOG_LEVEL_DEBUG, "Update took $update_time seconds" );

                unless( $temp_table->{count} > $BULK_ACTION_CUTOFF )
                {
                    # We perform insert/update action with one fell swoop in generage_update_statement iff
                    # the above condition is met.
                    tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                    $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_INSERT;
                    tied( $WORKER_STATUSES )->shunlock();
                    &set_program_name( $handle, "insert $CACHE_HASH->{name}" );
                    my $insert_start = [ gettimeofday() ];
                    my $insert_result = generate_insert_statement(
                        $handle,
                        $temp_table,
                        $CACHE_HASH
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
                    $insert_time = tv_interval( $insert_start, [ gettimeofday() ] );
                    _log( $LOG_LEVEL_DEBUG, "Insert took $insert_time seconds" );
                }
                else
                {
                    _log( $LOG_LEVEL_DEBUG, "Fast update skipped INSERT" );
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

                &set_program_name( $handle, "idle $CACHE_HASH->{name}" );
                _log( $LOG_LEVEL_DEBUG, "====================== Applied $max_peeked_lsn" );
                $max_applied_lsn = $max_peeked_lsn;
                tied( $WORKER_STATUSES )->shlock( LOCK_EX );
                $WORKER_STATUSES->{$worker_pid}->{status} = $WORKER_STATUS_IDLE;
                $WORKER_STATUSES->{$worker_pid}->{last_lsn} = $max_applied_lsn;
                tied( $WORKER_STATUSES )->shunlock();
            }

            select( undef, undef, undef, $SLEEP_TIMER );
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
unless( shm_pre_cleanup() )
{
    _log( $LOG_LEVEL_ERROR, "Failed to prune shared memory on startup" );
}

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
my $handle = &db_connect();

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
my $XID_MAP              = [];

tie(
    $WORKER_FILTER_TABLES,
    'IPC::Shareable',
    {
        key     => 'WORKER_FILTER_TABLES',
        create  => 1,
        destroy => 1,
        limit   => 0,
        size    => $DEFAULT_WFT_SIZE,
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

if( $ENABLE_FAST_DELETE )
{
    tie(
        $XID_MAP,
        'IPC::Shareable',
        {
            key     => 'XID',
            create  => 1,
            destroy => 1,
        }
    );
}

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

if( !defined( $worker_data ) || scalar( @$worker_data ) == 0 )
{
    _log( $LOG_LEVEL_INFO, "No workers to start, please populate pgctblmgr.tb_maintenance_object" );
}
else
{
    tied( $WORKER_STATUSES )->shlock( LOCK_EX );
    foreach my $worker_entry( @$worker_data )
    {
        my $filter_tables         = $worker_entry->{filter_tables};
        my $wal_level             = $worker_entry->{wal_level};
        my $maintenance_channel   = $worker_entry->{maintenance_channel};
        my $pk_maintenance_object = $worker_entry->{maintenance_object};
        my $ct_name               = $worker_entry->{name};
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

            $WORKER_STATUSES->{$child_pid}->{status}             = $WORKER_STATUS_STARTUP;
            $WORKER_STATUSES->{$child_pid}->{shutdown}           = 0;
            $WORKER_STATUSES->{$child_pid}->{replace}            = 0;
            $WORKER_STATUSES->{$child_pid}->{last_lsn}           = undef;
            $WORKER_STATUSES->{$child_pid}->{maintenance_object} = $pk_maintenance_object;
            $WORKER_STATUSES->{$child_pid}->{name}               = $ct_name;
            _log( $LOG_LEVEL_DEBUG, "Parent created child $child_pid" );
        }
        else
        {
            _log( $LOG_LEVEL_FATAL, "Failed to fork worker process" );
        }
    }

    # We've started workers, lets start processing WAL
    _log( $LOG_LEVEL_INFO, "All workers started" );
    tied( $WORKER_STATUSES )->shunlock();
}

&set_program_name( undef, "parent process" );
parent_loop( $WORKER_STATUSES, $WORKER_FILTER_TABLES, $worker_mapping );
_log( $LOG_LEVEL_ERROR, "Parent exited main loop" );
shm_cleanup();
exit( 0 );
