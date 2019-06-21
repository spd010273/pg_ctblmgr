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
    return 0;
}
