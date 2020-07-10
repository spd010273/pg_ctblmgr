#ifndef STRINGS_H
#define STRINGS_H

const char * replication_check = "\
    SELECT plugin, \
           slot_type \
      FROM pg_catalog.pg_replication_slots \
     WHERE slot_name = $1";

const char * replication_slot_create = "\
    SELECT * \
      FROM pg_catalog.pg_create_logical_replication_slot( \
               '__pg_ctblmgr', \
               'pg_ctblmgr' \
           )";

const char * replication_slot_destroy = "\
    SELECT pg_catalog.pg_drop_replication_slot( '__pg_ctblmgr' )";

const char * get_distinct_filter_tables = "\
    SELECT DISTINCT x AS filter_table \
      FROM " EXTENSION_SCHEMA ".__pgctblmgr_repl_slot rs \
INNER JOIN unnest( rs.filter ) x \
        ON TRUE ";

const char * get_slot_filter_tables = "\
    SELECT DISTINCT x AS filter_table \
      FROM " EXTENSION_SCHEMA ".__pgctblmgr_repl_slot rs \
INNER JOIN unnest( rs.filter ) x \
        ON TRUE \
     WHERE rs.maintenance_channel = $1";

const char * replication_seek = "\
    SELECT lsn, \
           xid, \
           data::JSONB AS data \
      FROM pg_catalog.pg_logical_slot_get_changes( \
               $1, \
               $2, \
               NULL, \
               'wal-level', \
               $3, \
               'filter-tables', \
               $4, \
               'include-transaction', \
               'TRUE' \
           ) ";

const char * replication_peek = "\
    SELECT lsn, \
           xid, \
           data::JSONB AS data \
      FROM pg_catalog.pg_logical_slot_peek_changes( \
               $1, \
               NULL, \
               NULL, \
               'wal-level', \
               $2, \
               'filter-tables', \
               $3, \
               'include-transaction', \
               'TRUE' \
           ) ";

const char * get_worker_list = "\
    SELECT rs.maintenance_channel, \
           rs.filter, \
           mg.wal_level \
      FROM " EXTENSION_SCHEMA ".__pgctblmgr_repl_slot rs \
INNER JOIN " EXTENSION_SCHEMA ".tb_maintenance_object mo \
        ON mo.maintenance_object = rs.id \
INNER JOIN " EXTENSION_SCHEMA ".tb_maintenance_group mg \
        ON mg.maintenance_group = mo.maintenance_group";

const char * extension_check_query = "\
    SELECT n.nspname AS ext_schema \
      FROM pg_catalog.pg_extension e \
INNER JOIN pg_catalog.pg_namespace n \
        ON n.oid = e.extnamespace \
     WHERE e.extname = $1";

#endif // STRINGS_H
