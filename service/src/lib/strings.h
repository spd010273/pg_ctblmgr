#ifndef STRINGS_H
#define STRINGS_H

const char * replication_check = "\
    SELECT plugin, \
           slot_type \
      FROM pg_replication_slots \
     WHERE slot_name = $1";

const char * replication_seek = "\
    SELECT location, \
           xid, \
           data \
      FROM pg_logical_slot_get_changes( \
               $1, \
               NULL, \
               NULL, \
               'wal-level', \
               $2, \
               'filter-tables' \
               $3 \
           ) ";

const char * get_worker_list = "\
    SELECT rs.name AS slot_name, \
           rs.filter, \
           mg.wal_level \
      FROM " EXTENSION_NAME ".__pgctblmgr_repl_slot rs \
INNER JOIN " EXTENSION_NAME ".tb_maintenance_object mo \
        ON mo.maintenance_object = rs.id \
INNER JOIN " EXTENSION_NAME ".tb_maintenance_group mg \
        ON mg.maintenance_group = mo.maintenance_group";

const char * extension_check_query = "\
    SELECT n.nspname AS ext_schema \
      FROM pg_catalog.pg_extension e \
INNER JOIN pg_catalog.pg_namespace n \
        ON n.oid = e.extnamespace \
     WHERE e.extname = $1";
#endif // STRINGS_H
