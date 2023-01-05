
#include "util.h"

#define VERSION "0.1"

extern char ** environ; // declared in unistd.h

static context_t worker_context  = INVALID_CONTEXT;
static context_t array_context   = INVALID_CONTEXT;
static context_t workers_context = INVALID_CONTEXT;
static context_t string_context  = INVALID_CONTEXT;

workers_ref_t workers = NULLREF;
worker_ref_t  parent  = NULLREF;

char *           conninfo      = NULL;
FILE *           log_file      = NULL;
unsigned int     max_argv_size = 0;
bool             daemonize     = false;
bool             no_log        = false;

volatile sig_atomic_t got_sighup  = false;
volatile sig_atomic_t got_sigint  = false;
volatile sig_atomic_t got_sigterm = false;

static const char * usage_string = "\
Usage: pg_ctblmgr\n \
    -U DB user (default: postgres)\n \
    -p DB port (default: 5432)\n \
    -h DB host (default: localhost)\n \
    -d DB name (default: <DB user>)\n \
  [ -D daemonize\n \
    -v VERSION\n \
    -l Log to stdout\n \
    -? HELP ]\n";


void set_nolog( void )
{
    no_log = true;
    return;
}

void _parse_args( int argc, char ** argv )
{
    int c = 0;
    char * username = NULL;
    char * dbname   = NULL;
    char * port     = NULL;
    char * hostname = NULL;

    opterr = 0;

    while( ( c = getopt( argc, argv, "U:p:d:h:Dvl?" ) ) != -1 )
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
            case 'l':
                no_log = true;
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
    if( no_log )
    {
        if(
               log_level == LOG_LEVEL_WARNING
            || log_level == LOG_LEVEL_ERROR
            || log_level == LOG_LEVEL_FATAL
          )
        {
            output_handle = stderr;
        }
        else
        {
            output_handle = stdout;
        }
    }
    else
    {
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
    }

    va_start( args, message );

#ifndef DEBUG
    if( log_level != LOG_LEVEL_DEBUG )
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
        __term();
    }

    return;
}

worker_ref_t new_worker(
    uint8_t           type,
    uint8_t           id,
    int32_t           my_argc,
    char **           my_argv,
    void (*function)( void * ),
    worker_ref_t      workerslot,
    char *            channel,
    char **           filter_tables,
    uint16_t          num_tables,
    char              wal_level
)
{
    worker_ref_t    result      = NULLREF;
    worker_ref_t *  worker_arr  = NULL;
    struct worker * worker      = NULL;
    pid_t           pid         = 0;
    char *          worker_name = NULL;
    unsigned int    size        = 0;
    struct worker * p           = NULL;

    if( workerslot == NULLREF )
    {
        result = rsmalloc(
            worker_context,
            sizeof( struct worker )
        );
    }
    else
    {
        result = workerslot;
    }

    if( result == NULLREF )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Could not allocate shared memory for worker"
        );
        return NULLREF;
    }

    worker = ( struct worker * ) to_ptr( worker_context, result );

    if( worker == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Could not dereferences shared memory for worker"
        );
        return NULLREF;
    }

    worker->tx_in_progress = false;
    worker->pid            = getpid();
    worker->conn           = NULL;
    worker->my_argc        = my_argc;
    worker->my_argv        = my_argv;
    worker->type           = type;
    worker->buffer         = NULLREF;

    if( workerslot == NULLREF )
        worker->config.filter_tables = NULLREF;

    worker_set_config(
        result,
        channel,
        filter_tables,
        num_tables,
        wal_level
    );

    if( type == WORKER_TYPE_PARENT )
    {
        parent = result;
        signal( SIGHUP, __sighup );
        signal( SIGTERM, __sigterm );
        signal( SIGINT, __sigint );
        _set_process_title(
            my_argv,
            my_argc,
            WORKER_TITLE_PARENT,
            &max_argv_size
        );

        return result;
    }

    if( parent == NULLREF )
    {
        _log(
            LOG_LEVEL_ERROR,
            "No parent process table entry to initialize with"
        );
        free_worker( result );
        return NULLREF;
    }

    p = ( struct worker * ) to_ptr( worker_context, parent );

    if( p == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to dereference parent's PID table entry"
        );
        free_worker( result );
        return NULLREF;
    }

    if( workers == NULLREF )
    {
        _log(
            LOG_LEVEL_ERROR,
            "No PID table found"
        );
        return NULLREF;
    }

    worker_arr = ( worker_ref_t * ) to_ptr( workers_context, workers );

    if( worker_arr == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to dereference PID table array"
        );
    }

    worker_arr[id] = result;
    pid = fork();

    if( pid == 0 ) // child
    {
        if( !slab_init() )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to initialize slab post fork"
            );
            exit( 0 );
        }

        setvbuf( stdout, NULL, _IONBF, 0 );
        worker = ( struct worker * ) to_ptr( worker_context, result );
        p      = ( struct worker * ) to_ptr( worker_context, parent );

        if( worker == NULL || p == NULL )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to deref pid slices after fork()"
            );
            exit( 0 );
        }

        worker->pid = getpid();
        worker->max_workers = p->max_workers;
        worker->num_workers = p->max_workers;
        signal( SIGHUP, __sighup );
        signal( SIGTERM, __sigterm );
        signal( SIGINT, __sigint );

        size = strlen( WORKER_TITLE_CHILD ) - 2 + strlen( channel ) + 1;
        worker_name = ( char * ) calloc(
            size,
            sizeof( char )
        );

        if( worker_name == NULL )
            exit( 0 );

        snprintf( worker_name, size, WORKER_TITLE_CHILD, channel );

        _set_process_title(
            my_argv,
            my_argc,
            worker_name,
            &max_argv_size
        );

        free( worker_name );
        worker_name = NULL;
        worker->status = WORKER_STATUS_STARTUP;

        if( !worker_set_config( result, channel, filter_tables, num_tables, wal_level ) )
        {
            _log( LOG_LEVEL_ERROR, "Worker failed to initialize config" );
            exit( 0 );
        }

        worker->buffer = p->buffer;
        function( ( void * ) worker );
        exit( 0 );
    }
    else if( pid < 0 )
    {
        return NULLREF;
    }

    return result;
}

