#include "pg_ctblmgr.h"

int main( int argc, char ** argv )
{
    int worker_count = 0;
    set_nolog();
    _parse_args( argc, argv );

    if( !parent_init( argc, argv ) )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Failed to initialize parent process"
        );
    }

    if( !initialize_contexts() )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Failed to initialize shared memory contexts"
        );
    }

    if( !initialize_changeset_context() )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Failed to initialize changeset shared memory context"
        );
    }

    if( !db_connect( NULL ) )
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

    if( !setup_replication_slot() )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Failed to create replication slot"
        );
    }

    if( !initialize_buffer() )
    {
        _log(
            LOG_LEVEL_FATAL,
            "Failed to initialize WAL buffers"
        );
    }

    worker_count = start_workers();

    if( worker_count <= 0 )
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
    char **         filter_tables = NULL;
    char *          output        = NULL;
    uint16_t        num_tables    = 0;
    uint16_t        i             = 0;
    uint32_t        size          = 0;
    struct worker * p             = NULL;

    p = get_parent_ptr( true );

    if( p == NULL )
        return NULL;

    get_filter_tables_by_channel(
        p,
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
    char                qual[QUAL_MAX] = {0};
    changeset_ref_t     changeset      = NULLREF;
    buffer_pin_ref_t    bufferpin      = NULLREF;
    changeset_ref_t *   changeset_arr  = NULL;
    struct changeset *  cs             = NULL;
    struct changeset *  cs_array       = NULL;
    unsigned int        num_cs_array   = 0;
    char *              commit_lsn     = NULL;
    char *              schema_name    = NULL;
    char *              table_name     = NULL;
    bool                no_seek        = false;
    struct worker *     p              = NULL;

    p = get_parent_ptr( true );

    if( p == NULL )
        return;

    filter_tables = get_filter_tables_string();

    if( filter_tables == NULL )
        return;

    // XXX Another strategy here is to just have the parent feed each worker's buffer,
    // then turn around and mop up after they've been consumed.
    // The tough part here is not losing xlog changes. Especially in the case where >1 worker
    // relies on changes from one table and finish at different rates
_ML:while( true )
    {
        /*
         * Parent needs to:
         *     - Verify workers are still running and alive
         *     - Collect and maintain statistics
         *     - Consume translated WAL and insert into each child's SLPQ / trie
         */
        if( got_sigterm )
            __term();

        if( got_sigint )
            __term();

        sleep( 1.0 );
        if(
                get_changeset_batch(
                    filter_tables,
                    &changeset_arr,
                    &num_cs_array,
                    &commit_lsn
                )
          )
        {
            _log( LOG_LEVEL_DEBUG, "Got CS array %p, size: %u lsn %s", changeset_arr, num_cs_array, commit_lsn );
            if( num_cs_array == 0 )
            {
                _log( LOG_LEVEL_DEBUG, "No changes for our filter table(s)" );
                // If num_cs_array == 0 && commit_lsn != NULL && get_changeset_batch() returned true, we can
                // assume it's safe to seek to that LSN. Could have been a DDL change and we're just seeing:
                // BEGIN
                // < DDL that isn't sent in the logical replication slot >
                // COMMIT
                _log( LOG_LEVEL_DEBUG, "Parent seeking to change %s", commit_lsn );
                params[0] = MAIN_CHANNEL;
                params[1] = commit_lsn;
                params[2] = "M";
                params[3] = filter_tables;

                result = execute_query(
                    p,
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

                _log( LOG_LEVEL_DEBUG, "Committed LSN is %s", commit_lsn );
                free( commit_lsn );
                commit_lsn = NULL;
                continue; // no committed changes
            }

            // Failure mode here is to get 'stuck'. We don't want to lose transactions if at all possible,
            // so if we have trouble parsing a specific changeset, we'll wedge ourselves between the peek
            // and failure point to avoid seeking that change out of the queue.
            for( i = 0; i < num_cs_array; i++ )
            {
                changeset = changeset_arr[i];
                if( unlikely( changeset == NULLREF ) )
                {
                    _log(
                        LOG_LEVEL_ERROR,
                        "Bad changeset at index %u in changeset array %p",
                        ( uint32_t ) i,
                        cs_array
                    );
                    goto _ML;
                }

                cs = ( struct changeset * ) to_ptr( get_changeset_context(), changeset );

                if( unlikely( cs == NULL ) )
                {
                    _log(
                        LOG_LEVEL_ERROR,
                        "Dereferenced changeset at ref_t %lu is NULL (index: %u)",
                        ( uint64_t ) changeset,
                        ( uint32_t ) i
                    );
                    goto _ML;
                }

                _log( LOG_LEVEL_DEBUG, "Got change LSN %s", offset_to_lsn( cs->lsn ) );
                //dump_changeset( changeset );
                _log( LOG_LEVEL_DEBUG, "Propogating changes to %d workers", get_worker_count() );
                schema_name = ( char * ) to_ptr( get_changeset_string_context(), cs->schema_name );
                table_name  = ( char * ) to_ptr( get_changeset_string_context(), cs->table_name );

                if( schema_name == NULL || table_name == NULL )
                {
                    _log(
                        LOG_LEVEL_ERROR,
                        "Invalid schema or table name %s.%s",
                        schema_name == NULL ? "NULL" : schema_name,
                        table_name == NULL ? "NULL" : table_name
                    );
                    goto _ML;
                }

                memset( qual, '\0', QUAL_MAX );
                strncpy( qual, schema_name, strlen( schema_name ) );
                strncat( qual, ".", 1 );
                strncat( qual, table_name, strlen( table_name ) );

                bufferpin = buffer_get_pin_by_name( p->buffer, qual );

                if( unlikely( bufferpin == NULLREF ) )
                {
                    _log(
                        LOG_LEVEL_ERROR,
                        "Got invalid bufferpin for worker %lu",
                        ( uint64_t ) j
                    );
                    no_seek = true;
                    continue;
                }

                if( !buffer_pin_push( bufferpin, ( ref_t ) changeset ) )
                {
                    _log(
                        LOG_LEVEL_ERROR,
                        "Failed to push changeset for %s.%s to buffer",
                        schema_name,
                        table_name
                    );
                    no_seek = true;
                    continue;
                }
                else
                {
                    _log(
                        LOG_LEVEL_DEBUG,
                        "Pushed changeset %p to bp %p for qual %s",
                        cs,
                        to_ptr( get_buffer_pin_context(), bufferpin ),
                        qual
                    );
                }

                schema_name = NULL;
                table_name  = NULL;
            }

            num_cs_array = 0;
            /* for testing - disable seek of records
            if( !no_seek )
            {
                params[0] = MAIN_CHANNEL;
                params[1] = commit_lsn;
                params[2] = "M";
                params[3] = filter_tables;

                result = execute_query(
                    p,
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

                _log( LOG_LEVEL_DEBUG, "Committed LSN is %s", commit_lsn );
            }
            */
            // Need a reset mechanism for no_seek. We need to detect if the workers have miraculously
            // caught up, though this mechanism is supposed to keep the bad changeset's isolated until
            // a human can intervene
            free( commit_lsn );
            commit_lsn = NULL;
            free( cs_array );
            cs_array = NULL;
            PQclear( result );
        }
        else
        {
            _log( LOG_LEVEL_ERROR, "Failure in fetching changesets" );
        }

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
    char *           channel      = NULL;
    char **          filter       = NULL;
    char *           wal_level    = NULL;
    uint16_t         i            = 0;
    uint16_t         j            = 0;
    uint16_t         worker_count = 0;
    uint16_t         num_tables   = 0;
    struct worker *  p            = NULL;
    worker_ref_t *   w_arr        = NULL;

    if( parent == NULLREF )
        return -1;

    p = get_parent_ptr( true );

    if( p == NULL )
        return -1;

    result = execute_query( p, ( char * ) get_worker_list, NULL, 0 );

    if( result == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to read in worker list"
        );

        return -1;
    }

    if( PQntuples( result ) <= 0 )
    {
        return 0;
    }

    worker_count = PQntuples( result );
    _log(
        LOG_LEVEL_DEBUG,
        "Need to start %u worker(s)",
        worker_count
    );

    if( workers == NULLREF )
    {
        // Setup workers[] pid table
        workers = ( workers_ref_t ) rsmalloc(
            get_workers_context(),
            sizeof( worker_ref_t ) * worker_count
        );

        if( workers == NULLREF )
        {
            _log(
                LOG_LEVEL_FATAL,
                "Failed to initialize PID table"
            );
            return -1;
        }

        w_arr = ( worker_ref_t * ) to_ptr( get_workers_context(), workers );

        if( w_arr == NULL )
        {
            _log( LOG_LEVEL_ERROR, "Workers array dereferenced to NULL" );
            return -1;
        }
        for( i = 0; i < worker_count; i++ )
            w_arr[i] = NULLREF;
    }
    else if( worker_count > get_worker_max() )
    {
        workers = ( workers_ref_t ) rsrealloc(
            get_workers_context(),
            ( ref_t ) workers,
            sizeof( worker_ref_t ) * worker_count
        );

        if( workers == NULLREF )
        {
            _log(
                LOG_LEVEL_FATAL,
                "Failed to resize PID table"
            );
            return -1;
        }

        w_arr = ( worker_ref_t * ) to_ptr( get_workers_context(), workers );

        if( w_arr == NULL )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Workers array reallocation dereferenced to NULL"
            );
            return -1;
        }

        for( i = get_worker_max() - 1; i < worker_count; i++ )
            w_arr[i] = NULLREF;

        p->max_workers = worker_count;
    }
    else
    {
        w_arr = ( worker_ref_t * ) to_ptr( get_workers_context(), workers );
    }

    if( w_arr == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to dereference workers[] array"
        );
        return -1;
    }

    if( worker_count > get_worker_count() )
    {
        p->num_workers = worker_count;

        for( i = 0; i < worker_count; i++ )
        {
            channel   = get_column_value( (int) i, result, "maintenance_channel" );
            wal_level = get_column_value( (int) i, result, "wal_level" );

            if( get_worker_by_channel( channel ) == NULLREF )
            {
                // worker for this channel isn't started
                // need to look for an open worker slot
                for( j = 0; j < worker_count; j++ )
                {
                    if( w_arr[j] == NULLREF )
                    {
                        get_filter_tables_by_channel(
                            p,
                            channel,
                            &filter,
                            &num_tables
                        );

                        if( num_tables == 0 )
                        {
                            _log( LOG_LEVEL_DEBUG, "Cannot start worker: no tables" );
                            return -1;
                        }

                        _log( LOG_LEVEL_DEBUG, "Launching worker %u channel %s", ( uint32_t ) i, channel );
                        w_arr[j] = new_worker(
                            WORKER_TYPE_CHILD,
                            i,
                            p->my_argc,
                            p->my_argv,
                            worker_entrypoint,
                            NULLREF,
                            channel,
                            filter,
                            num_tables,
                            wal_level[0]
                        );

                        if( w_arr[i] == NULLREF )
                        {
                            return -1;
                        }
                    }
                }
            }

            // Maybe check that this worker is running?
        }
    }

    PQclear( result );
    return worker_count;
}

