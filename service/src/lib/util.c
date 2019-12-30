
#include "util.h"

#define VERSION "0.1"

extern char ** environ; // declared in unistd.h

struct worker ** workers       = NULL;
struct worker *  parent        = NULL;
char *           conninfo      = NULL;
FILE *           log_file      = NULL;
unsigned int     max_argv_size = 0;
bool             daemonize     = false;

sig_atomic_t got_sighup  = false;
sig_atomic_t got_sigint  = false;
sig_atomic_t got_sigterm = false;

static const char * usage_string = "\
Usage: pg_ctblmgr\n \
    -U DB user (default: postgres)\n \
    -p DB port (default: 5432)\n \
    -h DB host (default: localhost)\n \
    -d DB name (default: <DB user>)\n \
  [ -D daemonize\n \
    -v VERSION\n \
    -? HELP ]\n";

void _parse_args( int argc, char ** argv )
{
    int c = 0;
    char * username = NULL;
    char * dbname   = NULL;
    char * port     = NULL;
    char * hostname = NULL;

    opterr = 0;

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
        buff_time,
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
    result->change_buffer  = NULL;

    return result;
}

bool parent_init( int argc, char ** argv )
{
    if( daemonize )
    {
        if( daemon( 1, 1 ) != 0 )
        {
            _log(
                LOG_LEVEL_FATAL,
                "Daemonization failed"
            );
        }

        log_file = fopen( LOG_FILE_NAME, "a" );

        if( log_file == NULL )
        {
            return false;
        }
    }

    parent = new_worker( WORKER_TYPE_PARENT, argc, argv, NULL );

    if( parent == NULL )
    {
        return false;
    }

    if( !create_pid_file() )
    {
        return false;
    }

    return true;
}

bool create_pid_file( void )
{
    unsigned int    string_offset  = 0;
    unsigned int    total_size     = 0;
    char            path[PATH_MAX] = {0};
    struct passwd * pw             = NULL;
    DIR *           dir            = NULL;
    FILE *          pfh            = NULL;
    char *          pid_path       = NULL;
    char *          my_pid         = NULL;
    char *          pid_file       = "pg_ctblmgr.pid";
    struct stat     statbuffer     = {0};

    dir = opendir( "/var/run" );
    // Try /var/run
    if( dir != NULL )
    {
        closedir( dir ); // We have permissions to this directory
        pfh = fopen( "/var/run/__test.pid", "w" );

        if( pfh != NULL )
        {
            fclose( pfh );
            remove( "/var/run/__test.pid" );
            pfh = NULL;
            strncpy( path, "/var/run", 8 );
        }
    }

    // Try ~/
    if( strlen( path ) == 0 )
    {
        if( getenv( "HOME" ) != NULL )
        {
            strncpy( path, getenv( "HOME" ), sizeof( path ) );
        }
    }

    // Try current working dir
    if( strlen( path ) == 0 )
    {
        pw = getpwuid( geteuid() );

        if( pw != NULL )
        {
            strncpy( path, pw->pw_dir, sizeof( path ) );
        }
    }

    if( strlen( path ) == 0 )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to determine location for PID file"
        );

        return false;
    }

    pfh = NULL;

    if( path[strlen( path ) - 1] != '/' )
    {
        string_offset = 1;
    }

    total_size = strlen( path )
               + strlen( pid_file )
               + string_offset + 1;

    pid_path = ( char * ) calloc(
        sizeof( char ),
        total_size
    );

    if( pid_path == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to allocate memory for PID file path"
        );

        return false;
    }

    if( total_size > PATH_MAX )
    {
        _log(
            LOG_LEVEL_ERROR,
            "PATH_MAX not large enough to prevent buffer overflow"
        );

        return false;
    }

    strncpy( pid_path, path, total_size );

    if( string_offset == 1 )
    {
        strncat( pid_path, "/", 1 );
    }

    strncat( pid_path, pid_file, strlen( pid_file ) );

    pid_path[total_size - 1] = '\0';

    _log(
        LOG_LEVEL_DEBUG,
        "Opening pid file %s",
        pid_path
    );

    // Does the file we've selected already exist?
    if( stat( pid_path, &statbuffer ) >= 0 )
    {
        _log(
            LOG_LEVEL_ERROR,
            "PID file %s already exists, is pg_ctblmgr already running?",
            pid_path
        );
        free( pid_path );
        return false;
    }

    // Prepare pid string
    total_size = ( unsigned int ) ceil( log10( ( int ) getpid() ) + 3 );

    my_pid = ( char * ) calloc(
        sizeof( char ),
        total_size
    );

    if( my_pid == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to allocate memory for PID string"
        );

        free( pid_path );
        return false;
    }

    snprintf( my_pid, total_size, "%d\n", ( int ) getpid() );

    pfh = fopen( pid_path, "w" );

    if( pfh == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to open file '%s' for writing",
            pid_path
        );

        free( pid_path );
        free( my_pid );
        return false;
    }

    fprintf( pfh, "%s", my_pid );
    parent->pidfile = pid_path;
    fclose( pfh );
    free( my_pid );

    return true;
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

        for( i = 0; i < argc; i++ )
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