bool worker_set_config(
    worker_ref_t    worker,
    char *          channel,
    char **         filter_tables,
    uint16_t        num_tables,
    char            wal_level
)
{
    uint16_t        i       = 0;
    uint16_t        j       = 0;
    struct worker * w       = NULL;
    char *          ft_elem = NULL;
    string_ref_t *  arr     = NULL;

    _log( LOG_LEVEL_DEBUG, "Worker set config startup" );
    if( worker == NULLREF )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Cannot set worker config, NULL slot"
        );
        return false;
    }

    w = ( struct worker * ) to_ptr( worker_context, worker );

    if( w == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Cailed to dereference worker slice"
        );
        return false;
    }

    if( wal_level != 'R' && wal_level != 'F' && wal_level != 'M' )
    {
        _log(
            LOG_LEVEL_ERROR,
            "invalid worker config WAL LEVEL: %c",
            wal_level
        );
        return false;
    }

    if( w->config.filter_tables != NULLREF && w->type == WORKER_TYPE_PARENT )
    {
        _log(
            LOG_LEVEL_DEBUG,
            "Freeing non-clean filter tables in worker shm context"
        );
        if( w->config.num_tables > 0 )
        {
            arr = ( string_ref_t * ) to_ptr( array_context, w->config.filter_tables );

            if( arr != NULL )
            {
                for( i = 0; i < w->config.num_tables; i++ )
                {
                    rsfree( string_context, arr[i] );
                }
            }
        }

        rsfree( array_context, w->config.filter_tables );
    }

    if( w->config.filter_tables != NULLREF && w->type != WORKER_TYPE_PARENT )
    {
        // Clear (got copied in at fork), so that we can diverge our filter tables
        w->config.filter_tables = NULLREF;
    }

    if( channel != NULL )
    {
        strncpy(
            w->config.channel,
            channel,
            strnlen( channel, MAX_CHANNEL_LENGTH )
        );

        w->config.channel[strnlen( channel, MAX_CHANNEL_LENGTH )] = '\0';
    }

    if( filter_tables != NULL )
    {
        w->config.num_tables    = num_tables;
        w->config.filter_tables = ( array_ref_t ) rsmalloc(
            array_context,
            num_tables * sizeof( string_ref_t )
        );

        if( w->config.filter_tables == NULLREF )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Worker config memory allocation failed"
            );
            rsfree( array_context, w->config.filter_tables );
            return false;
        }

        arr = ( string_ref_t * ) to_ptr( array_context, w->config.filter_tables );

        if( arr == NULL )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to dereference filter_tables[] ref"
            );
            rsfree( array_context, w->config.filter_tables );
            return false;
        }

        for( i = 0; i < num_tables; i++ )
        {
            arr[i] = ( string_ref_t ) rsmalloc(
                string_context,
                ( strlen( filter_tables[i] ) + 1 ) * sizeof( char )
            );

            if( arr[i] == NULLREF )
            {
                _log(
                    LOG_LEVEL_ERROR,
                    "worker config filter table slot allocation failed"
                );

                for( j = 0; j < i; j++ )
                {
                    rsfree( string_context, arr[j] );
                }
                rsfree( array_context, w->config.filter_tables );
                return false;
            }

            ft_elem = ( char * ) to_ptr( string_context, arr[i] );

            if( ft_elem == NULL )
            {
                _log(
                    LOG_LEVEL_ERROR,
                    "Filter table element %u failed to dereference",
                    ( uint32_t ) i
                );

                for( j = 0; j < i; j++ )
                {
                     rsfree( string_context, arr[j] );
                }
                rsfree( array_context, w->config.filter_tables );
                return false;
            }

            strncpy(
                ft_elem,
                filter_tables[i],
                strlen( filter_tables[i] )
            );

            ft_elem[strlen(filter_tables[i])] = '\0';
            _log( LOG_LEVEL_DEBUG, "added filter_table[%u]: %s", i, ft_elem );
        }
    }

    w->config.wal_level = wal_level;
    __FENCE();
    return true;
}

