#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <sys/wait.h>

#include "../src/lib/slab.h"
#define TEST_SIZE 256
int main( void );
static void child_routine( __ref );

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
    pid_t      child = 0;
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

    dump_control();
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
        ptr[i] = TEST_SIZE - i;

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


    dump_control();
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
        ptr[i] = TEST_SIZE + i + 1;

    fprintf( stdout, "Performing readback test on both allocations...\n" );

    ptr = get_ptr( ref );
    for( i = 0; i < TEST_SIZE; i++ )
    {
        if( ptr[i] != TEST_SIZE - i )
        {
            fprintf(
                stderr,
                "Failed at index %lu of first allocation\n"
                "  got %lu (%x%x), expected %lu\n",
                ( uint64_t ) i,
                ptr[i],
                ( uint32_t ) ( ptr[i] >> 32 ),
                ( uint32_t ) ptr[i],
                TEST_SIZE - i
            );
            return 1;
        }
    }

    ptr = get_ptr( ref2 );
    for( i = 0; i < ( TEST_SIZE * 4 ) + 2; i++ )
    {
        if( ptr[i] != TEST_SIZE + i + 1 )
        {
            fprintf(
                stderr,
                "Failed at index %lu of second allocation. Expected %lu, got %lu (%x%x)\n",
                ( uint64_t ) i,
                TEST_SIZE + i + 1,
                ptr[i],
                ( uint32_t ) ( ptr[i] >> 32 ),
                ( uint32_t ) ptr[i]

            );
            return 1;
        }
    }

    fprintf( stdout, "Performing single allocation tests...\n" );
    ref3 = rsmalloc( slab, sizeof( uint64_t ) );
    ref4 = rsmalloc( slab, sizeof( uint64_t ) );
    ref5 = rsmalloc( slab, sizeof( uint64_t ) );

    // Write the three prior allocations
    *( ( uint64_t * ) get_ptr( ref3 ) ) = 0xAAAAAAAAAAAAAAAA;
    *( ( uint64_t * ) get_ptr( ref4 ) ) = 0xBBBBBBBBBBBBBBBB;
    *( ( uint64_t * ) get_ptr( ref5 ) ) = 0xCCCCCCCCCCCCCCCC;

    fprintf( stdout, "Performing allocations in low space conditions...\n" );
    ref6 = rsmalloc( slab, sizeof( uint64_t ) * 64 );
    ptr = get_ptr( ref6 );
    if( ptr == NULL )
    {
        fprintf( stderr, "Failed, ref6 pointer is NULL\n" );
        return 1;
    }

    for( i = 0; i < 64; i++ )
        ptr[i] = 0xDDDDDDDDDDDDDDDD;

    ref7 = rsmalloc( slab, sizeof( uint64_t ) * 7 );
    ptr = get_ptr( ref7 );

    if( ptr == NULL )
    {
        fprintf( stderr, "Failed, ref7 pointer is NULL\n" );
        return 1;
    }

    for( i = 0; i < 7; i++ )
        ptr[i] = 0xEEEEEEEEEEEEEEEE;

    // New test case- making rsmalloc for low space applications
    ref8 = rsmalloc( slab, sizeof( uint64_t ) * 67 );

    ptr = get_ptr( ref8 );
    if( ptr == NULL )
    {
        fprintf( stderr, "Failed, ref8 pointer is NULL\n" );
        return 1;
    }

    for( i = 0; i < 67; i++ )
        ptr[i] = 0xFFFFFFFFFFFFFFFF;

    fprintf( stdout, "Performing allocations in extension mode...\n" );
    // final fsm word should be 1111111111111111 1110000000000000 0000000000000000 0000000000000111
    // We're going to ask for the remainder, but this /should/ cause a segment extension
    ref9 = rsmalloc( slab, sizeof( uint64_t ) * 26 );

    ptr = get_ptr( ref9 );

    if( ptr == NULL )
    {
        fprintf( stderr, "Failed, ref9 pointer is NULL\n" );
        return 1;
    }

    for( i = 0; i < 26; i++ )
        ptr[i] = 0x9999999999999999;

    fprintf( stdout, "Testing initial writes after allocation tests...\n" );
    // Repeat of earlier tests, but certifies that the FSM didnt get muddied up
    ptr = get_ptr( ref );
    for( i = 0; i < TEST_SIZE; i++ )
    {
        if( ptr[i] != TEST_SIZE - i )
        {
            fprintf(
                stderr,
                "Failed at index %lu of first allocation\n"
                "  got %lu (%x%x), expected %lu\n",
                ( uint64_t ) i,
                ptr[i],
                ( uint32_t ) ( ptr[i] >> 32 ),
                ( uint32_t ) ( ptr[i] ),
                TEST_SIZE - i
            );
            return 1;
        }
    }

    ptr = get_ptr( ref2 );
    for( i = 0; i < ( TEST_SIZE * 4 ) + 2; i++ )
    {
        if( ptr[i] != TEST_SIZE + i + 1 )
        {
            fprintf(
                stderr,
                "Failed at index %lu of second allocation. Expected %lu, got %lu (%x%x)\n",
                ( uint64_t ) i,
                TEST_SIZE + i + 1,
                ptr[i],
                ( uint32_t ) ( ptr[i] >> 32 ),
                ( uint32_t ) ptr[i]
            );
            return 1;
        }
    }
    //dump_context( slab );
    fprintf( stdout, "Freeing allocations...\n" );

    rsfree( slab, ref );
    rsfree( slab, ref2 );
    rsfree( slab, ref3 );
    rsfree( slab, ref4 );
    rsfree( slab, ref5 );
    rsfree( slab, ref6 );
    rsfree( slab, ref7 );
    rsfree( slab, ref8 );
    dump_context( slab );

    fprintf( stdout, "Performing first reallocation test (no segment extension)...\n" );
    //dump_context( slab );
    ref9 = rsrealloc( slab, ref9, sizeof( uint64_t ) * 1024 );
    if( ref_is_null( ref9 ) )
    {
         fprintf( stderr, "Failed to reallocate.\n" );
         return 1;
    }

    dump_context( slab );
    ptr = get_ptr( ref9 );
    if( ptr == NULL )
    {
        fprintf( stderr, "Failed - reallocated pointer is NULL\n" );
        return 1;
    }

    for( i = 0; i < 1024; i++ )
        ptr[i] = i * i;

    fprintf( stdout, "Performing second reallocation test (realloc with segment extension)...\n" );
    ref9 = rsrealloc( slab, ref9, sizeof( uint64_t ) * 2048 );

    if( ref_is_null( ref9 ) )
    {
        fprintf( stderr, "Failed to reallocate and extend segment\n" );
        return 1;
    }

    ptr = get_ptr( ref9 );

    if( ptr == NULL )
    {
        fprintf( stderr, "Failed - reallocated pointer with extension is NULL\n" );
        return 1;
    }

    for( i = 0; i < 1024; i++ )
    {
        if( i * i != ptr[i] )
        {
            fprintf(
                stderr,
                "Failed: Readback of reallocated data returned mismatch at %lu. ptr[%lu] != %lu (got %lu, %x%x)\n",
                ( uint64_t ) i,
                ( uint64_t ) i,
                ( uint64_t ) ( i * i ),
                ptr[i],
                ( uint32_t ) ( ptr[i] >> 32 ),
                ( uint32_t ) ptr[i]
            );
            return 1;
        }
    }

    for( i = 0; i < 2048; i++ )
        ptr[i] = 42;


    fprintf( stdout, "Beginning SMP test...\n" );
    child = fork();

    if( child == 0 )
    {
        child_routine( ref9 );
        exit(0);
    }

    wait( NULL );

    ptr = get_ptr( ref9 );
    if( ptr == NULL )
    {
        fprintf( stderr, "Failed - parent returned NULL pointer after child exit\n" );
        return 1;
    }

    fprintf( stdout, "Parent confirming child baseline writes...\n" );
    for( i = 0; i < 2048; i++ )
    {
        if( ptr[i] != 42 + i )
        {
            fprintf(
                stderr,
                "Failed - child writes not visible to parent at index %lu, got %lu, expected %lu\n",
                ( uint64_t ) i,
                ( uint64_t ) ptr[i],
                ( uint64_t ) 42 + i
            );
            return 1;
        }
    }

    fprintf( stdout, "Parent confirming child extended writes...\n" );
    dump_context( slab );
    for( i = 2048; i < 4096; i++ )
    {
        if( ptr[i] != 42 * i )
        {
            fprintf(
                stderr,
                "Failed - extended write not visible to parent at index %lu, got %lu, expected %lu\n",
                ( uint64_t ) i,
                ( uint64_t ) ptr[i],
                ( uint64_t ) 42 * i
            );
            return 1;
        }
    }

    fprintf( stdout, "Freed all references, destroying slab...\n" );
    rsfree( slab, ref9 );
    destroy_slab( slab );
    fprintf( stdout, "Done.\n" );
    return 0;
}

