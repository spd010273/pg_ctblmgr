package DataPopulation::TestCase::VwFssDashboardEntityRoles::EntityResetDeletion;

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

use parent 'DataPopulation::TestCase::VwFssDashboardEntityRoles';

sub TEST_CASE_NAME ## class method
{
    return 'vw_fss_dashboard_entity_roles entity reset deletion';
}

sub get_data_for_change ## class method
{
    my $self = shift;

    my $db_handle = $self->{db_handle};

    my $get_entity_role_reset_result = $db_handle->selectall_arrayref(
    <<"END_SQL",
      SELECT er.entity,
             er.role,
             er.reset,
             array_agg( DISTINCT er.entity_reset ) as entity_resets
        FROM public.tb_entity_reset er
        JOIN public.tb_config c
          ON er.role::varchar = c.value::varchar
         AND c.name = 'dashboard/fss/index.php#entity_filter.role.pk'
        JOIN public.tb_config_requirement cr
          ON c.config = cr.config
        JOIN jsonb_array_elements_text( ( cr.requirement->2 )::jsonb ) virtual_url
          ON virtual_url ilike 'dashboard/%/%_.php'
    GROUP BY er.entity,
             er.role,
             er.reset
       LIMIT 1
END_SQL
        { Slice => {} },
    );

    unless( defined $get_entity_role_reset_result )
    {
        return 'Failed to get entity role reset alignment';
    }

    unless( @$get_entity_role_reset_result )
    {
        return;
    }

    my $first_row = $get_entity_role_reset_result->[0];

    $first_row->{key_field} = 'reset';
    $first_row->{key_value} = $first_row->{reset};

    return $first_row;
}

sub apply_base_table_change ## class method
{
    my $self = shift;

    my ( $data_for_change ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    my $db_handle = $self->{db_handle};

    my $pk_entity     = $data_for_change->{entity};
    my $pk_role       = $data_for_change->{role};
    my $pk_reset      = $data_for_change->{reset};
    my $entity_resets = $data_for_change->{entity_resets};

    unless( defined $pk_entity )
    {
        return "Required parameter 'entity' not provided";
    }

    unless( defined $pk_role )
    {
        return "Required parameter 'role' not provided";
    }

    unless( defined $pk_reset )
    {
        return "Required parameter 'reset' not provided";
    }

    unless( defined $entity_resets )
    {
        return "Required parameter 'entity_resets' not provided";
    }

    print_info( "Delete all entity $pk_entity role $pk_role via reset $pk_reset alignments" ); ## no critics

    my $delete_result = $db_handle->do(
<<'END_SQL',
DELETE FROM tb_entity_reset
      WHERE entity_reset = ANY( ?::int[] )
END_SQL
        undef,
        $entity_resets
    );

    unless( $delete_result )
    {
        return 'Failed to delete entity resets';
    }

    return;
}

1;
