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
