#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "../src/lib/slab.h"
#include "../src/lib/slpq.h"
#define TEST_CONTEXT_NAME "TEST"
#define NUM_TESTS 20

const char * test_data[NUM_TESTS] = {
    "A",
    "b",
    "C",
    "DE",
    "f",
    "FOOOOOOOO",
    "this is a test queue entry",
    "this is another test queue entry but it's different when compared to the previous one",
    "Lorem ipsum or however it goes",
    "Man i wish I could just dump this list full of arbitrary md5 hashes",
    "z",
    "I think it's spelled 'zed' in the Queen's English, my good sir",
    "Oh, right. Sorry about that",
    "ASDF",
    "QUERTY",
    "This is most certainly not a test string",
    "Test string. Please ignore.",
    "I have three more. oops two more. I guess three is correct if you include this one.",
    "Hey man, you got anymore test strings that could possibly overwrite themselves???",
    "Nope."
};

int main( void );

char * strings[NUM_TESTS] = {NULL};

int main( void )
{
    context_t     slpq_context      = INVALID_CONTEXT;
    context_t     slpq_node_context = INVALID_CONTEXT;
    context_t     test_context      = INVALID_CONTEXT;
    slpq_ref_t    slpq_head         = NULLREF;
    ref_t         char_ref          = NULLREF;
    char *        string            = NULL;
    uint8_t       i                 = 0;
    struct slpq * slpq              = NULL;

    if( !slab_init() )
    {
        printf( "FAILED: Could not initialize slab\n" );
        return -1;
    }

    slpq_context      = new_slab( SLPQ_CONTEXT_NAME, sizeof( struct slpq ) );
    slpq_node_context = new_slab( SLPQ_NODE_CONTEXT_NAME, sizeof( struct slpq_node ) );
    test_context      = new_slab( TEST_CONTEXT_NAME, sizeof( char ) );


    if( slpq_context == INVALID_CONTEXT )
    {
        printf( "FAILED: Could not initialize slpq memory context\n" );
        return -1;
    }

    if( slpq_node_context == INVALID_CONTEXT )
    {
        printf( "FAILED: Could not initialize slpq node memory context\n" );
        return -1;
    }

    if( test_context == INVALID_CONTEXT )
    {
        printf( "FAILED: Could not initialize test context\n" );
        return -1;
    }

    if( !set_slpq_context( slpq_context ) )
    {
        printf( "FAILED: Failed to set SLPQ context\n" );
        return -1;
    }

    if( !set_slpq_node_context( slpq_node_context ) )
    {
        printf( "FAILED: Failed to set SLPQ node context\n" );
        return -1;
    }

    slpq_head = new_slpq();

    if( slpq_head == NULLREF )
    {
        printf( "FAILED: Could not initialize SLPQ\n" );
        return -1;
    }

    // Phase 1 push/pop tests
    for( i = 0; i < NUM_TESTS; i++ )
    {
        string = smalloc( test_context, sizeof( char ) * ( strlen( test_data[i] ) + 1 ) );

        if( string == NULL )
        {
            printf(
                "FAILED: Could not allocate space for string %u: '%s'\n",
                ( uint32_t ) i,
                test_data[i]
            );
            return -1;
        }

        strncpy( string, test_data[i], strlen( test_data[i] ) );
        strings[i] = string;
        char_ref = ( ref_t ) to_ref( test_context, string );

        if( char_ref == NULLREF )
        {
            printf( "FAILED: Could not generate ref to %p for SLPQ insertion\n", string );
            return -1;
        }

        if( !slpq_push( slpq_head, char_ref ) )
        {
            printf( "FAILED: Could not push %u onto SLPQ\n", ( uint32_t ) i );
            return -1;
        }
    }

    for( i = 0; i < NUM_TESTS; i++ )
    {
        char_ref = slpq_pop( slpq_head );

        if( char_ref == NULLREF )
        {
            printf( "FAILED: Could not pop SLPQ element\n" );
            return -1;
        }

        string = ( char * ) to_ptr( test_context, char_ref );

        if( string == NULL )
        {
            printf( "FAILED: Popped NULL string at %u\n", ( uint32_t ) i );
            return -1;
        }

        if( strncmp( string, ( char * ) test_data[i], strlen( test_data[i] ) ) != 0)
        {
            printf(
                "FAILED: Popped element does not match expected element at %u:\nEx: '%s'\n G:  '%s'\n",
                ( uint32_t ) i,
                test_data[i],
                string
            );
            return -1;
        }
    }

    slpq = ( struct slpq * ) to_ptr( slpq_context, slpq_head );

    if( slpq == NULL )
    {
        printf( "FAILED: Failed to dereference SLPQ head\n" );
        return -1;
    }

    if( slpq->size != 0 )
    {
        printf( "FAILED: SLPQ size is not 0\n" );
        return -1;
    }

    if( slpq->head != NULLREF )
    {
        printf( "FAILED: SLPQ head is not NULL\n" );
        return -1;
    }

    if( slpq->tail != NULLREF )
    {
        printf( "FAILED: SLPQ tail is not NULL\n" );
        return -1;
    }

    for( i = 0; i < NUM_TESTS; i++ )
    {
        char_ref = to_ref( test_context, strings[i] );

        if( char_ref == NULLREF )
        {
            printf( "FAILED: ref to strings[%u] is NULL\n", ( uint32_t ) i );
            return -1;
        }

        if( !slpq_shift( slpq_head, char_ref ) )
        {
            printf( "FAILED: Could not shift string %u onto SLPQ\n", ( uint32_t ) i );
            return -1;
        }
    }

    if( slpq->size != NUM_TESTS )
    {
        printf( "FAILED: SLPQ size is incorrect after all items shifted into it\n" );
        return -1;
    }

    for( i = 0; i < NUM_TESTS; i++ )
    {
        char_ref = ( ref_t ) slpq_unshift( slpq_head );

        if( char_ref == NULLREF )
        {
            printf( "FAILED: Could not unshift element %u from SLPQ\n", ( uint32_t ) i );
            return -1;
        }

        string = ( char * ) to_ptr( test_context, char_ref );

        if( string == NULL )
        {
            printf( "FAILED: string %u dereferenced to NULL\n", ( uint32_t ) i );
            return -1;
        }

        if( strncmp( string, test_data[i], strlen( test_data[i] ) ) != 0 )
        {
            printf(
                "FAILED: Unshifted element %u does not match expected value\nEX: '%s'\nG:  '%s'\n",
                ( uint32_t ) i,
                test_data[i],
                string
            );
            return -1;
        }

        rsfree( test_context, char_ref );
    }

    if( slpq->size != 0 )
    {
        printf( "FAILED: SLPQ not empty after shift/unshift test\n" );
        return -1;
    }

    if( slpq->head != NULLREF )
    {
        printf( "FAILED: SLPQ head is not NULL when empty\n" );
        return -1;
    }

    if( slpq->tail != NULLREF )
    {
        printf( "FAILED, SLPQ tail is not NULL when empty\n" );
        return -1;
    }

    slpq_free( slpq_head );

    printf( "All tests passed\n" );
    return 0;
}
