package DataPopulation::TestCase::CtResetRollup;

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
    return 'ct_reset_rollup';
}

sub verify_data_population ## class method
{
    my $self = shift;

    my ( $data_for_change ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    unless( $data_for_change->{reset} )
    {
        croak "Required parameter 'reset' not specified'";
    }

    return $self->_validate_ct_reset_rollup( $data_for_change->{reset} );
}

sub _validate_ct_reset_rollup
{
    my $self = shift;

    my ( $reset ) = validate_pos(
        @_,
        { type => SCALAR }
    );

    my $db_handle = $self->{db_handle};

    my $get_cached_rows_query = 'SELECT * FROM ct_reset_rollup WHERE reset = ?';
    my $validation_query = $self->{definition_query} . ' WHERE ttr.reset = ?';

    # To speedup the query
    $validation_query =~ s/AND r[.]archived IS NULL/AND r.archived IS NULL AND r.reset = ?/;

    my $cached_rows = $db_handle->selectall_arrayref( $get_cached_rows_query, { Slice => {} }, $reset );
    my $validation_rows = $db_handle->selectall_arrayref( $validation_query, { Slice => {} }, $reset, $reset );

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
