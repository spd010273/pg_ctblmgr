#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <sys/wait.h>
#include "../src/lib/trie.h"

#define TEST_CONTEXT "TEST"
int main ( void );

#define NUM_TESTS 4
const char * idents[NUM_TESTS] = {
    "tb_foo",
    "tb_bar",
    "tb_baz",
    "tb_foo_bar_baz"
};
const char * test_strings[NUM_TESTS] = {
    "Test leaf 1", // 12
    "test leaf two", // 14
    " This is a long string", // 23
    "this is an even longer string, some would say that it is significantly longer than the last"
};

int main( void )
{
    context_t  trie_context = INVALID_CONTEXT;
    context_t  test_context = INVALID_CONTEXT;
    ref_t      dataref      = NULLREF;
    uint8_t    i            = 0;
    char *     test_string  = NULL;
    trie_ref_t trie         = NULLREF;

    if( !slab_init() )
    {
        printf( "FAILED: Failed to initialize slab\n" );
        return -1;
    }

    trie_context = new_slab( TRIE_CONTEXT_NAME, sizeof( struct trie ) );

    if( trie_context == INVALID_CONTEXT )
    {
        printf( "FAILED: Failed to initialize trie memory context\n" );
        return -1;
    }

    set_trie_context( trie_context );

    test_context = new_slab( TEST_CONTEXT, sizeof( char ) );

    if( test_context == INVALID_CONTEXT )
    {
        printf( "FAILED: Failed to initialize test context\n" );
        return -1;
    }

    // Phase 1: Generate trie
    for( i = 0; i < NUM_TESTS; i++ )
    {
        test_string = smalloc( test_context, sizeof( char ) * ( strlen( test_strings[i] ) + 1 ) );

        if( test_string == NULL )
        {
            printf( "FAILED: Could not allocate memory for test string\n" );
            return -1;
        }

        dataref = ( ref_t ) to_ref( test_context, test_string );

        if( dataref == NULLREF )
        {
            printf( "FAILED: Could not generate a ref_t reference for %p", test_string );
            return -1;
        }

        strncpy(
            test_string,
            test_strings[i],
            strlen( test_strings[i] )
        );
        test_string[strlen(test_strings[i])] = '\0';

        if( !trie_insert( &trie, ( char * ) idents[i], dataref ) )
        {
            printf( "FAILED: Could not insert into trie ident %s:\n'%s'\n", idents[i], test_string );
            return -1;
        }

        dataref = NULLREF;
        dataref = trie_search( trie, ( char * ) idents[i] );

        test_string = ( char * ) to_ptr( test_context, dataref );

        if( test_string == NULL )
        {
            printf( "FAILED: Inline dereference returned null for ident %s\n", idents[i] );
            return -1;
        }

        if( strncmp( test_string, ( char * ) test_strings[i], strlen( test_strings[i] ) ) != 0 )
        {
            printf(
                "FAILED: Inline test string retrieved from trie ident '%s' does not match expected value.\nEX: '%s'\nST: '%s'\n",
                idents[i],
                test_strings[i],
                test_string
            );
            return -1;
        }
    }

    // Phase 2: check trie contents
    for( i = 0; i < NUM_TESTS; i++ )
    {
        dataref = trie_search( trie, ( char * ) idents[i] );

        if( dataref == NULLREF )
        {
            printf( "FAILED: Could not retrieve dataref for trie ident '%s'\n", idents[i] );
            return -1;
        }

        test_string = ( char * ) to_ptr( test_context, dataref );

        if( test_string == NULL )
        {
            printf( "FAILED: Could not dereference retrieved string from trie ident '%s'\n", idents[i] );
            return -1;
        }

        if( strncmp( test_string, ( char * ) test_strings[i], strlen( test_strings[i] ) ) != 0 )
        {
            printf(
                "FAILED: Test string retrieved from trie ident '%s' does not match expected value.\nEx: '%s'\nGot: '%s'\n",
                idents[i],
                test_strings[i],
                test_string
            );
            return -1;
        }
    }

    printf( "All tests passed\n" );
    return 0;
}
