CREATE OR REPLACE FUNCTION @extschema@.fn_get_dependencies
(
    in_maintenance_object INTEGER
)
RETURNS VARCHAR AS
 $_$
    WITH tt_dependencies AS
    (
        SELECT dt.schema_name || '.' || dt.table_name AS object
          FROM @extschema@.tb_maintenance_object mo
    INNER JOIN @extschema@.fn_get_dependent_tables( mo.definition ) dt
            ON TRUE
         WHERE mo.maintenance_object = in_maintenance_object
         UNION
        SELECT COALESCE( jet->>'schema', 'public' ) || jet.key AS object
          FROM @extschema.tb_maintenance_object mo
    INNER JOIN jsonb_each_text( mo.datamap ) jet
            ON TRUE
         WHERE mo.maintenance_object = in_maintenance_object
    )
        SELECT array_to_string( array_agg( tt.object ), ',' )
          FROM tt_dependencies tt
 $_$
    LANGUAGE SQL STABLE PARALLEL SAFE;
