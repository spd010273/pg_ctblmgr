#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <sys/wait.h>

#include "../src/lib/slab.h"
#define TEST_SIZE 256
int main( void );
static void child_routine( ref_t );

int main( void )
{
    context_t  slab = 0;
    canary_t   save_canary = {0};
    ref_t      ref  = NULLREF;
    ref_t      ref2 = NULLREF;
    ref_t      ref3 = NULLREF;
    ref_t      ref4 = NULLREF;
    ref_t      ref5 = NULLREF;
    ref_t      ref6 = NULLREF;
    ref_t      ref7 = NULLREF;
    ref_t      ref8 = NULLREF;
    ref_t      ref9 = NULLREF;

    pid_t      child = 0;
    uint64_t * ptr  = NULL;
    uint64_t   i    = 0;

    if( !slab_init() )
    {
        fprintf( stderr, "FAILED: Could not initialize slab\n" );
        return -1;
    }

    __test_harness(); // quiet canary failure we're going to hit later
    slab = new_slab( "TEST", sizeof( uint64_t ) );
    slab_set_count_hint( slab, TEST_SIZE );

    if( slab == INVALID_CONTEXT )
    {
        fprintf( stderr, "FAILED: Could not initialize slab context\n" );
        return -1;
    }

    ref = rsmalloc( slab, sizeof( uint64_t ) * TEST_SIZE );

    if( ref == NULLREF )
    {
        fprintf( stderr, "FAILED: Could not allocate shared memory\n" );
        return -1;
    }

    ptr = ( uint64_t * ) to_ptr( slab, ref );

    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED: Failed to dereference pointer to local\n" );
        return -1;
    }

    for( i = 0; i < TEST_SIZE; i++ )
        ptr[i] = TEST_SIZE - i;

    // Canary check should pass as we've stayed within allocated bounds
    if( !force_canary_check( slab ) )
    {
        fprintf( stderr, "FAILED: Canary check failed after bounded write\n" );
        return -1;
    }
    // Check setting flag - don't actually want to crash the test ;)
#ifndef _FORCE_SIGSEGV_ON_CANARY_FAILURE
    save_canary = ptr[i];
    ptr[i]=42;

    if( force_canary_check( slab ) )
    {
        fprintf( stderr, "FAILED: Canary check passed after unbounded write\n" );
        return -1;
    }

    ptr[i] = save_canary;
