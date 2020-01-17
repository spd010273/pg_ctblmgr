#include "pg_ctblmgr.h"

int main( int argc, char ** argv )
{
    int worker_count = 0;

    _parse_args( argc, argv );

    if( !parent_init( argc, argv ) )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Failed to initialize parent process"
        );
    }

    if( !db_connect( parent ) )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Failed to connect to database"
        );
    }

    if( !extension_installed() )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Extension " EXTENSION_NAME " is not installed"
        );
    }

    if( !setup_replication_slot( parent ) )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Failed to create replication slot"
        );
    }

    if( !initialize_buffer( parent ) )
    {
        destroy_replication_slot( parent );
        _log(
            LOG_LEVEL_FATAL,
            "Failed to initialize WAL buffers"
        );
    }

    worker_count = start_workers();

    if( worker_count < 0 )
    {
        destroy_replication_slot( parent );
        _log(
            LOG_LEVEL_INFO,
            "No tables to maintain, shutting down..."
        );
        // Maybe make a standby mode and listen on the maint channel
        __term();
    }

    // Main loop
    while( true )
    {

    }

    destroy_replication_slot( parent );
    __term();
}

static int start_workers( void )
{
    PGresult *       result       = NULL;
    struct worker ** temp         = NULL;
    char *           channel      = NULL;
    char **          filter       = NULL;
    char *           wal_level    = NULL;
    unsigned int     i            = 0;
    unsigned int     worker_count = 0;
    unsigned int     num_tables   = 0;

    if( parent == NULL || parent->type != WORKER_TYPE_PARENT )
    {
        return -1;
    }

    result = execute_query( parent, ( char * ) get_worker_list, NULL, 0 );

    if( result == NULL || PQntuples( result ) <= 0 )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to read in worker list"
        );

        return -1;
    }

    worker_count = PQntuples( result );
    _log(
        LOG_LEVEL_DEBUG,
        "Need to start %u worker(s)",
        worker_count
    );

    if( workers == NULL || num_workers == 0 || worker_count > num_workers )
    {
        temp = create_shared_memory( sizeof( struct worker * ) * worker_count );

        if( temp == NULL )
        {
            return -1;
        }
    }

    if( workers == NULL )
    {
        num_workers = worker_count;
    }
    else
    {
        memcpy( temp, workers, num_workers );
        munmap( workers, num_workers * sizeof( struct worker * ) );
        num_workers = worker_count;
    }

    workers = temp;
    temp    = NULL;

    for( i = 0; i < worker_count; i++ )
    {
        channel   = get_column_value( (int) i, result, "maintenance_channel" );
        wal_level = get_column_value( (int) i, result, "wal_level" );

        if( get_worker_by_channel( channel ) == NULL )
        {
            for( i = 0; i < worker_count; i++ )
            {
                if( workers[i] == NULL )
                {
                    get_filter_tables_by_channel( parent, channel, &filter, &num_tables );
                    
                    if( num_tables == 0 )
                    {
                        return -1;
                    }

                    workers[i] = new_worker(
                        WORKER_TYPE_CHILD,
                        i,
                        parent->my_argc,
                        parent->my_argv,
                        worker_entrypoint,
                        NULL,
                        channel,
                        filter,
                        num_tables,
                        wal_level[0]
                    );

                    if( workers[i] == NULL )
                    {
                        return -1;
                    }

                    if( workers[i]->type == WORKER_TYPE_CHILD )
                    {
                        exit(0);
                    }
                }
            }
        }

        // Maybe check that this worker is running?
    }

    PQclear( result );
    return worker_count;
}

static bool extension_installed( void )
{
    PGresult * result    = NULL;
    char *     params[1] = {NULL};

    if( parent == NULL )
    {
        return false;
    }

    params[0] = EXTENSION_NAME;
    result = execute_query(
        parent,
        ( char * ) extension_check_query,
        params,
        1
    );

    if( result == NULL )
    {
        return false;
    }

    if( PQntuples( result ) > 0 )
    {
        return true;
    }

    PQclear( result );
    return false;
}

