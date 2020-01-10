#ifndef PG_CTBLMGR_H
#define PG_CTBLMGR_H

#include "lib/util.h"
#include "lib/query.h"
#include "lib/strings.h"
#include "lib/buffer.h"

int main( int, char ** );
static int start_workers( void );
static bool extension_installed( void );
static void worker_entrypoint( void * );
#endif // PG_CTBLMGR_H
