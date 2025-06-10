#!/usr/bin/perl

use strict;
use warnings;

use DBI;
use Params::Validate qw( :all );
use Carp;
use Readonly;
use English qw( -no_match_vars );

use Getopt::Std;
use Time::HiRes qw( gettimeofday tv_interval );
use IO::Socket;
use IO::Select;
use Net::Ping;
use Errno;
use Data::Dumper;
use Socket;
require 'sys/ioctl.ph';

use FindBin;
use lib "$FindBin::Bin/lib";

use Util;
use ConfigManager;
use DB;

our $OUTPUT_AUTOFLUSH = 1;
our $|                = 1;

my $got_sighup      = 0;
my $got_alarm       = 0;
my $LOG_FILE        = '';
my $LOG_FH          = undef;
my $handle;
my $DAEOMONIZE      = 0;
my $PARENT_PID      = $PROCESS_ID;
my $XID_SERVICE_PORT;
my $MAX_RESERVATION_TIME;
my $CACHE_AGE_DEFAULT = 5;

# DB lib globals
our $CONFIG_MANAGER;
our $CONNECTION_MAP              = {};
our $SKIP_LOCK_CHECK             = 1;
our $LOCAL_PK_MAINTENANCE_OBJECT = 0;

# There are some commented out sections that should stay in-place
# related to the XID service in DB.pm and parts of this file.
# They relate to global snapshotting and lock detection.
# Once PostgreSQL supports importing snapshots across databases.
Readonly my $XID_REGISTER_LOCATION => <<"END_SQL";
INSERT INTO ${SCHEMA_NAME}.tb_location
            (
                location,
                hostname,
                port,
                namespace
            )
     VALUES
            (
                0,
                ?,
                ?,
                'XID_SERVICE'
            )
ON CONFLICT ( location )
         DO UPDATE
        SET hostname  = EXCLUDED.hostname,
            port      = EXCLUDED.port,
            namespace = EXCLUDED.namespace;
END_SQL

sub get_interface_address($)
{
    my( $interface ) = validate_pos(
        @_,
        { type => SCALAR },
    );

    my $socket;

    socket(
        $socket,
        PF_INET,
        SOCK_STREAM,
        ( getprotobyname('tcp') )[2]
    ) || return undef;

    my $buf = pack( 'a256', $interface );

	if( ioctl( $socket, SIOCGIFADDR(), $buf ) )
	{
        my @address = unpack( 'x20 C4', $buf );

        if( scalar( @address ) > 0 )
        {
		    return join( '.', @address );
        }
	}

    return undef;
}

sub determine_local_host()
{
    # Determine a reachable host for this service. We'll try to use localhost first
    # from the perspective of the regular pg_ctblmgr service, but in the case that
    # this service is not running locally, we find a backup address
    if( defined( $CONFIG_MANAGER ) )
    {
        my $potential_host = $CONFIG_MANAGER->get_config_value( 'xid_service_host' );
        if( defined $potential_host )
        {
            return $potential_host;
        }
    }

    my @interfaces;
    # fetch using ip addr
    my $result = `ip addr show | grep "^[0-9]:" | grep -e "state UP" | cut -d ':' -f2`;
    chomp( $result );

    my @prepruned = split( "\n", $result );

    foreach my $if( @prepruned )
    {
        $if =~ s/^\s+//;
        $if =~ s/\s+$//;
        push( @interfaces, $if );
    }

    if( scalar( @interfaces ) == 0 )
    {
        # try with ifconfig
        $result = `ifconfig | grep "^[[:alnum:]]" | grep -e "UP" | cut -d ':' -f1`;
        chomp( $result );

        @prepruned = split( "\n", $result );

        foreach my $interface( @prepruned )
        {
            $interface =~ s/^\s+//;
            $interface =~ s/\s+$//;
            next if( $interface eq 'lo' );
            push( @interfaces, $interface );
        }
    }

    return if( scalar( @interfaces ) == 0 );
    my @routable_ips;

    my $p = Net::Ping->new( 'syn' );

    return unless( $p );
    foreach my $interface( @interfaces )
    {
        my $interface_ip = get_interface_address( $interface );
        next unless( $interface_ip );
        next unless( $p );
        $p->bind( $interface_ip );
        if( $p->ping( '8.8.8.8', 2 ) )
        {
            push( @routable_ips, $interface_ip );
        }
        elsif( $p->ping( '1.1.1.1', 2 ) )
        {
            push( @routable_ips, $interface_ip );
        }
    }

    return if( scalar( @routable_ips ) == 0 );

    return $routable_ips[0];
}