static bool extension_installed( void )
{
    PGresult *      result    = NULL;
    char *          params[1] = {NULL};
    struct worker * p         = NULL;

    p = get_parent_ptr( true );

    if( p == NULL )
        return false;

    params[0] = EXTENSION_NAME;
    result = execute_query(
        p,
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
    struct worker *      me        = NULL;
    PGresult *           result    = NULL;
    buffer_pin_ref_t   * pins      = NULL;
    uint32_t             i         = 0;
    uint64_t             lsn       = 0;
    uint64_t             max_lsn   = 0;
    struct changeset *   cs        = NULL;
    changeset_ref_t      changeset = NULLREF;
    char *               currlsn   = NULL;
    char *               lastlsn   = NULL;
    string_ref_t *       ft_arr    = NULL;
    char *               ft_elem   = NULL;

    // TODO: Lets be clever about the sleep timer on a per-process basis - we can either get woken up by the parent
    // when changes get pushed into our data structuer, or we can have a running-average type rate, and check based on that.

    if( data == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Worker received NULL data struct"
        );
        return;
    }

    if( !initialize_contexts() )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Worker failed to initialize buffer contexts"
        );
        return;
    }

    if( !initialize_changeset_context() )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Worker failed to initialize changeset context"
        );
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

    while( pins == NULL )
    {
        get_worker_pins( &pins );
        sleep( 1 );
        _log( LOG_LEVEL_DEBUG, "Worker waiting on pins (%p)...", pins );
    }

    _log( LOG_LEVEL_DEBUG, "Pins: %p", pins );
    ft_arr = ( string_ref_t * ) to_ptr(
        get_array_context(),
        me->config.filter_tables
    );

    if( ft_arr == NULL )
    {
        _log( LOG_LEVEL_ERROR, "Failed to dereference filter tables[]" );
        return;
    }

