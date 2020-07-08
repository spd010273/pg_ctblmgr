#ifndef CHANGESET_H
#define CHANGESET_H

#define JSON_TOKENS 16

#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "util.h"
#define JSMN_HEADER
#include "jsmn/jsmn.h"

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
    PGC_DML_TX_BARRIER
} pg_ctblmgr_dml_type;

struct changeset {
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

struct changeset * json_to_changeset( char *, pg_ctblmgr_wal_level );

#endif // CHANGESET_H
