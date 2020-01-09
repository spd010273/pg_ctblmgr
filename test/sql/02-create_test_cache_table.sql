CREATE TABLE tb_a( foo INTEGER PRIMARY KEY, bar INTEGER, baz INTEGER );
CREATE TABLE tb_b( foo INTEGER PRIMARY KEY, bar INTEGER, baz INTEGER );
CREATE TABLE tb_c( foo INTEGER NOT NULL, bar INTEGER NOT NULL, baz INTEGER, UNIQUE( foo, bar ) );
CREATE TABLE tb_d( foo INTEGER PRIMARY KEY, bar INTEGER, baz INTEGER );
CREATE TABLE tb_e( foo INTEGER PRIMARY KEY, bar INTEGER, baz INTEGER );
CREATE TABLE tb_f( foo INTEGER NOT NULL, bar INTEGER NOT NULL, baz INTEGER, UNIQUE( foo, bar ) );
ALTER TABLE tb_c REPLICA IDENTITY USING INDEX tb_c_foo_bar_key;
ALTER TABLE tb_d REPLICA IDENTITY FULL;
CREATE FUNCTION fn_test( in_t INTEGER )
RETURNS INTEGER AS
 $_$
DECLARE
    my_foo INTEGER;
BEGIN
    SELECT foo
      INTO my_foo
      FROM tb_c
     WHERE foo = in_t;
    RETURN my_foo;
END
 $_$
    LANGUAGE plpgsql;

INSERT INTO pgctblmgr.tb_maintenance_group( title, wal_level )
     VALUES ( 'test', 'M' );

INSERT INTO pgctblmgr.tb_location( hostname, port, username, namespace )
     VALUES ('localhost', 5432, 'postgres', 'pg_ctblmgr_test' );

INSERT INTO pgctblmgr.tb_maintenance_object(
                maintenance_group,
                definition,
                namespace,
                name,
                driver,
                location,
                datamap
            )
     SELECT mg.maintenance_group,
            'WITH tt_test AS( SELECT a.foo FROM tb_a a ) SELECT a.foo, fn_test( a.foo ) AS bar, b.baz FROM tt_test a JOIN tb_b b ON b.foo = a.foo',
            'public',
            'ct_test',
            d.driver,
            l.location,
            '{"tb_c":{"schema":"public","masq":{"as":"tb_a","query":"SELECT c.foo FROM tb_c WHERE c.foo = $1"}}}'::JSONB
       FROM pgctblmgr.tb_maintenance_group mg
 INNER JOIN pgctblmgr.tb_location l
         ON l.hostname = 'localhost'
        AND l.port = 5432
        AND l.username = 'postgres'
        AND l.namespace = 'pg_ctblmgr_test'
 INNER JOIN pgctblmgr.tb_driver d
         ON d.name = 'postgresql'
      WHERE mg.title = 'test'
        AND mg.wal_level = 'M';
