#!/usr/bin/perl

use strict;
use warnings;
use utf8;

use FindBin;
use Data::Dumper;

use lib "$FindBin::Bin/lib";

use QueryParser;
use Util;
use DB;

$CONNECTION_MAP->{connection_string} = 'dbi:Pg:dbname=thd;host=10.1.1.147;port=5432';
$CONNECTION_MAP->{user_name} = 'postgres';

my $definition = <<END_SQL;
WITH tt_active_resets AS MATERIALIZED
(
    SELECT r.reset,
           r.execution_date,
           r.prewalk_due_date,
           r.in_scope,
           r.labor_duration,
           r.end_date,
           r.creator,
           r.modifier,
           r.bay_count,
           r.canceled,
           r.wbse,
           r.reported_labor_duration,
           r.signoff_received,
           r.completed AS reset_completed,
           r.reset_status,
           r.reset_team,
           pg.opt_in,
           pg.program_type,
           pg.name AS program_name,
           pg.program,
           pg.fiscal_year AS program_fiscal_year,
           pj.project,
           pj.tier,
           pj.project_type,
           pj.name AS project_name,
           pj.project_status,
           pj.completed AS project_completed,
           l.location,
           l.name AS location_name,
           l.number AS location_number,
           l.abbreviation AS location_abbreviation,
           COALESCE( r.archived, pj.archived ) AS archived
      FROM ONLY public.tb_reset r
INNER JOIN public.tb_location l
        ON l.location = r.location
INNER JOIN public.tb_project pj
        ON pj.project = r.project
       AND pj.archived IS NULL
INNER JOIN public.tb_program pg
        ON pg.program = pj.program
     WHERE r.in_scope IS TRUE
       AND r.canceled IS NULL
       AND r.archived IS NULL
),
tt_hours_rollup AS MATERIALIZED
(
    WITH tt_tet AS MATERIALIZED
    (
        SELECT ttr.reset,
               tet.time_entry_type
          FROM tt_active_resets ttr
    INNER JOIN public.tb_time_entry_type tet
            ON TRUE
    ),
    tt_forecast_hours AS
    (
        SELECT ttr.reset,
               '"' || ttr.time_entry_type::VARCHAR || '":"' || SUM( COALESCE( fte.duration, '00:00:00'::INTERVAL ) )::VARCHAR || '"' AS fte_fragment
          FROM tt_tet ttr
     LEFT JOIN ONLY public.tb_forecast_time_entry fte
            ON fte.reset = ttr.reset
           AND fte.time_entry_type = ttr.time_entry_type
      GROUP BY ttr.reset,
               ttr.time_entry_type
    ),
    tt_actual_hours AS
    (
        SELECT ttr.reset,
               '"' || ttr.time_entry_type::VARCHAR || '":"' || SUM( COALESCE( ate.duration, '00:00:00'::INTERVAL ) )::VARCHAR || '"' AS ate_fragment
          FROM tt_tet ttr
     LEFT JOIN ONLY public.tb_actual_time_entry ate
            ON ate.reset = ttr.reset
           AND ate.time_entry_type = ttr.time_entry_type
      GROUP BY ttr.reset,
               ttr.time_entry_type
    )
        SELECT f.reset,
               ( '{' || array_to_string( array_agg( fte_fragment ), ',' ) || '}' )::JSONB AS forecast_hours,
               ( '{' || array_to_string( array_agg( ate_fragment ), ',' ) || '}' )::JSONB AS actual_hours
          FROM tt_forecast_hours f
    INNER JOIN tt_actual_hours a
            ON a.reset = f.reset
      GROUP BY f.reset
),
tt_prewalk_hours_hstore AS MATERIALIZED
(
    WITH tt_reset_prewalks AS MATERIALIZED
    (
        SELECT ttr.reset,
               ttr.end_date,
               rs.reviewed,
               rs.reset_prewalk,
               rs.reset_prewalk_status,
               CASE WHEN public.get_translation_immutable( rps.label, 'fallback' ) = 'Completed'
                     AND rs.reviewed > ttr.end_date
                    THEN 1
                    ELSE 0
                     END AS completed_late,
               CASE WHEN public.get_translation_immutable( rps.label, 'fallback' ) NOT IN( 'Completed', 'In Review' )
                     AND now()::TIMESTAMPTZ::DATE > ttr.end_date
                    THEN 1
                    ELSE 0
                     END AS late
          FROM tt_active_resets ttr
     LEFT JOIN (
                       ONLY public.tb_reset_prewalk rs
                  JOIN public.tb_prewalk s
                    ON s.prewalk = rs.prewalk
                  JOIN public.tb_prewalk_type pt
                    ON pt.prewalk_type = s.prewalk_type
                   AND public.get_translation_immutable( pt.label, 'fallback' ) = 'Prewalk'
                  JOIN public.tb_reset_prewalk_status rps
                    ON rps.reset_prewalk_status = rs.reset_prewalk_status
               )
            ON rs.reset = ttr.reset
    ),
    tt_prewalk_statuses AS
    (
        SELECT rs.reset,
               rss.reset_prewalk_status,
               COUNT( rp.reset_prewalk ) AS reset_prewalk_count
          FROM tt_active_resets rs
    CROSS JOIN public.tb_reset_prewalk_status rss
     LEFT JOIN tt_reset_prewalks rp
            ON rp.reset_prewalk_status = rss.reset_prewalk_status
           AND rp.reset = rs.reset
      GROUP BY rs.reset,
               rss.reset_prewalk_status
         UNION ALL
        SELECT rs.reset,
               -1 AS reset_prewalk_status, -- complete late psuedo status
               SUM( completed_late ) AS reset_prewalk_count
          FROM tt_reset_prewalks rs
      GROUP BY rs.reset
         UNION ALL
        SELECT rs.reset,
               -2 AS reset_prewalk_status, --late psuedo status
               SUM( late ) AS reset_prewalk_count
          FROM tt_reset_prewalks rs
      GROUP BY rs.reset
         UNION ALL
        SELECT rs.reset,
               0 AS reset_prewalk_status, -- count
               COUNT( rs.reset_prewalk ) AS reset_prewalk_status
          FROM tt_reset_prewalks rs
      GROUP BY rs.reset
    )
        SELECT reset,
               (
                   array_to_string(
                       array_agg(
                           reset_prewalk_status || '=>' || reset_prewalk_count
                       ),
                       ','
                   )
               )::public.HSTORE AS prewalk_hours
          FROM tt_prewalk_statuses
      GROUP BY reset
),
tt_issues_rollup AS MATERIALIZED
(
    WITH tt_issues AS
    (
        SELECT r.reset,
               ist.issue_Status || '=>' || COUNT( ri.issue ) AS hstore_fragment
          FROM tt_active_resets r
    INNER JOIN public.tb_issue_status ist
            ON ist.issue_status > 0
           AND ist.issue_class = 1
     LEFT JOIN ONLY public.tb_reset_issue ri
            ON ri.reset = r.reset
           AND ri.issue_status = ist.issue_status
           AND ri.archived IS NULL
      GROUP BY r.reset,
               ist.issue_status
    )
        SELECT reset,
               ( array_to_string( array_agg( hstore_fragment ), ',' ) )::public.HSTORE AS issue_counts
          FROM tt_issues
      GROUP BY reset
)
    SELECT ttr.*,
           rst.label AS reset_status_label,
           rst.abbreviation AS reset_status_abbreviation,
           rst.color AS reset_status_color,
           rpw.reset_prewalk,
           rpw.reset_prewalk_status,
           rpw.reset_prewalk_judgment_reason,
           ltt.labor_team_type,
           ltt.label AS labor_team_type_label,
           fm2.fiscal_month AS reset_month,
           fc2.year AS reset_year,
           pw.prewalk,
           pw.prewalk_type,
           fc.year AS execution_year,
           fc.month AS execution_month,
           ttpwh.prewalk_hours AS reset_prewalk_counts,
           tti.issue_counts AS reset_issue_counts,
           tth.forecast_hours,
           tth.actual_hours
      FROM tt_active_resets ttr
