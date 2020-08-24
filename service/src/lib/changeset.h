#ifndef CHANGESET_H
#define CHANGESET_H

#define JSON_TOKENS 16

#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include "util.h"
#include "xlog.h"
#define JSMN_HEADER
#include "jsmn/jsmn.h"

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
    uint64_t            lsn;
    char **             keys;
    char **             vals;
    unsigned int        num_keys;
    char **             columns;
    char **             new_vals;
    char **             old_vals;
    unsigned int        num_columns;
    char *              schema_name;
    char *              table_name;
    unsigned long int   xid;
    pg_ctblmgr_dml_type type;
    time_t              timestamp;
};

extern struct changeset * json_to_changeset( char *, pg_ctblmgr_wal_level );
extern void free_changeset( struct changeset * );
extern void dump_changeset( struct changeset * );
#endif // CHANGESET_H
