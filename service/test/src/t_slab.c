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
    __ref      ref8 = get_null_ref();
    __ref      ref9 = get_null_ref();

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

    ref = rsmalloc( slab, sizeof( uint64_t ) * TEST_SIZE );

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
    ref2 = rsmalloc( slab, sizeof( uint64_t ) * ( ( TEST_SIZE * 4 ) + 2 ));

    if( ref_is_null( ref2 ) )
    {
        fprintf( stderr, "Failed to extend allocation\n" );
        exit( 1 );
    }

    fprintf( stdout, "Making second allocation...\n" );
    ptr = ( uint64_t * ) get_ptr( ref2 );

    if( ptr == NULL )
    {
        fprintf(
            stderr,
            "Failed to dereference pointer to local for second allocation\n"
        );
        return 1;
    }

    for( i = 0; i < ( TEST_SIZE * 4 ) + 2; i++ )
    {
        ptr[i] = ( TEST_SIZE - i ) + 1;
    }

    fprintf( stdout, "Performing readback test on both allocations...\n" );
   
    ptr = get_ptr( ref );
    
    for( i = 0; i < TEST_SIZE; i++ )
    {
        if( ptr[i] != TEST_SIZE - i )
        {
            fprintf(
                stderr,
                "Failed at index %lu of first allocation\n"
                "  got %lu, expected %lu\n",
                ( uint64_t ) i,
                ptr[i],
                TEST_SIZE - i
            );
            return 1;
        }
    }

    ptr = get_ptr( ref2 );
    for( i = 0; i < ( TEST_SIZE * 4 ) + 2; i++ )
    {
        if( ptr[i] != TEST_SIZE - i + 1 )
        {
            fprintf( stderr, "Failed at index %lu of second allocation\n", ( uint64_t ) i );
            return 1;
        }
    }
    
    ref3 = rsmalloc( slab, sizeof( uint64_t ) );    
    ref4 = rsmalloc( slab, sizeof( uint64_t ) );
    ref5 = rsmalloc( slab, sizeof( uint64_t ) );

    ref6 = rsmalloc( slab, sizeof( uint64_t ) * 64 );
    ref7 = rsmalloc( slab, sizeof( uint64_t ) * 7 );
    // New test case- making rsmalloc for low space applications
    ref8 = rsmalloc( slab, sizeof( uint64_t ) * 67 );
    // final fsm word should be 1111111111111111 1110000000000000 0000000000000000 0000000000000111
    // We're going to ask for the remainder, but this /should/ cause a segment extension
    ref9 = rsmalloc( slab, sizeof( uint64_t ) * 26 ); 
    //dump_context( slab );
    fprintf( stdout, "Freeing allocation\n" );
     
    rsfree( slab, ref );
    fprintf( stdout, "Freed first ref\n" );
    rsfree( slab, ref2 );
    fprintf( stdout, "Freed second ref\n" );
    rsfree( slab, ref3 );
    fprintf( stdout, "Freed third ref\n" );
    rsfree( slab, ref4 );
    fprintf( stdout, "Freed fourth ref\n" );
    rsfree( slab, ref5 );
    fprintf( stdout, "Freed fifth ref\n" );
    rsfree( slab, ref6 );
    fprintf( stdout, "Freed sixth ref\n" );
    rsfree( slab, ref7 );
    fprintf( stdout, "Freed seventh ref\n" );
    rsfree( slab, ref8 );
    fprintf( stdout, "Freed eighth ref\n" );
    
    fprintf( stdout, "Testing realloc of ninth ref\n" );
    //dump_context( slab ); 
    ref9 = rsrealloc( slab, ref9, sizeof( uint64_t ) * 1024 );
    if( ref_is_null( ref9 ) )
    {
         fprintf( stderr, "Failed to reallocat.\n" );
         return 1;
    }

    ref9 = rsrealloc( slab, ref9, sizeof( uint64_t ) * 2048 );

    if( ref_is_null( ref9 ) )
    {
        fprintf( stderr, "Failed to reallocate and extend segment\n" );
        return 1;
    }

    rsfree( slab, ref9 );
    fprintf( stdout, "Freed ninth ref\n" );
    destroy_slab( slab );
    //dump_context( slab );
    return 0;
}