#endif // _FORCE_SIGSEGV_ON_CANARY_FAILURE

    ref2 = rsmalloc( slab, sizeof( uint64_t ) * ( ( TEST_SIZE * 4 ) + 2 ));

    if( ref_is_null( ref2 ) )
    {
        fprintf( stderr, "FAILED: Failed to extend allocation\n" );
        return -1;
    }

    ptr = ( uint64_t * ) to_ptr( slab, ref2 );

    if( ptr == NULL )
    {
        fprintf(
            stderr,
            "FAILED: Failed to dereference pointer to local for second allocation\n"
        );
        return -1;
    }

    for( i = 0; i < ( TEST_SIZE * 4 ) + 2; i++ )
        ptr[i] = TEST_SIZE + i + 1;

    ptr = to_ptr( slab, ref );
    for( i = 0; i < TEST_SIZE; i++ )
    {
        if( ptr[i] != TEST_SIZE - i )
        {
            fprintf(
                stderr,
                "FAILED: Failed at index %lu of first allocation\n"
                "  got %lu (%x%x), expected %lu\n",
                ( uint64_t ) i,
                ptr[i],
                ( uint32_t ) ( ptr[i] >> 32 ),
                ( uint32_t ) ptr[i],
                TEST_SIZE - i
            );
            return -1;
        }
    }

    ptr = to_ptr( slab, ref2 );
    for( i = 0; i < ( TEST_SIZE * 4 ) + 2; i++ )
    {
        if( ptr[i] != TEST_SIZE + i + 1 )
        {
            fprintf(
                stderr,
                "FAILED: Failed at index %lu of second allocation. Expected %lu, got %lu (%x%x)\n",
                ( uint64_t ) i,
                TEST_SIZE + i + 1,
                ptr[i],
                ( uint32_t ) ( ptr[i] >> 32 ),
                ( uint32_t ) ptr[i]

            );
            return -1;
        }
    }

    ref3 = rsmalloc( slab, sizeof( uint64_t ) );
    ref4 = rsmalloc( slab, sizeof( uint64_t ) );
    ref5 = rsmalloc( slab, sizeof( uint64_t ) );

    // Write the three prior allocations
    *( ( uint64_t * ) to_ptr( slab, ref3 ) ) = 0xAAAAAAAAAAAAAAAA;
    *( ( uint64_t * ) to_ptr( slab, ref4 ) ) = 0xBBBBBBBBBBBBBBBB;
    *( ( uint64_t * ) to_ptr( slab, ref5 ) ) = 0xCCCCCCCCCCCCCCCC;

    ref6 = rsmalloc( slab, sizeof( uint64_t ) * 64 );
    ptr = to_ptr( slab, ref6 );
    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED: ref6 pointer is NULL\n" );
        return -1;
    }

    for( i = 0; i < 64; i++ )
        ptr[i] = 0xDDDDDDDDDDDDDDDD;

    ref7 = rsmalloc( slab, sizeof( uint64_t ) * 7 );
    ptr = to_ptr( slab, ref7 );

    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED: ref7 pointer is NULL\n" );
        return -1;
    }

    for( i = 0; i < 7; i++ )
        ptr[i] = 0xEEEEEEEEEEEEEEEE;

    // New test case- making rsmalloc for low space applications
    ref8 = rsmalloc( slab, sizeof( uint64_t ) * 67 );

    ptr = to_ptr( slab, ref8 );
    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED ref8 pointer is NULL\n" );
        return -1;
    }

    // Check ref3->ref5
    if( *( ( uint64_t * ) to_ptr( slab, ref3 ) ) != 0xAAAAAAAAAAAAAAAA )
    {
        fprintf( stderr, "FAILED: ref3 corrupted\n" );
        return -1;
    }

    if( *( ( uint64_t * ) to_ptr( slab, ref4 ) ) != 0xBBBBBBBBBBBBBBBB )
    {
        fprintf( stderr, "FAILED: ref4 corrupted\n" );
        return -1;
    }

    if( *( ( uint64_t * ) to_ptr( slab, ref5 ) ) != 0xCCCCCCCCCCCCCCCC )
    {
        fprintf( stderr, "FAILED: ref5 corrupted\n" );
        return -1;
    }

    for( i = 0; i < 67; i++ )
        ptr[i] = 0xFFFFFFFFFFFFFFFF;

    // final fsm word should be 1111111111111111 1110000000000000 0000000000000000 0000000000000111
    // We're going to ask for the remainder, but this /should/ cause a segment extension
    ref9 = rsmalloc( slab, sizeof( uint64_t ) * 26 );

    ptr = to_ptr( slab, ref9 );

    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED ref9 pointer is NULL\n" );
        return -1;
    }

    for( i = 0; i < 26; i++ )
        ptr[i] = 0x9999999999999999;

    // Repeat of earlier tests, but certifies that the FSM didnt get muddied up
    ptr = to_ptr( slab, ref );
    for( i = 0; i < TEST_SIZE; i++ )
    {
        if( ptr[i] != TEST_SIZE - i )
        {
            fprintf(
                stderr,
                "FAILED: Failed at index %lu of first allocation\n"
                "  got %lu (%x%x), expected %lu\n",
                ( uint64_t ) i,
                ptr[i],
                ( uint32_t ) ( ptr[i] >> 32 ),
                ( uint32_t ) ( ptr[i] ),
                TEST_SIZE - i
            );
            return -1;
        }
    }

    ptr = to_ptr( slab, ref2 );
    for( i = 0; i < ( TEST_SIZE * 4 ) + 2; i++ )
    {
        if( ptr[i] != TEST_SIZE + i + 1 )
        {
            fprintf(
                stderr,
                "FAILED: Failed at index %lu of second allocation. Expected %lu, got %lu (%x%x)\n",
                ( uint64_t ) i,
                TEST_SIZE + i + 1,
                ptr[i],
                ( uint32_t ) ( ptr[i] >> 32 ),
                ( uint32_t ) ptr[i]
            );
            return -1;
        }
    }

    rsfree( slab, ref );
    rsfree( slab, ref2 );
    rsfree( slab, ref3 );
    rsfree( slab, ref4 );
    rsfree( slab, ref5 );
    rsfree( slab, ref6 );
    // Save ref7 so the linked list has a node in it
    //rsfree( slab, ref7 );
    rsfree( slab, ref8 );

    ref9 = rsrealloc( slab, ref9, sizeof( uint64_t ) * 1024 );
    if( ref_is_null( ref9 ) )
    {
         fprintf( stderr, "FAILED: Failed to reallocate.\n" );
         return -1;
    }

    //dump_context( slab );
    ptr = to_ptr( slab, ref9 );
    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED: reallocated pointer is NULL\n" );
        return -1;
    }

    for( i = 0; i < 1024; i++ )
        ptr[i] = i * i;

    ref9 = rsrealloc( slab, ref9, sizeof( uint64_t ) * 2048 );

    if( ref_is_null( ref9 ) )
    {
        fprintf( stderr, "FAILED: Failed to reallocate and extend segment\n" );
        return -1;
    }

    ptr = to_ptr( slab, ref9 );

    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED: reallocated pointer with extension is NULL\n" );
        return -1;
    }

    for( i = 0; i < 1024; i++ )
    {
        if( i * i != ptr[i] )
        {
            fprintf(
                stderr,
                "FAILED: Readback of reallocated data returned mismatch at %lu. ptr[%lu] != %lu (got %lu, %x%x)\n",
                ( uint64_t ) i,
                ( uint64_t ) i,
                ( uint64_t ) ( i * i ),
                ptr[i],
                ( uint32_t ) ( ptr[i] >> 32 ),
                ( uint32_t ) ptr[i]
            );
            return -1;
        }
    }

    for( i = 0; i < 2048; i++ )
        ptr[i] = 42;


    child = fork();

    if( child == 0 )
    {
        child_routine( ref9 );
        exit(0);
    }

    wait( NULL );
    ptr = to_ptr( slab, ref9 ); // incase of realloc

    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED: parent returned NULL pointer after child exit\n" );
        return -1;
    }

    /*
     * Here's the issue - the allocation gets moved by the child because it's large - but the ref remains absolute as an offset into the page.
     * We'll need to offset off of that when allocs get relocated, possibly by wrapping to_ptr in slab.c, possibly by allocset index and offset?
     */
    for( i = 0; i < 2048; i++ )
    {
        if( ptr[i] != 42 + i )
        {
            fprintf(
                stderr,
                "FAILED: child writes not visible to parent at index %lu, got %lu, expected %lu\n",
                ( uint64_t ) i,
                ( uint64_t ) ptr[i],
                ( uint64_t ) 42 + i
            );
            return -1;
        }
    }

    for( i = 2048; i < 4096; i++ )
    {
        if( ptr[i] != 42 * i )
        {
            fprintf(
                stderr,
                "FAILED: extended write not visible to parent at index %lu (%p), got %lu, expected %lu\n",
                ( uint64_t ) i,
                &( ptr[i] ),
                ( uint64_t ) ptr[i],
                ( uint64_t ) 42 * i
            );
            return -1;
        }
    }

    if( !force_canary_check( slab ) )
    {
        fprintf( stderr, "FAILED: TEST slab failed canary check\n" );
        return -1;
    }

    rsfree( slab, ref9 );
    rsfree( slab, ref7 );
    destroy_slab( slab );
    fprintf( stdout, "All tests passed\n" );
    return 0;
}

