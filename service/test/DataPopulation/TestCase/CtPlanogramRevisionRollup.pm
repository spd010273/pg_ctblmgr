package DataPopulation::TestCase::CtPlanogramRevisionRollup;

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
    return 'ct_planogram_revision_rollup';
}

sub verify_data_population ## class method
{
    my $self = shift;

    my ( $data_for_change ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    if( $data_for_change->{planogram_revision} )
    {
        return $self->_validate_ct_planogram_revision_rollup( 'planogram_revision', $data_for_change->{planogram_revision} );
    }
    elsif( $data_for_change->{planogram} )
    {
        return $self->_validate_ct_planogram_revision_rollup( 'planogram', $data_for_change->{planogram} );
    }
    else
    {
        return "Neither of 'planogram' nor 'planogram_revision' is specified";
    }

}

sub _validate_ct_planogram_revision_rollup
{
    my $self = shift;

    my ( $key_field, $key_value ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => SCALAR },
    );

    my $db_handle = $self->{db_handle};

    my ( $get_cached_rows_query, $validation_query );

    if( $key_value eq 'planogram_revision' )
    {
        $get_cached_rows_query = 'SELECT * FROM ct_planogram_revision_rollup WHERE self_planogram_revision = ?';
        $validation_query = $self->{definition_query} . ' AND i_pogr.planogram_revision = ?';
    }
    else
    {
        $get_cached_rows_query = 'SELECT * FROM ct_planogram_revision_rollup WHERE planogram = ?';
        $validation_query = $self->{definition_query} . ' AND i_pog.planogram = ?';
    }

    my $cached_rows = $db_handle->selectall_arrayref( $get_cached_rows_query, { Slice => {} }, $key_value );
    my $validation_rows = $db_handle->selectall_arrayref( $validation_query, { Slice => {} }, $key_value );

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
