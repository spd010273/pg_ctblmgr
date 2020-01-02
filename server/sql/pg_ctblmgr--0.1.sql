DO
 $_$
BEGIN
    IF( pg_catalog.regexp_matches( version(), 'PostgreSQL (\d+)\.(\d+)\.?(\d+)?'::VARCHAR )::INTEGER[] < ARRAY[9,4]::INTEGER[] ) THEN
        RAISE EXCEPTION 'pg_ctblmgr requires PostgreSQL 9.4 or better';
    END IF;
END
 $_$
    LANGUAGE 'plpgsql';
CREATE TABLE IF NOT EXISTS @extschema@.__pgctblmgr_repl_slot
(
    id      INTEGER NOT NULL,
    name    VARCHAR NOT NULL,
    filter  VARCHAR NOT NULL,
    UNIQUE( id )
);

COMMENT ON TABLE @extschema@.__pgctblmgr_repl_slot IS 'Stores mapping of replication slots to their accompanying maintenance_objects';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.id IS 'The maintenance_object PK that this replication slot maps to';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.name IS 'Name of the replication slot and LISTEN/NOTIFY maintenance channel';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.filter IS 'Comma-delimited list of base objects for this table';
CREATE SEQUENCE @extschema@.sq_pk_driver;

CREATE TABLE IF NOT EXISTS @extschema@.tb_driver
(
    driver INTEGER PRIMARY KEY DEFAULT nextval( '@extschema@.sq_pk_driver' ),
    name   VARCHAR NOT NULL
);

INSERT INTO @extschema@.tb_driver( driver, name )
     VALUES ( 1, 'postgresql' ),
            ( 2, 'memcached' );

COMMENT ON TABLE @extschema@.tb_driver IS 'Used to identify supported drivers for pg_ctblmgr';
CREATE SEQUENCE @extschema@.sq_pk_location;

CREATE TABLE IF NOT EXISTS @extschema@.tb_location
(
    location    INTEGER PRIMARY KEY DEFAULT nextval( '@extschema@.sq_pk_location' ),
    hostname    VARCHAR NOT NULL DEFAULT 'localhost',
    port        SMALLINT NOT NULL,
    username    VARCHAR,
    namespace   VARCHAR,
    CHECK( port > 0 AND port < 65536 )
);

COMMENT ON TABLE @extschema@.tb_location IS 'Declares a location for which a maintenance object resides';
COMMENT ON COLUMN @extschema@.tb_location.hostname IS 'Hostname of the server which holds this resource';
COMMENT ON COLUMN @extschema@.tb_location.port IS 'Port where the service storing the resource is listening on';
COMMENT ON COLUMN @extschema@.tb_location.username IS 'Username with which we can use to access this resource';
COMMENT ON COLUMN @extschema@.tb_location.namespace IS 'Database name of memcached namespace which can be used to find the resource';
CREATE SEQUENCE @extschema@.sq_pk_maintenance_group;

CREATE TABLE IF NOT EXISTS @extschema@.tb_maintenance_group
(
    maintenance_group   INTEGER PRIMARY KEY DEFAULT nextval( '@extschema@.sq_pk_maintenance_group' ),
    title               VARCHAR NOT NULL,
    wal_level           CHAR NOT NULL DEFAULT 'F'::CHAR,
    CHECK( wal_level = ANY( ARRAY[ 'F','M','R' ]::CHAR[] ) )
);

COMMENT ON TABLE @extschema@.tb_maintenance_group IS 'Defines a group for maintainence objects, used to organize and control WAL for these objects';
COMMENT ON COLUMN @extschema@.tb_maintenance_group.wal_level IS 'Defines the verbosity of WAL for these objects, can be F for full, R for reduced, or M for minimal';
CREATE SEQUENCE @extschema@.sq_pk_maintenance_object;

