CREATE OR REPLACE FUNCTION @extschema@.fn_notify_maintenace_channel
(
    in_maintenance_object INTEGER,
    in_command VARCHAR
)
RETURNS VOID AS
 $_$
    SELECT pg_notify( rs.name, in_command )
      FROM @extschema@.tb_maintenance_object mo
INNER JOIN @extschema@.__pgctblmgr_repl_slot rs
        ON rs.id = mo.maintenance_object
     WHERE in_command IN(
            'index_update',
            'definition_update',
            'object_remove'
            'full_refresh'
           )
       AND mo.maintenance_object = in_maintenance_object;
 $_$
    LANGUAGE SQL VOLATILE PARALLEL UNSAFE;