static void child_routine( __ref ref )
{
    context_t  slab = INVALID_CONTEXT;
    uint64_t * ptr  = NULL;
    uint64_t   i    = 0;

    if( !slab_init() )
    {
        fprintf( stderr, "Failed - Child could not initialize slab\n" );
        return;
    }

    slab = new_slab( "TEST", 8 );

    if( slab == INVALID_CONTEXT )
    {
        fprintf( stderr, "Failed - child could not get context\n" );
        return;
    }

    ptr = get_ptr( ref );

    if( ptr == NULL )
    {
        fprintf( stderr, "Failed - child dereferenced NULL ref\n" );
        return;
    }

    fprintf( stdout, "Performing SMD read/write test...\n" );
    for( i = 0; i < 2048; i++ )
    {
        if( ptr[i] != 42 )
        {
            fprintf(
                stderr,
                "Failed - child read of index %lu did returned %lu, expected %lu\n",
                ( uint64_t ) i,
                ( uint64_t ) ptr[i],
                ( uint64_t ) 42
            );
            return;
        }

        ptr[i] = 42 + i;
    }

    fprintf( stdout, "Child extending slab...\n" );
    ref = rsrealloc( slab, ref, sizeof( uint64_t ) * 4096 );
    ptr = get_ptr( ref );

    if( ptr == NULL )
    {
        fprintf( stderr, "Failed - child could not extend segment.\n" );
        return;
    }

    fprintf( stdout, "Child performing extended read/write test...\n" );
    for( i = 0; i < 4096; i++ )
    {
        if( i < 2048 )
        {
            if( ptr[i] != 42 + i )
            {
                fprintf(
                    stderr,
                    "Failed - old data corrupted at index %lu, got %lu, expected %lu\n",
                    ( uint64_t ) i,
                    ( uint64_t ) ptr[i],
                    ( uint64_t ) i + 42
                );
                return;
            }
        }
        else
        {
            ptr[i] = 42 * i;
        }
    }
    //dump_context( slab );

    return;
}