static void worker_entrypoint( void * data )
{
    struct worker *   me        = NULL;
    PGresult *        result    = NULL;

    if( data == NULL )
    {
        return;
    }

    me = ( struct worker * ) data;

    _log(
        LOG_LEVEL_DEBUG,
        "Worker %u at entrypoint",
        ( unsigned int ) getpid()
    );

    if( me->pid != getpid() )
    {
        // PID mismatch
        me->status = WORKER_STATUS_DEAD;
        return;
    }

    if( !db_connect( me ) )
    {
        // could not connect to database
        me->status = WORKER_STATUS_DEAD;
        return;
    }

    result = execute_query(
        me,
        "SELECT 1",
        NULL,
        0
    );

    if( result == NULL || me->conn == NULL )
    {
        // no conn
        me->status = WORKER_STATUS_DEAD;
        return;
    }

    PQclear( result );

    // Start main program
    me->status = WORKER_STATUS_IDLE;

    while( 1 )
    {
        if( me->conn == NULL )
        {
            db_connect( me );
        }

        // Start looking at buffer
    }

    me->status = WORKER_STATUS_DEAD;
    return;
}

static bool setup_replication_slot( struct worker * me )
{
    PGresult *      result    = NULL;
    char *          params[1] = {NULL};

    if( me == NULL || me->type != WORKER_TYPE_PARENT )
        return false;

    params[0] = MAIN_CHANNEL;

    result = execute_query(
        me,
        ( char * ) replication_check,
        params,
        1
    );

    if( result == NULL || me->conn == NULL )
        return false;
   
    if( PQntuples( result ) > 1 )
        return false; // slot already exists

    PQclear( result );

    result = execute_query(
        me,
        ( char * ) replication_slot_create,
        NULL,
        0
    );

    if( result == NULL || me->conn == NULL )
        return false;

    PQclear( result );
    return true;
}

/*
 * Go ahead and allocate memory for our data structure,
 * as well as prepolulate the Trie section with the distinct
 * tables we will be using
 */
static bool initialize_buffer( struct worker * me )
{
    struct buffer * buff          = NULL;
    char **         filter_tables = NULL;
    unsigned int    num_tables    = 0;
    unsigned int    i             = 0;

    if( me == NULL || me->type != WORKER_TYPE_PARENT )
        return false;

    new_buffer( &buff, NULL, NULL ); 

    get_filter_tables_by_channel(
        me,
        NULL,
        &filter_tables,
        &num_tables
    );

    if( num_tables == 0 )
    {
        return false;
    }

    buffer_populate_trie(
        &(me->buffer),
        filter_tables,
        num_tables
    );

    for( i = 0; i < num_tables; i++ )
    {
        free( filter_tables[i] );
    }

    free( filter_tables );

    return true;
}

static void get_filter_tables_by_channel(
    struct worker * me,
    char * channel,
    char *** filter,
    unsigned int * num_tables
)
{
    PGresult *   filter_result = NULL;
    char *       params[1]     = {NULL};
    unsigned int i             = 0;
    char *       table         = NULL;

    if( me == NULL || me->conn == NULL )
        return;

    if( channel == NULL )
    {
        filter_result = execute_query(
            me,
            ( char * ) get_distinct_filter_tables,
            NULL,
            0
        );
    }
    else
    {
        params[0] = channel;

        filter_result = execute_query(
            me,
            ( char * ) get_slot_filter_tables,
            params,
            1
        );
    }

    if( filter_result == NULL || me->conn == NULL )
    {
        *num_tables = 0;
        return;
    }

    if( PQntuples( filter_result ) == 0 )
    {
        *num_tables = 0;
        PQclear( filter_result );
        return;
    }

    *num_tables = PQntuples( filter_result );
    (*filter) = ( char ** ) calloc(
        sizeof( char * ),
        PQntuples( filter_result )
    );

    if( *filter == NULL )
    {
        *num_tables = 0;
        PQclear( filter_result );
        return;
    }

    for( i = 0; i < PQntuples( filter_result ); i++ )
    {
        table = get_column_value( (int) i, filter_result, "filter_table" );
        (*filter)[i] = ( char * ) calloc(
            sizeof( char ),
            strlen( table ) + 1
        );
          
        if( (*filter)[i] == NULL )
        {
            *num_tables = 0;
            PQclear( filter_result );
            return;
        }

        strncpy( (*filter)[i], table, strlen( table ) );
        (*filter)[i][strlen(table) + 1] = '\0';
    }

    PQclear( filter_result );
    return;
}

static void destroy_replication_slot( struct worker * me )
{
    PGresult * result = NULL;

    if( me == NULL || me->type != WORKER_TYPE_PARENT )
        return;

    result = execute_query(
        me,
        ( char * ) replication_slot_destroy,
        NULL,
        0
    );

    if( result == NULL || me->conn == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to drop replication slot, please remove manually"
        );
    }

    PQclear( result );
    return;
}
