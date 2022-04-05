#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "../src/lib/barrier.h"
#include "../src/lib/buffer.h"
#include "../src/lib/util.h"

#define NUM_TESTS 10
#define TEST_CTX_NAME "TEST"

const char * quals[NUM_TESTS] = {
    "public.tb_a",
    "public.tb_foobar",
    "public.tb_abcde",
    "public.tb_foobar",
    "public.tb_a",
    "otherschema.tb_b",
    "public.tb_abcde",
    "public.tb_foo",
    "public.tb_foo",
    "public.tb_a"
};

const char * data[NUM_TESTS] = {
    "Test A",
    "Test B",
    "Test C",
    "Test D",
    "Test E",
    "Test F",
    "Test G",
    "Test H",
    "Test I",
    "Test J"
};

int main( void );

int main( void )
{
    context_t           slab     = INVALID_CONTEXT;
    buffer_pin_ref_t    bp       = NULLREF;
    buffer_ref_t        b        = NULLREF;
    char *              test     = NULL;
    ref_t               bdata    = NULLREF;
    unsigned int        i        = 0;

    if( !slab_init() )
    {
        printf( "Failed to initialize slab for test data\n" );
        return -1;
    }

    slab = new_slab( TEST_CTX_NAME, sizeof( char ) );

    if( slab == INVALID_CONTEXT )
    {
        printf( "Failed to get a new shared memory context\n" );
        return -1;
    }

    bdata = rscalloc( slab, sizeof( char ), 2 );
    if( bdata == NULLREF )
    {
        printf( "Failed to allocate string\n" );
    }

    test = ( char * ) to_ptr( slab, bdata );
    if( test == NULL )
    {
        printf( "Failed to allocate string\n" );
        return -1;
    }

    test[0] = 'A';
    test[1] = '\0';

    if( !initialize_contexts() )
    {
        printf( "Failed to initialize buffer contexts\n" );
        return -1;
    }

    new_buffer( &b, "test", bdata );

    if( b == NULLREF )
    {
        printf( "Failed to instantiate new buffer\n" );
        return -1;
    }

    bdata = rscalloc( slab, sizeof( char ), 2 );

    if( bdata == NULLREF )
    {
        printf( "Failed to allocate second string\n" );
        return -1;
    }

    test = ( char * ) to_ptr( slab, bdata );
    if( test == NULL )
    {
        printf( "Failed to allocate second string\n" );
        return -1;
    }

    test[0] = 'B';
    test[1] = '\0';

    if( !buffer_add( b, "test_2", bdata ) )
    {
        printf( "Failed to add test_2 to buffer\n" );
        return -1;
    }

    bp = buffer_get_pin_by_name( b, "test" );

    if( bp == NULLREF )
    {
        printf( "Failed to retreive buffer object\n" );
        return -1;
    }

    bdata = NULLREF;
    bdata = buffer_pin_pop( bp );

    if( bdata == NULLREF )
    {
        printf( "Bufferpin returned NULL ref\n" );
        return -1;
    }

    test = ( char * ) to_ptr( slab, bdata );
    if( test == NULL )
    {
        printf( "Bufferpin returned dereferenced NULL value\n" );
        return -1;
    }

    if( strncmp( test, "A", 1 ) != 0 )
    {
        printf( "Returned pinned object does not match input\n" );
        return -1;
    }

    rsfree( slab, bdata );
    test = NULL;
    bdata = NULLREF;

    bp = buffer_get_pin_by_name( b, "test_2" );

    if( bp == NULLREF )
    {
        printf( "Failed to retreive buffer object for qual 'test_2'\n" );
        return -1;
    }

    bdata = buffer_pin_pop( bp );

    if( bdata == NULLREF )
    {
        printf( "Bufferpin for 'test_2' return NULL ref\n" );
        return -1;
    }

    test = ( char * ) to_ptr( slab, bdata );
    if( test == NULL )
    {
        printf( "Bufferpin for 'test_2' returned NULL value\n" );
        return -1;
    }

    if( strncmp( test, "B", 1 ) != 0 )
    {
        printf( "Returned pinned object for test_2 does not match input\n" );
        return -1;
    }

    rsfree( slab, bdata );
    // Clean up the trie and prep for full test
    if( !remove_buffer_pin_by_name( b, "test_2" ) )
    {
        printf( "Removing buffer pin 'test_2' failed\n" );
        return -1;
    }

    if( !remove_buffer_pin_by_name( b, "test" ) )
    {
        printf( "Removing buffer pin 'test' failed\n" );
        return -1;
    }

    for( i = 0; i < NUM_TESTS; i++ )
    {
        bdata = rsmalloc( slab, sizeof( char * ) * strlen( data[i] ) );
        test = ( char * ) to_ptr( slab, bdata );

        if( test == NULL || bdata == NULLREF )
        {
            printf( "Failed to allocate test string at data index %u", ( unsigned int ) i );
            return  -1;
        }

        strncpy( test, data[i], strlen( data[i] ) );
        if( !buffer_add( b, ( char * ) quals[i], bdata ) )
        {
            printf( "Failed to add index %u to buffer\n", i );
            return -1;
        }
    }

    bdata = NULLREF;

    for( i = 0; i < NUM_TESTS; i++ )
    {
        bdata = buffer_pop( b, ( char * ) quals[i] );

        if( bdata == NULLREF )
        {
            printf( "Failed to pop buffer item for index %u\n", i );
            return -1;
        }

        test = ( char * ) to_ptr( slab, bdata );
        if( strncmp( ( char * ) test, ( char * ) data[i], strlen( data[i] ) ) != 0 )
        {
            printf(
                "Returned data from pin does not match, got B: '%s' and E: '%s'\n",
                ( char * ) test,
                ( char * ) data[i]
            );
            return -1;
        }

        rsfree( slab, bdata );
    }

    printf( "All tests passed\n" );
    return 0;
}
