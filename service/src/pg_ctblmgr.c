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

    worker_count = start_workers();

    if( worker_count < 0 )
    {
        return 1;
    }

    // Main loop
    while( true )
    {

    }

    return 0;
}

static int start_workers( void )
{
    PGresult *        result       = NULL;
    struct worker **  temp         = NULL;
    char *            channel      = NULL;
    char *            filter       = NULL;
    char *            wal_level    = NULL;
    unsigned long int i            = 0;
    unsigned long int worker_count = 0;

    if( parent == NULL || parent->type != WORKER_TYPE_PARENT )
    {
        return -1;
    }

    result = _execute_query( parent, ( char * ) get_worker_list, NULL, 0 );

    if( result == NULL || PQntuples( result ) <= 0 )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to read in worker list"
        );

        return -1;
    }

    worker_count = PQntuples( result );

    if( workers == NULL || num_workers == 0 || worker_count > num_workers )
    {
        temp = create_shared_memory( sizeof( struct worker * ) * num_workers );

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
        channel   = get_column_value( (int) i, result, "slot_name" );
        filter    = get_column_value( (int) i, result, "filter" );
        wal_level = get_column_value( (int) i, result, "wal_level" );

        if( get_worker_by_channel( channel ) == NULL )
        {
            for( i = 0; i < worker_count; i++ )
            {
                if( workers[i] == NULL )
                {
                    workers[i] = new_worker(
                        WORKER_TYPE_CHILD,
                        i,
                        parent->my_argc,
                        parent->my_argv,
                        worker_entrypoint,
                        NULL,
                        channel,
                        filter,
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
    PGresult * result = NULL;

    if( parent == NULL )
    {
        return false;
    }

    result = _execute_query( parent, ( char * ) extension_check_query, NULL, 0 );

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
    struct worker *   me         = NULL;
    PGresult *        wal_result = NULL;
    char *            params[3]  = {NULL};
    unsigned long int i          = 0;

    if( data == NULL )
    {
        return;
    }

    me = ( struct worker * ) data;

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

    wal_result = _execute_query(
        me,
        "SELECT 1",
        NULL,
        0
    );

    if( wal_result == NULL || me->conn == NULL )
    {
        // no conn
        me->status = WORKER_STATUS_DEAD;
        return;
    }

    PQclear( wal_result );
    params[0] = me->config.channel;

    // Verify that our target replication slot exists
    wal_result = _execute_query(
        me,
        ( char * ) replication_check,
        params,
        1
    );

    if( wal_result == NULL )
    {
        // Could not get slot status
        me->status = WORKER_STATUS_DEAD;
        return;
    };

    if(
            strcmp(
                get_column_value(
                    1,
                    wal_result,
                    "plugin"
                ),
                PLUGIN_NAME
            ) != 0
         || strcmp(
                get_column_value(
                    1,
                    wal_result,
                    "slot_type"
                ),
                "logical"
           ) != 0
      )
    {
        // Slot doesnt exist or is not logical
        me->status = WORKER_STATUS_DEAD;
        PQclear( wal_result );
        return;
    }

    PQclear( wal_result );

    params[1] = &(me->config.wal_level);
    params[2] = me->config.filter_tables;
    // Start main program
    me->status = WORKER_STATUS_IDLE;

    while( 1 )
    {
        if( me->conn == NULL )
        {
            db_connect( me );
        }

        wal_result = _execute_query(
            me,
            ( char * ) replication_seek,
            params,
            1
        );

        if( wal_result == NULL )
        {
            continue;
        }

        if( PQntuples( wal_result ) > 0 )
        {
            me->status = WORKER_STATUS_PROCESS_WAL;

            for( i = 0; i < PQntuples( wal_result ); i++ )
            {

            }
        }

        PQclear( wal_result );
    }

    me->status = WORKER_STATUS_DEAD;
    return;
}