_CL:while( 1 )
    {
        sleep( 2 );
        if( me->conn == NULL )
        {
            db_connect( me );
        }
        /*
         * Worker needs to:
         *     - consume SLPQ entries from oldest LSN to newest for tables
         */

        // Start looking at buffer
        _log( LOG_LEVEL_DEBUG, "Worker in main loop" );
        for( i = 0; i < me->config.num_tables; i++ )
        {
            changeset = ( changeset_ref_t ) buffer_pin_pop( pins[i] );
            _log(
                LOG_LEVEL_DEBUG,
                "Worker popped %lu from pin %lu",
                ( uint64_t ) data,
                ( uint64_t ) pins[i]
            );
            if( changeset == NULLREF )
            {
                ft_elem = ( char * ) to_ptr(
                    get_string_context(),
                    ( ref_t ) ft_arr[i]
                );
                _log(
                    LOG_LEVEL_DEBUG,
                    "Buffer pin %lu (%s) empty",
                    ( uint64_t ) pins[i],
                    ft_elem
                );
                continue;
                //goto _CL;
            }
            else
            {
                // data is a valid changeset and we'll add it to our todo list
                cs = ( struct changeset * ) to_ptr( get_changeset_context(), changeset );

                if( unlikely( cs == NULL ) )
                {
                    _log(
                        LOG_LEVEL_ERROR,
                        "Failed to dereference changeset ref %lu",
                        ( uint64_t ) changeset
                    );
                    goto _CL;
                }

                lsn = cs->lsn;

                dump_changeset( changeset );
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

                // Do stuff with the changeset
            }
        }
    }

    me->status = WORKER_STATUS_DEAD;
    return;
}

