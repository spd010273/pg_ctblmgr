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

use Data::Dumper;

use FindBin;
use lib "$FindBin::Bin/lib";

use Util;
use DB;
use QueryParser;
use Shm;

# DEV NOTES:
# - This can read queries but is relatively untested against all the possible
#   variations and expressiveness of SQL. Therefore, the simpler and less
#   deeply nested a query can be, the better. There are safety checks to
#   prevent bad queries from executing.
# - This requires, like matviews, that a unique expression exists on the table,
#   though this can support multiple unique indicies.
# enables holding past transactions open for a trailing XID chain we can use
# to lookup historic data

Readonly my $ACTIVE_CHANGES_KEY => '__ACTIVE_CHANGES__';
Readonly my $REFRESH_ON_START   => 0;
Readonly my $XID_IDLE_TIMEOUT   => 1000 * 3600; # 1 hour
Readonly my $SLEEP_TIMER        => 0.25; # seconds for main loop

Readonly my $WFT_KEY => 17783312;
Readonly my $WS_KEY  => 17783313;
Readonly my $XID_KEY => 17783314;
Readonly my $TIMING => 0;
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

sub update_status($;$)
{
    my( $info_hash, $override_pid ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR, optional => 1 },
    );

    my $success = 0;
    my $target_pid = $PROCESS_ID;
    $target_pid = $override_pid if( defined $override_pid && $PARENT_PID == $PROCESS_ID );
    return 0 if( !defined( $info_hash ) );
    do_lock( $WS_KEY, $WRITE_LOCK );
    my $WORKER_STATUSES = readmem( $WS_KEY );
    if( defined( $WORKER_STATUSES ) )
    {
        $WORKER_STATUSES->{$target_pid}->{status}             = $info_hash->{status}             if( $info_hash->{status} );
        $WORKER_STATUSES->{$target_pid}->{name}               = $info_hash->{name}               if( $info_hash->{name} );
        $WORKER_STATUSES->{$target_pid}->{maintenance_object} = $info_hash->{maintenance_object} if( $info_hash->{maintenance_object} );
        $WORKER_STATUSES->{$target_pid}->{shutdown}           = $info_hash->{shutdown}           if( $info_hash->{shutdown} );
        $WORKER_STATUSES->{$target_pid}->{replace}            = $info_hash->{replace}            if( $info_hash->{replace} );
        $WORKER_STATUSES->{$target_pid}->{last_lsn}           = $info_hash->{last_lsn}           if( $info_hash->{last_lsn} );
        writemem( $WS_KEY, $WORKER_STATUSES );
        $success = 1;
    }
    else
    {
        $success = 0;
    }

    do_lock( $WS_KEY, $WRITE_UNLOCK );
    return $success;
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
        &do_shm_cleanup();
    }

    if( @_ )
    {
        CORE::die( @_ );
    }

    exit( 0 );
}

$SIG{INT} = \&_terminate_sigint;
$SIG{__DIE__} = \&_terminate;


sub shm_pre_cleanup()
{
    do_cleanup_key( $WS_KEY );
    do_cleanup_key( $XID_KEY );
    do_cleanup_key( $WFT_KEY );
    return 1;
}