sub xid_service_register()
{
    return 0 unless( xid_service_lock( $handle ) );
    # Determine routable ip
    my $host = determine_local_host();
    $host    = 'localhost' if( !defined( $host ) );

    my $reg_sth = $handle->prepare( $XID_REGISTER_LOCATION );

    return 0 unless( defined( $reg_sth ) );
    $reg_sth->bind_param( 1, $host );
    $reg_sth->bind_param( 2, $XID_SERVICE_PORT );

    return 0 unless( $reg_sth->execute() );
    return 1;
}

sub alarm_handler()
{
    $got_alarm = 1;
    return;
}

sub start_alarm()
{
    $got_alarm = 0;
    return alarm( 1 );
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

sub new_xid_placeholder($$$$)
{
    my( $new_handle, $new_xid, $new_snapshot, $xid_idle_timeout ) = validate_pos(
        @_,
        { type => SCALARREF },
        { type => SCALARREF },
        { type => SCALARREF },
        { type => SCALAR },
    );

    if( !defined( $$new_handle ) )
    {
        $$new_handle = &db_connect( $$new_handle );
        return 0 unless( $$new_handle );
    }

    $$new_handle->do( "SET idle_session_timeout = ?", undef, $xid_idle_timeout );
    $$new_handle->do( "SET idle_in_transaction_session_timeout = ?", undef, $xid_idle_timeout );
    $$new_handle->do( 'BEGIN' );

    my $sth = $$new_handle->prepare( 'SELECT txid_current() AS xid' );

    unless( $sth )
    {
        _rollback_and_disconnect( $$new_handle );
        return 0;
    }

    unless( $sth->execute() )
    {
        $sth->finish();
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
            $sth->finish();
            _rollback_and_disconnect( $$new_handle );
            return 0;
        }

        $row = $sth->fetchrow_hashref();
        $$new_snapshot = $row->{snapshot};
        $sth->finish();
        $$new_handle->do(
            "SET application_name = '$EXTENSION_NAME snapshot for $$new_xid ($$new_snapshot)'"
        );

        $$new_handle->do( 'SELECT 1' );
        return 1;
    }

    $$new_handle->do( 'ROLLBACK' );
    $$new_handle->disconnect();
    return 0;
}

local $SIG{ALRM} = \&alarm_handler;

## Main Program
our( $opt_D, $opt_d, $opt_U, $opt_h, $opt_p, $opt_c );
my @original_argv = @ARGV;

usage( 'Invalid arguments' ) unless( getopts( 'd:U:h:p:c:D' ) );

my $db_name     = $opt_d;
my $db_host     = $opt_h;
my $db_port     = $opt_p;
my $db_user     = $opt_U;
my $config_file = $opt_c;
$DAEMONIZE      = $opt_D;

usage( 'Invalid port' ) if( !defined( $db_port ) || !( $db_port =~ m/^\d+$/ ) );
usage( 'Port number out of range' ) if( $db_port < 1 || $db_port > 65535 );
usage( 'Invalid database name' ) if( !defined( $db_name ) || length( $db_name ) == 0 );
usage( 'Invalid username' ) if( !defined( $db_user ) || length( $db_user ) == 0 );
usage( 'Invalid host name' ) if( !defined( $db_host ) || length( $db_host ) == 0 );

my $conn_string    = "dbi:Pg:dbname=${db_name};host=${db_host};port=${db_port}";
my $pg_conn_string = "dbi:Pg:dbname=postgres;host=${db_host};port=${db_port}";

$CONNECTION_MAP->{connection_string}    = $conn_string;
$CONNECTION_MAP->{pg_connection_string} = $pg_conn_string;
$CONNECTION_MAP->{user_name}            = $db_user;
$CONNECTION_MAP->{dbname}               = $db_name;

$CONFIG_MANAGER          = ConfigManager->new( config_file => $config_file );
my $ENABLE_FAST_DELETE   = $CONFIG_MANAGER->get_config_value( 'enable_fast_delete' );
my $XID_BUCKET_TIMES     = $CONFIG_MANAGER->get_config_value( 'xid_bucket_times' );
my $XID_BUCKET_COUNT     = $CONFIG_MANAGER->get_config_value( 'xid_bucket_count' );
my $XID_IDLE_TIMEOUT     = $CONFIG_MANAGER->get_config_value( 'xid_idle_timeout' );
$XID_SERVICE_PORT        = $CONFIG_MANAGER->get_config_value( 'xid_service_port' );
$MAX_RESERVATION_TIME    = $CONFIG_MANAGER->get_config_value( 'xid_max_reservation_time' );
my $local_xid_map        = {};
my $bucket_id            = 0;