static void get_worker_pins( buffer_pin_ref_t ** bp_array )
{
    uint32_t        i       = 0;
    struct worker * me      = NULL;
    string_ref_t *  arr     = NULL;
    char *          ft_elem = NULL;

    me = get_worker_ptr_by_pid();

    if( me == NULL || bp_array == NULL )
    {
        _log( LOG_LEVEL_DEBUG, "NULL worker or BP array" );
        return;
    }

    if( me->config.num_tables == 0 )
    {
        _log( LOG_LEVEL_DEBUG, "no pins, num tables == 0" );
        return;
    }

    if( *bp_array != NULL )
    {
        free( *bp_array );
        (*bp_array) = NULL;
    }

    _log( LOG_LEVEL_DEBUG, "worker allocating %u pins", me->config.num_tables );
    *bp_array = ( buffer_pin_ref_t * ) calloc(
        me->config.num_tables,
        sizeof( buffer_pin_ref_t )
    );

    if( *bp_array == NULL )
    {
        _log( LOG_LEVEL_ERROR, "Failed to allocate BP array" );
        return;
    }

    if( me->config.filter_tables == NULLREF )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Worker missing filter_tables array"
        );
        return;
    }

    arr = ( string_ref_t * ) to_ptr( get_array_context(), me->config.filter_tables );

    if( arr == NULL )
    {
        _log( LOG_LEVEL_ERROR, "Failed to dereference filter_tables[]" );
        return;
    }

    for( i = 0; i < me->config.num_tables; i++ )
    {
        if( arr[i] == NULLREF )
        {
            _log(
                LOG_LEVEL_ERROR,
                "NULLREF in filter_tables[%u]",
                ( uint32_t ) i
            );
            continue;
        }

        ft_elem = ( char * ) to_ptr( get_string_context(), arr[i] );

        if( ft_elem == NULL )
        {
            _log(
                LOG_LEVEL_ERROR,
                "NULL dereference for filter_tables[%u]",
                ( uint32_t ) i
            );
            continue;
        }

        _log( LOG_LEVEL_DEBUG, "Adding pin for table %s", ft_elem );
        (*bp_array)[i] = buffer_get_pin_by_name(
            me->buffer,
            ft_elem
        );
    }

    return;
}

