DO
 $_$
BEGIN
    IF( pg_catalog.regexp_matches( version(), 'PostgreSQL (\d+)\.(\d+)\.?(\d+)?'::VARCHAR )::INTEGER[] < ARRAY[9,4]::INTEGER[] ) THEN
        RAISE EXCEPTION 'pg_ctblmgr requires PostgreSQL 9.4 or better';
    END IF;
END
 $_$
    LANGUAGE 'plpgsql';
