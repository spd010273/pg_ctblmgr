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

$CONNECTION_MAP->{connection_string} = 'dbi:Pg:dbname=__pgc_testing__;host=localhost;port=5432';
$CONNECTION_MAP->{user_name} = 'postgres';

my $definition = <<END_SQL;
WITH tt_foo AS
(
    WITH tt_union_test AS
    (
        SELECT c.bar FROM public.tb_c c
         UNION
        SELECT b.bar FROM public.tb_b b
         EXCEPT
        SELECT x.foo FROM ( SELECT a.bar FROM tb_a a WHERE a.baz > 1 ) x
    )
        SELECT bar
          FROM tt_union_test
),
tt_bar AS
(
    SELECT b.bar
      FROM tb_b b
      JOIN public.tb_a a
        ON a.baz = b.baz
     WHERE a.baz = 2
       AND TRUE
       AND a.bar = 1
)
SELECT a.foo
  FROM tb_a a
  JOIN tb_b b
    ON b.bar = a.bar
  JOIN tb_c c
    ON c.baz = ANY( ARRAY[ 1,2,3] )
  JOIN tt_bar ttb
    ON ttb.bar = a.bar
  JOIN tt_foo ttf
    ON ttf.bar = a.bar
 WHERE a.baz IS NOT NULL
 LIMIT 10
END_SQL


# This script is helpful for debugging the query parser without loading in / doing all the extra stuff pg_ctblmgr does
my $handle = DBI->connect( $CONNECTION_MAP->{connection_string}, $CONNECTION_MAP->{user_name}, undef );

unless( $handle )
{
    die( "failed to connect\n" );
}

my $test_change = { 'public' => { 'tb_a' => { 'foo' => [ 1 ] }, 'tb_b' => { 'foo'=>[2,3]}, 'tb_c' => {'foo'=>[4,5]} } };
my $filter_tables = [ 'public.tb_a', 'public.tb_b', 'public.tb_c' ];
my $relcache = get_relcache( $handle );
my $table_mapping = {};
my $data = find_table_aliases( $handle, $relcache, $definition, $filter_tables, $table_mapping );
#print Dumper( $data );
#print Dumper( $table_mapping );
my $substituted_query = apply_filters( $handle, $data, $table_mapping, $definition, $test_change );
print "$substituted_query\n";

