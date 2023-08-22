package DataPopulation::TestCase::CtLocationGroup;

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
use lib "$FindBin::Bin/../..";

use parent 'DataPopulation::TestCase';

sub CACHE_TABLE_NAME ## class method
{
    return 'ct_location_group';
}

sub verify_data_population ## class method
{
    my $self = shift;

    my ( $data_for_change ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    unless( $data_for_change->{location} )
    {
        croak "Required parameter 'reset' not specified'";
    }

    return $self->_validate_ct_location_group( $data_for_change->{location} );
}

sub _validate_ct_location_group
{
    my $self = shift;

    my ( $location ) = validate_pos(
        @_,
        { type => SCALAR },
    );

    my $db_handle = $self->{db_handle};

    my $get_cached_rows_query = 'SELECT * FROM ct_location_group WHERE location = ?';
    my $validation_query = $self->{definition_query} . ' AND l.location = ?';

    my $cached_rows = $db_handle->selectall_arrayref( $get_cached_rows_query, { Slice => {} }, $location );
    my $validation_rows = $db_handle->selectall_arrayref( $validation_query, { Slice => {} }, $location );

    if( not defined $cached_rows )
    {
        return 'Error getting cached rows';
    }

    if( not defined $validation_rows )
    {
        return 'Error getting validation rows';
    }

    return $self->_validate_cached_rows( $cached_rows, $validation_rows );
}

1;