bool parent_init( int argc, char ** argv )
{
    setvbuf( stdout, NULL, _IONBF, 0 );
    if( !no_log )
    {
        log_file = fopen( LOG_FILE_NAME, "a" );

        if( log_file == NULL )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to open '%s': %s",
                LOG_FILE_NAME,
                strerror( errno )
            );
            return false;
        }

        _log(
            LOG_LEVEL_DEBUG,
            "Opened logfile '%s'",
            LOG_FILE_NAME
        );
    }

    if( !slab_init() )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Failed to initialize slab"
        );
        return false;
    }

    if( !initialize_util_contexts() )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Failed to initialize contexts"
        );
        return false;
    }

    if( daemonize )
    {
        if( daemon( 1, 1 ) != 0 )
        {
            _log(
                LOG_LEVEL_FATAL,
                "Daemonization failed"
            );
        }

        _log(
            LOG_LEVEL_DEBUG,
            "Daemonized"
        );
    }

    parent = new_worker(
        WORKER_TYPE_PARENT,
        0,
        argc,
        argv,
        NULL,
        NULLREF,
        MAIN_CHANNEL,
        NULL,
        0,
        'F'
    );

    if( parent == NULLREF )
    {
        _log( LOG_LEVEL_ERROR, "Failed to dispatch new_Worker() for parent" );
        return false;
    }

    if( !create_pid_file() )
    {
        _log( LOG_LEVEL_ERROR, "Failed to create pid file" );
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
               + string_offset
               + 1;

    pid_path = ( char * ) calloc(
        total_size,
        sizeof( char )
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
        total_size,
        sizeof( char )
    );

    if( my_pid == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to allocate memory for PID string"
        );

        remove( pid_path );
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

        remove( pid_path );
        free( pid_path );
        free( my_pid );
        return false;
    }

    fprintf( pfh, "%s", my_pid );
    pid_file_name = pid_path;
    fclose( pfh );
    free( my_pid );

    return true;
}

