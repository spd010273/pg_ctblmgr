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

#define WORKER_CONTEXT_NAME "PGCTBLMGR_WORKER"
#define ARRAY_CONTEXT_NAME "PGCTBLMGR_ARRAY"
#define WORKERS_CONTEXT_NAME "PGCTBLMGR_PIDTABLE"
#define STRING_CONTEXT_NAME "PGCTBLMGR_STRINGTABLE"
#define MIN(x,y) (x>y?y:x)
#define MAX_CHANNEL_LENGTH 64
#define WORKERS_ARRAY_INC 16

/* Structures and Flags */

bool daemonize;
char * conninfo;
FILE * log_file;
char * pid_file_name;

typedef ref_t worker_ref_t;
typedef ref_t workers_ref_t;
typedef ref_t string_ref_t;
typedef ref_t array_ref_t;

volatile sig_atomic_t got_sighup;
volatile sig_atomic_t got_sigint;
volatile sig_atomic_t got_sigterm;

struct pgc_conf {
    char        channel[MAX_CHANNEL_LENGTH];
    array_ref_t filter_tables;
    uint16_t    num_tables;
    char        wal_level;
    bool        include_transactions;
};

struct worker {
    uint8_t         type;
    uint8_t         status;
    PGconn *        conn;
    pid_t           pid;
    bool            tx_in_progress;
    int32_t         my_argc;
    char **         my_argv;
    struct pgc_conf config;
    ref_t           buffer;
    uint64_t        last_lsn;
    uint16_t        num_workers; // stored only on parent - fetch with get_worker_count()
    uint16_t        max_workers; // stored only on parent - fetch with get_worker_max()
};

workers_ref_t workers;
worker_ref_t  parent;

/* Function Declarations */

extern void _parse_args( int, char ** );
extern void _usage( char * ) __attribute__ ((noreturn));
extern void _log( unsigned short, char *, ... ) __attribute__ ((format (gnu_printf, 2, 3)));
extern void set_nolog( void );

extern worker_ref_t new_worker(
    uint8_t,            // type
    uint8_t,            // id
    int32_t,            // argc
    char **,            // argv
    void (*)( void * ), // Entrypoint
    worker_ref_t,       // workerslot
    char *,             // channel
    char **,            // filter_tables
    uint16_t,           // num_tables
    char                // wal_level
);

extern bool worker_set_config(
    worker_ref_t,
    char *,
    char **,
    uint16_t,
    char
);

extern bool parent_init( int, char ** );
extern void free_worker( worker_ref_t );
extern bool create_pid_file( void );

extern void __sigterm( int );
extern void __sigint( int );
extern void __sighup( int );
extern void __term( void ) __attribute__ ((noreturn));

extern void _set_process_title( char **, int, char *, unsigned int * );

extern worker_ref_t get_worker_by_channel( char * );
extern worker_ref_t get_worker_by_pid( void );
extern struct worker * get_worker_ptr_by_channel( char * );
extern struct worker * get_worker_ptr_by_pid( void );
extern struct worker * get_parent_ptr( bool );
extern uint16_t get_worker_count( void );
extern uint16_t get_worker_max( void );

extern bool initialize_util_contexts( void );
extern context_t get_worker_context( void );
extern context_t get_workers_context( void );
extern context_t get_array_context( void );
extern context_t get_string_context( void );
extern char * get_pid_file( void );
#endif // UTIL_H
