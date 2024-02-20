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
    unique_index       VARCHAR[] NOT NULL,
    not_null_unique    VARCHAR[],
    indexes            VARCHAR[],
    CONSTRAINT check_not_null_unique_in_unique CHECK ( not_null_unique IS NULL OR not_null_unique <@ unique_index )
);

COMMENT ON TABLE @extschema@.tb_maintenance_object IS 'Definition of object which pg_ctblmgr is maintaining';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.maintenance_group IS 'Which group this object belongs to';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.definition IS 'Definition for the object';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.namespace IS 'Which namespace (memcached) or schema (PostgreSQL) this object belongs to';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.name IS 'Canonical name of the object within its respective store';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.driver IS 'Driver used to maintain this object';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.location IS 'The location of this object';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.unique_index IS 'Specifies column(s) participating in the required unique index. Each member is a column of that index';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.not_null_unique IS 'Specifies the members of the unqiue_index array that are guaranteed to be not null, if any. This enables more performant DML generation.';
COMMENT ON COLUMN @extschema@.tb_maintenance_object.indexes IS 'Specifies additional indexes, where each element is an index and index members are separated by commas';