void free_worker( worker_ref_t worker )
{
    struct worker * w     = NULL;
    worker_ref_t *  w_arr = NULL;
    uint16_t        i     = 0;
    string_ref_t *  s_arr = NULL;

    if( worker == NULLREF )
        return;

    w = ( struct worker * ) to_ptr( worker_context, worker );

    if( w == NULL )
    {
        rsfree( worker_context, worker );
        return;
    }

    if( w->conn != NULL && PQstatus( w->conn ) == CONNECTION_OK )
    {
        if( w->tx_in_progress )
        {
            PQexec( w->conn, "ROLLBACK" );
            w->tx_in_progress = false;
        }

        PQfinish( w->conn );
        w->conn = NULL;
    }

    w->status   = WORKER_STATUS_DEAD;
    w->my_argc  = 0;
    w->my_argv  = NULL;
    w->buffer   = NULLREF;
    w->last_lsn = 0;

    s_arr = ( string_ref_t * ) to_ptr( array_context, w->config.filter_tables );

    if( s_arr != NULL )
    {
        for( i = 0; i < w->config.num_tables; i++ )
        {
            if( s_arr[i] != NULLREF )
                rsfree( string_context, s_arr[i] );
        }
    }

    if( w->config.filter_tables != NULLREF )
        rsfree( array_context, w->config.filter_tables );

    w_arr = ( worker_ref_t * ) to_ptr( workers_context, workers );

    if( w_arr == NULL )
    {
        rsfree( worker_context, worker );
        return;
    }

    for( i = 0; i < w->num_workers; i++ )
    {
        if( w_arr[i] == worker )
        {
            w_arr[i] = NULLREF;
            break;
        }
    }

    rsfree( worker_context, worker );
    return;
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
    return;
}

void __sigterm( int sig )
{
    got_sigterm = true;
    __term();
}

void __sigint( int sig )
{
    got_sigint = true;
    __term();
}

void __sighup( int sig )
{
    got_sighup = true;
    return;
}

uint16_t get_worker_count( void )
{
    struct worker * p = NULL;

    if( parent == NULLREF )
        return 0;

    p = ( struct worker * ) to_ptr( worker_context, parent );

    if( p == NULL )
        return 0;

    return p->num_workers;
}

uint16_t get_worker_max( void )
{
    struct worker * p = NULL;

    if( parent == NULLREF )
        return 0;

    p = ( struct worker * ) to_ptr( worker_context, parent );

    if( p == NULL )
        return 0;

    return p->max_workers;
}

worker_ref_t get_worker_by_channel( char * channel )
{
    struct worker * worker      = NULL;
    worker_ref_t *  workers_arr = NULL;
    uint16_t        i           = 0;

    if( channel == NULL || workers == NULLREF )
        return NULLREF;

    workers_arr = ( worker_ref_t * ) to_ptr( workers_context, workers );

    if( workers_arr == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Workers array dereferenced to NULL"
        );
        return NULLREF;
    }

    for( i = 0; i < get_worker_count(); i++ )
    {
        if( workers_arr[i] == NULLREF )
            continue;

        worker = ( struct worker * ) to_ptr( worker_context, workers_arr[i] );

        if( worker == NULL )
            continue;

        if(
            strncmp(
                worker->config.channel,
                channel,
                MIN(
                    strnlen( worker->config.channel, MAX_CHANNEL_LENGTH ),
                    strnlen( channel, MAX_CHANNEL_LENGTH )
                )
            ) == 0
          )
        {
            return workers_arr[i];
        }
    }

    return NULLREF;
}

worker_ref_t get_worker_by_pid( void )
{
    struct worker * worker      = NULL;
    struct worker * p           = NULL;
    uint16_t        i           = 0;
    pid_t           pid         = 0;
    worker_ref_t *  workers_arr = NULL;

    pid = getpid();
    p = get_parent_ptr( false );

    if( p != NULL && p->pid == pid )
        return parent;

    if( workers == NULLREF )
        return NULLREF;

    workers_arr = ( worker_ref_t * ) to_ptr( workers_context, workers );

    for( i = 0; i < get_worker_count(); i++ )
    {
        worker = ( struct worker * ) to_ptr( worker_context, workers_arr[i] );

        if( worker == NULL )
            continue;

        if( worker->pid == pid )
            return workers_arr[i];
    }

    return NULLREF;
}

struct worker * get_worker_ptr_by_channel( char * channel )
{
    struct worker * worker      = NULL;
    worker_ref_t *  workers_arr = NULL;
    uint16_t        i           = 0;

    if( channel == NULL || workers == NULLREF )
        return NULL;

    workers_arr = ( worker_ref_t * ) to_ptr( workers_context, workers );

    if( workers_arr == NULL )
        return NULL;

