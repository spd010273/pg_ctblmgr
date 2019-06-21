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

#define MAX_LOCK_WAIT 5 // seconds

#define DEFAULT_BUFFER_SIZE 16

#define WORKER_TITLE_PARENT "pg_ctblmgr logical receiver"
#define WORKER_TITLE_CHILD "pg_ctblmgr subscriber"
#define LOG_FILE_NAME "/var/log/pg_ctblmgr/pg_ctblmgr.log"

bool daemonize;
char * conninfo;
FILE * log_file;

sig_atomic_t got_sighup;
sig_atomic_t got_sigint;
sig_atomic_t got_sigterm;

struct change_buffer {
    unsigned long  size;
    unsigned long  num_entries;
    char **        entries;
    bool           _locked;
};

struct worker {
    unsigned short type;
    unsigned short status;
    PGconn *       conn;
    pid_t          pid;
    bool           tx_in_progress;
    int            my_argc;
    char **        my_argv;
    char *         pidfile;  // used by parent to remove pid file on term
    void *         change_buffer;
};

struct worker ** workers;
struct worker * parent;

void _parse_args( int, char ** );
void _usage( char * ) __attribute__ ((noreturn));
void _log( unsigned short, char *, ... ) __attribute__ ((format (gnu_printf, 2, 3)));

struct worker * new_worker(
    unsigned short,
    int,
    char **,
    struct worker *
);

bool parent_init( int, char ** );
void free_worker( struct worker * );
bool create_pid_file( void );

void __sigterm( int ) __attribute__ ((noreturn));
void __sigint( int ) __attribute__ ((noreturn));
void __sighup( int );
void __term( void ) __attribute__ ((noreturn));

void * create_shared_memory( size_t );
void _set_process_title( char **, int, char *, unsigned int * );

bool _wait_and_set_mutex( bool * );
bool __test_and_set( bool * );

struct change_buffer * new_change_buffer( void );
bool resize_change_buffer( struct change_buffer *, long int );

#endif // UTIL_H
