#ifndef PG_CTBLMGR_H
#define PG_CTBLMGR_H

#include "lib/util.h"
#include "lib/query.h"
#include "lib/strings.h"
#include "lib/buffer.h"
//#include "lib/changeset.h"

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
static void destroy_replication_slot( struct worker * );

#endif // PG_CTBLMGR_H
