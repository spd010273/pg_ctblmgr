#ifndef UTIL_H
#define UTIL_H

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <regex.h>
#include <signal.h>
#include <libpq-fe.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/user.h>
#include <limits.h>
#include <pwd.h>
#include <dirent.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>

#include "slab.h"
#include "buffer.h"

#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_WARNING 3
#define LOG_LEVEL_ERROR 4
#define LOG_LEVEL_FATAL 5

#define WORKER_TYPE_PARENT 1
#define WORKER_TYPE_CHILD 2

#define WORKER_STATUS_DEAD 0
#define WORKER_STATUS_STARTUP 1
#define WORKER_STATUS_IDLE 2
#define WORKER_STATUS_UPDATE 3
#define WORKER_STATUS_REFRESH 4
#define WORKER_STATUS_PROCESS_WAL 5

#define MAX_LOCK_WAIT 5 // seconds

#define DEFAULT_BUFFER_SIZE 16
#define EXTENSION_NAME "pg_ctblmgr"
#define EXTENSION_SCHEMA "pgctblmgr"
#define MAIN_CHANNEL "__pg_ctblmgr"
#define PLUGIN_NAME "pg_ctblmgr"
#define WORKER_TITLE_PARENT "pg_ctblmgr logical receiver"
#define WORKER_TITLE_CHILD "pg_ctblmgr worker (%s)"
#define LOG_FILE_NAME "/var/log/pg_ctblmgr/pg_ctblmgr.log"

#define MIN(x,y) (x>y?y:x)
#define MAX_CHANNEL_LENGTH 64

/* Structures and Flags */

bool daemonize;
char * conninfo;
FILE * log_file;

volatile sig_atomic_t got_sighup;
volatile sig_atomic_t got_sigint;
volatile sig_atomic_t got_sigterm;

struct pgc_conf {
    char         channel[MAX_CHANNEL_LENGTH];
    char **      filter_tables;
    unsigned int num_tables;
    char         wal_level;
    bool         include_transactions;
};

struct worker {
    unsigned short  type;
    unsigned short  status;
    PGconn *        conn;
    pid_t           pid;
    bool            tx_in_progress;
    int             my_argc;
    char **         my_argv;
    char *          pidfile;  // used by parent to remove pid file on term
    struct pgc_conf config;
    ref_t           buffer;
    uint64_t        last_lsn;
};

struct worker ** workers;
struct worker * parent;
unsigned int num_workers;

/* Function Declarations */

extern void _parse_args( int, char ** );
extern void _usage( char * ) __attribute__ ((noreturn));
extern void _log( unsigned short, char *, ... ) __attribute__ ((format (gnu_printf, 2, 3)));

extern struct worker * new_worker(
    unsigned short,     // type
    unsigned long int,  // id
    int,                // argc
    char **,            // argv
    void (*)( void * ), // Entrypoint
    struct worker *,    // workerslot
    char *,             // channel
    char **,            // filter_tables
    unsigned int,       // num_tables
    char                // wal_level
);

extern void worker_set_config(
    struct worker *,
    char *,
    char **,
    unsigned int,
    char
);

extern bool parent_init( int, char ** );
extern void free_worker( struct worker * );
extern bool create_pid_file( void );

extern void __sigterm( int );
extern void __sigint( int );
extern void __sighup( int );
extern void __term( void ) __attribute__ ((noreturn));

extern void free_shared_memory( void *, size_t );
extern void * create_shared_memory( size_t );
extern void * resize_shared_memory( void *, size_t, size_t );
extern void _set_process_title( char **, int, char *, unsigned int * );

extern struct worker * get_worker_by_channel( char * );
extern struct worker * get_worker_by_pid( void );
#endif // UTIL_H
