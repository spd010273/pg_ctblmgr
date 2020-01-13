/*
 * This trigger maintains the state of logical replication slots used to feed
 * changes made in WAL to pg_ctblmgr forked processes.
 */

CREATE OR REPLACE FUNCTION @extschema@.fn_manage_publication()
RETURNS TRIGGER AS
 $_$
BEGIN
    IF( TG_OP = 'UPDATE' ) THEN
        IF(
                NEW.definition IS NOT DISTINCT FROM OLD.definition
            AND NEW.namespace IS NOT DISTINCT FROM OLD.namespace
            AND NEW.name IS NOT DISTINCT FROM OLD.name
          ) THEN
            -- Avoid dummy updates
            RETURN NEW;
        END IF;

        IF(
                NEW.namespace IS DISTINCT FROM OLD.namespace
             OR NEW.name IS DISTINCT FROM OLD.name
          ) THEN
            /* Prevent renaming of a resource, this would change the replication slot name
               and detach the worker from its WAL source */
            RAISE EXCEPTION 'Cannot rename a replication slot for %.% - you'
                            ' need to drop this object then create it',
                            NEW.namespace,
                            NEW.name;
        END IF;
    ELSIF( TG_OP = 'DELETE' ) THEN
        PERFORM @extschema@.fn_notify_maintenance_channel(
            OLD.maintenance_object,
            'object_remove'
        );
        DELETE FROM @extschema@.__pgctblmgr_repl_slot
              WHERE id = OLD.maintenance_object;
        RETURN OLD;
    END IF;

    INSERT INTO @extschema@.__pgctblmgr_repl_slot
                (
                    id,
                    maintenance_channel,
                    filter
                )
         VALUES
                (
                    NEW.maintenance_object,
                    @extschema@.fn_get_maintenance_channel_name(
                        NEW.namespace,
                        NEW.name
                    ),
                    @extschema@.fn_get_dependencies( NEW.maintenance_object )
                );
    RETURN NEW;
END
 $_$
    LANGUAGE 'plpgsql' VOLATILE PARALLEL UNSAFE;
