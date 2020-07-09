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
        _log(
            LOG_LEVEL_FATAL,
            "Failed to initialize WAL buffers"
        );
    }

    worker_count = start_workers();

    if( worker_count < 0 )
    {
        _log(
            LOG_LEVEL_INFO,
            "No tables to maintain, shutting down..."
        );
        // Maybe make a standby mode and listen on the maint channel
        __term();
    }

    parent_main_loop();
    __term();
}

static char * get_filter_tables_string( void )
{
    char **      filter_tables = NULL;
    char *       output        = NULL;
    unsigned int num_tables    = 0;
    unsigned int i             = 0;
    unsigned int size          = 0;

    get_filter_tables_by_channel(
        parent,
        NULL,
        &filter_tables,
        &num_tables
    );

    if( filter_tables == NULL || num_tables == 0 )
        return NULL;

    for( i = 0; i < num_tables; i++ )
    {
        size += strlen( filter_tables[i] ) + 1;
    }

    size++;

    output = ( char * ) calloc(
        size,
        sizeof( char )
    );

    if( output == NULL )
        return NULL;

    strncpy( output, filter_tables[0], strlen( filter_tables[0] ) );
    free( filter_tables[0] );

    for( i = 1; i < num_tables; i++ )
    {
        strncat( output, ",", 1 );
        strncat( output, filter_tables[i], strlen( filter_tables[i] ) );
        free( filter_tables[i] );
    }

    free( filter_tables );
    return output;
}

static void parent_main_loop( void )
{
    char *              params[4]      = {NULL};
    char *              filter_tables  = NULL;
    PGresult *          result         = NULL;
    unsigned int        i              = 0;
    unsigned int        j              = 0;
    uint32_t            xid            = 0;
    char *              data           = NULL;
    char                qual[QUAL_MAX] = {0};
    struct changeset *  cs             = NULL;
    struct buffer_pin * bp             = NULL;
    uint64_t            curr_lsn       = 0;
    uint64_t            latest_lsn     = 0;

    filter_tables = get_filter_tables_string();

    if( filter_tables == NULL )
        return;

    // XXX Another strategy here is to just have the parent feed each worker's buffer,
    // then turn around and mop up after they've been consumed.
    // The tough part here is not losing xlog changes. Especially in the case where >1 worker
    // relies on changes from one table and finish at different rates
    while( true )
    {
        /*
         * Parent needs to:
         *     - Verify workers are still running and alove
         *     - Collect and maintain statistics
         *     - Consume translated WAL and insert into each child's SLPQ / trie
         */
        sleep( 1.0 );

        // Start of proto loop
        params[0] = MAIN_CHANNEL;
        params[1] = "F"; // Need way to switch this?
        params[2] = filter_tables; // Filter tables

        result = execute_query(
            parent,
            ( char * ) replication_peek,
            params,
            3
        );

        if( result == NULL )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Replication peek failed"
            );
            continue;
        }

        // This could be made more efficient by constructing a trie
        // pointing to an array of BPs so it's not exactly N**2
        for( i = 0; i < PQntuples( result ); i++ )
        {
            data     = get_column_value( i, result, "data" );
            xid      = xid_in( get_column_value( i, result, "xid"  ) );
            curr_lsn = lsn_to_offset( get_column_value( i, result, "lsn" ) );
            cs       = json_to_changeset( data, PGC_WAL_FULL );
            cs->lsn  = curr_lsn;

            _log(
                LOG_LEVEL_DEBUG,
                "Processing XID %u, LSN %s",
                xid,
                get_column_value( i, result, "lsn" )
            );

            for( j = 0; j < num_workers; j++ )
            {
                memset( qual, 0, QUAL_MAX );
                strncpy( qual, cs->schema_name, strlen( cs->schema_name ) );
                strncat( qual, ".", 1 );
                strncat( qual, cs->table_name, strlen( cs->table_name ) );
                bp = buffer_get_pin_by_name( workers[i]->buffer, qual );

                if( bp != NULL )
                {
                    if( !buffer_pin_push( bp, ( void * ) cs ) )
                    {
                        _log(
                            LOG_LEVEL_ERROR,
                            "Failed to push changeset for %s.%s to worker %d",
                            cs->schema_name,
                            cs->table_name,
                            (workers[i])->pid
                        );
                    }
                }
            }

            if( curr_lsn > latest_lsn )
                latest_lsn = curr_lsn;
        }

        params[0] = MAIN_CHANNEL;
        params[1] = offset_to_lsn( latest_lsn );;
        params[2] = "M";
        params[3] = filter_tables;
        PQclear( result );
        result = execute_query(
            parent,
            ( char * ) replication_seek,
            params,
            4
        );

        if( result == NULL )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to consume changes up to LSN %s",
                params[1]
            );
        }

        free( params[1] );

        if( got_sighup )
        {
            free( filter_tables );
            filter_tables = get_filter_tables_string();
            if( filter_tables == NULL )
            {
                _log(
                    LOG_LEVEL_ERROR,
                    "Failed to get filter tables string"
                );
                return;
            }
        }
    }

    return;
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
                    get_filter_tables_by_channel(
                        parent,
                        channel,
                        &filter,
                        &num_tables
                    );

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

                    if( workers[i]->type != WORKER_TYPE_CHILD )
                    {
                        _log( LOG_LEVEL_FATAL, "Error creating new worker" );
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
        PQclear( result );
        return true;
    }

    PQclear( result );
    return false;
}

