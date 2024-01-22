CREATE OR REPLACE FUNCTION @extschema@.fn_manage_triggers()
RETURNS TRIGGER AS
 $_$
DECLARE
    my_rel      VARCHAR;
    my_table_pk VARCHAR;
    my_record   RECORD;
BEGIN
    PERFORM pid
       FROM pg_catalog.pg_stat_activity
      WHERE datname = current_database()
        AND application_name = 'pg_restore';

    IF FOUND THEN
        -- Do nothing during restore
        RETURN NEW;
    END IF;

    IF( TG_OP = 'INSERT' ) THEN
        my_record := NEW;
        FOR my_rel IN( SELECT unnest( NEW.filter ) ) LOOP
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
                         || '    FOR EACH ROW EXECUTE PROCEDURE @extschema@.fn_pgctblmgr_notify_change( %L )',
                            my_table_pk
                        );
            END IF;
        END LOOP;
    ELSIF( TG_OP = 'UPDATE' ) THEN
        IF( NEW.filter::VARCHAR IS NOT DISTINCT FROM OLD.filter::VARCHAR ) THEN
            RETURN NEW;
        END IF;
    
        my_record := NEW;

        FOR my_rel IN(
                        WITH tt_all AS
                        (
                            SELECT array_agg( DISTINCT x ) AS arr
                              FROM @extschema@.__pgctblmgr_repl_slot rs
                              JOIN unnest( rs.filter ) x
                                ON TRUE
                        )
                            SELECT x
                              FROM unnest( OLD.filter ) x
                             WHERE NOT ARRAY[ x ]  @> NEW.filter
                               AND NOT ARRAY[ x ] <@ ( SELECT arr FROM tt_all )
                     ) LOOP 
            EXECUTE 'DROP TRIGGER IF EXISTS tr_pgctblmgr_notify_change ON ' || my_rel;
        END LOOP;

        FOR my_rel IN( SELECT x FROM unnest( NEW.filter ) x WHERE NOT ARRAY[ x ] @> OLD.filter ) LOOP
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
                         || '    FOR EACH ROW EXECUTE PROCEDURE @extschema@.fn_pgctblmgr_notify_change( %L )',
                            my_table_pk
                        );
            END IF;
        END LOOP;
    ELSE
        my_record := OLD;

        FOR my_rel IN(
                        WITH tt_all AS
                        (
                            SELECT array_agg( DISTINCT x ) AS arr
                              FROM @extschema@.__pgctblmgr_repl_slot rs
                              JOIN unnest( rs.filter ) x
                                ON TRUE
                        )
                            SELECT x
                              FROM unnest( OLD.filter ) x
                             WHERE NOT ARRAY[ x ] <@ ( SELECT arr FROM tt_all )
                     ) LOOP 
           EXECUTE 'DROP TRIGGER IF EXISTS tr_pgctblmgr_notify_change ON ' || my_rel;
        END LOOP;
    END IF;

    RETURN my_record;
END
 $_$
    LANGUAGE plpgsql VOLATILE PARALLEL UNSAFE;
