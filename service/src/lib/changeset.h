/*--------------------------------------------------------------------------
 *
 * changeset.h
 *     Internal structure storing WAL
 *
 * This structure stores differential records related to changes made in
 * the database we're replicating from.
 *
 * JSON coming from our replication slot is parsed and placed in this data
 * structure. A reference to this structure is then queued in the SLPQ
 * located at the end of the trie (see buffer.h for a diagram of this
 * structure) by the parent. When the child process has time, it'll dequeue
 * these changesets, and compute what the base table changes mean in the
 * context of the table they are managing.
 *
 * Copyright (c) 2019-2022, MerchLogix, Inc.
 *
 * IDENTIFICATION
 *          service/src/changeset.h
 *--------------------------------------------------------------------------
 */

#ifndef CHANGESET_H
#define CHANGESET_H

#define JSON_TOKENS 16

#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include "compiler.h"
#include "barrier.h"
#include "slab.h"

#include "xlog.h"
#define JSMN_HEADER
#include "jsmn/jsmn.h"

#define CHANGESET_CONTEXT_NAME "CHANGESET"
#define CHANGESET_STRING_CONTEXT_NAME "CHANGESET_STR"
#define CHANGESET_ARRAY_CONTEXT_NAME "CHANGESET_ARR"

typedef ref_t changeset_ref_t;
typedef ref_t changeset_string_ref_t;
typedef ref_t changeset_array_ref_t;

// TODO need locking mechanism for changesets
#define _CS_FREE(x,y) free_shared_memory(x,y)
#define _CS_ALLOC(x) create_shared_memory(x)
#define _CS_REALLOC(x,y,z) resize_shared_memory(x,y,z)

#define MIN(x,y) (x>y?y:x)

typedef enum {
    PGC_WAL_FULL,
    PGC_WAL_REDUCED,
    PGC_WAL_MINIMAL
} pg_ctblmgr_wal_level;

typedef enum {
    PGC_DML_UNINITIALIZED,
    PGC_DML_INSERT,
    PGC_DML_UPDATE,
    PGC_DML_DELETE,
    PGC_DML_TRUNCATE,
    PGC_DML_COMMIT,
    PGC_DML_BEGIN,
    PGC_DML_ROLLBACK
} pg_ctblmgr_dml_type;

struct changeset {
    uint64_t               lsn;
    changeset_array_ref_t  keys; // Array of changed keys
    changeset_array_ref_t  vals; // Array of the changed values
    uint16_t               num_keys; // Postgres can only have 1600 columns
    changeset_array_ref_t  columns;
    changeset_array_ref_t  new_vals;
    changeset_array_ref_t  old_vals;
    uint16_t               num_columns;
    changeset_string_ref_t schema_name;
    changeset_string_ref_t table_name;
    uint64_t               xid;
    pg_ctblmgr_dml_type    type;
    time_t                 timestamp;
};

extern changeset_ref_t json_to_changeset( char *, pg_ctblmgr_wal_level );
extern void free_changeset( changeset_ref_t );
extern void dump_changeset( changeset_ref_t );
extern bool initialize_changeset_context( void );
extern context_t get_changeset_array_context( void );
extern context_t get_changeset_context( void );
extern context_t get_changeset_string_context( void );

#endif // CHANGESET_H