    for( i = 0; i < get_worker_count(); i++ )
    {
        worker = ( struct worker * ) to_ptr( worker_context, workers_arr[i] );

        if( worker == NULL )
            continue;

        if(
            strncmp(
                worker->config.channel,
                channel,
                MIN(
                    strnlen( worker->config.channel, MAX_CHANNEL_LENGTH ),
                    strnlen( channel, MAX_CHANNEL_LENGTH )
                )
            ) == 0
          )
        {
            return worker;
        }
    }

    return NULL;
}

struct worker * get_parent_ptr( bool must_be_parent )
{
    struct worker * me = NULL;

    me = ( struct worker * ) to_ptr( worker_context, parent );

    if( me == NULL )
        return NULL;

    if( must_be_parent && me->type != WORKER_TYPE_PARENT && me->pid != getpid() )
        return NULL;

    return me;
}

struct worker * get_worker_ptr_by_pid( void )
{
    struct worker * worker      = NULL;
    struct worker * p           = NULL;
    uint16_t        i           = 0;
    pid_t           pid         = 0;
    worker_ref_t *  workers_arr = NULL;

    pid = getpid();
    p = get_parent_ptr( false );

    if( p != NULL && p->pid == pid )
        return ( struct worker * ) to_ptr( worker_context, parent );

    if( workers == NULLREF )
        return NULL;

    workers_arr = ( worker_ref_t * ) to_ptr( workers_context, workers );

    for( i = 0; i < get_worker_count(); i++ )
    {
        worker = ( struct worker * ) to_ptr( worker_context, workers_arr[i] );

        if( worker == NULL )
            continue;

        if( worker->pid == pid )
            return worker;
    }

    return NULL;
}

void __term( void )
{
    struct worker * me       = NULL;
    struct stat     filestat = {0};
    //worker_ref_t    me_ref   = NULLREF;

    //me_ref = get_worker_by_pid();
    //me = ( struct worker * ) to_ptr( worker_context, me_ref );
    me = get_worker_ptr_by_pid();

    if( me == NULL )
        exit( 0 );

    if( me->type == WORKER_TYPE_PARENT )
    {
        // Wait for children to shut down and purge WAL buffers if at
        // all possible, then cleanup
        if( pid_file_name != NULL && stat( pid_file_name, &filestat ) >= 0 )
        {
            if( remove( pid_file_name) != 0 )
            {
                _log(
                    LOG_LEVEL_ERROR,
                    "Failed to remove PID file '%s'",
                    pid_file_name
                );
            }

            free( pid_file_name );
            pid_file_name = NULL;
        }

        if( log_file != NULL && !no_log )
        {
            fclose( log_file );
        }

        log_file = NULL;
    }
    else
    {
        free_worker( get_worker_by_pid() );
    }

    exit( 0 );
}

bool initialize_util_contexts( void )
{
    if( worker_context == INVALID_CONTEXT )
    {
        worker_context = new_slab( WORKER_CONTEXT_NAME, sizeof( struct worker ) );

        if( worker_context == INVALID_CONTEXT )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to initialize worker context"
            );
            return false;
        }
    }

    if( array_context == INVALID_CONTEXT )
    {
        array_context = new_slab( ARRAY_CONTEXT_NAME, sizeof( string_ref_t ) );

        if( array_context == INVALID_CONTEXT )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to initialize array context"
            );
            return false;
        }
    }

    if( workers_context == INVALID_CONTEXT )
    {
        workers_context = new_slab( WORKERS_CONTEXT_NAME, sizeof( worker_ref_t ) );

        if( workers_context == INVALID_CONTEXT )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to initialize pid table context"
            );
            return false;
        }
    }

    if( string_context == INVALID_CONTEXT )
    {
        string_context = new_slab( STRING_CONTEXT_NAME, sizeof( char ) );

        if( string_context == INVALID_CONTEXT )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed ti initialize string context"
            );
            return false;
        }
    }

    return true;
}

context_t get_worker_context( void )
{
    return worker_context;
}

context_t get_workers_context( void )
{
    return workers_context;
}

context_t get_array_context( void )
{
    return array_context;
}

context_t get_string_context( void )
{
    return string_context;
}

char * get_pid_file( void )
{
    return pid_file_name;
}
