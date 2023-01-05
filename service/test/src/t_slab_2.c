#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <sys/wait.h>

#include "../src/lib/slab.h"
#define MAGIC 0xdeadbeef

int main( void );
void child_process( ref_t );

int main( void )
{
    context_t  slab_1 = INVALID_CONTEXT;
    context_t  slab_2 = INVALID_CONTEXT;
    ref_t      ref1   = NULLREF;
    ref_t      ref2   = NULLREF;
    uint64_t * ptr    = NULL;
    pid_t      child  = 0;
    uint64_t   cnt    = 0;

    if( !slab_init() )
    {
        fprintf( stderr, "FAILD: Could not initialize slab\n" );
        return -1;
    }

    slab_1 = new_slab( "TEST", sizeof( uint64_t ) );
    
    if( slab_1 == INVALID_CONTEXT )
    {
        fprintf( stderr, "FAILED: Could not initialize slab context\n" );
        return -1;
    }

    ref1 = rsmalloc( slab_1, sizeof( uint64_t ) * 2 );

    if( ref1 == NULLREF )
    {
        fprintf( stderr, "FAILED: Could not allocate shared memory\n" );
        return -1;
    }

    ptr = to_ptr( slab_1, ref1 );

    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED: Reference derefed to NULL\n" );
        return -1;
    }

    ptr[0] = 42;
    ptr[1] = 43;
    child = fork();

    if( child == 0 )
    {
        child_process( ref1 );
        exit( 0 );
    }

    while( 1 )
    {
        sleep( 1 );
        if( ptr[0] > 10 )
        {
            ptr[0]++;
        }
        cnt++;
        //fprintf( stdout, "PARENT: v: %lu v2: %lu\n", ptr[0], ptr[1] );

        if( ptr[1] > 52 )
            break;

        if( cnt > 15 )
        {
            fprintf( stderr, "FAILED: Child did not increment\n" );
            return -1;
        }
    }

    sleep( 10 );
    slab_2 = ( context_t ) ptr[0];
    ref2   = ( ref_t ) ptr[1];
    slab_2 = new_slab( "TEST2", sizeof( uint64_t ) );
    ptr = ( uint64_t * ) to_ptr( slab_2, ref2 );

    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED: Second test dereferenced to NULL\n" );
        sleep( 60 );
        return -1;    
    }

    if( *ptr != MAGIC )
    {
        fprintf( stderr, "FAILED: magic incorrect\n" );
        return -1;
    }

    *ptr = 0;
    
    wait( NULL );
    fprintf( stdout, "All tests passed.\n" );
    return 0;
}

void child_process( ref_t r )
{
    context_t  slab_1 = INVALID_CONTEXT;
    context_t  slab_2 = INVALID_CONTEXT;
    uint64_t * ptr    = NULL;
    uint64_t   cnt    = 0;
    ref_t      ref    = NULLREF;

    if( !slab_init() )
    {
        fprintf( stderr, "FAILED: Child could not initialize slab\n" );
        return;
    }
    slab_1 = new_slab( "TEST", sizeof( uint64_t ) );

    if( slab_1 == INVALID_CONTEXT )
    {
        fprintf( stderr, "FAILED: could not initialize slab context\n" );
        return;
    }

    ptr = ( uint64_t * ) to_ptr( slab_1, r );
    
    if( ptr == NULL )
    {
        fprintf( stderr, "FAILED: Dereferenced passed ref to NULL\n" );
        return;
    }


    while( 1 )
    {
        sleep(1 );
        ptr[1]++;
        cnt++;
        //fprintf( stdout, "CHILD: v: %lu v2: %lu\n", ptr[0], ptr[1] );
        if( ptr[0] >= 52 )
        {
            break;
        }

        // Avoid inf loop
        if( cnt > 15 )
        {
            fprintf( stderr, "FAILED: No parent increment in shared space\n" );
            exit( 0 );
        }
    }
    sleep( 10 );
    slab_2 = new_slab( "TEST2", sizeof( uint64_t ) );

    ref = ( uint64_t ) rsmalloc( slab_2, sizeof( uint64_t ) );
    if( ref == NULLREF )
    {
        fprintf( stderr, "FAILED: Couldnt allocate from new slab" );
    }

    ptr[0] = ( uint64_t ) slab_2;
    ptr[1] = ( uint64_t ) ref;
    ptr = to_ptr( slab_2, ref );

    *ptr = MAGIC;
    cnt = 0;
    while( 1 )
    {
        sleep( 0.5 );
        cnt++;
        if( *ptr == 0 )
            break;
        if( cnt > 60 )
            break;
    }
    sleep( 60 );
    return;
}
