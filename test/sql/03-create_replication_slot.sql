WITH tt_remove_slots AS
(
    SELECT rs.slot_name
      FROM pg_catalog.pg_replication_slots rs 
INNER JOIN pgctblmgr.__pgctblmgr_repl_slot pgcrs 
        ON rs.slot_name::VARCHAR = pgcrs.name 
)
    SELECT pg_catalog.pg_drop_replication_slot( tt.slot_name )
      FROM tt_remove_slots tt;

SELECT x.*
  FROM pgctblmgr.__pgctblmgr_repl_slot rs
  JOIN pg_catalog.pg_create_logical_replication_slot( rs.name, 'pg_ctblmgr' ) x
    ON TRUE;
