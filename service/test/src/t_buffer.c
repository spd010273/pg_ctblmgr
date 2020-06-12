#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "../src/lib/buffer.h"

#define NUM_TESTS 10

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
    struct buffer_pin * bp      = NULL;
    struct buffer *     b       = NULL;
    char *              test    = NULL;
    void *              bp_data = NULL;
    unsigned int        i       = 0;

    test = ( char * ) calloc( sizeof( char ), 2 );

    if( test == NULL )
    {
        printf( "Failed to allocate string\n" );
        return -1;
    }

    test[0] = 'A';
    test[1] = '\0';

    new_buffer( &b, "test", ( void * ) test );

    if( b == NULL )
    {
        printf( "Failed to instantiate new buffer\n" );
        return -1;
    }

    test = ( char * ) calloc( sizeof( char ), 2 );

    if( test == NULL )
    {
        printf( "Failed to allocate string\n" );
        return -1;
    }

    test[0] = 'B';
    test[1] = '\0';

    if( !buffer_add( b, "test_2", ( void * ) test ) )
    {
        printf( "Failed to add test_2 to buffer\n" );
        return -1;
    }

    bp = buffer_get_pin_by_name( b, "test" );

    if( bp == NULL )
    {
        printf( "Failed to retreive buffer object\n" );
        return -1;
    }

    test = ( char * ) buffer_pin_pop( bp );

    if( test == NULL )
    {
        printf( "Bufferpin returned NULL value\n" );
        return -1;
    }

    if( strncmp( test, "A", 1 ) != 0 )
    {
        printf( "Returned pinned object does not match input\n" );
        return -1;
    }

    free( test );
    test = NULL;

    bp = buffer_get_pin_by_name( b, "test_2" );

    if( bp == NULL )
    {
        printf( "Failed to retreive buffer object for qual 'test_2'\n" );
        return -1;
    }

    test = ( char * ) buffer_pin_pop( bp );

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
        if( !buffer_add( b, ( char * ) quals[i], ( void * ) data[i] ) )
        {
            printf( "Failed to add index %u to buffer\n", i );
            return -1;
        }
    }

    for( i = 0; i < NUM_TESTS; i++ )
    {
        bp_data = buffer_pop( b, ( char * ) quals[i] );

        if( bp_data == NULL )
        {
            printf( "Failed to pop buffer item for index %u\n", i );
            return -1;
        }

        if( strncmp( ( char * ) bp_data, ( char * ) data[i], strlen( data[i] ) ) != 0 )
        {
            printf(
                "Returned data from pin does not match, got B: '%s' and E: '%s'\n",
                ( char * ) bp_data,
                ( char * ) data[i]
            );
            return -1;
        }
    }

    printf( "All tests passed\n" );
    return 0;
}
