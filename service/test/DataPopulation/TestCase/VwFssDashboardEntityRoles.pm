package DataPopulation::TestCase::VwFssDashboardEntityRoles;

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
    return 'vw_fss_dashboard_entity_roles';
}

sub verify_data_population ## class method
{
    my $self = shift;

    my ( $data_for_change ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    unless( $data_for_change->{entity} )
    {
        croak "Required parameter 'entity' not specified'";
    }

    unless( $data_for_change->{role} )
    {
        croak "Required parameter 'role' not specified'";
    }

    unless( $data_for_change->{key_field} )
    {
        croak "Required parameter 'key_field' not specified'";
    }

    unless( $data_for_change->{key_value} )
    {
        croak "Required parameter 'key_value' not specified'";
    }

    return $self->_validate_vw_fss_dashboard_entity_roles(
        $data_for_change->{entity},
        $data_for_change->{role},
        $data_for_change->{key_field},
        $data_for_change->{key_value},
    );
}

sub _validate_vw_fss_dashboard_entity_roles
{
    my $self = shift;

    my ( $entity, $role, $key_field, $key_value ) = validate_pos(
        @_,
        { type => SCALAR  },
        { type => SCALAR  },
        { type => SCALAR  },
        { type => SCALAR  },
    );

    my $db_handle = $self->{db_handle};

    my $get_cached_rows_query = "SELECT * FROM vw_fss_dashboard_entity_roles WHERE entity = ? AND role = ? AND $key_field = ?"; ## no critics
    my $validation_query = $self->{definition_query} . " WHERE aer.entity = ? AND aer.role = ? AND aer.$key_field = ?";

    my $cached_rows = $db_handle->selectall_arrayref( $get_cached_rows_query, { Slice => {} }, $entity, $role, $key_value );
    my $validation_rows = $db_handle->selectall_arrayref( $validation_query, { Slice => {} }, $entity, $role, $key_value );

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
