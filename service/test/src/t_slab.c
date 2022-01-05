#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <sys/wait.h>

#include "../src/lib/slab.h"
#define TEST_SIZE 32
int main( void );

int main( void )
{
    context_t  slab = 0;
    __ref      ref  = get_null_ref();
    uint64_t * ptr  = NULL;
    uint64_t   i    = 0;

    if( !slab_init() )
    {
        fprintf( stderr, "FAILED: Could not initialize slab\n" );
        return 1;
    }

    slab = new_slab( "TEST", sizeof( uint64_t ) );

    if( slab == INVALID_CONTEXT )
    {
        fprintf( stderr, "FAILED: Could not initialize slab context\n" );
        return 1;
    }

    ref = smalloc( slab, sizeof( uint64_t ) * TEST_SIZE );

    if( ref == get_null_ref() )
    {
        fprintf( stderr, "FAILED: Could not allocate shared memory\n" );
        return 1;
    }

    ptr = ( uint64_t * ) get_ptr( ref );

    if( ptr == NULL )
    {
        fprintf( stderr, "Failed to dereference pointer to local\n" );
        return 1;
    }

    for( i = 0; i < TEST_SIZE; i++ )
    {
        ptr[i] = TEST_SIZE - i;
    }

    return 0;
}
