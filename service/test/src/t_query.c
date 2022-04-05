#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "../src/lib/query.h"

int main( int, char ** );

int main( int argc, char ** argv )
{
    PGresult * result    = NULL;
    char *     params[5] = {NULL};
    char *     val       = NULL;
    char *     ptr       = NULL;
    int        i         = 0;

    _parse_args( argc, argv );

    if( conninfo == NULL )
    {
        printf( "FAILED: Failed to parse conninfoi\n" );
        return -1;
    }

    if( !parent_init( argc, argv ) )
    {
        printf( "FAILED: Failed to initialize parent pid slice\n" );
        if( parent )
            remove( parent->pidfile );
        return -1;
    }

    if( !db_connect( parent ) )
    {
        printf( "FAILED: Failed to connect to database\n" );
        remove( parent->pidfile );
        return -1;
    }

    if( !begin_transaction( parent ) )
    {
        printf( "FAILED: BEGIN failed\n" );
        remove( parent->pidfile );
        return -1;
    }

    if( !rollback_transaction( parent ) )
    {
        printf( "FAILED: ROLLBACK failed\n" );
        remove( parent->pidfile );
        return -1;
    }

    if( commit_transaction( parent ) )
    {
        printf( "FAILED: COMMIT happened on unopen transaction\n" );
        remove( parent->pidfile );
        return -1;
    }

    if( !begin_transaction( parent ) )
    {
        printf( "FAILED: Second BEGIN failed\n" );
        remove( parent->pidfile );
        return -1;
    }

    result = execute_query( parent, "SELECT 1 AS foo", NULL, 0 );

    if( result == NULL )
    {
        printf( "FAILED: SELECT failed\n" );
        remove( parent->pidfile );
        return -1;
    }

    if( is_column_null( 0, result, "foo" ) )
    {
        printf( "FAILED: Unexpected NULL\n" );
        remove( parent->pidfile );
        return -1;
    }

    val = get_column_value( 0, result, "foo" );

    if( val == NULL || strncmp( val, "1", 1 ) != 0 )
    {
        printf( "FAILED: Unexpected result '%s'\n", val );
        remove( parent->pidfile );
        return -1;
    }

    if( !commit_transaction( parent ) )
    {
        printf( "FAILED: COMMIT failed\n" );
        remove( parent->pidfile );
        return -1;
    }

    PQclear( result );
    if( !begin_transaction( parent ) )
    {
        printf( "FAILED: Third BEGIN failed\n" );
        remove( parent->pidfile );
        return -1;
    }

    params[0] = "1";
    params[1] = "10";
    result    = execute_query(
        parent,
        "SELECT generate_series( $1::INTEGER, $2::INTEGER ) AS foo",
        params,
        2
    );

    if( result == NULL )
    {
        printf( "FAILED: Second SELECT failed\n" );
        remove( parent->pidfile );
        return -1;
    }

    if( PQntuples( result ) <= 0 )
    {
        printf( "FAILED: insufficient result tuples\n" );
        remove( parent->pidfile );
        return -1;
    }

    for( i = 0; i < PQntuples( result ); i++ )
    {
        val = get_column_value( i, result, "foo" );

        if( val == NULL )
        {
            printf( "FAILED: Unexpected null value\n" );
            remove( parent->pidfile );
            return -1;
        }

        if( strtol( val, &ptr, 10 ) != i + 1 )
        {
            printf( "FAILED: Unexpected output for second query\n" );
            remove( parent->pidfile );
            return -1;
        }
    }

    if( !rollback_transaction( parent ) )
    {
        printf( "FAILED: ROLLBACK failed\n" );
        remove( parent->pidfile );
        return -1;
    }

    if( remove( parent->pidfile ) != 0 )
    {
        printf( "FAILED: Failed to cleanup pidfile\n" );
        return -1;
    }

    printf( "All tests passed\n" );
    return 0;
}
