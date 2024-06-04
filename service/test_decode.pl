#!/usr/bin/perl

use strict;
use warnings;
use utf8;

use FindBin;
use Data::Dumper;

use lib "$FindBin::Bin/lib";

use QueryParser;
use Util;
use DB;

$CONNECTION_MAP->{connection_string} = 'dbi:Pg:dbname=thd_backup;host=10.0.0.59;port=5432';
$CONNECTION_MAP->{user_name} = 'postgres';

my $definition = <<END_SQL;
SELECT r.reset, r.execution_date,
       get_Translation( pj.name )
  FROM public.tb_reset r
  JOIN public.tb_project pj
    ON pg.project = r.project
 LIMIT 10
END_SQL

# This script is helpful for debugging the query parser without loading in / doing all the extra stuff pg_ctblmgr does
my $handle = DBI->connect( $CONNECTION_MAP->{connection_string}, $CONNECTION_MAP->{user_name}, undef );

unless( $handle )
{
    die( "failed to connect\n" );
}
our $LOCAL_PK_MAINTENANCE_OBJECT = 1234;
our $PARENT_PID = $$;
my $test_change = { 'public' => { 'tb_actual_time_entry' => { 'actual_time_entry' => [ 1 ] }, 'tb_reset' => { 'reset' => [ 2 ] }, 'tb_reset_issue' => { 'issue' => [ 3, 4] } } };
my $filter_tables = [ 'public.tb_reset', 'public.tb_project' ];
my $relcache = get_relcache( $handle );
$relcache->{typmods} = populate_typmods( $handle, $filter_tables );
my $table_mapping = {};
my $data = find_table_aliases( $handle, $relcache, $definition, $table_mapping );
#print Dumper( $data );
#print Dumper( $table_mapping );

my $map = {
    handle => $handle,
    query_data => $data,
    table_mapping => $table_mapping,
    definition => $definition,
    relcache => $relcache,
    filters => $test_change
};

my $where_expressions = generate_where_expressions( $map );
foreach my $bind_position( keys %$where_expressions )
{
    $map->{where_expressions}->{$bind_position} = $where_expressions->{$bind_position};
    my $substituted_query = apply_filters( $map );
        print "$substituted_query\n";
}
