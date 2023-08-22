package DataPopulation::Util;

#### STRICTURES ####
use strict;
use warnings;
use feature qw( say switch );

#### NEADWERX MODULES ####
use Carp;
use Readonly;
use Perl6::Export::Attrs;
use English qw( -no_match_vars );
use Fatal qw( open );
use Params::Validate qw( :all );

#### CPAN MODULES ####
use Term::ANSIColor;

###################################################
# Printer Functions
###################################################
sub print_test_case_name :Export(:DEFAULT)
{
    my( $test_case_name ) = validate_pos(
        @_,
        {
            type => SCALAR,
        }
    );

    print_section_name( "Test Case: $test_case_name" );

    return;
}

sub print_section_name :Export(:DEFAULT)
{
    my( $section_name ) = validate_pos(
        @_,
        {
            type => SCALAR,
        }
    );

    print "\n";
    print colored( " -----------------------------------------------------------------\n", 'bold' );
    print colored( "| $section_name\n", 'bold' );
    print colored( " -----------------------------------------------------------------\n", 'bold' );

    return;
}

sub print_summary :Export(:DEFAULT)
{
    my( $test_results ) = validate_pos(
        @_,
        {
            type => HASHREF,
        }
    );

    my $passed_tests = $test_results->{passed};
    my $failed_tests = $test_results->{failed};
    my $count_passed_tests = scalar( @$passed_tests );
    my $count_total_tests = scalar( @$passed_tests ) + scalar( @$failed_tests );

    print colored( "\nOut of $count_total_tests test cases, $count_passed_tests passed\n", 'bold bright_yellow' );

    if( @$passed_tests )
    {
        print colored( "\nPassed:\n", 'bold bright_green' );

        foreach my $passed_test ( @$passed_tests )
        {
            print colored( "  - $passed_test\n", 'bold bright_green' );
        }
    }

    if( @$failed_tests )
    {
        print colored( "\nFailed:\n", 'bold bright_red' );

        foreach my $failed_test ( @$failed_tests )
        {
            print colored( "  - $failed_test\n", 'bold bright_red' );
        }
    }

    return;
}

sub print_info :Export(:DEFAULT)
{
    my( $message ) = validate_pos(
        @_,
        {
            type => SCALAR,
        }
    );

    print colored( "INFO: $message\n", 'yellow' );

    return;
}

sub print_and_collect_test_result :Export(:DEFAULT)
{
    my( $test_case_name, $validation_error, $test_results ) = validate_pos(
        @_,
        {
            type => SCALAR,
        },
        {
            type => SCALAR|UNDEF ## no critics
        },
        {
            type => HASHREF,
        }
    );

    if( $validation_error )
    {
        push @{$test_results->{failed}}, $test_case_name;
        print colored( "\nTest Failed:\n", 'bold bright_red' );
        foreach my $error_line ( split( /\n/, $validation_error ) )
        {
            print "    $error_line\n";
        }
        print "\n";
    }
    else
    {
        push @{$test_results->{passed}}, $test_case_name;
        print colored( "\nTest Passed\n", 'bold bright_green' );
    }

    return;
}

sub gen_random_string :Export(:DEFAULT)
{
    my @char_set = ('0' ..'9', 'A' .. 'F');

    return join '' => map $char_set[rand @char_set], 1 .. 8; ## no critics
}

1;