static void worker_entrypoint( void * data )
{
    struct worker *      me      = NULL;
    PGresult *           result  = NULL;
    struct buffer_pin ** pins    = NULL;
    unsigned int         i       = 0;
    uint64_t             lsn     = 0;
    uint64_t             max_lsn = 0;
    struct changeset *   cs      = NULL;
    char *               currlsn = NULL;
    char *               lastlsn = NULL;

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

    _log( LOG_LEVEL_DEBUG, "Worker entering main loop" );

    get_worker_pins( me, &pins );

    if( pins == NULL )
        return;

    while( 1 )
    {
        sleep( 1 );
        if( me->conn == NULL )
        {
            db_connect( me );
        }
        /*
         * Worker needs to:
         *     - consume SLPQ entries from oldest LSN to newest for tables
         */

        // Start looking at buffer
        for( i = 0; i < me->config.num_tables; i++ )
        {
            data = buffer_pin_pop( pins[i] );

            if( data == NULL )
            {
                _log(
                    LOG_LEVEL_DEBUG,
                    "Buffer pin %p (%s) empty",
                    pins[i],
                    me->config.filter_tables[i]
                );
            }
            else
            {
                // data is a valid changeset and we'll add it to our todo list
                cs = ( struct changeset * ) data;
                lsn          = cs->lsn;

                // Sanity check to ensure we are consuming changes in order
                if( cs->lsn <= me->last_lsn )
                {
                    currlsn = offset_to_lsn( cs->lsn );
                    lastlsn = offset_to_lsn( me->last_lsn );

                    _log(
                        LOG_LEVEL_ERROR,
                        "Out of order LSN encountered, "
                        "currently at %s, last %s",
                        currlsn,
                        lastlsn
                    );

                    free( currlsn );
                    free( lastlsn );
                }
                else
                {
                    me->last_lsn = lsn;

                    if( lsn > max_lsn )
                        max_lsn = lsn;
                }
            }
        }
    }

    me->status = WORKER_STATUS_DEAD;
    return;
}

static void get_worker_pins( struct worker * me, struct buffer_pin *** bp_array )
{
    unsigned int i = 0;

    if( me == NULL || bp_array == NULL )
        return;

    if( me->config.num_tables == 0 )
        return;

    if( *bp_array == NULL )
    {
        free( *bp_array );
        (*bp_array) = NULL;
    }

    *bp_array = ( struct buffer_pin ** ) calloc(
        me->config.num_tables,
        sizeof( struct buffer_pin * )
    );

    if( *bp_array == NULL )
        return;

    for( i = 0; i < me->config.num_tables; i++ )
    {
        _log( LOG_LEVEL_DEBUG, "Adding pin for table %s", me->config.filter_tables[i] );
        (*bp_array)[i] = buffer_get_pin_by_name(
            me->buffer,
            me->config.filter_tables[i]
        );
    }

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

    if( PQntuples( result ) > 0 )
    {
        _log( LOG_LEVEL_DEBUG, "Connecting to existing replication slot" );
        PQclear( result );
        return true;
    }

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
    char **      filter_tables = NULL;
    unsigned int num_tables    = 0;
    unsigned int i             = 0;

    if( me == NULL || me->type != WORKER_TYPE_PARENT )
        return false;

    new_buffer( &(me->buffer), NULL, NULL );

    get_filter_tables_by_channel(
        me,
        NULL,
        &filter_tables,
        &num_tables
    );

    if( num_tables == 0 )
    {
        _log( DEBUG, "No tables in channel" );
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
        (*filter)[i][strlen(table)] = '\0';
    }

    PQclear( filter_result );
    return;
}
