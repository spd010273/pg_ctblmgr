CREATE OR REPLACE FUNCTION @extschema@.fn_pgctblmgr_notify_change()
RETURNS TRIGGER AS
 $_$
DECLARE
    my_unique       VARCHAR;
    my_val          VARCHAR;
    my_record       RECORD;
    my_data         VARCHAR[];
    my_data_string  VARCHAR;
    my_rel          VARCHAR;
    my_schema       VARCHAR;
    my_channel      VARCHAR;
BEGIN
        SELECT n.nspname::VARCHAR,
               c.relname::VARCHAR
          INTO my_schema,
               my_rel
          FROM pg_class c
    INNER JOIN pg_namespace n
            ON n.oid = c.relnamespace
         WHERE c.oid = TG_RELID;

    IF( TG_OP = 'INSERT' ) THEN
        my_record := NEW;
    ELSIF( TG_OP = 'UPDATE' ) THEN
        my_record := NEW;
    ELSE
        my_record := OLD;
    END IF;

    FOR my_unique IN( SELECT unnest( TG_ARGV[0]::VARCHAR[] ) ) LOOP
        EXECUTE 'SELECT $1.' || my_unique || '::VARCHAR'
           INTO my_val
          USING my_record;

        my_data := array_append( my_data, '"' || my_unique || '":"' || my_val || '"' );
    END LOOP;

    my_data_string := '{"xid":' || txid_current()::BIGINT
                   || ',"data":{'
                   || '"schema_name":"' || my_schema
                   || '","table_name":"' || my_rel
                   || '","key":{' || array_to_string( my_data, ',' ) || '}}}';
    FOR my_channel IN(
                        SELECT maintenance_channel
                          FROM @extschema@.__pgctblmgr_repl_slot
                         WHERE ARRAY[ my_schema || '.' || my_rel ]::VARCHAR[] <@ filter
                     ) LOOP
        EXECUTE 'NOTIFY ' || my_channel || ',''' || my_data_string || '''';
    END LOOP;

    RETURN my_record;
END
 $_$
    LANGUAGE 'plpgsql' VOLATILE PARALLEL UNSAFE;
