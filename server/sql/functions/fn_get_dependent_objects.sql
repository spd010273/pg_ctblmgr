CREATE OR REPLACE FUNCTION public.fn_get_dependent_tables( in_query TEXT )
RETURNS TABLE
(
    table_name  VARCHAR,
    column_name VARCHAR[]
)
AS
 $_$
BEGIN
    /*
        Given a query, this function attempts to find the tables used in it for
        pg_ctblmgr. For each of these tables, we find and return, with the
        descending precedence, the columns used for the following:
        - Primary key for the table
        - Unique constraint
        - Unique index
     */
    EXECUTE 'CREATE TEMP VIEW tt_vw_column_check AS( ' || in_query || ' )';
    RETURN QUERY
    WITH tt_pk_locator AS
    (
        SELECT c.relname::VARCHAR AS table_name,
               array_agg( DISTINCT con_a_att.attname::VARCHAR ) AS primary,
               array_agg( DISTINCT con_b_att.attname::VARCHAR ) AS secondary,
               array_agg( DISTINCT con_c_att.attname::VARCHAR ) AS tertiary
          FROM pg_depend d
    INNER JOIN pg_rewrite rw
            ON rw.oid = d.objid
    INNER JOIN pg_class c_v
            ON c_v.oid = rw.ev_class
           AND c_v.relname = 'tt_vw_column_check'
           AND c_v.relkind = 'v'
    INNER JOIN pg_namespace n
            ON n.oid = c_v.relnamespace
           AND n.oid = pg_my_temp_schema()
    INNER JOIN pg_class c
            ON c.oid = d.refobjid
    INNER JOIN pg_attribute a
            ON a.attrelid = c.oid
           AND a.attnum = d.refobjsubid
    INNER JOIN pg_class c_f
            ON c_f.relname = 'pg_class'
           AND c_f.oid = d.refclassid
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
      GROUP BY c.relname::VARCHAR
    )
        SELECT tt.table_name,
               COALESCE(
                   NULLIF( tt.primary, ARRAY[NULL]::VARCHAR[] ),
                   NULLIF( tt.secondary, ARRAY[NULL]::VARCHAR[] ),
                   NULLIF( tt.tertiary, ARRAY[NULL]::VARCHAR[] )
               ) AS column_name
          FROM tt_pk_locator tt;
    DROP VIEW tt_vw_column_check;
END
 $_$
    LANGUAGE 'plpgsql' VOLATILE;
