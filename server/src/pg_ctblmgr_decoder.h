#ifndef PG_CTBLMGR_DECODER_H
#define PG_CTBLMGR_DECODER_H

#include "postgres.h"
#include "miscadmin.h"
#include "replication/logical.h"
#include "replication/output_plugin.h"
#include "access/genam.h"
#include "access/sysattr.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "nodes/parsenodes.h"
#include "utils/typcache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/guc_tables.h"
#include "utils/guc.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

// XID interface
#include "access/xlog.h"
#include "access/xact.h"
#include "access/transam.h"

// Defined in src/backend/utils/adt/txid.c
/*
typedef uint64 txid;
typedef struct {
    TransactionId last_xid;
    uint32        epoch;
} TxidEpoch;

// GUC Records
typedef struct guc_change {
    const char *    name;
    const char *    value;
    pid_t           backend_pid;
    TransactionId * xid;
};

extern bool should_forward_guc_to_wal( const char * );
*/

enum pg_ctblmgr_wal_level {
    PGC_WAL_FULL,
    PGC_WAL_REDUCED,
    PGC_WAL_MINIMAL
};

extern void _PG_init( void );
extern void PGDLLEXPORT _PG_output_plugin_init( OutputPluginCallbacks * );

const char * transaction_boundary_full = "{\
\"type\":\"%s\",\
\"xid\":\"%u\",\
\"timestamp\":\"%s\"\
}";

const char * transaction_boundary_reduced = "{\
\"type\":\"%s\",\
\"xid\":\"%u\"\
}";

const char * transaction_boundary_minimal = "{\
\"x\":\"%u\"\
}";

const char * dml_preamble_full = "{\
\"type\":\"%s\",\
\"xid\":\"%u\",\
\"timestamp\":\"%s\",\
\"schema_name\":\"%s\",\
\"table_name\":\"%s\",";

const char * dml_preamble_reduced = "{\
\"type\":\"%s\",\
\"xid\":\"%u\",\
\"schema_name\":\"%s\",\
\"table_name\":\"%s\",";

const char * dml_preamble_minimal = "{\
\"x\":\"%u\",\
\"s\":\"%s\",\
\"t\":\"%s\",";


typedef struct {
    MemoryContext               context;
    unsigned long               num_changes;
    bool                        wrote_tx_changes;
    List *                      filter_tables;
    enum pg_ctblmgr_wal_level   wal_level;
    bool                        include_transaction;
    bool                        enable_data_write;
} decode_data;


struct pgc_table {
    char * schema_name;
    char * table_name;
    bool   all_tables;
    bool   all_schemas;
};

static void pg_ctblmgr_decode_startup(
    LogicalDecodingContext *,
    OutputPluginOptions *,
    bool
);

static void pg_ctblmgr_decode_shutdown( LogicalDecodingContext * );
static void pg_ctblmgr_decode_begin_tx(
    LogicalDecodingContext *,
    ReorderBufferTXN *
);

static void pg_ctblmgr_decode_commit_tx(
    LogicalDecodingContext *,
    ReorderBufferTXN *,
    XLogRecPtr
);

#if PG_VERSION_NUM >= 90600
static void pg_ctblmgr_decode_message(
    LogicalDecodingContext *,
    ReorderBufferTXN *,
    XLogRecPtr,
    bool,
    const char *,
    Size,
    const char *
);
#endif // PG_VERSION_NUM

#if PG_VERSION_NUM >= 110000
static void pg_ctblmgr_decode_truncate(
    LogicalDecodingContext *,
    ReorderBufferTXN *,
    int,
    Relation[],
    ReorderBufferChange *
);
#endif // PG_VERSION_NUM
static void pg_ctblmgr_decode_change(
    LogicalDecodingContext *,
    ReorderBufferTXN *,
    Relation,
    ReorderBufferChange *
);
static void append_tuple_value(
    StringInfo,
    TupleDesc,
    HeapTuple,
    unsigned int
);

static void append_literal_value( StringInfo, Oid, char * );
static void append_tuple( StringInfo, TupleDesc, HeapTuple );
static bool config_to_filter_table( char *, List ** );
static bool parse_table_identifier( List *, List ** );
#endif // PG_CTBLMGR_DECODER_H
