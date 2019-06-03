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

bool daemonize;
char * conn_info;
FILE * log_file;

sig_atomic_t got_sighup;
sig_atomic_t got_sigint;
sig_atomic_t got_sigterm;

struct worker {
    unsigned short type;
    PGconn *       conn;
    pid_t          pid;
    bool           tx_in_progress;
    int            my_argc;
    char **        my_argv;
};

extern void _parse_args( int, char ** );
extern void _usage( char * ) __attribute__ ((noreturn));
extern void _log( unsigned short, char *, ... ) __attribute__ ((format (gnu_printf, 2, 3)));

struct worker * new_worker(
    unsigned short,
    int,
    char **,
    struct worker *
);

void free_worker( struct worker * );

void __sigterm( int ) __attribute__ ((noreturn));
void __sigint( int ) __attribute__ ((noreturn));
void __sighup( int );
void __term( void ) __attribute__ ((noreturn));

void * create_shared_memory( size_t );
void _set_process_title( char **, int, char *, unsigned int * );

#endif // UTIL_H