static bool setup_replication_slot( void )
{
    PGresult *      result    = NULL;
    char *          params[1] = {NULL};
    struct worker * me        = NULL;

    me = get_worker_ptr_by_pid();

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
static bool initialize_buffer( void )
{
    struct worker * me            = NULL;
    char **         filter_tables = NULL;
    uint16_t        num_tables    = 0;
    uint16_t        i             = 0;

    me = get_parent_ptr( true );

    if( me == NULL )
        return false;

    _log( LOG_LEVEL_DEBUG, "Parent got buffer %lu", ( uint64_t ) me->buffer );
    get_filter_tables_by_channel(
        me,
        NULL,
        &filter_tables,
        &num_tables
    );

    if( num_tables == 0 )
    {
        _log( DEBUG, "No tables in channel" );
        return true;
    }

    // This will initialize the buffer object by-reference
    buffer_populate_trie(
        &(me->buffer),
        filter_tables,
        num_tables
    );

    _log(
        LOG_LEVEL_DEBUG,
        "Populated trie with %u tables, reference given is %lu",
        num_tables,
        ( uint64_t ) me->buffer
    );

    for( i = 0; i < num_tables; i++ )
    {
        free( filter_tables[i] );
    }

    free( filter_tables );

    return true;
}


// We have a possibility of caching these results rather than continuously querying the database
static void get_filter_tables_by_channel(
    struct worker * me,
    char *          channel,
    char ***        filter,
    uint16_t *      num_tables
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

/*
 *   scans the wal records looking for a batch of changes that have been committed
 *   These are returned in the result, with the size of the array in num_results.
 *   The commit lsn is set to the LSN of the commit message for the batch.
 *   In the case where the transaction was aborted / rolled back, the results will be
 *   null but the function will return true. return of false indicates an error
 */
static bool get_changeset_batch(
    char *               filter_tables,
    changeset_ref_t   ** result,
    unsigned int *       num_results,
    char **              commit_lsn
)
{
    char *             params[4]   = {NULL};
    PGresult *         pgresult    = NULL;
    unsigned int       i           = 0;
    unsigned int       j           = 0;
    char *             data        = NULL;
    uint32_t           xid         = 0;
    uint64_t           lsn         = 0;
    char *             lsn_str     = NULL;
    struct changeset * cs          = NULL;
    changeset_ref_t    changeset   = NULLREF;
    bool               begin_found = false;
    struct worker *    p           = NULL;

    p = get_parent_ptr( true );

    if( p == NULL )
        return false;

    if( result == NULL || num_results == NULL || commit_lsn == NULL )
        return false;

    *num_results = 0;
    params[0] = MAIN_CHANNEL;
    params[1] = "F";
    params[2] = filter_tables;

    pgresult = execute_query(
        p,
        ( char * ) replication_peek,
        params,
        3
    );

    if( pgresult == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Replication peek failed"
        );
        return false;
    }

    _log( LOG_LEVEL_DEBUG, "Got PQntuples %u", ( uint32_t ) PQntuples( pgresult ) );
    if( PQntuples( pgresult ) == 0 )
        return true;

    for( i = 0; i < PQntuples( pgresult ); i++ )
    {
        data      = get_column_value( i, pgresult, "data" );
        xid       = xid_in( get_column_value( i, pgresult, "xid" ) );
        lsn_str   = get_column_value( i, pgresult, "lsn" );
        lsn       = lsn_to_offset( lsn_str );
        _log( LOG_LEVEL_DEBUG, "Entering changeset parse for xid %s", get_column_value( i, pgresult, "xid" ) );
        changeset = json_to_changeset( data, PGC_WAL_FULL );

        if( changeset == NULLREF )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to parse changeset at xid %u, LSN %s",
                xid,
                lsn_str
            );
            return false;
        }

        cs = ( struct changeset * ) to_ptr( get_changeset_context(), changeset );

        if( cs == NULL )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to dereference changeset at xid %u, LSN %s CS: %lu",
                xid,
                lsn_str,
                ( uint64_t ) changeset
            );
        }
        if( begin_found )
        {
            // XXX We assume that the results arrive in order IE: BEGIN DML DML .. COMMIT|ROLLBACK,
            // ... might want to ensure that this happens
            // We can also get the situation where no changes happened that are relevent to us ie:
            // BEGIN <no DML> COMMIT
            _log( LOG_LEVEL_DEBUG, "BEGIN located, parsing changes" );
            if(
                   cs->type == PGC_DML_INSERT
                || cs->type == PGC_DML_UPDATE
                || cs->type == PGC_DML_DELETE
              )
            {
                cs->lsn = lsn;
                (*result)[*num_results] = changeset;
                (*num_results)++;
                _log(
                    LOG_LEVEL_DEBUG,
                    "Stored change in LSN %s, index %u, OP %s",
                    lsn_str, 
                    (*num_results) - 1,
                    cs->type == PGC_DML_INSERT ? "INSERT" :
                    cs->type == PGC_DML_UPDATE ? "UPDATE" :
                    cs->type == PGC_DML_DELETE ? "DELETE" : "UNKNOWN"
                );
            }
            else if( cs->type == PGC_DML_ROLLBACK )
            {
                _log( LOG_LEVEL_DEBUG, "ROLLBACK located, cleaning up..." );
                free_changeset( changeset );

                for( j = 0; j < *num_results; j++ )
                {
                    free_changeset( (*result)[j] );
                    (*result)[j] = NULLREF;
                }

                free( *result );
                (*num_results) = 0;
                begin_found    = false;
                cs             = NULL;
                changeset      = NULLREF;
            }
            else if( cs->type == PGC_DML_COMMIT )
            {
                // Save LSN of commit message
                _log( LOG_LEVEL_DEBUG, "COMMIT record found for LSN %s", lsn_str );
                free_changeset( changeset );
                begin_found = false;
                cs          = NULL;
                (*commit_lsn) = ( char * ) calloc(
                    strlen( lsn_str ) + 1,
                    sizeof( char )
                );

                if( (*commit_lsn) == NULL )
                {
                    PQclear( pgresult );
                    return false;
                }

                strncpy( *commit_lsn, lsn_str, strlen( lsn_str ) );
                PQclear( pgresult );
                return true;
            }
        }
        else if( cs != NULL && cs->type == PGC_DML_BEGIN )
        {
            begin_found = true;
            (*result)   = ( changeset_ref_t * ) calloc(
                PQntuples( pgresult ) - 2, // exclude BEGIN & COMMIT/ROLLBACK
                sizeof( changeset_ref_t )
            );

            _log(
                LOG_LEVEL_DEBUG,
                "Located BEGIN record, allocating changeset array size %u",
                ( uint32_t ) ( PQntuples( pgresult ) - 2 )
            );
            // Discard BEGIN changeset
            free_changeset( changeset );

            if( (*result) == NULL )
            {
                _log( LOG_LEVEL_ERROR, "Allocation for local changeset array failed" );
                PQclear( pgresult );
                return false;
            }
        }
    }

    if( begin_found )
    {
        _log(
            LOG_LEVEL_WARNING,
            "Incomplete commit record found, last good LSN was %s",
            lsn_str
        );
    }

    PQclear( pgresult );
    return true;
}
