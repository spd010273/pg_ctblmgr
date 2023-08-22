package DataPopulation::TestCase::CtLocationGroup::LocationNameUpdate;

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

use FindBin;
use lib "$FindBin::Bin/../../..";
use DataPopulation::Util;

use parent 'DataPopulation::TestCase::CtLocationGroup';

sub TEST_CASE_NAME ## class method
{
    return 'ct_location_group location name update';
}

sub get_data_for_change ## class method
{
    my $self = shift;

    my $db_handle = $self->{db_handle};

    my $get_location_query = $self->{definition_query} . ' LIMIT 1';

    my $get_location_result = $db_handle->selectall_arrayref( $get_location_query, { Slice => {} } );

    unless( defined $get_location_result )
    {
        return 'Failed to get location';
    }

    unless( @$get_location_result )
    {
        return;
    }

    return $get_location_result->[0];
}

sub apply_base_table_change ## class method
{
    my $self = shift;

    my ( $data_for_change ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    my $db_handle = $self->{db_handle};

    my $pk_location = $data_for_change->{'location'};

    unless( defined $pk_location )
    {
        return "Required parameter 'location' not provided";
    }

    my $test_location_name = gen_random_string();

    print_info( "Updated location with pk $pk_location to use name '$test_location_name'" );

    my $update_result = $db_handle->do(
<<'END_SQL',
        UPDATE tb_location
           SET name = add_translation( null, ? )
         WHERE location = ?
END_SQL
        undef,
        $test_location_name,
        $pk_location,
    );

    unless( $update_result )
    {
        return 'Failed to update location name';
    }

    return;
}

1;
