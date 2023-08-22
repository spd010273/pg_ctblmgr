package DataPopulation::TestCase::CtResetRollup::ResetIssueStatusUpdate;

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

use parent 'DataPopulation::TestCase::CtResetRollup';

sub TEST_CASE_NAME ## class method
{
    return 'ct_reset_rollup reset issue status update';
}

sub get_data_for_change ## class method
{
    my $self = shift;

    my $db_handle = $self->{db_handle};

    my $get_reset_issue_query = <<'END_SQL';
        SELECT ri.reset,
               ri.issue
          FROM ONLY tb_reset_issue ri
          JOIN tb_issue_status ist
            ON ri.issue_status = ist.issue_status
          JOIN ONLY tb_reset r
            ON r.reset = ri.reset
          JOIN tb_project pj
            ON r.project = pj.project
           AND pj.archived IS NULL
     LEFT JOIN tb_reset_prewalk rpw
            ON r.reset = rpw.reset
         WHERE get_translation_immutable( ist.label, 'fallback' ) = 'Open'
           AND r.canceled IS NULL
           AND r.in_scope
           AND r.archived IS NULL
           AND rpw.reset_prewalk IS NULL
           AND r.execution_date IS NOT NULL
         LIMIT 1
END_SQL

    my $get_reset_issue_result = $db_handle->selectall_arrayref( $get_reset_issue_query, { Slice => {} } );

    unless( defined $get_reset_issue_result )
    {
        return 'Failed to get reset issue';
    }

    unless( @$get_reset_issue_result )
    {
        return;
    }

    return $get_reset_issue_result->[0];
}

sub apply_base_table_change ## class method
{
    my $self = shift;

    my ( $data_for_change ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    my $db_handle = $self->{db_handle};

    my $pk_reset       = $data_for_change->{'reset'};
    my $pk_reset_issue = $data_for_change->{'issue'};

    unless( defined $pk_reset )
    {
        return "Required parameter 'reset' not provided";
    }

    unless( defined $pk_reset_issue )
    {
        return "Required parameter 'reset_issue' not provided";
    }

    print_info( "Update 'Open' reset issue $pk_reset_issue under reset $pk_reset to 'Closed'" ); ## no critics

    my $update_result = $db_handle->do(
<<"END_SQL",
    UPDATE tb_reset_issue ri
       SET issue_status = (
               SELECT issue_status
                 FROM tb_issue_status ist
                 JOIN tb_issue_class icl
                   ON ist.issue_class = icl.issue_class
                  AND icl.table_name = 'tb_reset'
                  AND get_translation_immutable( ist.label, 'fallback' ) = 'Closed'
           )
     WHERE ri.issue = ?
END_SQL
        undef,
        $pk_reset_issue
    );

    unless( $update_result )
    {
        return 'Failed to update reset issue status';
    }

    return;
}

1;