CREATE TABLE IF NOT EXISTS @extschema@.tb_maintenance_object
(
    maintenance_object INTEGER PRIMARY KEY DEFAULT nextval( '@extschema@.sq_pk_maintenance_object' ),
    maintenance_group  INTEGER NOT NULL,
    definition         TEXT NOT NULL,
    namespace          VARCHAR NOT NULL DEFAULT 'public',
    name               VARCHAR NOT NULL,
    driver             INTEGER NOT NULL,
    location           INTEGER NOT NULL,
    datamap            JSONB NOT NULL
);

COMMENT ON TABLE @extschema@.tb_maintenance_object IS 'Definition of object which pg_ctblmgr is maintaining';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.maintenance_group IS 'Which group this object belongs to';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.definition IS 'Definition for the object';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.namespace IS 'Which namespace (memcached) or schema (PostgreSQL) this object belongs to';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.name IS 'Canonical name of the object within its respective store';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.driver IS 'Driver used to maintain this object';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.location IS 'The location of this object';
CREATE OR REPLACE FUNCTION @extschema@.fn_get_dependencies
(
    in_maintenance_object INTEGER
)
RETURNS VARCHAR AS
 $_$
DECLARE
    my_query    TEXT;
    my_result   VARCHAR;
BEGIN
    SELECT definition
      INTO my_query
      FROM @extschema@.tb_maintenance_object
     WHERE maintenance_object = in_maintenance_object;

    IF( my_query IS NULL ) THEN
        RAISE EXCEPTION 'Maintenance object % doesn''t exist or has no definition', in_maintenance_object;
    END IF;

    EXECUTE 'CREATE TEMP VIEW tt_vw_column_check AS( ' || my_query || ')';
 
    WITH tt_pk_locator AS
    (
        SELECT c_n.nspname::VARCHAR AS schema_name,
               c.relname::VARCHAR AS table_name,
               array_agg( DISTINCT con_a_att.attname::VARCHAR ) AS primary,
               array_agg( DISTINCT con_b_att.attname::VARCHAR ) AS secondary,
               array_agg( DISTINCT con_c_att.attname::VARCHAR ) AS tertiary
          FROM pg_depend d
    INNER JOIN pg_rewrite rw
            ON rw.oid = d.objid
    INNER JOIN pg_class c_v
            ON c_v.oid = rw.ev_class
           AND c_v.relname = 'tt_vw_column_check'
           AND c_v.relkind = 'v'
    INNER JOIN pg_namespace n
            ON n.oid = c_v.relnamespace
           AND n.oid = pg_my_temp_schema()
    INNER JOIN pg_class c
            ON c.oid = d.refobjid
    INNER JOIN pg_namespace c_n
            ON c_n.oid = c.relnamespace
    INNER JOIN pg_attribute a
            ON a.attrelid = c.oid
           AND a.attnum = d.refobjsubid
    INNER JOIN pg_class c_f
            ON c_f.relname = 'pg_class'
           AND c_f.oid = d.refclassid
     LEFT JOIN (
                     pg_constraint con_a
                JOIN pg_attribute con_a_att
                  ON con_a_att.attrelid = con_a.conrelid
                 AND con_a_att.attnum = ANY( con_a.conkey )
                 AND con_a_att.attnum > 0
               )
            ON con_a.contype = 'p'
           AND con_a.conrelid = c.oid
     LEFT JOIN (
                     pg_constraint con_b
                JOIN pg_attribute con_b_att
                  ON con_b_att.attrelid = con_b.conrelid
                 AND con_b_att.attnum = ANY( con_b.conkey )
                 AND con_b_att.attnum > 0
               )
            ON con_b.contype = 'u'
           AND con_b.conrelid = c.oid
     LEFT JOIN (
                     pg_index i
                JOIN pg_attribute con_c_att
                  ON con_c_att.attrelid = i.indrelid
                 AND con_c_att.attnum = ANY( i.indkey )
                 AND con_c_att.attnum > 0
               )
            ON i.indrelid = c.oid
           AND i.indisunique IS TRUE
      GROUP BY c_n.nspname::VARCHAR,
               c.relname::VARCHAR
    ),
    tt_dependencies AS
    (
        SELECT tt.schema_name || '.' || tt.table_name AS object
          FROM tt_pk_locator tt
         UNION
        SELECT COALESCE( ( jet.value::JSONB )->>'schema', 'public' ) || '.' || jet.key AS object
          FROM @extschema@.tb_maintenance_object mo
    INNER JOIN pg_catalog.jsonb_each_text( mo.datamap ) jet
            ON TRUE
         WHERE mo.maintenance_object = in_maintenance_object
    )
        SELECT pg_catalog.array_to_string(
                   pg_catalog.array_agg( tt.object ),
                   ','
               )
          INTO my_result
          FROM tt_dependencies tt;
    DROP VIEW tt_vw_column_check;
    RETURN my_result;
