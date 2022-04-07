#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "../src/lib/query.h"

int main( int, char ** );

int main( int argc, char ** argv )
{
    PGresult *      result    = NULL;
    char *          params[5] = {NULL};
    char *          val       = NULL;
    char *          ptr       = NULL;
    int             i         = 0;
    struct worker * p         = NULL;
    _parse_args( argc, argv );

    if( conninfo == NULL )
    {
        printf( "FAILED: Failed to parse conninfoi\n" );
        return -1;
    }

    if( !parent_init( argc, argv ) )
    {
        printf( "FAILED: Failed to initialize parent pid slice\n" );
        return -1;
    }

    p = get_parent_ptr( true );

    if( p == NULL )
    {
        printf( "FAILED: Could not get parent worker slot" );
        return -1;
    }

    if( !db_connect( p ) )
    {
        printf( "FAILED: Failed to connect to database\n" );
        remove( get_pid_file() );
        return -1;
    }

    if( !begin_transaction( p ) )
    {
        printf( "FAILED: BEGIN failed\n" );
        remove( get_pid_file() );
        return -1;
    }

    if( !rollback_transaction( p ) )
    {
        printf( "FAILED: ROLLBACK failed\n" );
        remove( get_pid_file() );
        return -1;
    }

    if( commit_transaction( p ) )
    {
        printf( "FAILED: COMMIT happened on unopen transaction\n" );
        remove( get_pid_file() );
        return -1;
    }

    if( !begin_transaction( p ) )
    {
        printf( "FAILED: Second BEGIN failed\n" );
        remove( get_pid_file() );
        return -1;
    }

    result = execute_query( p, "SELECT 1 AS foo", NULL, 0 );

    if( result == NULL )
    {
        printf( "FAILED: SELECT failed\n" );
        remove( get_pid_file() );
        return -1;
    }

    if( is_column_null( 0, result, "foo" ) )
    {
        printf( "FAILED: Unexpected NULL\n" );
        remove( get_pid_file() );
        return -1;
    }

    val = get_column_value( 0, result, "foo" );

    if( val == NULL || strncmp( val, "1", 1 ) != 0 )
    {
        printf( "FAILED: Unexpected result '%s'\n", val );
        remove( get_pid_file() );
        return -1;
    }

    if( !commit_transaction( p ) )
    {
        printf( "FAILED: COMMIT failed\n" );
        remove( get_pid_file() );
        return -1;
    }

    PQclear( result );
    if( !begin_transaction( p ) )
    {
        printf( "FAILED: Third BEGIN failed\n" );
        remove( get_pid_file() );
        return -1;
    }

    params[0] = "1";
    params[1] = "10";
    result    = execute_query(
        p,
        "SELECT generate_series( $1::INTEGER, $2::INTEGER ) AS foo",
        params,
        2
    );

    if( result == NULL )
    {
        printf( "FAILED: Second SELECT failed\n" );
        remove( get_pid_file() );
        return -1;
    }

    if( PQntuples( result ) <= 0 )
    {
        printf( "FAILED: insufficient result tuples\n" );
        remove( get_pid_file() );
        return -1;
    }

    for( i = 0; i < PQntuples( result ); i++ )
    {
        val = get_column_value( i, result, "foo" );

        if( val == NULL )
        {
            printf( "FAILED: Unexpected null value\n" );
            remove( get_pid_file() );
            return -1;
        }

        if( strtol( val, &ptr, 10 ) != i + 1 )
        {
            printf( "FAILED: Unexpected output for second query\n" );
            remove( get_pid_file() );
            return -1;
        }
    }

    if( !rollback_transaction( p ) )
    {
        printf( "FAILED: ROLLBACK failed\n" );
        remove( get_pid_file() );
        return -1;
    }

    if( remove( get_pid_file() ) != 0 )
    {
        printf( "FAILED: Failed to cleanup pidfile\n" );
        return -1;
    }

    printf( "All tests passed\n" );
    return 0;
}
