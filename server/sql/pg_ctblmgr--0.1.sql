DO
 $_$
DECLARE
    my_current_count INTEGER := 0;
    my_enabled_count INTEGER := 0;
    my_wal_level     VARCHAR := 0;
BEGIN
    SELECT COUNT(*)
      INTO my_current_count
      FROM pg_catalog.pg_replication_slots
     WHERE slot_type = 'logical';

    SELECT current_setting( 'max_replication_slots' )
      INTO my_enabled_count;

    IF( my_current_count > 0 AND my_current_count = my_enabled_count ) THEN
        RAISE EXCEPTION 'No available logical replication slots. Please increase max_replication_slots by at least 1';
    END IF;

    IF( my_enabled_count = 0 ) THEN
        RAISE EXCEPTION 'No available logical replication slots. max_replication_slots must be >= 0 with at least one available slot';
    END IF;

    SELECT current_setting( 'wal_level' )
      INTO my_wal_level;
    
    IF( my_wal_level != 'logical' ) THEN
        RAISE EXCEPTION 'A WAL Level of ''logical'' is required for logical decoding';
    END IF;
    
    PERFORM slot_name
       FROM pg_replication_slots
      WHERE slot_type = 'logical'
        AND plugin = 'pg_ctblmgr'
        AND database::VARCHAR = current_database();

    IF FOUND THEN
        RAISE EXCEPTION 'There is already a pg_ctblmgr replication slot for this database';
    END IF;
END
 $_$
    LANGUAGE 'plpgsql';

-- We'll have the service control this
--SELECT pg_create_logical_replication_slot( '__pg_ctblmgr', 'pg_ctblmgr' );
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
    id                  INTEGER NOT NULL,
    maintenance_channel VARCHAR NOT NULL,
    filter              VARCHAR[] NOT NULL,
    UNIQUE( id )
);

COMMENT ON TABLE @extschema@.__pgctblmgr_repl_slot IS 'Stores mapping of replication slots to their accompanying maintenance_objects';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.id IS 'The maintenance_object PK that this replication slot maps to';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.maintenance_channel IS 'Name of the LISTEN/NOTIFY maintenance channel used to notify workers of changes to the object';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.filter IS 'array of base objects for this table';
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
/*
 * This function feeds the tables used by a maintenance object to
 * __pgctblmgr_repl_slot.filter. Additionally, we set or upgrade the replica
 * identity of the table so that pg_ctblmgr can extract approprate information
 * about modified tuples from WAL. We try to be mindful that a replica ident
 * already exists, and dont 'downgrade'
 */
CREATE OR REPLACE FUNCTION @extschema@.fn_get_dependencies
(
    in_maintenance_object INTEGER
)
RETURNS VARCHAR[] AS
 $_$
DECLARE
    my_query             TEXT;
    my_result            VARCHAR[];
    my_schema            VARCHAR;
    my_table             VARCHAR;
    my_current_replident VARCHAR;
    my_desired_replident VARCHAR;
    my_index_name        VARCHAR;