sub get_distinct_filter_tables()
{
    my $WORKER_FILTER_TABLES;

    do_lock( $WFT_KEY, $READ_LOCK );
    $WORKER_FILTER_TABLES = readmem( $WFT_KEY );
    do_lock( $WFT_KEY, $READ_UNLOCK );
    my $DISTINCT_FILTER_TABLES = [];

    foreach my $pid( keys %$WORKER_FILTER_TABLES )
    {
        foreach my $filter_table( keys %{$WORKER_FILTER_TABLES->{$pid}} )
        {
            next if( $filter_table eq $ACTIVE_CHANGES_KEY );
            unless( grep /^$filter_table$/, @$DISTINCT_FILTER_TABLES )
            {
                push( @$DISTINCT_FILTER_TABLES, $filter_table );
            }
        }
    }

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

            $WORKER_DATA->{$pk_maintenance_object}->{hash} = $worker_entry->{hash};
            $WORKER_DATA->{$pk_maintenance_object}->{name} = $worker_entry->{name};
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
            next if( $current_workers->{$pk_mo}->{hash} eq $new_workers->{$pk_mo}->{hash} );

            #indicate a change to a CT
            $diff->{change}->{$pk_mo}  = $new_workers->{$pk_mo}->{name};
            $current_workers->{$pk_mo} = $new_workers->{$pk_mo}->{name};
        }
        else
        {
            #indicate a new CT has been added
            $diff->{new}->{$pk_mo}     = $new_workers->{$pk_mo}->{name};
            $current_workers->{$pk_mo} = $new_workers->{$pk_mo}->{name};
        }
    }

    foreach my $pk_mo( keys %$current_workers )
    {
        next if( defined( $new_workers->{$pk_mo} ) );
        #indicate a removed CT
        $diff->{old}->{$pk_mo} = $current_workers->{$pk_mo}->{name};
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
sub parent_loop($)
{
    my( $worker_mapping ) = validate_pos(
        @_,
        { type => HASHREF }, # local mapping of pk_maint_obj -> pid
    );

    my $WORKER_STATUSES;
    my $WORKER_FILTER_TABLES;
    my $XID_MAP = [];
    my $handle = &db_connect();
    my $first_loop_done = 0;
    if( !check_extension_running( $handle ) )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to secure advisory lock in parent process" );
    }

    my $local_xid_map = {};

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
    my $xid_map_spread_ind     = 0;
    my $last_worker_count      = 0;
    # Timing vars
    my $xid_start;
    my $worker_check_start;
    my $youngest_lsn_proc_start;
    my $peek_start;
    my $dist_start;
    my $lsn_increment_start;
    my $idle_check_start;
    my $worker_last_lsn_start;

    while( 1 )
    {
        ## XID CHAIN MANAGEMENT
        ##=====================
        $xid_start = [ gettimeofday() ] if( $TIMING );
        if( $ENABLE_FAST_DELETE )
        {
            do_lock( $XID_KEY, $READ_LOCK );
            $XID_MAP = readmem( $XID_KEY );
            if( $xid_map_spread_ind % $XID_MAP_SPREAD == 0 )
            {
                $xid_map_spread_ind = 0;
            }

            if( $xid_map_spread_ind == 0 )
            {
                if( !defined( $XID_MAP ) || ref( $XID_MAP ) ne 'ARRAY' || scalar( @$XID_MAP ) < $MAX_XID_LENGTH )
                {
                    my $new_handle;
                    my $new_xid;
                    my $new_snapshot;

                    if( !$first_loop_done )
                    {
                        _log( $LOG_LEVEL_INFO, "Creating replication slot: $SLOT_NAME..." );
                        if( !create_replication_slot( $handle ) )
                        {
                            _log( $LOG_LEVEL_FATAL, "Failed to create replication slot" );
                        }
                        _log( $LOG_LEVEL_INFO ,"Slot $SLOT_NAME created!" );
                    }

                    if( !new_xid_placeholder( \$new_handle, \$new_xid, \$new_snapshot ) )
                    {
                        do_lock( $XID_KEY, $READ_UNLOCK );
                        _log( $LOG_LEVEL_DEBUG, "Could not generate new XID chain member" );
                        next;
                    }

                    do_lock( $XID_KEY, $READ_UNLOCK );
                    do_lock( $XID_KEY, $WRITE_LOCK );
                    $XID_MAP = readmem( $XID_KEY );
                    $local_xid_map->{$new_xid} = $new_handle;
                    push(
                        @$XID_MAP,
                        {
                            xid      => $new_xid,
                            snapshot => $new_snapshot,
                            in_use   => []
                        }
                    );

                    writemem( $XID_KEY, $XID_MAP );
                    do_lock( $XID_KEY, $WRITE_TO_READ );
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
                               defined( $elem )
                            && defined( $elem->{in_use} )
                            && scalar( @{$elem->{in_use}} ) == 0
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
                        do_lock( $XID_KEY, $READ_UNLOCK );
                        next;
                    }

                    do_lock( $XID_KEY, $READ_UNLOCK );
                    do_lock( $XID_KEY, $WRITE_LOCK );
                    $XID_MAP = readmem( $XID_KEY );
                    my $replace_handle = $local_xid_map->{$candidate_replace};

                    if( !defined( $replace_handle ) )
                    {
                        _log( $LOG_LEVEL_DEBUG, "No handle to remove" );
                        do_lock( $XID_KEY, $WRITE_UNLOCK );
                        next;
                    }

                    if( $replace_handle->ping() > 0 && $replace_handle->pg_ping() > 0 )
                    {
                        $replace_handle->do( 'ROLLBACK' );
                    }

                    $replace_handle->disconnect();
                    undef( $replace_handle );

                    my $snapshot;
                    my $new_xid;

                    delete( $local_xid_map->{$candidate_replace} );

                    if( !new_xid_placeholder( \$replace_handle, \$new_xid, \$snapshot ) )
                    {
                        _log( $LOG_LEVEL_DEBUG, "Failed to generate replacement xid member" );
                        do_lock( $XID_KEY, $WRITE_UNLOCK );
                        next;
                    }

                    if( $XID_MAP->[$candidate_replace_ind]->{xid} != $candidate_replace )
                    {
                        _log( $LOG_LEVEL_ERROR, "XID Chain replacement invalid - index $candidate_replace_ind is invalid for XID" );
                        do_lock( $XID_KEY, $WRITE_UNLOCK );
                        next;
                    }

                    $XID_MAP->[$candidate_replace_ind] = {
                        snapshot => $snapshot,
                        xid      => $new_xid,
                        in_use   => [],
                    };
                    writemem( $XID_KEY, $XID_MAP );
                    $local_xid_map->{$new_xid} = $replace_handle;

                    do_lock( $XID_KEY, $WRITE_TO_READ );
                }
            }
            do_lock( $XID_KEY, $READ_UNLOCK );
            $xid_map_spread_ind++;
        }
        
        if( $TIMING )
        {
            my $xid_delta = tv_interval( $xid_start, [ gettimeofday() ] );
            _log( $LOG_LEVEL_DEBUG, "XID management took $xid_delta seconds" );
        }

        if( !$first_loop_done && $REFRESH_ON_START )
        {
            do_lock( $WS_KEY, $WRITE_LOCK );
            $WORKER_STATUSES = readmem( $WS_KEY );
            foreach my $w_pid( keys %$WORKER_STATUSES )
            {
                $WORKER_STATUSES->{$w_pid}->{replace} = 1;
            }

            writemem( $WS_KEY, $WORKER_STATUSES );
            do_lock( $WS_KEY, $WRITE_UNLOCK );
            # first iteration - lets command all workers to rebuild
        }
        # Each loop we determine what LSN we can seek to, if any, and seek to that point

        my $seekable_lsn;
        ## CACHE TABLE MANAGEMENT
        $worker_check_start = [ gettimeofday() ] if( $TIMING );
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
        
        if(
                scalar( keys %$tmp_worker_data ) != $last_worker_count # Oneshot skips expensive diff
             && defined( $WORKER_DATA )
             && defined( $tmp_worker_data )
          )
        {
            $last_worker_count = scalar( keys %$tmp_worker_data );
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
                    if( update_status( { shutdown => 1 }, $target_pid ) )
                    {
                        _log( $LOG_LEVEL_DEBUG, "Parent terminating child $target_pid" );
                    }
                    else
                    {

                    }
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
                    do_lock( $WFT_KEY, $WRITE_LOCK );
                    $WORKER_FILTER_TABLES = readmem( $WFT_KEY );
                    delete( $WORKER_FILTER_TABLES->{$target_pid} ); ## this may leak shm
                    writemem( $WFT_KEY, $WORKER_FILTER_TABLES );
                    do_lock( $WFT_KEY, $WRITE_UNLOCK );
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
                        do_lock( $WFT_KEY, $WRITE_LOCK );
                        $WORKER_FILTER_TABLES = readmem( $WFT_KEY );
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
                        writemem( $WFT_KEY, $WORKER_FILTER_TABLES );
                        do_lock( $WFT_KEY, $WRITE_UNLOCK );
                        update_status(
                            {
                                status             => $WORKER_STATUS_STARTUP,
                                shutdown           => 0,
                                last_lsn           => undef,
                                maintenance_object => $pk_maintenance_object,
                                name               => $ct_name,
                                replace            => 0,
                            },
                            $child_pid
                        );
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
        
        if( $TIMING )
        {
            my $worker_check_delta = tv_interval( $worker_check_start, [ gettimeofday() ] );
            _log( $LOG_LEVEL_DEBUG, "Worker check took $worker_check_delta seconds" );
        }

        ## CHANGE MANAGEMENT
        ##==================

        my $num_in_flight_changes   = 0; # number of changes we're queueing
        my $num_outstanding_changes = 0; # number of changes we've queued previously

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
        $peek_start = [ gettimeofday() ] if( $TIMING );
        $data = &replication_peek(
            $handle,
            $all_filter_tables,
            $wal_level,
            \$last_peeked_lsn
        );

        if( $TIMING )
        {
            my $peek_delta = tv_interval( $peek_start, [ gettimeofday() ] );
            _log( $LOG_LEVEL_DEBUG, "Peek took $peek_delta seconds" );
            $dist_start = [ gettimeofday() ];
        }
        #_log( $LOG_LEVEL_DEBUG, "Peeking done - last $last_peeked_lsn" ) if( $last_peeked_lsn );

        if( $data )
        {
            # iterate over each change in outer loop - one change may go to one or more workers
            _log( $LOG_LEVEL_DEBUG, 'Distributing ' . scalar( @$data ) . ' changes' );
            do_lock( $WFT_KEY, $WRITE_LOCK );
            $WORKER_FILTER_TABLES = readmem( $WFT_KEY );

            foreach my $change( @$data )
            {
                $num_in_flight_changes++;
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

                        $WORKER_FILTER_TABLES->{$pid}->{$ACTIVE_CHANGES_KEY} = $WORKER_FILTER_TABLES->{$pid}->{$ACTIVE_CHANGES_KEY} + 1;
                    }
                }
            }

            writemem( $WFT_KEY, $WORKER_FILTER_TABLES );
            do_lock( $WFT_KEY, $WRITE_UNLOCK );

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

        if( $TIMING )
        {
            my $dist_delta = tv_interval( $dist_start, [ gettimeofday() ] );
            _log( $LOG_LEVEL_DEBUG, "Change distribution took $dist_delta seconds" );
            $idle_check_start = [ gettimeofday() ];
        }

        # Idle WT check
        # Note there is a lot of contention here, possibly consider moving to a different shm segment or maintaining a counter?
        do_lock( $WFT_KEY, $READ_LOCK );
        $WORKER_FILTER_TABLES = readmem( $WFT_KEY );
        foreach my $pid( keys %$WORKER_FILTER_TABLES )
        {
            $num_outstanding_changes += $WORKER_FILTER_TABLES->{$pid}->{$ACTIVE_CHANGES_KEY};
        }
        do_lock( $WFT_KEY, $READ_UNLOCK );
        # Get worker applied LSNs and ack up to the smallest LSN
        if( $TIMING )
        {
            my $idle_check_delta = tv_interval( $idle_check_start, [ gettimeofday() ] );
            _log( $LOG_LEVEL_DEBUG, "Idle WFT check took $idle_check_delta seconds" );
        }

        # Determine each workers last lsn and copy over to worker_lsns
        $worker_last_lsn_start = [ gettimeofday() ] if( $TIMING );
        my $worker_lsns = {};
        do_lock( $WS_KEY, $READ_LOCK );
        $WORKER_STATUSES = readmem( $WS_KEY );
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
        do_lock( $WS_KEY, $READ_UNLOCK );

        if( $TIMING )
        {
            my $worker_last_lsn_delta = tv_interval( $worker_last_lsn_start, [ gettimeofday() ] );
            _log( $LOG_LEVEL_DEBUG, "Worker last LSN check took $worker_last_lsn_delta seconds" );
        }

        $youngest_lsn_proc_start  = [ gettimeofday() ] if( $TIMING );
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

        if( $TIMING )
        {
            my $youngest_lsn_proc_delta = tv_interval( $youngest_lsn_proc_start, [ gettimeofday() ] );
            _log( $LOG_LEVEL_DEBUG, "Youngest LSN processing took $youngest_lsn_proc_delta seconds" );
        }
        ## LSN increment logic
        ##====================

        # max_idle_lsn contains the LSN of the first BEGIN change preceeding any
        # change we're actually concerned about. IFF no changes have happened,
        # we set it to last_peeked_lsn so that we have a consistent LSN to seek
        # to during idle times.

        $lsn_increment_start = [ gettimeofday() ] if( $TIMING );
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

        $seekable_lsn = $max_idle_lsn;

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
        
        if( $TIMING )
        {
            my $lsn_increment_delta = tv_interval( $lsn_increment_start, [ gettimeofday() ] );
            _log( $LOG_LEVEL_DEBUG, "LSN increment logic took $lsn_increment_delta seconds" );
        }
        
        select( undef, undef, undef, $SLEEP_TIMER );
        $first_loop_done = 1;

        ## WORKER HEALTH CHECKS
        ##=====================

        # TODO

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
    my $worker_shm_err = 0;
    $worker_shm_err = 1 unless( get_or_create_shm( $XID_KEY ) );
    $worker_shm_err = 1 unless( get_or_create_shm( $WS_KEY ) );
    $worker_shm_err = 1 unless( get_or_create_shm( $WFT_KEY ) );

    if( $worker_shm_err )
    {
        _log( $LOG_LEVEL_FATAL, "Worker failed to initialize SHM segments" );
    }

    my $count = 0;
    my $lim   = 15;

    until( do_lock( $WS_KEY, $READ_NOWAIT ) )
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

    # we dont use update status as we're competitively transitioning WSKEY from shared read to excl write
    do_lock( $WS_KEY, $READ_UNLOCK );
    update_status( { status => $WORKER_STATUS_RUNNING } );

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
        #unless( $REFRESH_ON_START )
        #{
        #    my $count_check_start = [ gettimeofday() ];
        #    &set_program_name( $handle, "size check: $CACHE_HASH->{name}" );
        #    my $desired_count = get_def_count( $handle, $CACHE_HASH->{definition} );
        #    my $current_count = get_table_count( $handle, $CACHE_HASH->{schema} . '.' . $CACHE_HASH->{name} );
        #    my $count_delta   = tv_interval( $count_check_start, [ gettimeofday() ] );
        #    _log( $LOG_LEVEL_DEBUG, "CT count check took $count_delta seconds" );

        #    if( $current_count != $desired_count )
        #    {
        #        _log( $LOG_LEVEL_INFO, "Out of date cache table detected on worker startup, initiating rebuild." );
        #        tied( $WORKER_STATUSES )->shlock( LOCK_EX );
        #        $WORKER_STATUSES->{$worker_pid}->{replace} = 1;
        #        tied( $WORKER_STATUSES )->shunlock();
        #    }
        #}
        #else
        #{
        #    tied( $WORKER_STATUSES )->shlock( LOCK_EX );
        #    $WORKER_STATUSES->{$worker_pid}->{replace} = 1;
        #    tied( $WORKER_STATUSES )->shunlock();
        #}

        # MAIN LOOP
        while( 1 )
        {
            # Check for commanded exit or replacement
            my $exit    = 0;
            my $replace = 0;

            do_lock( $WS_KEY, $READ_LOCK );
            $WORKER_STATUSES = readmem( $WS_KEY );
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

            do_lock( $WS_KEY, $READ_UNLOCK );

            if( defined $exit && $exit == 1 )
            {
                _log( $LOG_LEVEL_INFO, "PID $worker_pid commanded to shutdown" );
                update_status( { status => $WORKER_STATUS_EXITED } );

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
                update_status( { status => $WORKER_STATUS_REPLACE } );

                my $try_count = 0;
                until( &replace_cache_table( $handle, $pk_maintenance_object ) )
                {
                    _log(
                        $LOG_LEVEL_ERROR,
                        "Replacement of $CACHE_HASH->{name} failed after command to replace, retrying..."
                    );
                    $try_count++;

                    if( $try_count > 3 )
                    {
                        _log( $LOG_LEVEL_FATAL, "Aboring worker after $try_count attempt to rebuild cache table $CACHE_HASH->{name}" );
                    }
                }

                update_status( { status => $WORKER_STATUS_IDLE, replace => 0 } );
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

                    update_status( { status => $WORKER_STATUS_REPLACE } );

                    unless( &replace_cache_table( $handle, $pk_maintenance_object ) )
                    {
                        _log( $LOG_LEVEL_FATAL, "Replacement of $CACHE_HASH->{name} failed" );
                    }

                    update_status( { status => $WORKER_STATUS_IDLE, replace => 0 } );
                }
            }

            # Process changes
            my $changes  = {};
            my $WAL_DATA = {};

            # Quickly dequeue items to hold ex lock for minimum time
            do_lock( $WFT_KEY, $WRITE_LOCK );
            $WORKER_FILTER_TABLES = readmem( $WFT_KEY );
            my $wft_lock_try = 0;
            while( !defined( $WORKER_FILTER_TABLES ) || ref( $WORKER_FILTER_TABLES ) ne 'HASH' )
            {
                do_lock( $WFT_KEY, $WRITE_UNLOCK );
                sleep( 5 );
                do_lock( $WFT_KEY, $WRITE_LOCK );
                $WORKER_FILTER_TABLES = readmem( $WFT_KEY );
                $wft_lock_try++;
                if( $wft_lock_try > 5 )
                {
                    do_lock( $WFT_KEY, $WRITE_UNLOCK );
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
                        $WORKER_FILTER_TABLES->{$worker_pid}->{$ACTIVE_CHANGES_KEY} = $WORKER_FILTER_TABLES->{$worker_pid}->{$ACTIVE_CHANGES_KEY} - 1;
                        push( @{$WAL_DATA->{$filter_table}}, $change );
                    }
                }
            }

            writemem( $WFT_KEY, $WORKER_FILTER_TABLES );
            do_lock( $WFT_KEY, $WRITE_UNLOCK );
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

                update_status( { status => $WORKER_STATUS_QUERY_PARSE } );
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

                my $xid_map_size = 0;
                if( $ENABLE_FAST_DELETE )
                {
                    # search XID_MAP for suitable XID
                    do_lock( $XID_KEY, $READ_LOCK );
                    $XID_MAP = readmem( $XID_KEY );
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

                        $xid_map_size++;
                        $ind++;
                    }

                    # Add our PID to the list of PIDS using this XID/snapshot combo
                    if( defined( $best_candidate ) )
                    {
                        unless( grep( /^$worker_pid$/, @{$XID_MAP->[$best_candidate_ind]->{in_use}} ) )
                        {
                            do_lock( $XID_KEY, $READ_UNLOCK );
                            do_lock( $XID_KEY, $WRITE_LOCK );
                            $XID_MAP = readmem( $XID_KEY );
                            push( @{$XID_MAP->[$best_candidate_ind]->{in_use}}, $worker_pid );
                            $aged_snapshot   = $XID_MAP->[$best_candidate_ind]->{snapshot};
                            $using_xid       = $best_candidate;
                            $using_xid_ind   = $best_candidate_ind;
                            $can_fast_delete = 1;
                            _log( $LOG_LEVEL_DEBUG, 'Found candidate XID for fast delete' );
                            writemem( $XID_KEY, $XID_MAP );
                            do_lock( $XID_KEY, $WRITE_TO_READ );
                        }
                    }
                    else
                    {
                        _log( $LOG_LEVEL_DEBUG, "Could not find candidate XID for fast delete - looking for $youngest_xid. Candidates were:" );
                        foreach my $elem( @$XID_MAP )
                        {
                            _log( $LOG_LEVEL_DEBUG, "$elem->{xid}" );
                        }
                        _log( $LOG_LEVEL_DEBUG, "Change is for:" . Dumper( $changes ) );
                    }

                    do_lock( $XID_KEY, $READ_UNLOCK );
                }

                # Generate temp table containing state of rows relevent to the keys that have changed
                update_status( { status => $WORKER_STATUS_TEMP_TABLE } );
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

                    do_lock( $XID_KEY, $WRITE_LOCK );
                    $XID_MAP = readmem( $XID_KEY );
                    @{$XID_MAP->[$using_xid_ind]->{in_use}} = grep { $_ ne $worker_pid } @{$XID_MAP->[$using_xid_ind]->{in_use}};
                    writemem( $XID_KEY, $XID_MAP );
                    do_lock( $XID_KEY, $WRITE_UNLOCK );
                }

                if( $can_fast_delete && $tried_fast_delete && defined( $aged_handle ) )
                {
                    update_status( { status => $WORKER_STATUS_FAST_DELETE } );
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
                    do_lock( $XID_KEY, $WRITE_LOCK );
                    $XID_MAP = readmem( $XID_KEY );
                    @{$XID_MAP->[$using_xid_ind]->{in_use}} = grep { $_ ne $worker_pid } @{$XID_MAP->[$using_xid_ind]->{in_use}};
                    writemem( $XID_KEY, $XID_MAP );
                    do_lock( $XID_KEY, $WRITE_UNLOCK );
                    _log( $LOG_LEVEL_DEBUG, "Worker released snapshot $aged_snapshot" );
                }

                # this is a hack and shouldn't be here - but for ease on CI / Staging infra we're not going to
                # use slow deletes iff the XID map isn't full
                # For production use we're banking on steady-state operation
                if( !$can_fast_delete && $xid_map_size == $MAX_XID_LENGTH )
                {
                    update_status( { status => $WORKER_STATUS_SLOW_DELETE } );

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

                update_status( { status => $WORKER_STATUS_UPDATE } );
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
                    update_status( { status => $WORKER_STATUS_INSERT } );
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
                update_status( { status => $WORKER_STATUS_IDLE, last_lsn => $max_applied_lsn } );
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

shminit( $$ );
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

my $worker_data = get_worker_list( $handle );

$handle->disconnect();
undef( $handle );

## GLOBAL SHM VARIABLES
my $WORKER_FILTER_TABLES = {};
my $WORKER_STATUSES      = {};
my $XID_MAP              = [];
my $shm_init_err         = 0;
unless( get_or_create_shm( $WFT_KEY ) )
{
    do_lock( $WFT_KEY, $WRITE_LOCK );
    unless( writemem( $WFT_KEY, $WORKER_FILTER_TABLES ) )
    {
        warn "Failed to initialize worker filter tables\n";
        $shm_init_err = 1;
    }
    do_lock( $WFT_KEY, $WRITE_UNLOCK );
}

unless( get_or_create_shm( $WS_KEY ) )
{
    do_lock( $WS_KEY, $WRITE_LOCK );
    unless( write_mem( $WS_KEY, $WORKER_STATUSES ) )
    {
        warn "Failed to initialize worker statuses\n";
        $shm_init_err = 1;
    }
    do_lock( $WS_KEY, $WRITE_UNLOCK );
}

unless( get_or_create_shm( $XID_KEY ) )
{
    do_lock( $XID_KEY, $WRITE_LOCK );
    unless( writemem( $XID_KEY, $XID_MAP ) )
    {
        warn "Failed to write empty xid map\n";
        $shm_init_err = 1;
    }
    do_lock( $XID_KEY, $WRITE_UNLOCK );
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
    do_lock( $WS_KEY, $WRITE_LOCK );
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
            do_lock( $WFT_KEY, $WRITE_LOCK );

            $WORKER_FILTER_TABLES->{$child_pid}->{$ACTIVE_CHANGES_KEY} = 0;
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

            writemem( $WFT_KEY, $WORKER_FILTER_TABLES );
            do_lock( $WFT_KEY, $WRITE_UNLOCK );

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
    writemem( $WS_KEY, $WORKER_STATUSES );
    do_lock( $WS_KEY, $WRITE_UNLOCK );
}

&set_program_name( undef, "parent process" );
parent_loop( $worker_mapping );
_log( $LOG_LEVEL_ERROR, "Parent exited main loop" );
do_shm_cleanup();
exit( 0 );
