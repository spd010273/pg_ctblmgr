#ifndef PG_CTBLMGR_DECODER_H
#define PG_CTBLMGR_DECODER_H

#include "postgres.h"
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

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

extern void _PG_init( void );
extern void PGDLLEXPORT _PG_output_plugin_init( OutputPluginCallbacks * );

const char * transaction_boundary = "{\
\"type\":\"%s\",\
\"xid\":\"%u\",\
\"timestamp\":\"%s\"\
}";

const char * dml_preamble = "{\
\"type\":\"%s\",\
\"xid\":\"%u\",\
\"timestamp\":\"%s\",\
\"schema_name\":\"%s\",\
\"table_name\":\"%s\",";

typedef struct {
    MemoryContext context;
    unsigned long num_changes;
    bool          wrote_tx_changes;
} decode_data;

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
#endif

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
#endif // PG_CTBLMGR_DECODER_H
