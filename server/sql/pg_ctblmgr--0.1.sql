DO
 $_$
BEGIN
    IF( regexp_replace( version(), 'PostgreSQL (\d+)\.(\d+)\.?(\d+)?' )::INTEGER[] < ARRAY[9,4]::INTEGER[] ) THEN
        RAISE EXCEPTION 'pg_ctblmgr requires PostgreSQL 9.4 or better';
    END IF;
END
 $_$
    LANGUAGE 'plpgsql';
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
    title               VARCHAR NOT NULL
    wal_level           CHAR NOT NULL DEFAULT 'F'::CHAR,
    CHECK( wal_level = ANY( ARRAY[ 'F','M','R' ]::CHAR[] )
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
CREATE OR REPLACE FUNCTION public.fn_get_dependent_tables( in_query TEXT )
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