static void child_routine( ref_t ref )
{
    context_t  slab   = INVALID_CONTEXT;
    uint64_t * ptr    = NULL;
    uint64_t   i      = 0;

    if( !slab_init() )
    {
        fprintf( stderr, "FAILED: Child could not initialize slab\n" );
        return;
    }

    slab = new_slab( "TEST", 8 );

    if( slab == INVALID_CONTEXT )
    {
        fprintf( stderr, "FAILED: child could not get TEST context\n" );
        return;
    }

    ptr = to_ptr( slab, ref );

    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED: child dereferenced NULL ref\n" );
        return;
    }

    for( i = 0; i < 2048; i++ )
    {
        if( ptr[i] != 42 )
        {
            fprintf(
                stderr,
                "FAILED: child read of index %lu did returned %lu, expected %lu\n",
                ( uint64_t ) i,
                ( uint64_t ) ptr[i],
                ( uint64_t ) 42
            );
            return;
        }

        ptr[i] = 42 + i;
    }

    ref = rsrealloc( slab, ref, sizeof( uint64_t ) * 4096 );
    ptr = to_ptr( slab, ref );

    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED: child could not extend segment.\n" );
        return;
    }

    for( i = 0; i < 4096; i++ )
    {
        //fprintf( stdout, "CHILD ADDRESS %p offset %lu\n", &( ptr[i] ), ( uint64_t ) i );
        if( i < 2048 )
        {
            if( ptr[i] != 42 + i )
            {
                fprintf(
                    stderr,
                    "FAILED: old data corrupted at index %lu, got %lu, expected %lu\n",
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

    __FENCE();
    return;
}
