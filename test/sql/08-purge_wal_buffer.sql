SELECT *
  FROM pg_logical_slot_get_changes( '__pg_ctblmgr_public_ct_test', NULL, NULL );
