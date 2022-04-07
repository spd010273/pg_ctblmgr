#ifndef PG_CTBLMGR_H
#define PG_CTBLMGR_H

#include "lib/slab.h"
#include "lib/util.h"
#include "lib/query.h"
#include "lib/strings.h"
#include "lib/buffer.h"
#include "lib/changeset.h"
#include "lib/xlog.h"

#define REL_MAX_LEN 64
#define SCHEMA_MAX_LEN 64
#define QUAL_MAX ( REL_MAX_LEN + SCHEMA_MAX_LEN + 2 ) // 'schema.table\0'
int main( int, char ** );

static int start_workers( void );
static bool extension_installed( void );
static void worker_entrypoint( void * );
static bool setup_replication_slot( void );
static bool initialize_buffer( void );
static void get_filter_tables_by_channel(
    struct worker *,
    char *,
    char ***,
    uint16_t *
);
static char * get_filter_tables_string( void );
static void get_worker_pins( buffer_pin_ref_t ** );
static void parent_main_loop( void );
static bool get_changeset_batch( char *, changeset_ref_t **, unsigned int *, char ** );
#endif // PG_CTBLMGR_H
