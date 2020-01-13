CREATE OR REPLACE FUNCTION @extschema@.fn_get_maintenance_channel_name
(
    in_schema_name VARCHAR,
    in_table_name  VARCHAR
)
RETURNS VARCHAR AS
 $_$
    SELECT '__pg_ctblmgr_' || in_schema_name || '_' || in_table_name;
 $_$
    LANGUAGE SQL IMMUTABLE PARALLEL SAFE;
