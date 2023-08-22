#!/usr/bin/perl

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
use DBI;
use Data::Dumper;
use Getopt::Long;
use File::Find;
use Try::Tiny;
use Term::ANSIColor;

#### INTERNAL MODULES ####
use FindBin;
use lib "$FindBin::Bin/..";

use DataPopulation::Util;

###################################################
# Global Variables
###################################################
# Arguments
my $dbhost   = 'athena.internal';
my $dbname   = 'postgres';
my $dbuser   = 'postgres';
my $dbport   = 5432; ## no critics
my $waittime = 5;
my $help     = 0;

###################################################
# Printer Functions
###################################################
sub _usage
{
    my( $message ) = validate_pos(
        @_,
        {
            type     => SCALAR|UNDEF, ## no critics
            optional => 1,
        }
    );

    print "$message\n" if( $message );
    print <<"END_SQL";
    Usage: $PROGRAM_NAME
        --dbhost=<host>               Host to connect to (default=athena.internal)
        --dbport=<port>               Port to connect to (default=5432)
        --dbuser=<user>               User to connect as (default=postgres)
        --waittime=<waittime in secs> Wait time for data to populate (default=5)
END_SQL

    exit 1;
}

###################################################
# Other Functions
###################################################
sub _check_pg_ctblmgr
{
    my ( $db_handle, $database_name ) = validate_pos( @_, { type => OBJECT }, { type => SCALAR } );

    my $extension_exist_result = $db_handle->selectall_arrayref( "SELECT 1 FROM pg_extension WHERE extname = 'pg_ctblmgr'" );

    if( not defined $extension_exist_result )
    {
        croak 'Error checking if pg_ctblmgr extension is installed';
    }

    if( not @$extension_exist_result )
    {
        croak "pg_ctblmgr extension cannot be found installed at '$database_name' database";
    }

    my $process_running_result = $db_handle->selectall_arrayref( "SELECT 1 FROM pg_stat_activity WHERE application_name ~ 'pg_ctblmgr' and datname = ?", undef, $database_name );

    if( not defined $process_running_result )
    {
        croak 'Error checking pg_ctblmgr database process';
    }

    if( not @$process_running_result )
    {
        croak "pg_ctblmgr database process cannot be found running at '$database_name' database";
    }
}

sub _wait
{
    sleep $waittime;

    return;
}

###################################################
# Main: Argument Parsing
###################################################
unless(
    GetOptions(
        'dbhost=s'   => \$dbhost,
        'dbname=s'   => \$dbname,
        'dbuser=s'   => \$dbuser,
        'dbport=s'   => \$dbport,
        'waittime=f' => \$waittime,
        'help'       => \$help,
    )
)
{
    _usage( 'Invalid arguments' );
}

if( $help )
{
    _usage();
}

print colored( "Settings: dbhost [$dbhost], dbport [$dbport], dbname [$dbname], dbuser [$dbuser], waittime [$waittime seconds]\n\n", 'bold bright_yellow' );

###################################################
# Main: Initialization
###################################################
my $db_handle = DBI->connect(
    "dbi:Pg:dbname=$dbname;host=$dbhost;port=$dbport",
    'postgres',
    undef,
    {
        AutoCommit         => 1,
        ShowErrorStatement => 1,
    }
);

_check_pg_ctblmgr( $db_handle, $dbname );

my $cache_tables = $db_handle->selectall_hashref(
    'SELECT * FROM pgctblmgr.tb_maintenance_object',
    'name'
);

###################################################
# Main: Test Cases
###################################################

my $test_cases = [];

sub process_file
{
    if ( -f $_ )
    {
        no strict 'refs';    ## no critic
        ( my $test_case_class = $_ ) =~ s/\//::/g;
        $test_case_class =~ s/[.]pm$//;

        require $_;

        my $test_case_instance;

        try
        {
            $test_case_instance = ${test_case_class}->new( $db_handle, $cache_tables );

            $test_case_instance->TEST_CASE_NAME;

            push @$test_cases, $test_case_instance;
        }
        catch
        {
            if( $_ =~ 'Missing Cache Table Definition' )
            {
                print_info( "Skipped $test_case_class - $_" );
            }
            elsif( not $_ =~ 'not implemented' )
            {
                croak @_;
            }
        };
    }
}

find( { wanted => \&process_file, no_chdir => 1 }, 'DataPopulation/TestCase/' );

my $test_results = {
    passed => [],
    failed => [],
};

foreach my $test_case ( @$test_cases )
{
    print_test_case_name( $test_case->TEST_CASE_NAME );

    my $data_for_change;

    try
    {
        $data_for_change = $test_case->get_data_for_change();
    }
    catch
    {
        $data_for_change = $_;
    };

    unless( defined $data_for_change )
    {
        print_info( 'No data for change found. Test case skipped...' );
    }

    unless( ref( $data_for_change ) )
    {
        print_and_collect_test_result( $test_case->TEST_CASE_NAME, $data_for_change, $test_results );
        next;
    }

    my $apply_error;

    try
    {
        $apply_error = $test_case->apply_base_table_change( $data_for_change );
    }
    catch
    {
        $apply_error = $_;
    };

    if( $apply_error )
    {
        print_and_collect_test_result( $test_case->TEST_CASE_NAME, $apply_error, $test_results );
        next;
    }

    _wait();

    my $validation_error;

    try
    {
        $validation_error = $test_case->verify_data_population( $data_for_change );
    }
    catch
    {
        $validation_error = $_;
    };

    print_and_collect_test_result( $test_case->TEST_CASE_NAME, $validation_error, $test_results );
}

###################################################
# Main: Summary
###################################################
print_section_name( 'Summary' );
print_summary( $test_results );