BEGIN
    SELECT definition
      INTO my_query
      FROM @extschema@.tb_maintenance_object
     WHERE maintenance_object = in_maintenance_object;

    IF( my_query IS NULL ) THEN
        RAISE EXCEPTION 'Maintenance object % doesn''t exist or has no definition', in_maintenance_object;
    END IF;

    EXECUTE 'CREATE TEMP VIEW tt_vw_column_check AS( ' || my_query || ')';
    FOR my_schema, my_table IN(
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
        SELECT tt.schema_name AS schema_name,
               tt.table_name AS table_name
          FROM tt_pk_locator tt
         UNION
        SELECT COALESCE( ( jet.value::JSONB )->>'schema', 'public' ) AS schema_name,
               jet.key AS table_name
          FROM @extschema@.tb_maintenance_object mo
    INNER JOIN pg_catalog.jsonb_each_text( mo.datamap ) jet
            ON TRUE
         WHERE mo.maintenance_object = in_maintenance_object
    )
        SELECT schema_name,
               table_name
          FROM tt_dependencies tt
                              ) LOOP
        my_result := my_result::VARCHAR[] || ( quote_ident( my_schema ) || '.' || quote_ident( my_table ) )::VARCHAR;
        -- Verify that the table has a unique constraint or
        -- some kind of replica identity set
         SELECT CASE WHEN c.relreplident = 'n' AND c_pk.oid IS NOT NULL
                     THEN 'd'
                     WHEN c.relreplident = 'n' AND c_pk.oid IS NULL AND c_new_rid.oid IS NOT NULL
                     THEN 'i'
                     WHEN c.relreplident = 'd' AND c_pk.oid IS NULL AND c_new_rid.oid IS NOT NULL
                     THEN 'i'
                     WHEN c.relreplident = 'd' AND c_pk.oid IS NOT NULL
                     THEN 'd'
                     WHEN c.relreplident = 'i' AND c_rid.oid IS NOT NULL
                     THEN 'i'
                     ELSE 'f'
                      END AS desired_replident,
                CASE WHEN c.relreplident = 'n' AND c_pk.oid IS NOT NULL
                     THEN c_pk.relname::VARCHAR
                     WHEN c.relreplident = 'n' AND c_pk.oid IS NULL AND c_new_rid.oid IS NOT NULL
                     THEN c_new_rid.relname::VARCHAR
                     WHEN c.relreplident = 'd' AND c_pk.oid IS NULL AND c_new_rid.oid IS NOT NULL
                     THEN c_new_rid.relname::VARCHAR
                     WHEN c.relreplident = 'd' AND c_pk.oid IS NOT NULL
                     THEN c_pk.relname::VARCHAR
                     WHEN c.relreplident = 'i' AND c_rid.oid IS NOT NULL
                     THEN c_rid.relname::VARCHAR
                     ELSE NULL
                      END AS indexname,
                c.relreplident AS current_replident
           INTO my_desired_replident,
                my_index_name,
                my_current_replident
           FROM pg_class c
     INNER JOIN pg_namespace n
             ON n.oid = c.relnamespace
      LEFT JOIN (
                      pg_index i_rid
                 JOIN pg_class c_rid
                   ON i_rid.indexrelid = c_rid.oid
                )
             ON i_rid.indisreplident IS TRUE
            AND i_rid.indrelid = c.oid
      LEFT JOIN (
                      pg_index i_new_rid
                 JOIN pg_class c_new_rid
                   ON i_new_rid.indexrelid = c_new_rid.oid
                )
             ON i_new_rid.indrelid = c.oid
            AND i_new_rid.indpred IS NULL
            AND i_new_rid.indisunique IS TRUE
            AND i_new_rid.indimmediate IS TRUE
            AND i_new_rid.indnatts > 0
            AND i_new_rid.indisreplident IS FALSE
      LEFT JOIN (
                      pg_index i_pk
                 JOIN pg_class c_pk
                   ON i_pk.indexrelid = c_pk.oid
                )
             ON i_pk.indrelid = c.oid
            AND i_pk.indisprimary IS TRUE
          WHERE c.relname::VARCHAR = my_table
            AND n.nspname::VARCHAR = my_schema
            AND c.relkind IN( 'r', 'm' );
        IF( my_desired_replident IS NULL ) THEN
            EXECUTE 'ALTER TABLE ' || quote_ident( my_schema ) || '.' || quote_ident( my_table )
                 || ' REPLICA IDENTITY FULL ';
        ELSIF( my_desired_replident != my_current_replident ) THEN
            EXECUTE 'ALTER TABLE ' || quote_ident( my_schema ) || '.' || quote_ident( my_table )
                 || ' REPLICA IDENTITY ' || ( CASE WHEN my_desired_replident = 'd' THEN 'DEFAULT'
                                                   WHEN my_desired_replident = 'f' THEN 'FULL'
                                                   WHEN my_desired_replident = 'i' THEN 'USING INDEX ' || quote_ident( my_index_name )
                                                   ELSE 'FULL'
                                                    END );
            RAISE NOTICE 'REPLICA IDENTITY for %.% updated from % to %',
                my_schema,
                my_table,
                CASE WHEN my_current_replident IS NULL THEN 'NOTHING'
                     WHEN my_current_replident = 'n' THEN 'NOTHING'
                     WHEN my_current_replident = 'd' THEN 'DEFAULT'
                     WHEN my_current_replident = 'f' THEN 'FULL'
                     WHEN my_current_replident = 'i' THEN 'INDEX'
                     ELSE 'N/A'
                      END,
                CASE WHEN my_desired_replident IS NULL THEN 'NOTHING'
                     WHEN my_desired_replident = 'n' THEN 'NOTHING'
                     WHEN my_desired_replident = 'd' THEN 'DEFAULT'
                     WHEN my_desired_replident = 'f' THEN 'FULL'
                     WHEN my_desired_replident = 'i' THEN 'INDEX'
                     ELSE 'N/A'
                      END;
        END IF;
    END LOOP;

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
                    maintenance_channel,
                    filter
                )
         VALUES
                (
                    NEW.maintenance_object,
                    @extschema@.fn_get_maintenance_channel_name(
                        NEW.namespace,
                        NEW.name
                    ),
                    @extschema@.fn_get_dependencies( NEW.maintenance_object )
                );
    RETURN NEW;
END
 $_$
    LANGUAGE 'plpgsql' VOLATILE PARALLEL UNSAFE;
CREATE OR REPLACE FUNCTION @extschema@.fn_notify_maintenance_channel
(
    in_maintenance_object INTEGER,
    in_command VARCHAR
)
RETURNS VOID AS
 $_$
    SELECT pg_notify( rs.maintenance_channel, in_command )
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
