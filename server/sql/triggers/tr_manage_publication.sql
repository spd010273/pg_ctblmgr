CREATE TRIGGER tr_manage_publication
    AFTER INSERT OR DELETE OR UPDATE OF definition, namespace, name
    ON @extschema@.tb_maintenance_object
    FOR EACH ROW EXECUTE PROCEDURE @extschema@.fn_manage_publication;
