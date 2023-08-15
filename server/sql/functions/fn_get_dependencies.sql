/*
 * This function feeds the tables used by a maintenance object to
 * __pgctblmgr_repl_slot.filter. Additionally, we set or upgrade the replica
 * identity of the table so that pg_ctblmgr can extract approprate information
 * about modified tuples from WAL. We try to be mindful that a replica ident
 * already exists, and dont 'downgrade'
 */
CREATE OR REPLACE FUNCTION @extschema@.fn_get_dependencies
(
    in_maintenance_object INTEGER
)
RETURNS VARCHAR[] AS
 $_$
DECLARE
    my_query             TEXT;
    my_result            VARCHAR[];
    my_schema            VARCHAR;
    my_table             VARCHAR;
    my_current_replident VARCHAR;
    my_desired_replident VARCHAR;
    my_index_name        VARCHAR;
BEGIN
    SELECT definition
      INTO my_query
      FROM @extschema@.tb_maintenance_object
     WHERE maintenance_object = in_maintenance_object;

    IF( my_query IS NULL ) THEN
        RAISE EXCEPTION 'Maintenance object % doesn''t exist or has no definition', in_maintenance_object;
    END IF;

    EXECUTE 'CREATE TEMP VIEW tt_vw_column_check AS( ' || my_query || ')';
    FOR my_schema, my_table IN(
    WITH tt_pk_locator AS
    (
        SELECT c.oid,
			   c_n.nspname::VARCHAR AS schema_name,
               c.relname::VARCHAR AS table_name,
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
    INNER JOIN pg_namespace c_n
            ON c_n.oid = c.relnamespace
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
      GROUP BY c_n.nspname::VARCHAR,
               c.oid
    ),
    tt_dependencies AS
    (
		WITH RECURSIVE tt_inherited_dependencies AS
		(
			SELECT inh.inhrelid AS oid
			  FROM tt_pk_locator tt
		INNER JOIN pg_inherits inh
				ON inh.inhparent = tt.oid
			 UNION
			SELECT inh.inhrelid AS oid
			  FROM tt_inherited_dependencies tt
		INNER JOIN pg_inherits inh
				ON inh.inhparent = tt.oid
		)
			SELECT n.nspname::VARCHAR AS schema_name,
				   c.relname::VARCHAR AS table_name
			  FROM tt_inherited_dependencies tt
		INNER JOIN pg_class c
				ON c.oid = tt.oid
		INNER JOIN pg_namespace n
				ON n.oid = c.relnamespace
			 UNION
			SELECT tt.schema_name,
				   tt.table_name
			  FROM tt_pk_locator tt
    )
        SELECT schema_name,
               table_name
          FROM tt_dependencies tt
                              ) LOOP
        my_result := my_result::VARCHAR[] || ( quote_ident( my_schema ) || '.' || quote_ident( my_table ) )::VARCHAR;
        -- Verify that the table has a unique constraint or
        -- some kind of replica identity set
         SELECT CASE WHEN c.relreplident = 'n' AND c_pk.oid IS NOT NULL
                     THEN 'd'
                     WHEN c.relreplident = 'n' AND c_pk.oid IS NULL AND c_new_rid.oid IS NOT NULL
                     THEN 'i'
                     WHEN c.relreplident = 'd' AND c_pk.oid IS NULL AND c_new_rid.oid IS NOT NULL
                     THEN 'i'
                     WHEN c.relreplident = 'd' AND c_pk.oid IS NOT NULL
                     THEN 'd'
                     WHEN c.relreplident = 'i' AND c_rid.oid IS NOT NULL
                     THEN 'i'
                     ELSE 'f'
                      END AS desired_replident,
                CASE WHEN c.relreplident = 'n' AND c_pk.oid IS NOT NULL
                     THEN c_pk.relname::VARCHAR
                     WHEN c.relreplident = 'n' AND c_pk.oid IS NULL AND c_new_rid.oid IS NOT NULL
                     THEN c_new_rid.relname::VARCHAR
                     WHEN c.relreplident = 'd' AND c_pk.oid IS NULL AND c_new_rid.oid IS NOT NULL
                     THEN c_new_rid.relname::VARCHAR
                     WHEN c.relreplident = 'd' AND c_pk.oid IS NOT NULL
                     THEN c_pk.relname::VARCHAR
                     WHEN c.relreplident = 'i' AND c_rid.oid IS NOT NULL
                     THEN c_rid.relname::VARCHAR
                     ELSE NULL
                      END AS indexname,
                c.relreplident AS current_replident
           INTO my_desired_replident,
                my_index_name,
                my_current_replident
           FROM pg_class c
     INNER JOIN pg_namespace n
             ON n.oid = c.relnamespace
      LEFT JOIN (
                      pg_index i_rid
                 JOIN pg_class c_rid
                   ON i_rid.indexrelid = c_rid.oid
                )
             ON i_rid.indisreplident IS TRUE
            AND i_rid.indrelid = c.oid
      LEFT JOIN (
                      pg_index i_new_rid
                 JOIN pg_class c_new_rid
                   ON i_new_rid.indexrelid = c_new_rid.oid
                )
             ON i_new_rid.indrelid = c.oid
            AND i_new_rid.indpred IS NULL
            AND i_new_rid.indisunique IS TRUE
            AND i_new_rid.indimmediate IS TRUE
            AND i_new_rid.indnatts > 0
            AND i_new_rid.indisreplident IS FALSE
      LEFT JOIN (
                      pg_index i_pk
                 JOIN pg_class c_pk
                   ON i_pk.indexrelid = c_pk.oid
                )
             ON i_pk.indrelid = c.oid
            AND i_pk.indisprimary IS TRUE
          WHERE c.relname::VARCHAR = my_table
            AND n.nspname::VARCHAR = my_schema
            AND c.relkind IN( 'r', 'm' );
        IF( my_desired_replident IS NULL ) THEN
            EXECUTE 'ALTER TABLE ' || quote_ident( my_schema ) || '.' || quote_ident( my_table )
                 || ' REPLICA IDENTITY FULL ';
        ELSIF( my_desired_replident != my_current_replident ) THEN
            EXECUTE 'ALTER TABLE ' || quote_ident( my_schema ) || '.' || quote_ident( my_table )
                 || ' REPLICA IDENTITY ' || ( CASE WHEN my_desired_replident = 'd' THEN 'DEFAULT'
                                                   WHEN my_desired_replident = 'f' THEN 'FULL'
                                                   WHEN my_desired_replident = 'i' THEN 'USING INDEX ' || quote_ident( my_index_name )
                                                   ELSE 'FULL'
                                                    END );
            RAISE NOTICE 'REPLICA IDENTITY for %.% updated from % to %',
                my_schema,
                my_table,
                CASE WHEN my_current_replident IS NULL THEN 'NOTHING'
                     WHEN my_current_replident = 'n' THEN 'NOTHING'
                     WHEN my_current_replident = 'd' THEN 'DEFAULT'
                     WHEN my_current_replident = 'f' THEN 'FULL'
                     WHEN my_current_replident = 'i' THEN 'INDEX'
                     ELSE 'N/A'
                      END,
                CASE WHEN my_desired_replident IS NULL THEN 'NOTHING'
                     WHEN my_desired_replident = 'n' THEN 'NOTHING'
                     WHEN my_desired_replident = 'd' THEN 'DEFAULT'
                     WHEN my_desired_replident = 'f' THEN 'FULL'
                     WHEN my_desired_replident = 'i' THEN 'INDEX'
                     ELSE 'N/A'
                      END;
        END IF;
    END LOOP;

    DROP VIEW tt_vw_column_check;
    RETURN my_result;
END
 $_$
    LANGUAGE 'plpgsql' VOLATILE PARALLEL UNSAFE;
