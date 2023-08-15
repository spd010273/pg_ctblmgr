#!/usr/bin/perl

use DBI;
use Time::HiRes qw( tv_interval gettimeofday sleep );

use strict;
use warnings;

# Test will perform a change
# check will return non-updated rows - should be 0
# revert will put data back
my $TEST_CASES = [
    {
        'name'   => 'UPDATE name',
        'test'   => "UPDATE tb_location SET name = add_translation( NULL, lower( get_translation( name ) ), 'en_US' ) WHERE location = ?",
        'check'  => "SELECT COUNT(*) AS count FROM ct_location_group WHERE location_name != lower( location_name ) AND location = ?",
        'revert' => "UPDATE tb_location SET name = add_translation( NULL, upper( get_translation( name ) ), 'en_US' ) WHERE location = ?",
        'pre'    => 'SELECT COUNT(*) AS count FROM tb_location WHERE location = ? AND get_translation( name ) = lower( get_translation( name ) )'
    },
    {
        'name'   => 'UPDATE status',
        'test'   => 'UPDATE tb_location SET location_status = 2 WHERE location = ?',
        'check'  => 'SELECT COUNT(*) AS count FROM ct_location_group WHERE is_eligible_for_reset IS FALSE AND location = ?',
        'revert' => 'UPDATE tb_location SET location_status = 1 WHERE location = ?',
        'pre'    => 'SELECT COUNT(*) AS count FROM tb_location WHERE location_status = 1 AND location = ?',
    }
];

my $SLEEP_RES     = 0.25;
my $CHECK_TIMEOUT = 10;


sub get_random_test_case()
{
    return $TEST_CASES->[rand scalar( @$TEST_CASES )];
}


my $handle = DBI->connect( 'dbi:Pg:dbname=thd_restore;host=10.1.1.147;port=5432', 'postgres', undef );

die( 'Failed to connect' ) unless( $handle );

my $locations_q = <<END_SQL;
WITH tt_locs AS
(
    SELECT DISTINCT location
      FROM ct_location_group
)
    SELECT location
      FROM tt_locs
  ORDER BY random()
END_SQL

my $sth = $handle->prepare( $locations_q );

die( 'Failed to prep test data' ) unless( $sth && $sth->execute() );

my $inner_loop_stop = $CHECK_TIMEOUT * ( 1 / $SLEEP_RES );

PK: while( my $row = $sth->fetchrow_hashref() )
{
    my $pk_location = $row->{location};
    my $case        = get_random_test_case();
    my $test        = $case->{test};
    my $check       = $case->{check};
    my $revert      = $case->{revert};
    my $title       = $case->{name};
    my $pre         = $case->{pre};

    # Check test start state
    my $pre_check_sth = $handle->prepare( $pre );
    
    unless( $pre_check_sth )
    {
        print "Failed to check starting state for case $title, $pk_location\n";
        next;
    }

    $pre_check_sth->bind_param( 1, $pk_location );

    unless( $pre_check_sth->execute() )
    {
        print "Failed to validate starting state for case $title, $pk_location\n";
        next;
    }

    my $count_row = $pre_check_sth->fetchrow_hashref();
    if( $count_row->{count} == 0 )
    {
        print "Invalid starting state for $title, $pk_location\n";
        next;
    }

    $pre_check_sth->finish();
    # Make change
    my $test_sth    = $handle->prepare( $test );

    unless( $test_sth )
    {
        print "Failed to run test case $title for $pk_location\n";
        next;
    }

    $test_sth->bind_param( 1, $pk_location );

    unless( $test_sth->execute() )
    {
        print "Failed to execute test case $title for $pk_location\n";
    }

    my $t_start = [ gettimeofday() ];
    my $t_end;

    my $done = 0;
    my $iter = 0;
    my $check_sth = $handle->prepare( $check );

    unless( $check_sth )
    {
        print "Failed to prpare check case $title for $pk_location\n";
        next;
    }

    while( !$done && $iter < $inner_loop_stop )
    {
        sleep( $SLEEP_RES );

        $check_sth->bind_param( 1, $pk_location );

        next PK unless( $check_sth->execute() );

        my $c_row = $check_sth->fetchrow_hashref();

        if( $c_row->{count} == 0 )
        {
            $done  = 1;
            $t_end = [ gettimeofday() ];
        }

        $iter++;
    }

    if( !$done )
    {
        $t_end = [ gettimeofday() ];
        print "Test failed after $iter iterations $title - $pk_location\n";
    }

    my $t_delta = tv_interval( $t_start, $t_end );

    print "Test took $t_delta ( $title, $pk_location )\n";

    my $revert_sth = $handle->prepare( $revert );

    unless( $revert_sth )
    {
        print "failed to revert\n";
        next;
    }

    $revert_sth->bind_param( 1, $pk_location );

    unless( $revert_sth->execute() )
    {
        print "failed to execute revert statement\n";
        next;
    }

    # Validate revert
    my $post_check_sth = $handle->prepare( $check );
    
    unless( $post_check_sth )
    {
        print "Failed to check starting state for case $title, $pk_location\n";
        next;
    }

    $post_check_sth->bind_param( 1, $pk_location );

    unless( $post_check_sth->execute() )
    {
        print "Failed to validate starting state for case $title, $pk_location\n";
        next;
    }

    $count_row = $post_check_sth->fetchrow_hashref();
    if( $count_row->{count} == 0 )
    {
        print "Invalid starting state for $title, $pk_location\n";
        next;
    }

    $post_check_sth->finish();
    sleep( 1 );
}
