#!/usr/bin/perl

use strict;
use warnings;
use utf8;

use Params::Validate qw( :all );
use Carp;
use Readonly;
use English qw( -no_match_vars );

use IPC::Shareable qw( :lock );
use Data::Dumper;
use Text::Table;

use FindBin;
use lib "$FindBin::Bin/../service/lib";

use Util;

Readonly::Scalar my $EXTENSION_NAME => 'pg_ctblmgr';
Readonly::Scalar my $SCHEMA_NAME    => 'pg_ctblmgr';

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

    return $status;
}

sub read_xid_map()
{
    my $XID_MAP = [];

    unless( tie( $XID_MAP, 'IPC::Shareable', { key => 'XID' } ) )
    {
        carp( "Could not tie XID_MAP - is $EXTENSION_NAME running?" );
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

    unless( tie( $WORKER_STATUSES, 'IPC::Shareable', { key => 'STATUSES' } ) )
    {
        carp( "Could not tie WORKER_STATUSES - is $EXTENSION_NAME running?" );
        return undef;
    }

    tied( $WORKER_STATUSES )->shlock( LOCK_SH | LOCK_NB );
    my $worker_statuses = {};

    foreach my $pid( keys %$WORKER_STATUSES )
    {
        my $status                = $WORKER_STATUSES->{$pid}->{status};
        my $shutdown              = $WORKER_STATUSES->{$pid}->{shutdown};
        my $last_lsn              = $WORKER_STATUSES->{$pid}->{last_lsn};
        my $pk_maintenance_object = $WORKER_STATUSES->{$pid}->{maintenance_object};
        my $name                  = $WORKER_STATUSES->{$pid}->{name};
        $worker_statuses->{$pid}  = {
            status => $status,
            shutdown => $shutdown,
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

    unless( tie( $WORKER_FILTER_TABLES, 'IPC::Shareable', { key => 'WORKER_FILTER_TABLES' } ) )
    {
        carp( "Could not tie WORKER_FILTER_TABLES - is $EXTENSION_NAME running?" );
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

my $worker_filter_tables = read_worker_filter_tables();
my $xid_map = read_xid_map();
my $worker_statuses = read_worker_statuses();
my $table = Text::Table->new(
    'PID',
    'Cache Table',
    'Status',
    'Last LSN',
    'Filter Tables',
    'Total Queued',
    'Snapshot',
    'XID'
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

    $table->load( [ $pid, $ct_name, $status_text, $last_lsn, $filter_tables, $total_queued, $held_snapshot, $held_xid] );

    if( scalar( keys %$queue ) > 0 )
    {
        foreach my $filter_table( sort { $a cmp $b } keys %$queue )
        {
            my $count = $queue->{$filter_table};
            $table->load( [ undef, undef, undef, undef, $filter_table, $count, undef, undef ] );
        }
    }
}

print $table;
