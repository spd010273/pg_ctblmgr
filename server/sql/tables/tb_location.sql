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