END
 $_$
    LANGUAGE 'plpgsql' VOLATILE PARALLEL UNSAFE;
CREATE OR REPLACE FUNCTION @extschema@.fn_get_dependent_tables( in_query TEXT )
RETURNS TABLE
(
    table_name  VARCHAR,
    column_name VARCHAR[]
)
AS
 $_$
BEGIN
    /*
        Given a query, this function attempts to find the tables used in it for
        pg_ctblmgr. For each of these tables, we find and return, with the
        descending precedence, the columns used for the following:
        - Primary key for the table
        - Unique constraint
        - Unique index
     */
    EXECUTE 'CREATE TEMP VIEW tt_vw_column_check AS( ' || in_query || ' )';
    RETURN QUERY
    WITH tt_pk_locator AS
    (
        SELECT c.relname::VARCHAR AS table_name,
               array_agg( DISTINCT con_a_att.attname::VARCHAR ) AS primary,
               array_agg( DISTINCT con_b_att.attname::VARCHAR ) AS secondary,
               array_agg( DISTINCT con_c_att.attname::VARCHAR ) AS tertiary
          FROM pg_depend d
    INNER JOIN pg_rewrite rw
            ON rw.oid = d.objid
    INNER JOIN pg_class c_v
            ON c_v.oid = rw.ev_class
           AND c_v.relname = 'tt_vw_column_check'
           AND c_v.relkind = 'v'
    INNER JOIN pg_namespace n
            ON n.oid = c_v.relnamespace
           AND n.oid = pg_my_temp_schema()
    INNER JOIN pg_class c
            ON c.oid = d.refobjid
    INNER JOIN pg_attribute a
            ON a.attrelid = c.oid
           AND a.attnum = d.refobjsubid
    INNER JOIN pg_class c_f
            ON c_f.relname = 'pg_class'
           AND c_f.oid = d.refclassid
     LEFT JOIN (
                     pg_constraint con_a
                JOIN pg_attribute con_a_att
                  ON con_a_att.attrelid = con_a.conrelid
                 AND con_a_att.attnum = ANY( con_a.conkey )
                 AND con_a_att.attnum > 0
               )
            ON con_a.contype = 'p'
           AND con_a.conrelid = c.oid
     LEFT JOIN (
                     pg_constraint con_b
                JOIN pg_attribute con_b_att
                  ON con_b_att.attrelid = con_b.conrelid
                 AND con_b_att.attnum = ANY( con_b.conkey )
                 AND con_b_att.attnum > 0
               )
            ON con_b.contype = 'u'
           AND con_b.conrelid = c.oid
     LEFT JOIN (
                     pg_index i
                JOIN pg_attribute con_c_att
                  ON con_c_att.attrelid = i.indrelid
                 AND con_c_att.attnum = ANY( i.indkey )
                 AND con_c_att.attnum > 0
               )
            ON i.indrelid = c.oid
           AND i.indisunique IS TRUE
      GROUP BY c.relname::VARCHAR
    )
        SELECT tt.table_name,
               COALESCE(
                   NULLIF( tt.primary, ARRAY[NULL]::VARCHAR[] ),
                   NULLIF( tt.secondary, ARRAY[NULL]::VARCHAR[] ),
                   NULLIF( tt.tertiary, ARRAY[NULL]::VARCHAR[] )
               ) AS column_name
          FROM tt_pk_locator tt;
    DROP VIEW tt_vw_column_check;
