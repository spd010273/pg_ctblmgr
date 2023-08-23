package DataPopulation::TestCase::CtPlanogramRevisionRollup::PlanogramCheckout;

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

use parent 'DataPopulation::TestCase::CtPlanogramRevisionRollup';

sub TEST_CASE_NAME ## class method
{
    return 'ct_planogram_revision_rollup planogram checkout';
}

sub get_data_for_change ## class method
{
    my $self = shift;

    my $db_handle = $self->{db_handle};

    my $get_planogram_query = <<'END_SQL';
    SELECT planogram
      FROM tb_planogram
     WHERE obsolete IS FALSE
       AND dv_latest_planogram_revision IS NOT NULL
       AND dv_latest_checked_out_planogram_revision IS NULL
     LIMIT 1
END_SQL

    my $get_planogram_result = $db_handle->selectall_arrayref( $get_planogram_query, { Slice => {} } );

    unless( defined $get_planogram_result )
    {
        return 'Failed to get planogram to checkout';
    }

    unless( @$get_planogram_result )
    {
        return;
    }

    return $get_planogram_result->[0];
}

sub apply_base_table_change ## class method
{
    my $self = shift;

    my ( $data_for_change ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    my $db_handle = $self->{db_handle};

    my $pk_planogram = $data_for_change->{'planogram'};

    unless( defined $pk_planogram )
    {
        return "Required parameter 'planogram' not provided";
    }

    print_info( "Checkout Planogram $pk_planogram" ); ## no critics

    my $checkout_result = $db_handle->do(
<<"END_SQL",
    SELECT fn_check_out_planogram( ?, 0, False )
END_SQL
        undef,
        $pk_planogram
    );

    unless( $checkout_result )
    {
        return 'Failed to checkout planogram';
    }

    return;
}

1;
