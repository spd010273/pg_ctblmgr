CREATE TABLE IF NOT EXISTS @extschema@.__pgctblmgr_repl_slot
(
    id                  INTEGER NOT NULL,
    maintenance_channel VARCHAR NOT NULL,
    filter              VARCHAR[] NOT NULL,
    UNIQUE( id )
);

COMMENT ON TABLE @extschema@.__pgctblmgr_repl_slot IS 'Stores mapping of replication slots to their accompanying maintenance_objects';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.id IS 'The maintenance_object PK that this replication slot maps to';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.maintenance_channel IS 'Name of the LISTEN/NOTIFY maintenance channel used to notify workers of changes to the object';
COMMENT ON COLUMN @extschema@.__pgctblmgr_repl_slot.filter IS 'array of base objects for this table';
