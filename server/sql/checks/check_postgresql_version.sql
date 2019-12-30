DO
 $_$
BEGIN
    IF( regexp_replace( version(), 'PostgreSQL (\d+)\.(\d+)\.?(\d+)?' )::INTEGER[] < ARRAY[9,4]::INTEGER[] ) THEN
        RAISE EXCEPTION 'pg_ctblmgr requires PostgreSQL 9.4 or better';
    END IF;
END
 $_$
    LANGUAGE 'plpgsql';
