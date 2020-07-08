#ifndef PG_CTBLMGR_H
#define PG_CTBLMGR_H

#include "lib/util.h"
#include "lib/query.h"
#include "lib/strings.h"
#include "lib/buffer.h"
#include "lib/changeset.h"

#define QUAL_MAX 128
int main( int, char ** );

static int start_workers( void );
static bool extension_installed( void );
static void worker_entrypoint( void * );
static bool setup_replication_slot( struct worker * );
static bool initialize_buffer( struct worker * );
static void get_filter_tables_by_channel(
    struct worker *,
    char *,
    char ***,
    unsigned int *
);
static char * get_filter_tables_string( void );
static void get_worker_pins( struct worker *, struct buffer_pin *** );
static void parent_main_loop( void );
#endif // PG_CTBLMGR_H
