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

Readonly my $REFRESH_ON_START   => 0;
# Expressed in MS:
Readonly my $XID_IDLE_TIMEOUT   => ( 1000 * $XID_MAP_SPREAD ) + ( 1000 * 30 ); # XID_MAP_SPREAD + 30 seconds
Readonly my $SLEEP_TIMER        => 0.25; # seconds for main loop
Readonly my $XID_START_SIZE     => 768;
Readonly my $WS_KEY             => 17783313;
Readonly my $XID_KEY            => 17783314;
Readonly my $TIMING             => 0;

our $OUTPUT_AUTOFLUSH = 1;
our $|                = 1;

## GLOBAL VARIABLES
$PARENT_PID  = $PROCESS_ID;
$LOG_FILE    = '';
$LOG_FH      = undef;
$DAEMONIZE   = 0;

sub update_status($;$)
{
    my( $info_hash, $override_pid ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR, optional => 1 },
    );

    my $success = 0;
    my $target_pid = $PROCESS_ID;

    if( defined $override_pid && $PARENT_PID == $PROCESS_ID )
    {
        $target_pid = $override_pid
    }

    return 0 if( !defined( $info_hash ) );

    do_lock( $WS_KEY, $WRITE_LOCK );

    my $WORKER_STATUSES = readmem( $WS_KEY );

    if( defined( $WORKER_STATUSES ) )
    {
        my $targ = $WORKER_STATUSES->{$target_pid};

        foreach my $key( keys( %$info_hash ) )
        {
            next unless( defined( $info_hash->{$key} ) );
            $targ->{$key} = $info_hash->{$key};
        }

        if( $PROCESS_ID != $PARENT_PID )
        {
            $targ->{backend_pid} = $BACKEND_PID;
        }

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

sub _handle_sighup()
{
    # dummy for now
    return;
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

sub terminate_child_conns()
{
    my $handle = db_connect();
    my $WS_DATA;
    do_lock( $WS_KEY, $READ_LOCK );
    $WS_DATA = readmem( $WS_KEY );
    do_lock( $WS_KEY, $READ_UNLOCK );
    my $term_q = <<END_SQL;
    SELECT pg_terminate_backend( pid )
      FROM pg_stat_activity
     WHERE pid = ?
END_SQL
    my $term_sth = $handle->prepare( $term_q );

    return unless( $term_sth );
    foreach my $worker_pid( keys %$WS_DATA )
    {
        my $backend = $WS_DATA->{$worker_pid}->{backend_pid};

        if( $backend )
        {
            $term_sth->bind_param( 1, $backend );
            $term_sth->execute();
        }
    }
    return;
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
        #this is crucial to prevent running out of shm after crashes / term
        terminate_child_conns();
        unless( defined( $SKIP_SHM_CLEANUP ) && $SKIP_SHM_CLEANUP )
        {
            &do_shm_cleanup();
        }
    }

    if( @_ )
    {
        if( $file && $line )
        {
            CORE::die( $package, $file, $line );
        }
        else
        {
            CORE::die( @_ );
        }
    }

    exit( 0 );
}

$SIG{HUP} = \&_handle_sighup;
$SIG{INT} = \&_terminate_sigint;
$SIG{__DIE__} = \&_terminate;


sub shm_pre_cleanup()
{
    do_cleanup_key( $WS_KEY );
    do_cleanup_key( $XID_KEY );
    return 1;
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

            $WORKER_DATA->{$pk_maintenance_object}->{hash} = $worker_entry->{hash};
            $WORKER_DATA->{$pk_maintenance_object}->{name} = $worker_entry->{name};
        }
    }
    else
    {
        # no workers yet
        return undef;
    }

    return $WORKER_DATA;
}