INNER JOIN tt_hours_rollup tth
        ON tth.reset = ttr.reset
INNER JOIN tt_prewalk_hours_hstore ttpwh
        ON ttpwh.reset = ttr.reset
INNER JOIN tt_issues_rollup tti
        ON tti.reset = ttr.reset
INNER JOIN public.tb_reset_status rst
        ON rst.reset_status = ttr.reset_status
INNER JOIN public.tb_fiscal_calendar fc
        ON fc.day = ttr.execution_date
 LEFT JOIN (
                 public.tb_reset_prewalk rpw
            JOIN public.tb_prewalk pw
              ON pw.prewalk = rpw.prewalk
           )
        ON rpw.reset = ttr.reset
 LEFT JOIN (
                 public.tb_reset_team rt
            JOIN public.tb_labor_team_type ltt
              ON ltt.labor_team_type = rt.labor_team_type
           )
        ON rt.reset_team = ttr.reset_team
INNER JOIN public.tb_fiscal_calendar fc2
        ON fc2.day = (
                        CASE WHEN rpw.reset_prewalk_status IN( 1, 2 ) AND ttr.prewalk_due_date IS NOT NULL
                             THEN ttr.prewalk_due_date
                             ELSE ttr.execution_date
                              END
                     )
INNER JOIN public.tb_fiscal_month fm2
        ON fm2.fiscal_month = fc2.month
END_SQL

#my $definition = <<END_SQL;
#WITH tt_regress AS
#(
#    SELECT r.reset, r.end_date
#      FROM ONLY public.tb_reset r
#INNER JOIN public.tb_reset_status rs
#        ON rs.reset_status = r.reset_status
#       AND rs.reset_status > 0
#)
#    SELECT ttr.reset
#      FROM tt_regress ttr
#INNER JOIN public.tb_issue_status ist
#        ON TRUE
# LEFT JOIN ONLY public.tb_reset_issue ri
#        ON ri.reset = ttr.reset
#       AND ri.issue_status = ist.issue_status
#END_SQL
#
# This script is helpful for debugging the query parser without loading in / doing all the extra stuff pg_ctblmgr does
my $handle = DBI->connect( $CONNECTION_MAP->{connection_string}, $CONNECTION_MAP->{user_name}, undef );

unless( $handle )
{
    die( "failed to connect\n" );
}

my $test_change = { 'public' => { 'tb_actual_time_entry' => { 'actual_time_entry' => [ 1 ] }, 'tb_reset' => { 'reset' => [ 2 ] }, 'tb_reset_issue' => { 'issue' => [ 3, 4] } } };
my $filter_tables = [ 'public.tb_reset' ];
my $relcache = get_relcache( $handle );
my $table_mapping = {};
my $data = find_table_aliases( $handle, $relcache, $definition, $table_mapping );
#print Dumper( $data );
#print Dumper( $table_mapping );

my $map = {
    handle => $handle,
    query_data => $data,
    table_mapping => $table_mapping,
    definition => $definition,
    relcache => $relcache,
    filters => $test_change
};

my $where_expressions = generate_where_expressions( $map );

foreach my $bind_position( keys %$where_expressions )
{
    $map->{where_expressions}->{$bind_position} = $where_expressions->{$bind_position};
    my $substituted_query = apply_filters( $map );
    print "$substituted_query\n";
}
