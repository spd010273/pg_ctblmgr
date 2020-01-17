#ifndef CHANGESET_H
#define CHANGESET_H

#define JSON_TOKENS 16

#include <stdlib.h>
#include "jsmn/jsmn.h"

enum pg_ctblmgr_wal_level {
    PGC_WAL_FULL,
    PGC_WAL_REDUCED,
    PGC_WAL_MINIMAL
};

enum pg_ctblmgr_dml_type {
    PGC_DML_UNINITIALIZED,
    PGC_DML_INSERT,
    PGC_DML_UPDATE,
    PGC_DML_DELETE,
    PGC_DML_TRUNCATE,
    PGC_DML_TX_BARRIER
};

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

struct changeset * json_to_changeset( char * ); 

static struct changeset * _new_changeset( void );
static inline char * _json_token_to_string( char *, jsmntok_t *, jsmntype_t );
#endif // CHANGESET_H