END
 $_$
    LANGUAGE 'plpgsql' VOLATILE;
CREATE OR REPLACE FUNCTION @extschema@.fn_get_replication_slot_name
(
    in_schema_name VARCHAR,
    in_table_name  VARCHAR
)
RETURNS VARCHAR AS
 $_$
    SELECT '__pg_ctblmgr_' || in_schema_name || '_' || in_table_name;
 $_$
    LANGUAGE SQL IMMUTABLE PARALLEL SAFE;
/*
 * This trigger maintains the state of logical replication slots used to feed
 * changes made in WAL to pg_ctblmgr forked processes.
 */

CREATE OR REPLACE FUNCTION @extschema@.fn_manage_publication()
RETURNS TRIGGER AS
 $_$
BEGIN
    IF( TG_OP = 'UPDATE' ) THEN
        IF(
                NEW.definition IS NOT DISTINCT FROM OLD.definition
            AND NEW.namespace IS NOT DISTINCT FROM OLD.namespace
            AND NEW.name IS NOT DISTINCT FROM OLD.name
          ) THEN
            -- Avoid dummy updates
            RETURN NEW;
        END IF;

        IF(
                NEW.namespace IS DISTINCT FROM OLD.namespace
             OR NEW.name IS DISTINCT FROM OLD.name
          ) THEN
            /* Prevent renaming of a resource, this would change the replication slot name
               and detach the worker from its WAL source */
            RAISE EXCEPTION 'Cannot rename a replication slot for %.% - you'
                            ' need to drop this object then create it',
                            NEW.namespace,
                            NEW.name;
        END IF;
    ELSIF( TG_OP = 'DELETE' ) THEN
        -- Drop replication slot, if exists
        PERFORM *
           FROM pg_replication_slots
          WHERE slot_name = @extschema@.fn_get_replication_slot_name(
                                OLD.namespace,
                                OLD.name
                            );

        IF FOUND THEN
            PERFORM pg_drop_replication_slot(
                @extschema@.fn_get_replication_slot_name(
                    OLD.namespace,
                    OLD.name
                )
            );
        ELSE
            RAISE EXCEPTION 'Could not locate replication slot for object %.%',
                OLD.namespace,
                OLD.name;
        END IF;

        PERFORM @extschema@.fn_notify_maintenance_channel(
            OLD.maintenance_object,
            'object_remove'
        );
        DELETE FROM @extschema@.__pgctblmgr_repl_slot
              WHERE id = OLD.maintenance_object;
        RETURN OLD;
    END IF;

    INSERT INTO @extschema@.__pgctblmgr_repl_slot
                (
                    id,
                    name,
                    filter
                )
         VALUES
                (
                    NEW.maintenance_object,
                    @extschema@.fn_get_replication_slot_name(
                        NEW.namespace,
                        NEW.name
                    ),
                    @extschema@.fn_get_dependencies( NEW.maintenance_object )
                );
    RETURN NEW;
END
 $_$
    LANGUAGE 'plpgsql' VOLATILE PARALLEL UNSAFE;
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
CREATE OR REPLACE FUNCTION @extschema@.fn_notify_service
(
    in_command   VARCHAR,
    in_object_id INTEGER
)
RETURNS VOID AS
 $_$
    SELECT pg_notify( '__pg_ctblmgr', in_command )
     WHERE in_command IN(
               'new_table'
           );
 $_$
    LANGUAGE SQL VOLATILE PARALLEL UNSAFE;
CREATE TRIGGER tr_manage_publication
    AFTER INSERT OR DELETE OR UPDATE OF definition, namespace, name
    ON @extschema@.tb_maintenance_object
    FOR EACH ROW EXECUTE PROCEDURE @extschema@.fn_manage_publication();