bool _wait_and_set_mutex( bool * mutex )
{
    time_t lock_acquire_start = 0;
    double random_backoff     = 0.0;
    double last_backoff       = 0.0;

    lock_acquire_start = time( NULL );

    last_backoff = 1.0;

    while( *mutex == true || __test_and_set( mutex ) == true )
    {
        if( difftime( time( NULL ), lock_acquire_start ) > MAX_LOCK_WAIT )
        {
            _log(
                LOG_LEVEL_WARNING,
                "Max lock wait time %d exceeded",
                MAX_LOCK_WAIT
            );

            return false;
        }

        sleep( last_backoff + random_backoff );
        last_backoff   = last_backoff + random_backoff;
        random_backoff = 2 * ( ( double ) rand() / ( double ) RAND_MAX );
    }

    *mutex = true;
    return true;
}

bool __test_and_set( bool * mutex )
{
    bool initial = true;
    initial = *mutex;
    *mutex = true;
    return initial;
}

struct change_buffer * new_change_buffer( void )
{
    struct change_buffer * cb = NULL;

    cb = ( struct change_buffer * ) create_shared_memory(
        sizeof( struct change_buffer )
    );

    if( cb == NULL )
    {
        return NULL;
    }

    cb->size        = DEFAULT_BUFFER_SIZE;
    cb->num_entries = 0;
    cb->_locked     = false;
    cb->entries     = ( char ** ) create_shared_memory(
        sizeof( char * ) * DEFAULT_BUFFER_SIZE
    );

    if( cb->entries == NULL )
    {
        munmap( cb, sizeof( struct change_buffer ) );
        return NULL;
    }

    return cb;
}

bool resize_change_buffer( struct change_buffer * cb, long int num_entries )
{
    char **       temp              = NULL;
    unsigned long num_allocations   = 0;
    unsigned long i                 = 0;
    long int      delta             = 0;
    long int      offset            = 0;

    if( cb == NULL || ( cb->entries == NULL && cb->num_entries != 0 ) )
    {
        return NULL;
    }

    if( num_entries == cb->num_entries )
    {
        return true;
    }

    delta = cb->num_entries - num_entries;

    if( delta > 0 )
    {
        num_allocations = cb->size + ( ceil(
            ( double ) delta / ( double ) DEFAULT_BUFFER_SIZE
        ) * DEFAULT_BUFFER_SIZE );
    }
    else if( delta < 0 )
    {
        num_allocations = cb->size - ( floor(
            ( double ) delta / ( double ) DEFAULT_BUFFER_SIZE
        ) * DEFAULT_BUFFER_SIZE );

        if( num_allocations < cb->num_entries )
        {
            return false;
        }
    }

    temp = ( char ** ) create_shared_memory(
        sizeof( char * ) * num_allocations
    );

    if( temp == NULL )
    {
        return false;
    }

    if( !_wait_and_set_mutex( &(cb->_locked) ) )
    {
        munmap( temp, sizeof( char * ) * num_allocations );
        return false;
    }

    for( i = 0; i < cb->num_entries; i++ )
    {
        if( cb->entries[i] == NULL )
        {
            offset++;
        }

        if( delta < 0 && i > num_allocations )
        {
            // We won't truncate the buffer
            munmap( temp, sizeof( char * ) * num_allocations );
            return false;
        }

        temp[i] = cb->entries[i+offset];
    }

    cb->num_entries = cb->num_entries - offset;
    munmap( cb->entries, sizeof( char * ) * cb->size );
    cb->entries = temp;
    cb->size    = num_allocations;
    cb->_locked = false;

    return true;
}
