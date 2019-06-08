DO
 $$
BEGIN
    if( regexp_matches( version(), 'PostgreSQL (\d+)\.(\d+)\.?(\d+)?' )::INTEGER[] < ARRAY[9,4]::INTEGER[] ) THEN
        RAISE EXCEPTION 'pg_ctblmgr requires PostgreSQL 9.4 or better';
    END IF;
END
 $$
    LANGUAGE 'plpgsql';


CREATE SEQUENCE @extschema@.sq_pk_maintenance_group;
CREATE TABLE @extschema@.maintenance_group
(
    maintenance_group   INTEGER PRIMARY KEY DEFAULT nextval( '@extschema@.sq_pk_maintenance_group' ),
    title               VARCHAR NOT NULL,
    wal_level           CHAR() NOT NULL DEFAULT 'F'::CHAR
);

CREATE UNIQUE INDEX ix_unique_maintenance_group_title ON @extschema@.tb_maintenance_group( title );

SELECT pg_catalog.pg_extension_config_dump( '@extschema@.sq_pk_maintnenace_group', '' );
SELECT pg_catalog.pg_extension_config_dump( '@extschema@.tb_maintnenace_group', '' );

CREATE TABLE @extschema@.tb_driver
(
    driver INTEGER PRIMARY KEY,
    name   VARCHAR
);

INSERT INTO @extschema@.tb_driver( driver, name )
     VALUES ( 1, 'SQL' ),
            ( 2, 'memcached' );


CREATE SEQUENCE @extschema@.sq_pk_location;
CREATE TABLE @extschema@.tb_location
(
    location    INTEGER PRIMARY KEY DEFAULT nextval( '@extschema@.sq_pk_location' ),
    hostname    VARCHAR NOT NULL DEFAULT 'localhost',
    port        SMALLINT NOT NULL,
    username    VARCHAR NOT NULL
);

SELECT pg_catalog.pg_extension_config_dump( '@extschema@.sq_pk_location', '' );
SELECT pg_catalog.pg_extension_config_dump( '@extschema@.tb_location', '' );

-- List of maintained objects
CREATE SEQUENCE @extschema@.sq_pk_maintenance_object;
CREATE TABLE @extschema@.maintenance_object
(
    maintenance_object INTEGER PRIMARY KEY DEFAULT nextval( '@extschema@.sq_pk_maintenance_object' ),
    maintenance_group  INTEGER NOT NULL REFERENCES @extschema@.tb_maintenance_group,
    definition         TEXT NOT NULL, -- how to get data for this object
    namespace          VARCHAR NOT NULL DEFAULT 'public', -- schema / namespace
    name               VARCHAR NOT NULL, -- table_name
    driver             INTEGER NOT NULL REFERENCES @extschema@.tb_driver,
    location           INTEGER NOT NULL REFERENCES @extschema@.tb_location
);

CREATE UNIQUE INDEX ix_unique_maintenance_object_name ON @extschema@.tb_maintenance_object( namespace, name );
SELECT pg_catalog.pg_extension_config_dump( '@extschema@.sq_pk_maintnenace_object', '' );
SELECT pg_catalog.pg_extension_config_dump( '@extschema@.tb_maintnenace_object', '' );
