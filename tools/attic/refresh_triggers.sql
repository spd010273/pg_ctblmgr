DO
 $_$
DECLARE
    my_rel VARCHAR;
    my_table_pk VARCHAR[];
BEGIN
    FOR my_rel IN(
        WITH tt_installed AS
        (
            SELECT c.oid
              FROM pg_class c
              JOIN pg_trigger t
                ON t.tgname = 'tr_pgctblmgr_notify_change'
               AND t.tgrelid = c.oid
        ),
        tt_all AS
        (
            SELECT DISTINCT x
              FROM pgctblmgr.__pgctblmgr_repl_slot rs
              JOIN unnest( rs.filter ) x
                ON TRUE
        )
            SELECT n_i.nspname::VARCHAR || '.' || c_i.relname::VARCHAR AS rel
              FROM tt_installed i
              JOIN pg_class c_i
                ON c_i.oid = i.oid
              JOIN pg_namespace n_i
                ON n_i.oid = c_i.relnamespace
         LEFT JOIN (
                          pg_class c
                     JOIN pg_namespace n
                       ON n.oid = c.relnamespace
                     JOIN tt_all a
                       ON a.x = n.nspname::VARCHAR || '.' || c.relname::VARCHAR
                   )
                ON c.oid = i.oid
             WHERE c.oid IS NULL
                     ) LOOP
            EXECUTE 'DROP TRIGGER IF EXISTS tr_pgctblmgr_notify_change ON ' || my_rel;
    END LOOP;
    FOR my_rel IN(
        WITH tt_needed AS
        (
            WITH tt_all AS
            (
                SELECT DISTINCT x
                  FROM pgctblmgr.__pgctblmgr_repl_slot rs
                  JOIN unnest( rs.filter ) x
                    ON TRUE
            )
                SELECT c.oid
                  FROM pg_class c
                  JOIN pg_namespace n
                    ON n.oid = c.relnamespace
                  JOIN tt_all a
                    ON a.x = n.nspname::VARCHAR || '.' || c.relname::VARCHAR
        )
            SELECT n_n.nspname::VARCHAR || '.' || n_c.relname::VARCHAR AS rel
              FROM tt_needed n
              JOIN pg_class n_c
                ON n_c.oid = n.oid
              JOIN pg_namespace n_n
                ON n_n.oid = n_c.relnamespace
         LEFT JOIN (
                         pg_class c
                    JOIN pg_trigger t
                      ON t.tgname = 'tr_pgctblmgr_notify_change'
                     AND t.tgrelid = c.oid
                   )
                ON c.oid = n.oid
            ) LOOP
        WITH tt_pks AS
        (
            SELECT c.oid,
                   c_n.nspname::VARCHAR AS schema_name,
                   c.relname::VARCHAR AS table_name,
                   NULLIF( array_agg( DISTINCT con_a_att.attname::VARCHAR ), ARRAY[ NULL ]::VARCHAR[] ) AS primary,
                   NULLIF( array_agg( DISTINCT con_b_att.attname::VARCHAR ), ARRAY[ NULL ]::VARCHAR[] ) AS secondary,
                   NULLIF( array_agg( DISTINCT con_c_att.attname::VARCHAR ), ARRAY[ NULL ]::VARCHAR[] ) AS tertiary
              FROM pg_class c
        INNER JOIN pg_namespace c_n
                ON c_n.oid = c.relnamespace
        INNER JOIN pg_attribute a
                ON a.attrelid = c.oid
        INNER JOIN pg_class c_f
                ON c_f.relname = 'pg_class'
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
             WHERE c_n.nspname || '.' || c.relname = my_rel
          GROUP BY c_n.nspname::VARCHAR,
                   c.oid
        )
            SELECT COALESCE( "primary", secondary, tertiary )
              INTO my_table_pk
              FROM tt_pks;

        IF( my_table_pk IS NULL ) THEN
            RAISE NOTICE 'Relation % will miss updates - no primary or unique key', my_rel;
        ELSE
            EXECUTE format(
                        'CREATE OR REPLACE TRIGGER tr_pgctblmgr_notify_change '
                     || '    AFTER INSERT OR DELETE OR UPDATE ON ' || my_rel
                     || '    FOR EACH ROW EXECUTE PROCEDURE pgctblmgr.fn_pgctblmgr_notify_change( %L )',
                        my_table_pk
                    );
        END IF;
    END LOOP;
END
 $_$
    LANGUAGE plpgsql;
