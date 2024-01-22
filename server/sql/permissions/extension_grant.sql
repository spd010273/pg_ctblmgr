GRANT USAGE ON SCHEMA @extschema@ TO PUBLIC;
GRANT SELECT ON ALL TABLES IN SCHEMA @extschema@ TO PUBLIC;
GRANT USAGE ON ALL SEQUENCES IN SCHEMA @extschema@ TO PUBLIC;
SELECT pg_catalog.pg_extension_config_dump( '@extschema@.tb_driver', '' );
SELECT pg_catalog.pg_extension_config_dump( '@extschema@.tb_location', '' );
SELECT pg_catalog.pg_extension_config_dump( '@extschema@.tb_maintenance_group', '' );
SELECT pg_catalog.pg_extension_config_dump( '@extschema@.tb_maintenance_object', '' );
