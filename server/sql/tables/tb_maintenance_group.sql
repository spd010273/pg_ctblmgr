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
