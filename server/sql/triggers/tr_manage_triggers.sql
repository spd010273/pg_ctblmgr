CREATE TRIGGER tr_manage_triggers
    AFTER INSERT OR DELETE OR UPDATE OF filter ON @extschema@.__pgctblmgr_repl_slot
    FOR EACH ROW EXECUTE PROCEDURE @extschema@.fn_manage_triggers();