for( $bucket_id = 0; $bucket_id < $XID_BUCKET_COUNT; $bucket_id++ )
{
    $local_xid_map->{$bucket_id} = {
        xid      => undef,
        handle   => undef,
        created  => undef,
        snapshot => undef,
        reserved => undef,
        next     => {
            xid      => undef,
            handle   => undef,
            created  => undef,
            snapshot => undef,
            reserved => undef,
        },
    };
}

unless( $ENABLE_FAST_DELETE )
{
    print( 'Fast delete is not enabled!' );
    exit( 0 );
}

unless( defined( $DAEMONIZE ) && $DAEMONIZE )
{
    daemonize();
    $PARENT_PID = $PROCESS_ID;
}

$handle = &db_connect();
xid_service_register();

my $listen = IO::Socket::INET->new(
    LocalPort => $XID_SERVICE_PORT,
    Proto     => 'tcp',
    LocalAddr => '0.0.0.0',
    Listen    => 10,
    ReuseAddr => 1,
);

if( !defined $listen )
{
    croak( "Failed to listen on TCP port $XID_SERVICE_PORT: $OS_ERROR by $PROCESS_ID" );
}

my $select = IO::Select->new( $listen );
my $snap_cache = {};
# Main loop
while( 1 )
{
    if( $got_sighup )
    {
        $CONFIG_MANAGER->load_configs( 1 );
        $got_sighup = 0;
    }

    ## Bucket management logic
    foreach my $bucket_id( sort { $a <=> $b } keys %$local_xid_map )
    {
        my $current_slot    = $local_xid_map->{$bucket_id};
        my $next_slot;

        if( $bucket_id + 1 < $XID_BUCKET_COUNT )
        {
            $next_slot = $local_xid_map->{$bucket_id + 1};
        }

        my $max_age_sec = ( $XID_BUCKET_TIMES->[$bucket_id] * 2 );
        if(
                defined( $current_slot->{reserved} )
             && tv_interval( $current_slot->{reserved}, [gettimeofday()] ) >= $MAX_RESERVATION_TIME
          )
        {
            $current_slot->{reserved} = undef;
        }

        # Each bucket has it's current slot and the next replacement. This is done so that the snapshots
        # can be aged appropriately by the time they come active. Handles / snapshots in the {next} section
        # of the hash are not available for reservation.
        if( !$current_slot->{next}->{handle} )
        {
            if(
                    (
                        !defined( $current_slot->{reserved} )
                     || tv_interval( $current_slot->{reserved}, [gettimeofday()] ) >= $MAX_RESERVATION_TIME
                    )
                 && defined( $current_slot->{handle} )
                 && tv_interval( $current_slot->{created}, [gettimeofday()] ) < $XID_BUCKET_TIMES->[$bucket_id]
              )
            {
                my $curr = $current_slot->{handle};

                if( !$curr || !$curr->ping() )
                {
                    _log( $LOG_LEVEL_ERROR, "Bad snapshot $current_slot->{snapshot}" );
                    $current_slot->{handle} = undef;
                }
                next;
            }

            my $new_handle;
            my $new_xid;
            my $new_snapshot;

            if( !new_xid_placeholder( \$new_handle, \$new_xid, \$new_snapshot, $XID_IDLE_TIMEOUT ) )
            {
                _log( $LOG_LEVEL_ERROR, "Failed to create new XID snapshot" );
            }
            else
            {
                $current_slot->{next} = {
                    handle   => $new_handle,
                    xid      => $new_xid,
                    snapshot => $new_snapshot,
                    created  => [ gettimeofday() ],
                    reserved => undef,
                };
            }
        }
        else
        {
            if( !$current_slot->{handle} )
            {
                next if( !$current_slot->{next}->{handle} );
                $current_slot->{handle}           = $current_slot->{next}->{handle};
                $current_slot->{xid}              = $current_slot->{next}->{xid};
                $current_slot->{snapshot}         = $current_slot->{next}->{snapshot};
                $current_slot->{created}          = $current_slot->{next}->{created};
                $current_slot->{reserved}         = undef;
                $current_slot->{next}->{handle}   = undef;
                $current_slot->{next}->{xid}      = undef;
                $current_slot->{next}->{snapshot} = undef;
                $current_slot->{next}->{created}  = undef;
                $current_slot->{next}->{reserved} = undef;
            }
            else
            {
                if(
                        (
                            !defined( $current_slot->{reserved} )
                         || tv_interval( $current_slot->{reserved}, [gettimeofday()] ) >= $MAX_RESERVATION_TIME
                        )
                     && tv_interval( $current_slot->{created}, [ gettimeofday() ] ) >= $max_age_sec
                  )
                {
                    my $old_handle       = $current_slot->{handle};
                    my $replace_xid      = $current_slot->{xid};
                    my $new_xid          = $current_slot->{next}->{xid};
                    my $replace_snapshot = $current_slot->{snapshot};
                    my $new_snapshot     = $current_slot->{next}->{snapshot};

                    $old_handle->do( 'ROLLBACK' );
                    $current_slot->{handle}   = $current_slot->{next}->{handle};
                    $current_slot->{xid}      = $current_slot->{next}->{xid};
                    $current_slot->{snapshot} = $current_slot->{next}->{snapshot};
                    $current_slot->{created}  = $current_slot->{next}->{created};
                    $current_slot->{reserved} = undef;

                    if( !new_xid_placeholder( \$old_handle, \$new_xid, \$new_snapshot, $XID_IDLE_TIMEOUT ) )
                    {
                        _log( $LOG_LEVEL_ERROR, "Failed to create new XID snapshot" );
                    }
                    else
                    {
                        $current_slot->{next} = {
                            handle   => $old_handle,
                            xid      => $new_xid,
                            snapshot => $new_snapshot,
                            created  => [ gettimeofday() ],
                            reserved => undef,
                        };
                    }
                }
                else
                {
                    my $curr = $current_slot->{handle};
                    my $next = $current_slot->{next}->{handle};
                    unless( defined( $curr ) && $curr->ping() )
                    {
                        _log( $LOG_LEVEL_ERROR, "Bad XID snapshot $current_slot->{snapshot}" );
                        $current_slot->{handle} = undef;
                    }

                    unless( defined( $next ) && $next->ping() )
                    {
                        _log( $LOG_LEVEL_ERROR, "Bad next snapshot $current_slot->{next}->{snapshot}" );
                        $current_slot->{next}->{handle} = undef;
                    }
                }
            }
        }
    }

    foreach my $xid( keys %$snap_cache )
    {
        my $age = tv_interval( $snap_cache->{$xid}->{age}, [ gettimeofday() ] );
        delete( $snap_cache->{$xid} ) if( $age > $CACHE_AGE_DEFAULT );
    }

    if( $select->count() )
    {
        start_alarm();
        my @ready = $select->can_read();

        if( @ready )
        {
            my $client         = $listen->accept();
            my $client_id      = $client->peerhost();
            my $client_port    = $client->peerport();
            my $candidate_data = "";

            $client->recv( $candidate_data, 1024 );
            my $xid = 0;

            if( defined( $candidate_data ) && length( $candidate_data ) > 0 && $candidate_data =~ m/^\d+$/ )
            {
                $xid = $candidate_data;
            }
            else
            {
                _log( $LOG_LEVEL_ERROR, "Bad XID request from $client_id:$client_port " );
            }

            my $candidates = {};

            if( $xid > 0 )
            {
                if( defined $snap_cache->{$xid} )
                {
                    $client->send( $snap_cache->{$xid}->{snapshot} );
                }
                else
                {
                    foreach my $bucket_id( sort { $a <=> $b } keys %$local_xid_map )
                    {
                        my $current_slot  = $local_xid_map->{$bucket_id};
                        my $candidate_xid = $current_slot->{xid};

                        if( $candidate_xid < $xid )
                        {
                            $candidates->{$candidate_xid} = $bucket_id;
                        }
                    }

                    if( scalar( keys %$candidates ) > 0 )
                    {
                        my @sorted_candidates  = sort { $a <=> $b } keys %$candidates;
                        my $best_candidate     = $sorted_candidates[0];
                        my $best_candidate_ind = $candidates->{$best_candidate};
                        my $snapshot           = $local_xid_map->{$best_candidate_ind}->{snapshot};
                        _log(
                            $LOG_LEVEL_DEBUG,
                            "Received request for Snapshot of '$xid' or better, sent '$snapshot', ($best_candidate)"
                        );
                        $client->send( $snapshot );
                        $local_xid_map->{$best_candidate_ind}->{reserved} = [ gettimeofday() ];
                        $snap_cache->{$xid} = {
                            snapshot => $snapshot,
                            age      => [ gettimeofday() ],
                        };
                    }
                    else
                    {
                        $client->send( '-1' );
                        _log( $LOG_LEVEL_DEBUG, 'No good candidate for XID requested' );
                    }
                }
            }

            $client->shutdown( SHUT_RDWR );
        }
        else
        {
            if( $got_alarm )
            {
                $got_alarm = 0;
                next;
            }
        }
    }
    else
    {
        print "Dead handle?\n";
        # Lost the handle?
    }

    if( $handle->pg_ping < 0 )
    {
        print "DB handle died :*(\n";
        $handle = &db_connect( $handle );
        xid_service_register();
    }
}