sub check_for_new_cache_tables($$)
{
    my( $current_workers, $new_workers ) = validate_pos(
        @_,
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
            if(
                  $current_workers->{$pk_mo}->{hash}
               eq $new_workers->{$pk_mo}->{hash}
              )
            {
                next;
            }

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
    $$new_handle->do( "SET idle_in_transaction_session_timeout = $XID_IDLE_TIMEOUT" );
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
        _log(
            $LOG_LEVEL_FATAL,
            'Failed to secure advisory lock in parent process'
        );
    }

    my $local_xid_map = {};

    unless( $handle )
    {
        _log( $LOG_LEVEL_FATAL, 'Failed to connect to database' );
    }

    unless( check_extension_running( $handle ) )
    {
        _log(
            $LOG_LEVEL_FATAL,
            'Failed to obtain lock on database - another '
          . "$EXTENSION_NAME instance seems to be running"
         );
    }

    my $WORKER_DATA            = {};
    $WORKER_DATA               = populate_worker_data( $handle, $WORKER_DATA );
    my $wal_level              = 'M';
    my $dispatched_changes     = {};
    my $last_xid_create;
    my $last_worker_count      = 0;
    # Timing vars
    my $xid_start;
    my $worker_check_start;

    while( 1 )
    {
        ## XID CHAIN MANAGEMENT
        ##=====================
        $xid_start = [ gettimeofday() ] if( $TIMING );
        if(
            $ENABLE_FAST_DELETE
         && (
                !defined( $last_xid_create )
             || tv_interval( $last_xid_create, [ gettimeofday() ] ) >= $XID_MAP_SPREAD
            )
          )
        {
            do_lock( $XID_KEY, $WRITE_LOCK );
            $XID_MAP = readmem( $XID_KEY );
            $last_xid_create = [ gettimeofday() ];

            # Safety check - remove XIDs that have been hanging around for a long time.
            my $max_allowed_age = ( $MAX_XID_LENGTH * $XID_MAP_SPREAD ) + $XID_MAP_SPREAD;
            $max_allowed_age    = ( $XID_IDLE_TIMEOUT / 1000 ) if( ( $XID_IDLE_TIMEOUT / 1000 ) > $max_allowed_age );

            foreach my $aged_xid( keys %$local_xid_map )
            {
                if( tv_interval( $local_xid_map->{$aged_xid}->{created}, [ gettimeofday() ] ) >= $max_allowed_age )
                {
                    _log( $LOG_LEVEL_WARNING, "Old XID map member found older than $max_allowed_age seconds. Pruning" );
                    my $prune_handle = $local_xid_map->{$aged_xid}->{handle};
                    if( $prune_handle && $prune_handle->ping() > 0 && $prune_handle->pg_ping() > 0 )
                    {
                        $prune_handle->disconnect();
                        undef( $prune_handle );
                    }

                    delete( $local_xid_map->{$aged_xid} );
                    my $found = 0;
                    my $ind = 0;
                    foreach my $xid_map_ind( @$XID_MAP )
                    {
                        if( defined( $xid_map_ind ) && $xid_map_ind->{xid} == $aged_xid )
                        {
                            $found = 1;
                            splice( @$XID_MAP, $ind, 1 );
                            last;
                        }
                        $ind++;
                    }

                    if( !$found )
                    {
                        _log( $LOG_LEVEL_ERROR, "Mismatch between XID map and local copy, could not locate xid $aged_xid" );
                    }

                    writemem( $XID_KEY, $XID_MAP );
                }
                else
                {
                    my $aged_handle = $local_xid_map->{$aged_xid}->{handle};
                    if( !defined $aged_handle || $aged_handle->ping() <= 0 || $aged_handle->pg_ping() <= 0 )
                    {
                        _log( $LOG_LEVEL_DEBUG, "Found local XID entry with bad handle for $aged_xid" );
                        delete( $local_xid_map->{$aged_xid} );
                        my $ind = 0;
                        my $found = 1;
                        foreach my $xid_map_ind( @$XID_MAP )
                        {
                            $found = 1;
                            splice( @$XID_MAP, $ind, 1 );
                            last;
                        }
                        $ind++;

                        if( !$found )
                        {
                            _log(
                                $LOG_LEVEL_ERROR,
                                "Mismatch between XID map and local copy. Terminated handle for $aged_xid could not be found"
                            );
                        }

                        writemem( $XID_KEY, $XID_MAP );
                    }
                }
            }

            if(
                  !defined( $XID_MAP )
               || ref( $XID_MAP ) ne 'ARRAY'
               || scalar( @$XID_MAP ) < $MAX_XID_LENGTH
              )
            {
                my $new_handle;
                my $new_xid;
                my $new_snapshot;

                if( !new_xid_placeholder( \$new_handle, \$new_xid, \$new_snapshot ) )
                {
                    _log( $LOG_LEVEL_DEBUG, "Could not generate new XID chain member" );
                    do_lock( $XID_KEY, $WRITE_UNLOCK );
                    next;
                }

                $XID_MAP = readmem( $XID_KEY );
                $local_xid_map->{$new_xid} = { handle => $new_handle, created => [ gettimeofday() ] };
                push(
                    @$XID_MAP,
                    {
                        xid      => $new_xid,
                        snapshot => $new_snapshot,
                        in_use   => []
                    }
                );

                writemem( $XID_KEY, $XID_MAP );
            }
            else
            {
                # Replace oldest chain member in terms of XID
                my $candidate_replace;
                my $candidate_replace_ind;
                my $replace_ind     = 0;

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
                    # Safety check - leave no dangling XIDs, perform a time-based check

                    do_lock( $XID_KEY, $WRITE_UNLOCK );
                    next;
                }

                $XID_MAP = readmem( $XID_KEY );
                my $replace_handle = $local_xid_map->{$candidate_replace}->{handle};

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
                    _log( $LOG_LEVEL_ERROR, "Failed to generate replacement xid member" );
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
                $local_xid_map->{$new_xid} = { handle => $replace_handle, created => [ gettimeofday() ] };
            }

            do_lock( $XID_KEY, $WRITE_UNLOCK );
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
                    my $filter_tables       = $worker_data->{filter_tables};
                    my $ct_name             = $worker_data->{name};
                    my $child_pid           = fork();

                    if( defined( $child_pid ) and $child_pid == 0 )
                    {
                        &worker_entrypoint(
                            $filter_tables,
                            $pk_maintenance_object
                        );
                        exit( 0 );
                    }
                    elsif( defined( $child_pid ) and $child_pid > 0 )
                    {
                        $worker_mapping->{$pk_maintenance_object} = $child_pid;
                        update_status(
                            {
                                status             => $WORKER_STATUS_STARTUP,
                                shutdown           => 0,
                                maintenance_object => $pk_maintenance_object,
                                name               => $ct_name,
                                replace            => 0,
                                backend_pid        => 0,
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

    # TODO, we should filter the relcache based on our worker's filter tables to save RAM
    $cache_hash->{relcache}      = &get_relcache( $handle );
    $cache_hash->{table_mapping} = {};
    $cache_hash->{parse_tree} = &find_table_aliases(
        $handle,
        $cache_hash->{relcache},
        $cache_hash->{definition},
        $cache_hash->{table_mapping}
    );

    # This is where we fixup the trigger'd tables in __pgctblmgr_repl_slot. The
    # initial parse will likely be incorrect if the query is complex. We've now
    # had the chance for a much more accurate parse and can update the filters
    # to be 100% correct.
    my $binds = $cache_hash->{table_mapping}->{BINDS};
    my $filter_tables_hash = {};
    my $new_filter_tables  = [];
    my $ft_needs_fixup     = 0; # onshot for said update

    foreach my $bind_position( keys %$binds )
    {
        foreach my $bind_schema( keys %{$binds->{$bind_position}->{rels}} )
        {
            foreach my $bind_alias( keys %{$binds->{$bind_position}->{rels}->{$bind_schema}} )
            {
                 foreach my $bind_table( keys %{$binds->{$bind_position}->{rels}->{$bind_schema}->{$bind_alias}} )
                 {
                     # Note - no need to attempt to discover relations in the outer-join bind infos, if present.
                     # This will not lead to undiscovered tables but is more for bind planning.
                     next unless( $cache_hash->{relcache}->{rels}->{$bind_schema}->{$bind_table} );
                     next if defined( $filter_tables_hash->{$bind_schema}->{$bind_table} );
                     $filter_tables_hash->{$bind_schema}->{$bind_table} = 1;
                     my $rel = "${bind_schema}.${bind_table}";
                     push( @$new_filter_tables, $rel );
                     unless( grep /^$rel$/, @$filter_tables )
                     {
                        $ft_needs_fixup = 1;
                     }
                 }
            }
        }
    }

    if( $ft_needs_fixup )
    {
        _log( $LOG_LEVEL_INFO, "Detected new filter tables update for __pgctblmgr_repl_slot!" );
        @$filter_tables = @$new_filter_tables;
        unless( update_filter_tables( $handle, $new_filter_tables, $pk_maintenance_object ) )
        {
            _log(
                $LOG_LEVEL_ERROR,
                'Failed to update filter tables (and install triggers) '
              . "for maintenance object $pk_maintenance_object. This is likely "
              . 'due to deadlocking. The service WILL miss some updates. Please '
              . 'restart the service at a time when your cluster is less busy.'
            );
        }
    }

    # Optimization to cache relevant typmods to this worker
    $cache_hash->{relcache}->{typmods} = &populate_typmods( $handle, $filter_tables );

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

sub release_all_xid()
{
    my $XID_MAP = [];
    my $ind = 0;
    do_lock( $XID_KEY, $WRITE_LOCK );
    $XID_MAP = readmem( $XID_KEY );
    foreach my $elem( @$XID_MAP )
    {
        @{$XID_MAP->[$ind]->{in_use}} = grep { $_ ne $PROCESS_ID } @{$XID_MAP->[$ind]->{in_use}};
        $ind++;
    }
    writemem( $XID_KEY, $XID_MAP );
    do_lock( $XID_KEY, $WRITE_UNLOCK );
    return;
}

## WORKER
sub worker_entrypoint($$)
{
    my(
        $filter_tables,
        $pk_maintenance_object
      ) = validate_pos(
        @_,
        { type => ARRAYREF },
        { type => SCALAR },
    );

    &set_program_name( undef, "worker startup" );
    my $CACHE_HASH           = {};
    my $WORKER_STATUSES      = {};
    my $XID_MAP              = [];
    my $backend_pid          = 0;
    my $worker_pid           = $PROCESS_ID;
    my $worker_shm_err       = 0;
    $LOCAL_PK_MAINTENANCE_OBJECT = $pk_maintenance_object;

    if( $ENABLE_FAST_DELETE )
    {
        $worker_shm_err = 1 unless( get_or_create_shm( $XID_KEY ) );
    }

    $worker_shm_err = 1 unless( get_or_create_shm( $WS_KEY ) );

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

    $MAINTENANCE_CHANNEL = $CACHE_HASH->{maintenance_channel};
    # Table mapping and parse tree are (relatively) static and only change if
    # our query changes underneath us
    # TODO: Add detection and correction for the above
    _log( $LOG_LEVEL_DEBUG, "Worker $worker_pid running" );
    &set_program_name( $handle, "idle $CACHE_HASH->{name}" );
    do_listen( $handle );
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

                my $dct = try_query( $handle, "DROP TABLE IF EXISTS $CACHE_HASH->{schema}.$CACHE_HASH->{name}" );

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
                update_status( { status => $WORKER_STATUS_REPLACE, replace => 0 } );
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

                $replace = 0;
                update_status( { status => $WORKER_STATUS_IDLE, replace => 0 } );
            }

            # check to see if definition has changed
            my $test_hash = &get_ct_digest( $handle, $pk_maintenance_object );
            if( !defined $test_hash )
            {
                _log(
                    $LOG_LEVEL_ERROR,
                    'Failed to check maintenance object '
                  . 'for definition change (SHA256)'
                );
                # CT may have been removed, lets exit
                do_lock( $WS_KEY, $WRITE_LOCK );
                $WORKER_STATUSES = readmem( $WS_KEY );
                $WORKER_STATUSES->{$worker_pid}->{shutdown} = 1;
                writemem( $WS_KEY, $WORKER_STATUSES );
                do_lock( $WS_KEY, $WRITE_UNLOCK );
                next;
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

            #TODO: Replace dequeue w/ listen here. WAL_DATA is structured as $WAL_DATA->{filter_table}->[ changes ]
            # We block here waiting on data

            # Note: There's a strange behavior in DBI's control methods here for pg_notifies.
            # If notifications happen too closely, they can sometime pile up into the next notification
            # without triggering the file_handle->can_read logic. I've opted to switch this out to a polling-based
            # timeout setup, where if an update occurs we pass this block without waiting for timeout, but if we're in
            # a period of heavy updates, there's not a chance we won't see the data until the next notification.

            # I did a crap job of explaining the above, so the behavior can be reproduced by spamming something like
            # UPDATE <table> SET <some_attribute> WHERE <pk> IN( SELECT <pk> FROM <table> ORDER BY random() LIMIT 1 );
            # with the $SELECTOR->can_read uncommented and the call to IO::SELECT commented
            #$SELECTOR->can_read;
            IO::Select::select( $SELECTOR, undef, undef, 2.5 );

            my $change_count = 0;
            while( my $notifications = $handle->func( 'pg_notifies' ) )
            {
                my $notify_channel = $notifications->[0];
                my $notify_pid     = $notifications->[1];
                my $notify_data    = $notifications->[2];

                next unless( defined( $notify_data ) && length( $notify_data ) > 0 );
                my $data;

                eval { $data = decode_json( $notify_data ); };

                if( $@ || !defined( $data ) )
                {
                    _log( $LOG_LEVEL_INFO, "Got spurious or corrupt data '$notify_data' from PID $notify_pid" );
                    next;
                }

                push( @{$WAL_DATA->{$data->{data}->{table_name}}}, $data );
                $change_count++;
            }

            _log( $LOG_LEVEL_DEBUG, "Got $change_count change(s) on $LOCAL_PK_MAINTENANCE_OBJECT" ) if( $change_count );
            my $oldest_xid;

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
                        if( !defined $oldest_xid || $change->{xid} < $oldest_xid )
                        {
                            $oldest_xid = $change->{xid};
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
                    }
                }

                # Digest changes for this filter table
            }

            my $aged_handle;
            my $aged_snapshot;

            if( scalar( keys %$changes ) > 0 )
            {
                my $map = {
                    handle        => $handle,
                    query_data    => $CACHE_HASH->{parse_tree},
                    table_mapping => $CACHE_HASH->{table_mapping},
                    definition    => $CACHE_HASH->{definition},
                    relcache      => $CACHE_HASH->{relcache},
                    filters       => $changes,
                };

                my $where_expressions = generate_where_expressions( $map );

                if( scalar( keys %$where_expressions ) == 0 )
                {
                    _log( $LOG_LEVEL_ERROR, "No bind positions generated for $CACHE_HASH->{name} with the following changes:" );
                    _log( $LOG_LEVEL_ERROR, Dumper( $changes ) );
                }

                # Note that we iterate over the different bind positions so that we do not accidentally logically ANDing
                # two disparate changes together:
                #  Example Query:
                #
                #  WITH tt_foo AS
                #  (
                #      SELECT bar
                #        FROM tb_bar
                #  )
                #      SELECT a.baz
                #        FROM tb_baz a
                #  INNER JOIN tt_foo b
                #          ON b.bar = a.bar
                #
                # with change { tb_baz => { baz => [ 1 ] }, tb_bar => { bar => [ 5 ] } }
                # If we naively bound all filters, we'd end up with
                #
                # WITH tt_Foo AS
                # (
                #     SELECT bar
                #       FROM tb_bar
                #      WHERE tb_bar.bar = '5'::INT
                # )
                #     SELECT a.baz,
                #       FROM tb_baz a
                # INNER JOIN tt_foo b
                #         ON b.bar = a.bar
                #      WHERE a.baz = '1'::INT
                my $filtered = 0;
                foreach my $bind_position( keys %$where_expressions )
                {
                    if( $CONSERVATIVE_TABLE_FILTERING )
                    {
                        $map->{where_expressions}->{$bind_position} = $where_expressions->{$bind_position};
                    }
                    else
                    {
                        last if( $filtered );
                        $map->{where_expressions} = $where_expressions;
                        $filtered = 1;
                    }

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

                    my $query = &apply_filters( $map );

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
RETRY_XID:
                    if( $ENABLE_FAST_DELETE )
                    {
                        # search XID_MAP for suitable XID
                        do_lock( $XID_KEY, $READ_LOCK );
                        $XID_MAP = readmem( $XID_KEY );
                        my $best_candidate;
                        my $best_candidate_ind;
                        my $ind = 0;
                        do_lock( $XID_KEY, $READ_UNLOCK );

                        foreach my $elem( @$XID_MAP )
                        {
                            if(
                                defined( $elem->{xid} )
                             && $elem->{xid} <= $oldest_xid
                             && (
                                    !defined( $best_candidate )
                                 || $elem->{xid} > $best_candidate )
                               )
                            {
                                $best_candidate     = $XID_MAP->[$ind]->{xid};
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
                                do_lock( $XID_KEY, $WRITE_LOCK );
                                $XID_MAP = readmem( $XID_KEY );
                                if( $XID_MAP->[$best_candidate_ind]->{xid} != $best_candidate )
                                {
                                    do_lock( $XID_KEY, $WRITE_UNLOCK );
                                    _log( $LOG_LEVEL_ERROR, "XID map changed during read lock promotion, retrying" );
                                    goto RETRY_XID;
                                }
                                push( @{$XID_MAP->[$best_candidate_ind]->{in_use}}, $worker_pid );
                                $aged_snapshot   = $XID_MAP->[$best_candidate_ind]->{snapshot};
                                $using_xid       = $best_candidate;
                                $using_xid_ind   = $best_candidate_ind;
                                $can_fast_delete = 1;
                                _log( $LOG_LEVEL_DEBUG, 'Found candidate XID for fast delete' );
                                writemem( $XID_KEY, $XID_MAP );
                                do_lock( $XID_KEY, $WRITE_UNLOCK );
                            }
                        }
                        else
                        {
                            _log(
                                $LOG_LEVEL_DEBUG,
                                'Could not find candidate XID for fast delete '
                              . "- looking for $oldest_xid. Candidates were:"
                            );
                            foreach my $elem( @$XID_MAP )
                            {
                                _log( $LOG_LEVEL_DEBUG, "$elem->{xid}" ) if( defined( $elem && $elem->{xid} ) );
                            }
                            _log( $LOG_LEVEL_DEBUG, "Change is for:" . Dumper( $changes ) );
                        }
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
                        $aged_handle = &db_connect( undef, 1 );

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


                        unless( $aged_handle->do( "SET idle_in_transaction_session_timeout = $XID_IDLE_TIMEOUT" ) )
                        {
                            $can_fast_delete = 0;
                            _log( $LOG_LEVEL_DEBUG, 'Fast delete failed - could not set idle session timeout' );
                            goto FD_FALLBACK;
                        }
                        _log(
                            $LOG_LEVEL_DEBUG,
                            "Established aged handle at snapshot $aged_snapshot "
                          . "with XID $using_xid, target $oldest_xid"
                        );
                    }

FD_FALLBACK:
                    if( !$can_fast_delete && $tried_fast_delete )
                    {
                        $SKIP_LOCK_CHECK = 0;
                        if( defined $aged_handle && $aged_handle->ping() > 0 )
                        {
                            $aged_handle->do( 'ROLLBACK' );
                            $aged_handle->disconnect();
                            undef( $aged_handle );
                        }
                        # OLD XID RELEASE
                    }

                    my $aged_data_type_hash = {};
                    my $where_filters = [];
                    my $aged_temp_table;

                    if( $can_fast_delete && $tried_fast_delete && defined( $aged_handle ) )
                    {
                        $SKIP_LOCK_CHECK = 1;
                        update_status( { status => $WORKER_STATUS_FAST_DELETE } );
                        &set_program_name( $handle, "fast delete $CACHE_HASH->{name}" );
                        &set_program_name( $aged_handle, "fast delete $CACHE_HASH->{name}" );
                        _log( $LOG_LEVEL_DEBUG, "Using fast delete" );
                        # Create temp table in aged handle && perform fast delete
                        $aged_temp_table = generate_temp_table( $aged_handle, $query, $CACHE_HASH );

                        unless( $aged_temp_table )
                        {
                            _log( $LOG_LEVEL_ERROR, "Fast delete failed - could not create aged temp table" );
                            $can_fast_delete   = 0;
                            $tried_fast_delete = 1;
                            $aged_handle->do( 'ROLLBACK' );
                            $aged_handle->disconnect();
                            $SKIP_LOCK_CHECK = 0;
                            goto FD_FALLBACK;
                        }

                        my $fast_delete_start = [ gettimeofday() ];
                        unless(
                            &fast_forward_aged_data(
                                $aged_handle,
                                $handle,
                                $aged_temp_table,
                                $temp_table,
                                $aged_data_type_hash,
                                $where_filters,
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
                            $SKIP_LOCK_CHECK = 0;
                        }

                        $fast_delete_time = tv_interval( $fast_delete_start, [ gettimeofday() ] );
                        _log( $LOG_LEVEL_DEBUG, "Fast delete took $fast_delete_time seconds" );
                        #OLD XID RELEASE
                        _log( $LOG_LEVEL_DEBUG, "Worker released snapshot $aged_snapshot" );
                    }

                    if( defined( $using_xid ) )
                    {
                        release_all_xid();
                        if( $aged_handle && $aged_handle->ping() > 0 )
                        {
                            $aged_handle->disconnect();
                            undef( $aged_handle );
                        }
                    }

                    if( $can_fast_delete && $tried_fast_delete )
                    {
                        $SKIP_LOCK_CHECK = 1;
                        unless(
                            &generate_aged_delete_statement(
                                $handle,
                                $aged_temp_table,
                                $temp_table,
                                $aged_data_type_hash,
                                $where_filters,
                                $CACHE_HASH
                            )
                              )
                        {
                            _log( $LOG_LEVEL_ERROR, "Fast delete failed, falling back to slow delete" );
                            $can_fast_delete   = 0;
                            $tried_fast_delete = 1;
                        }
                    }
                    # this is a hack and shouldn't be here - but for ease on CI / Staging infra we're not going to
                    # use slow deletes iff the XID map isn't full
                    # For production use we're banking on steady-state operation
                    if( !$can_fast_delete && ( $xid_map_size > 0 || !$ENABLE_FAST_DELETE ) )
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

                    last unless( $CONSERVATIVE_TABLE_FILTERING );
                }

                &set_program_name( $handle, "idle $CACHE_HASH->{name}" );
                update_status( { status => $WORKER_STATUS_IDLE } );
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
my $pg_conn_string = "dbi:Pg:dbname=postgres;host=${host};port=${port}";
$CONNECTION_MAP->{connection_string}    = $conn_string;
$CONNECTION_MAP->{pg_connection_string} = $pg_conn_string;
$CONNECTION_MAP->{user_name}            = $user;
$CONNECTION_MAP->{dbname}               = $dbname;

daemonize() unless( defined( $DAEMONIZE ) && $DAEMONIZE );

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

unless( shm_pre_cleanup() )
{
    _log( $LOG_LEVEL_ERROR, "Failed to prune shared memory on startup" );
}
shminit( $$ );

my $worker_data = get_worker_list( $handle );

$handle->disconnect();
undef( $handle );

## GLOBAL SHM VARIABLES
my $WORKER_STATUSES      = {};
my $XID_MAP              = [];
my $shm_init_err         = 0;

unless( get_or_create_shm( $WS_KEY ) )
{
    do_lock( $WS_KEY, $WRITE_LOCK );
    unless( writemem( $WS_KEY, $WORKER_STATUSES ) )
    {
        warn "Failed to initialize worker statuses\n";
        $shm_init_err = 1;
    }
    do_lock( $WS_KEY, $WRITE_UNLOCK );
}

if( $ENABLE_FAST_DELETE )
{
    unless( get_or_create_shm( $XID_KEY, $XID_START_SIZE ) )
    {
        do_lock( $XID_KEY, $WRITE_LOCK );
        unless( writemem( $XID_KEY, $XID_MAP ) )
        {
            warn "Failed to write empty xid map\n";
            $shm_init_err = 1;
        }
        do_lock( $XID_KEY, $WRITE_UNLOCK );
    }
}

# Wipe and start fresh if we crashed previously
my $worker_mapping = {};
$WORKER_STATUSES = {};
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
        my $pk_maintenance_object = $worker_entry->{maintenance_object};
        my $ct_name               = $worker_entry->{name};
        my $child_pid             = fork();

        if( defined( $child_pid ) and $child_pid == 0 )
        {
            &worker_entrypoint(
                $filter_tables,
                $pk_maintenance_object
            );
            exit( 0 );
        }
        elsif( defined( $child_pid ) and $child_pid > 0 )
        {
            $worker_mapping->{$pk_maintenance_object} = $child_pid;
            $WORKER_STATUSES->{$child_pid}->{status}             = $WORKER_STATUS_STARTUP;
            $WORKER_STATUSES->{$child_pid}->{shutdown}           = 0;
            $WORKER_STATUSES->{$child_pid}->{replace}            = 0;
            $WORKER_STATUSES->{$child_pid}->{maintenance_object} = $pk_maintenance_object;
            $WORKER_STATUSES->{$child_pid}->{name}               = $ct_name;
            $WORKER_STATUSES->{$child_pid}->{backend_pid}        = 0;
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
if( defined $SKIP_SHM_CLEANUP && $SKIP_SHM_CLEANUP )
{
    do_shm_cleanup();
}
exit( 0 );
