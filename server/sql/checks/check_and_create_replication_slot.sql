DO
 $_$
DECLARE
    my_current_count INTEGER := 0;
    my_enabled_count INTEGER := 0;
    my_wal_level     VARCHAR := 0;
BEGIN
    SELECT COUNT(*)
      INTO my_current_count
      FROM pg_catalog.pg_replication_slots
     WHERE slot_type = 'logical';

    SELECT current_setting( 'max_replication_slots' )
      INTO my_enabled_count;

    IF( my_current_count > 0 AND my_current_count = my_enabled_count ) THEN
        RAISE EXCEPTION 'No available logical replication slots. Please increase max_replication_slots by at least 1';
    END IF;

    IF( my_enabled_count = 0 ) THEN
        RAISE EXCEPTION 'No available logical replication slots. max_replication_slots must be >= 0 with at least one available slot';
    END IF;

    SELECT current_setting( 'wal_level' )
      INTO my_wal_level;
    
    IF( my_wal_level != 'logical' ) THEN
        RAISE EXCEPTION 'A WAL Level of ''logical'' is required for logical decoding';
    END IF;
    
    PERFORM slot_name
       FROM pg_replication_slots
      WHERE slot_type = 'logical'
        AND plugin = 'pg_ctblmgr'
        AND database::VARCHAR = current_database();

    IF FOUND THEN
        RAISE EXCEPTION 'There is already a pg_ctblmgr replication slot for this database';
    END IF;
END
 $_$
    LANGUAGE 'plpgsql';

-- We'll have the service control this
--SELECT pg_create_logical_replication_slot( '__pg_ctblmgr', 'pg_ctblmgr' );
