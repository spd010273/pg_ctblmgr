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

use FindBin;
use lib "$FindBin::Bin/../service/lib";

use Util;

Readonly::Scalar my $EXTENSION_NAME => 'pg_ctblmgr';
Readonly::Scalar my $SCHEMA_NAME    => 'pg_ctblmgr';
Readonly::Scalar my $USAGE          => <<USAGE;
USAGE:
 $0 [-C command -v cache_table]
    -C command: issue a command to pg_ctblmgr. Available commands are:
        rebuild
    -v cache_table: The cache table the command applies to
USAGE

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

sub parse_command($$)
{
    my( $command, $cache_table ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => SCALAR },
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
our( $opt_C, $opt_v, $opt_d, $opt_U, $opt_p, $opt_p );

print_usage( 'Invalid arguments' ) unless( getopts( 'C:v:d:U:h:p:' ) );

my $command = $opt_C;
my $ct      = $opt_v;

if(
      ( defined( $command ) && !defined( $ct ) )
   || ( defined( $ct ) && !defined( $command ) )
  )
{
    print_usage( 'Must specify -v and -C together' );
}

if( !defined( $command ) && !defined( $ct ) )
{
    print_worker_table();
    exit 0;
}

if( defined( $command ) && defined( $ct ) )
{
    parse_command( $command, $ct );
}
