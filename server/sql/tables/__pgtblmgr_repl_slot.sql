CREATE TABLE IF NOT EXISTS @extschema@.__pgctblmgr_repl_slot
(
    id      INTEGER NOT NULL,
    name    VARCHAR NOT NULL,
    filter  VARCHAR NOT NULL,
    UNIQUE( id )
);

COMMENT ON TABLE @extschema@.__pgctblmgr_repl_slot IS 'Stores mapping of replication slots to their accompanying maintenance_objects';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.id IS 'The maintenance_object PK that this replication slot maps to';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.name IS 'Name of the replication slot and LISTEN/NOTIFY maintenance channel';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.filter IS 'Comma-delimited list of base objects for this table';
