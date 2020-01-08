#!/usr/bin/perl

use warnings;
use strict;

use DBI;
use Carp;
use JSON;
use Readonly;
use English qw( -no_match_vars );
use Cwd qw( abs_path );

Readonly my $TEST_DIR => 'test/sql';
Readonly my $TEST_DATABASE => '__pgc_testing__';
Readonly my $CONNECTION_STRING => "dbi:Pg:dbname=$TEST_DATABASE;host=localhost;port=5432";
Readonly my $POSTGRES_CONN_STRING => 'dbi:Pg:dbname=postgres;host=localhost;port=5432';
Readonly my $DBNAME_CHECK_QUERY => <<END_SQL;
    SELECT COUNT(*) AS count
      FROM pg_database
     WHERE datname = ?
END_SQL

my $pg_handle = DBI->connect(
    $POSTGRES_CONN_STRING,
    'postgres',
    undef
);

unless( $pg_handle )
{
    croak( 'Failed to connect to database' );
}

my $sth = $pg_handle->prepare( $DBNAME_CHECK_QUERY );
$sth->bind_param( 1, $TEST_DATABASE );

unless( $sth->execute() )
{
    croak( "Failed to check the state of $TEST_DATABASE" );
}

my $row = $sth->fetchrow_hashref();

if( $row->{count} == 0 )
{
    unless( $pg_handle->do( "CREATE DATABASE \"$TEST_DATABASE\"" ) )
    {
        croak( 'Failed to create testing database' );
    }
}
else
{
    carp( "Test database $TEST_DATABASE already exists" );

    unless( $pg_handle->do( "DROP DATABASE \"$TEST_DATABASE\"" ) )
    {
        croak( "Failed to remove testing database \"$TEST_DATABASE\"" );
    }

    unless( $pg_handle->do( "CREATE DATABASE \"$TEST_DATABASE\"" ) )
    {
        croak( 'Failed to create testing database' );
    }
}

$sth->finish();
$pg_handle->disconnect();

my $handle = DBI->connect(
    $CONNECTION_STRING,
    'postgres',
    undef
);

unless( $handle )
{
    croak( 'Failed to connect to testing database' );
}

unless( $handle->do( "DROP EXTENSION IF EXISTS pg_ctblmgr" ) )
{
    croak( 'Failed to drop extension' );
}

unless( $handle->do( "CREATE EXTENSION pg_ctblmgr" ) )
{
    croak( 'Failed to create extension' );
}

my $path = abs_path( $PROGRAM_NAME );
my $ABS_TEST_PATH = $path;
$ABS_TEST_PATH =~ s/pg_ctblmgr\/.*$//;
$ABS_TEST_PATH .= "pg_ctblmgr/$TEST_DIR";

unless( opendir( TESTDIR, $ABS_TEST_PATH ) )
{
    croak( "Failed to open test directory '$ABS_TEST_PATH': $OS_ERROR" );
}

my @files = readdir( TESTDIR );
closedir( TESTDIR );

my $test_results = { };
my $test_count   = 0;
my $max_file_length = 0;

foreach my $file ( sort { $a cmp $b } @files )
{
    next if( $file =~ m/^\.$/ || $file =~ m/^\.\.$/ );
    next unless( $file =~ m/\.sql$/i );

    if( length( $file ) > $max_file_length )
    {
        $max_file_length = length( $file );
    }

    my $test_fh;
    my $test_name = $file;

    unless( open( $test_fh, '<:encoding(UTF-8)', "${ABS_TEST_PATH}/${file}" ) )
    {
        croak( "Failed to open '${ABS_TEST_PATH}/${file}': $OS_ERROR" );
    }

    my $file_data = '';

    while( my $line = <$test_fh> )
    {
        $file_data .= $line;
    }

    close( $test_fh );

    unless( $handle->do( $file_data ) )
    {
        croak( "Failed to execute $file" );
    }

    $test_count++;
}

print "$test_count tests succeeded\n";
exit 0;
