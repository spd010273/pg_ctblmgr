#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <sys/wait.h>

#include "../src/lib/slab.h"
#define TEST_SIZE 256
int main( void );

int main( void )
{
    context_t  slab = 0;
    __ref      ref  = get_null_ref();
    __ref      ref2 = get_null_ref();
    __ref      ref3 = get_null_ref();
    __ref      ref4 = get_null_ref();
    __ref      ref5 = get_null_ref();
    __ref      ref6 = get_null_ref();
    __ref      ref7 = get_null_ref();
    uint64_t * ptr  = NULL;
    uint64_t   i    = 0;

    if( !slab_init() )
    {
        fprintf( stderr, "FAILED: Could not initialize slab\n" );
        return 1;
    }

    slab = new_slab( "TEST", sizeof( uint64_t ) );
    slab_set_count_hint( slab, TEST_SIZE );

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

    fprintf( stdout, "Got ptr %p\n", ptr );
    for( i = 0; i < TEST_SIZE; i++ )
    {
/*
        fprintf(
            stdout,
            "%p [%lu] (%p): %lu (%x)\n",
            ptr,
            i,
            &(ptr[i]),
            TEST_SIZE - i,
            ( uint32_t ) ( TEST_SIZE - i )
        );
*/
        ptr[i] = TEST_SIZE - i;
    }

    fprintf( stdout, "Write check complete - running canary test\n" );
    // Canary check should pass as we've stayed within allocated bounds
    if( !force_canary_check( slab ) )
    {
        fprintf( stderr, "Canary check failed after bounded write\n" );
        return 1;
    }

    // Check setting flag - don't actually want to crash the test ;)
#ifdef _FORCE_SIGSEGV_ON_CANARY_FAILURE
    fprintf( stdout, "Making out-of-bounds write to %p (%lu)\n", &(ptr[i]), i );
    ptr[i]=42;

    if( force_canary_check( slab ) )
    {
        fprintf( stderr, "Canary check passed after unbounded write\n" );
        return 1;
    }
#else
    fprintf(
        stdout,
        "WARNING: Compiled with _FORCE_SIGSEGV_ON_CANARY_FAILURE."
        " Cannot test out-of-bounds write\n"
    );
#endif // _FORCE_SIGSEGV_ON_CANARY_FAILURE

    
    fprintf( stdout, "Extending allocation...\n" );
    ref2 = smalloc( slab, sizeof( uint64_t ) * TEST_SIZE * 4 );

    if( ref_is_null( ref2 ) )
    {
        fprintf( stderr, "Failed to extend allocation\n" );
        exit( 1 );
    }
   
    ref3 = smalloc( slab, sizeof( uint64_t ) );    
    ref4 = smalloc( slab, sizeof( uint64_t ) );
    ref5 = smalloc( slab, sizeof( uint64_t ) );

    ref6 = smalloc( slab, sizeof( uint64_t ) * 64 );
    ref7 = smalloc( slab, sizeof( uint64_t ) * 7 );
    // New test case- making smalloc for low space applications
    //ref7 = smalloc( slab, sizeof( uint64_t ) * 67 );
//    dump_context( slab );
    fprintf( stdout, "Freeing allocation\n" );
     
    sfree( slab, ref );
    sfree( slab, ref2 );
    sfree( slab, ref3 );
    sfree( slab, ref4 );
    sfree( slab, ref5 );
    sfree( slab, ref6 );
    sfree( slab, ref7 );
    return 0;
}
