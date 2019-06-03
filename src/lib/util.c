
#include "util.h"

#define VERSION "0.1"

extern char ** environ; // declared in unistd.h

sig_atomic_t got_sighup  = false;
sig_atomic_t got_sigint  = false;
sig_atomic_t got_sigterm = false;

FILE * log_file = NULL;
unsigned int max_argv_size = 0;

static const char * usage_string = "\
Usage: pg_ctblmgr\n \
    -U DB user (default: postgres)\n \
    -p DB port (default: 5432)\n \
    -h DB host (default: localhost)\n \
    -d DB name (default: <DB user>)\n \
  [ -D daemonize\n \
    -v VERSION\n \
    -? HELP ]\n";

void parse_args( int argc, char ** argv )
{
    int c = 0;
    char * username = NULL;
    char * dbname   = NULL;
    char * port     = NULL;
    char * hostname = NULL;

    operr = 0;

    while( ( c = getopt( argc, argv, "U:p:d:h:Dv?" ) ) != -1 )
    {
        switch( c )
        {
            case 'U':
                username = optarg;
                break;
            case 'p':
                port = optarg;
                break;
            case 'h':
                hostname = optarg;
                break;
            case 'd':
                dbname = optarg;
                break;
            case '?':
                _usage( NULL );
            case 'v':
                printf( "pg_ctblmgr, version %s\n", VERSION );
                exit( 0 );
            case 'D':
                daemonize = true;
                break;
            default:
                _usage( "Invalid argument" );
        }
    }

    if( port == NULL )
    {
        port = "5432";
    }

    if( username == NULL )
    {
        username = "postgres";
    }

    if( hostname == NULL )
    {
        hostname = "localhost";
    }

    if( dbname == NULL )
    {
        dbname = username;
    }

    conninfo = ( char * ) calloc(
        (
            strlen( username ) +
            strlen( port ) +
            strlen( dbname ) +
            strlen( hostname ) +
            26
        ),
        sizeof( char )
    );

    if( conninfo == NULL )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Failed to allocate memory for connection string"
        );
    }

    strcpy( conninfo, "user=" );
    strcat( conninfo, username );
    strcat( conninfo, " host=" );
    strcat( conninfo, hostname );
    strcat( conninfo, " port=" );
    strcat( conninfo, port );
    strcat( conninfo, " dbname=" );
    strcat( conninfo, dbname );

    return;
}

void _usage( char * message )
{
    if( message != NULL )
    {
        printf( "%s\n", message );
    }

    printf( "%s", usage_string );
    exit(1);
}

void _log( unsigned short log_level, char * message, ... )
{
    va_list args          = {{0}};
    FILE *  output_handle = NULL;
    struct  timeval tv    = {0};
    char    buff_time[28] = {0};
    char *  log_prefix    = NULL;

    if( message == NULL )
    {
        return;
    }

    if(      log_level == LOG_LEVEL_DEBUG   ) { log_prefix = "DEBUG";   }
    else if( log_level == LOG_LEVEL_INFO    ) { log_prefix = "INFO";    }
    else if( log_level == LOG_LEVEL_WARNING ) { log_prefix = "WARNING"; }
    else if( log_level == LOG_LEVEL_ERROR   ) { log_prefix = "ERROR";   }
    else if( log_level == LOG_LEVEL_FATAL   ) { log_prefix = "FATAL";   }
    else                                      { log_prefix = "UNKNOWN"; }

    gettimeofday( &tv, NULL );
    strftime(
        bufftime,
        sizeof( buff_time ) / sizeof( *buff_time ),
        "%Y-%m-%d %H:%M:%S",
        gmtime( &tv.tv_sec )
    );

    // If we have a logfile, set that as the output handle
    if( log_file != NULL )
    {
        output_handle = log_file;
    }

    if(
            output_handle == NULL
         && (
                log_level == LOG_LEVEL_WARNING
             || log_level == LOG_LEVEL_ERROR
             || log_level == LOG_LEVEL_FATAL
            )
      )
    {
        output_handle = stderr;
    }
    else if( output_handle == NULL )
    {
        output_handle = stdout;
    }

    va_start( args, message );

#ifndef DEBUG
    if( log_level == LOG_LEVEL_DEBUG )
    {
#endif
        fprintf(
            output_handle,
            "%s.%03d ",
            buff_time,
            ( int ) ( tv.tv_usec / 1000 )
        );

        fprintf(
            output_handle,
            "[%d] ",
            getpid()
        );

        fprintf(
            output_handle,
            "%s: ",
            log_prefix
        );

        vfprintf(
            output_handle,
            message,
            args
        );

        fprintf(
            output_handle,
            "\n"
        );
#ifndef DEBUG
    }
#endif

    va_end( args );
    fflush( output_handle );

    if( log_level == LOG_LEVEL_FATAL )
    {
        exit( 1 );
    }

    return;
}

struct worker * new_worker(
    unsigned short  type,
    int             my_argc,
    char **         my_argv,
    struct worker * workerslot
)
{
    struct worker * result = NULL;
    pid_t           pid    = 0;
    size_t          size   = 0;

    if( workerslot == NULL )
    {
        result = ( struct worker * ) create_shared_memory(
            sizeof( struct worker )
        );
    }
    else
    {
        result = workerslot;
    }

    if( result == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Could not allocate shared memory for worker"
        );

        return NULL;
    }

    result->tx_in_progress = false;
    result->pid            = 0;
    result->conn           = NULL;
    result->my_argc        = 0;
    result->my_argv        = NULL;

    return result;
}

void free_worker( struct worker * worker )
{
    if( worker == NULL )
    {
        return;
    }

    if( worker->conn != NULL && PQstatus( worker->conn ) == CONNECTION_OK )
    {
        if( worker->tx_in_progress )
        {
            PQexec( worker->conn, "ROLLBACK" );
            worker->tx_in_progress = false;
        }

        PQfinish( worker->conn );
        worker->conn = NULL;
    }

    munmap( worker, sizeof( struct worker ) );
    worker = NULL;
    return;
}

void * create_shared_memory( size_t size )
{
    void * ptr        = NULL;
    int    protection = 0;
    int    visibility = 0;

    protection = PROT_READ | PROT_WRITE;
    visibility = MAP_ANONYMOUS | MAP_SHARED;

    ptr = mmap( NULL, size, protection, visibility, 0, 0 );

    if( ptr == MAP_FAILED )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to allocate shared memory: %s",
            strerror( errno )
        );
        return NULL;
    }

    return ptr;
}

void _set_process_title(
    char **        argv,
    int            argc,
    char *         title,
    unsigned int * max_size
)
{
    unsigned int i    = 0;
    unsigned int size = 0;

    if( title == NULL || argv == NULL )
    {
        return;
    }

    if( max_size == NULL || *max_size == 0 )
    {
        size = 0;

        for( i = 0; i < argv; i++ )
        {
            if( argv[i] == NULL )
            {
                continue;
            }
            else
            {
                size += ( unsigned int ) ( strlen( argv[i] ) + 1 );
            }
        }

        *max_size = size;
    }

    size = *max_size;

    memset( argv[0], '\0', size );
    strncpy( argv[0], title, size );
}
