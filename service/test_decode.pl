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

$CONNECTION_MAP->{connection_string} = 'dbi:Pg:dbname=thd;host=10.1.1.147;port=5432';
$CONNECTION_MAP->{user_name} = 'postgres';

my $definition = <<END_SQL;
    SELECT r.*
      FROM tb_reset r
     WHERE r.execution_date > now()
END_SQL


# This script is helpful for debugging the query parser without loading in / doing all the extra stuff pg_ctblmgr does
my $handle = DBI->connect( $CONNECTION_MAP->{connection_string}, $CONNECTION_MAP->{user_name}, undef );

unless( $handle )
{
    die( "failed to connect\n" );
}

my $test_change = { 'public' => { 'tb_reset' => { 'reset' => [ 1 ] }, 'tb_reset_status' => { 'reset_status'=>[2,3]}} };
my $filter_tables = [ 'public.tb_a', 'public.tb_b', 'public.tb_c' ];
my $relcache = get_relcache( $handle );
my $table_mapping = {};
my $data = find_table_aliases( $handle, $relcache, $definition, $filter_tables, $table_mapping );
#print Dumper( $data );
#print Dumper( $table_mapping );
my $substituted_query = apply_filters( $handle, $data, $table_mapping, $definition, $test_change );
print "$substituted_query\n";

