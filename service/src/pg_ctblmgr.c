#include "pg_ctblmgr.h"

int main( int argc, char ** argv )
{
    _parse_args( argc, argv );

    if( !_parent_init( argc, argv ) )
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

    return 0;
}
