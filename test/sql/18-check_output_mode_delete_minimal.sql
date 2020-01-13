CREATE TEMP TABLE tt_all_xact AS
(
    SELECT lsn,
           xid,
           data::JSONB AS output
      FROM pg_catalog.pg_logical_slot_peek_changes(
               '__pg_ctblmgr',
               NULL,
               NULL,
               'wal-level',
               'M'
           )
);

SELECT 21::FLOAT / CASE WHEN COUNT(*) != 7 THEN 0 ELSE COUNT(*) END
  FROM tt_all_xact tt
 WHERE output->>'s' = 'public'
   AND output->>'t' = 'tb_a'
   AND output->>'d' = 'D'
   AND output->>'key' NOT ILIKE '%error%';

SELECT 21::FLOAT / CASE WHEN COUNT(*) != 7 THEN 0 ELSE COUNT(*) END
  FROM tt_all_xact tt
 WHERE output->>'s' = 'public'
   AND output->>'t' = 'tb_b'
   AND output->>'d' = 'D'
   AND output->>'key' NOT ILIKE '%error%';

SELECT 21::FLOAT / CASE WHEN COUNT(*) != 7 THEN 0 ELSE COUNT(*) END
  FROM tt_all_xact tt
 WHERE output->>'s' = 'public'
   AND output->>'t' = 'tb_c'
   AND output->>'d' = 'D'
   AND output->>'key' NOT ILIKE '%error%';

SELECT 21::FLOAT / CASE WHEN COUNT(*) != 7 THEN 0 ELSE COUNT(*) END
  FROM tt_all_xact tt
 WHERE output->>'s' = 'public'
   AND output->>'t' = 'tb_d'
   AND output->>'d' = 'D'
   AND output->>'key' NOT ILIKE '%error%';

SELECT 21::FLOAT / CASE WHEN COUNT(*) != 7 THEN 0 ELSE COUNT(*) END
  FROM tt_all_xact tt
 WHERE output->>'s' = 'public'
   AND output->>'t' = 'tb_e'
   AND output->>'d' = 'D'
   AND output->>'key' NOT ILIKE '%error%';

SELECT 21::FLOAT / CASE WHEN COUNT(*) != 7 THEN 0 ELSE COUNT(*) END
  FROM tt_all_xact tt
 WHERE output->>'s' = 'public'
   AND output->>'t' = 'tb_f'
   AND output->>'d' = 'D';

DROP TABLE tt_all_xact;
