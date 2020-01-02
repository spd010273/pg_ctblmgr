CREATE OR REPLACE FUNCTION @extschema@.fn_notify_service
(
    in_command   VARCHAR,
    in_object_id INTEGER
)
RETURNS VOID AS
 $_$
    SELECT pg_notify( '__pg_ctblmgr', in_command )
     WHERE in_command IN(
               'new_table'
           );
 $_$
    LANGUAGE SQL VOLATILE PARALLEL UNSAFE;
