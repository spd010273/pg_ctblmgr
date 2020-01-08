CREATE TEMP TABLE tt_all_xact AS
(
    SELECT lsn,
           xid,
           data::JSONB AS output
      FROM pg_catalog.pg_logical_slot_peek_changes(
               '__pg_ctblmgr_public_ct_test',
               NULL,
               NULL
           )
);

SELECT 21::FLOAT / CASE WHEN COUNT(*) != 7 THEN 0 ELSE COUNT(*) END
  FROM tt_all_xact tt
 WHERE output->>'schema_name' = 'public'
   AND output->>'table_name' = 'tb_a';

SELECT 21::FLOAT / CASE WHEN COUNT(*) != 7 THEN 0 ELSE COUNT(*) END
  FROM tt_all_xact tt
 WHERE output->>'schema_name' = 'public'
   AND output->>'table_name' = 'tb_b';
