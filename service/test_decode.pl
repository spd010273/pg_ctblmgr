#!/usr/bin/perl

use strict;
use warnings;
use utf8;

use FindBin;
use Data::Dumper;

use lib "$FindBin::Bin/lib";
use QueryParser;
use Util;
use ConfigManager;
use DB;

$CONNECTION_MAP->{connection_string} = 'dbi:Pg:dbname=mlx;host=devdb-primary;port=5432';
$CONNECTION_MAP->{user_name} = 'postgres';
our $CONFIG_MANAGER = ConfigManager->new( config_file => 'na' );
my $definition = <<END_SQL;
WITH tt_test AS
(
    SELECT r.reset,
           r.reset_team
      FROM ONLY public.tb_reset r
)
    SELECT tt.reset,
           tt.reset_team
      FROM tt_test tt
 LEFT JOIN (
               ONLY public.tb_reset_prewalk rpw
          JOIN public.tb_prewalk pw
            ON pw.prewalk = rpw.prewalk
          JOIN public.tb_prewalk_type pwt
            ON pwt.prewalk_type = pw.prewalk_type
           )
        ON rpw.reset = tt.reset
     WHERE true
END_SQL

# This script is helpful for debugging the query parser without loading in / doing all the extra stuff pg_ctblmgr does
my $handle = DBI->connect( $CONNECTION_MAP->{connection_string}, $CONNECTION_MAP->{user_name}, undef );

unless( $handle )
{
    die( "failed to connect\n" );
}
our $LOCAL_PK_MAINTENANCE_OBJECT = 1234;
our $PARENT_PID = $$;
my $test_change = { 'public' => { 'tb_prewalk_type' => { 'prewalk_type' => [2] } } };
my $filter_tables = [ 'public.tb_reset', 'public.tb_reset_team', 'public.tb_labor_team_type', 'public.tb_prewalk', 'public.tb_reset_prewalk', 'public.tb_prewalk_type', 'public.tb_space', 'public.tb_reset_space', 'public.tb_space_type' ];
my $relcache = get_relcache( $handle );
$relcache->{typmods} = populate_typmods( $handle, $filter_tables );
my $table_mapping = {};
my $data = find_table_aliases( $handle, $relcache, $definition, $table_mapping );
#print Dumper( $data );
#print "---------------------------------------------------\n";
#print Dumper( $table_mapping );

my $map = {
    handle        => $handle,
    query_data    => $data,
    table_mapping => $table_mapping,
    definition    => $definition,
    relcache      => $relcache,
    filters       => $test_change
};

my $where_expression_map = generate_where_expressions( $map );
my $where_expressions = $where_expression_map->{where};
my $alternate_where_expressions = $where_expression_map->{alternate};
my $bind_count = 0;
foreach my $bind_position( keys %$where_expressions )
{
    $map->{where_expressions}->{$bind_position} = $where_expressions->{$bind_position};
    my $substituted_query = apply_filters( $map, \$bind_count );
    print "$substituted_query\n";
    $map->{where_expressions}->{$bind_position} = $alternate_where_expressions->{$bind_position};
    my $alt_substituted_query = apply_filters( $map );
    print "$alt_substituted_query\n";
}
