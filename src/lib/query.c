#include "query.h";

PGresult * _execute_query( struct worker * me, char * query, char ** params, unsigned int param_count )
{
    PGresult *   result              = NULL;
    char *       last_sql_state      = NULL;
    char *       temp_last_sql_state = NULL;
    unsigned int retry_counter       = 0;
    unsigned int last_backoff_time   = 1;
    unsigned int i                   = 0;

    if( me == NULL )
    {
        return NULL;
    }

    if( me->conn == NULL )
    {
        me->tx_in_progress = false;

        if( !db_connect( me ) )
        {
            _log( LOG_LEVEL_ERROR, "Failed to connect to database" );
            return NULL;
        }
    }

    while(
            PQstatus( me->conn ) != CONNECTION_OK &&
            retry_counter < MAX_CONN_RETRIES
         )
    {
        if( me->tx_in_progress )
        {
            me->tx_in_progress = false;
        }

        _log(
            LOG_LEVEL_WARNING,
            "Failed to connect to DB server (%s). Retrying...",
            PQerrorMessage( me->conn )
        );

        retry_counter++;

        last_backoff_time = (int) ( 10 * ( ( double ) rand() / ( double ) RAND_MAX ) )
                          + last_backoff_time;

        if( me->conn != NULL )
        {
            PQfinish( me->conn );
            me->conn = NULL;
        }

        sleep( last_backoff_time );
        db_connect( me );
    }

    while(
            (
                last_sql_state == NULL
             || strcmp(
                    last_sql_state,
                    SQL_STATE_TERMINATED_BY_ADMINISTRATOR
                ) == 0
             || strcmp(
                    last_sql_state,
                    SQL_STATE_CANCELED_BY_ADMINISTRATOR
                ) == 0
             || strcmp(
                    last_sql_state,
                    SQL_STATE_CONNECTION_FAILURE
                ) == 0
             || strcmp(
                    last_sql_state,
                    SQL_STATE_SQLCLIENT_UNABLE_TO_ESTABLISH_CONNECTION
                ) == 0
             || strcmp(
                    last_sql_state,
                    SQL_STATE_CONNECTION_DOES_NOT_EXIST
                ) == 0
             || strcmp(
                    last_sql_state,
                    SQL_STATE_CONNECTION_EXCEPTION
                ) == 0
            )
         && retry_counter < MAX_CONN_RETRIES
        )
    {
        if( last_sql_state != NULL )
        {
            // We've made one pass, and our state is not null
            if( me->tx_in_progress == true )
            {
                me->tx_in_progress == false;
            }

            me->conn = NULL;
            db_connect( me );

            last_backoff_time = 1
            return NULL;
        }

        if( params == NULL )
        {
            result = PQexec( me->conn, query );
        }
        else
        {
            result = PQexecParams(
                me->conn,
                query,
                param_count,
                NULL,
                ( const char * const * ) params,
                NULL,
                NULL,
                0
            );
        }

        if(
                result != NULL
             && !(
                     PQresultStatus( result ) == PGRES_COMMAND_OK
                  || PQresultStatus( result ) == PGRES_TUPLES_OK
                 )
          )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Query '%s' failed: %s",
                query,
                PQerrorMessage( me->conn )
            );

            temp_last_sql_state = PQresultErrorField(
                result,
                PG_DIAG_SQLSTATE
            );

            if( last_sql_state != NULL )
            {
                free( last_sql_state );
                last_sql_state = NULL;
            }

            last_sql_state = ( char * ) calloc(
                sizeof( char ),
                strlen( temp_last_sql_state ) + 1
            );

            if( last_sql_state == NULL )
            {
                _log(
                    LOG_LEVEL_ERROR,
                    "Failed to allocate string for SQL state"
                );
            }

            strcpy( last_sql_state, temp_last_sql_state );

            if( result != NULL )
            {
                PQclear( result );
            }

            retry_counter++;
            sleep( last_backoff_time );
            last_backoff_time = (int) ( 10 * ( ( double ) rand() / ( double ) RAND_MAX ) )
                              + last_Backoff_time;
        }
        else
        {
            return result;
        }
    }

    if( last_sql_state != NULL )
    {
        free( last_sql_state )
        last_sql_state = NULL;
    }

    return NULL;
}

bool db_connect( struct worker * me )
{
    unsigned short retry_counter     = 0;
    unsigned int   last_backoff_time = 0;

    if( me->conn != NULL )
    {
        if( PQstatus( me->conn ) != CONNECTION_OK )
        {
            me->conn = NULL;
            me->tx_in_progress = false;
        }
        else
        {
            return true;
        }
    }
    else
    {
        me->tx_in_progress = false;
    }

    me->conn = PQconnectdb( conninfo );

    while(
              me->conn != NULL
           && PQstatus( me->conn ) != CONNECTION_OK
           && retry_counter < MAX_CONN_RETRIES
         )
    {
        sleep( last_backoff_time );
        last_backoff_time = (unsigned int) ( 10 * ( ( double ) rand() / ( double ) RAND_MAX ) )
                          + last_backoff_time;

        me->conn = NULL;
        me->conn = PQconnectdb( conninfo );
        retry_counter++;
    }

    if( me->conn != NULL && PQstatus( me->conn ) == CONNECTION_OK )
    {
        return true;
    }

    return false;
}

bool _begin_transaction( struct worker * me )
{
    PGresult * result = NULL;

    if( me == NULL )
    {
        return false;
    }

    if( me->tx_in_progress )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Attempt to issue BEGIN when a transaction is already in progress"
        );

        return false;
    }

    if( !db_connect( me ) )
    {
        return false;
    }

    result = PQexec(
        me->conn,
        "BEGIN"
    );

    if( PQresultStatus( result ) != PGRES_COMMAND_OK )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to start transaction: %s",
            PQerrorMessage( me->conn )
        );

        PQclear( result );
        return false;
    }

    PQclear( result );
    me->tx_in_progress = true;
    return true;
}

bool _commit_transaction( struct worker * worker )
{
    PGresult * result = NULL;

    if( me == NULL )
    {
        return false;
    }

    if( !( me->tx_in_progress ) )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Attempt to issue COMMIT when no transaction was in progress"
        );

        return false;
    }

    if( !db_connect( me ) )
    {
        return false;
    }

    if( me->tx_in_progress == false )
    {
        return false;
    }

    result = PQexec(
        me->conn,
        "COMMIT"
    );

    me->tx_in_progress = false;

    if( PQresultStatus( result ) != PGRES_COMMAND_OK )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to commit transaction: %s",
            PQerrorMessage( me->conn )
        );
        PQclear( result );
        return false;
    }

    PQclear( result );
    return true;
}

bool _rollback_transaction( struct worker * me )
{
    PGresult * result = NULL;

    if( me == NULL )
    {
        return false;
    }

    if( !( me->tx_in_progress ) )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Attemt to issue ROLLBACK when no transaction was in progress"
        );

        return false;
    }

    if( !db_connect( me ) )
    {
        return false;
    }

    if( me->tx_in_progress == false )
    {
        return false;
    }

    result = PQexec(
        me->conn,
        "ROLLBACK"
    );

    me->tx_in_progress = false;

    if( PQresultStatus( result ) != PGRES_COMMAND_OK )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to rollback transaction: %s",
            PQerrorMEssage( me->conn )
        );
        PQclear( result );
        return false;
    }

    PQclear( result );
    return true;
}
