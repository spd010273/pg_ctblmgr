#!/usr/bin/perl

use warnings;
use strict;

use DBI;
use Carp;
use JSON;
use Readonly;
use English qw( -no_match_vars );
use Cwd qw( abs_path getcwd );
use Data::Dumper;

Readonly my $TEST_DIR => 'test/';

my $current_path = getcwd;

unless( chdir( $TEST_DIR ) )
{
    croak( "Failed to change directory to service test directory" );
}

system( 'make clean && make' );

unless( chdir( $current_path ) )
{
    croak( 'Failed to return to cwd' );
}

my $SERVICE_TESTS;

unless( opendir( $SERVICE_TESTS, $TEST_DIR ) )
{
    croak( 'Failed to open directory for listing' );
}

my @tests;

while( my $file = readdir( $SERVICE_TESTS ) )
{
    next unless( $file =~ /t_/ );
    push( @tests, $file );
}

closedir( $SERVICE_TESTS );

foreach my $test( @tests )
{
    my $path = $TEST_DIR . $test;
    print "$test: ";
    my $result = system( $path );

    if( $result != 0 )
    {
        croak( "Service unit test $test failed" );
    }
}

exit 0;
