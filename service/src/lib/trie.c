/*--------------------------------------------------------------------------
 *
 * trie.c
 *     Trie data structure
 *
 * Prefix tree for rapidly sorting WAL changes for relations
 *
 * Copyright (c) 2019-2022, MerchLogix, Inc.
 *
 * IDENTIFICATION
 *          service/src/lib/trie.c
 *--------------------------------------------------------------------------
 */

#include "trie.h"

static bool _trie_has_children( trie_ref_t );
static trie_ref_t _new_trie_node( void );
static bool _trie_string_safety_check( char * );
static void _trie_free( trie_ref_t );

/*
 * List of valid characters for a trie span, listed in order
 * of their appearance on the ASCII table
 */
static const char _trie_search_chars[TRIE_SIZE] = "\
 !\"#$%&'()*+,-./0123456789:;<=\
>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\
\\]^_`abcdefghijklmnopqrstuvwxy\
z{|}~'";

static context_t trie_context = 0;

// Rely on external initialization of contexts
void set_trie_context( context_t ctx )
{
    if( !check_context( ctx ) )
        return;
    trie_context = ctx;
    return;
}

bool trie_insert( trie_ref_t * head, char * str, ref_t data )
{
    struct trie * curr = NULL;
    trie_ref_t    temp = NULLREF;

    if( unlikely( str == NULL ) )
        return false;

    if( unlikely( !_trie_string_safety_check( str ) ) )
        return false;

    if( *head == ( trie_ref_t ) NULLREF )
    {
        *head = ( trie_ref_t )  _new_trie_node();
        trie_insert( head, str, data );
        return true;
    }

    curr = ( struct trie * ) to_ptr( trie_context, ( ref_t ) *head );

    if( unlikely( curr == NULL ) )
        return false;

    while( *str )
    {
        if( curr->character[*str - ' '] == ( trie_ref_t ) NULLREF )
        {
            temp = _new_trie_node();

            if( unlikely( temp == ( trie_ref_t ) NULLREF ) )
                return false;

            curr->character[*str - ' '] = temp;
        }

        curr = ( struct trie * ) to_ptr( trie_context, ( ref_t ) curr->character[*str - ' '] );

        if( unlikely( curr == NULL ) )
            return false;

        str++;
    }

    curr->is_leaf = true;
    curr->data    = data;
    return true;
}

ref_t trie_search( trie_ref_t head, char * str )
{
    struct trie * curr = NULL;

    if( unlikely( head == ( trie_ref_t ) NULLREF || str == NULL ) )
    {
        return NULLREF;
    }

    curr = ( struct trie * ) to_ptr( trie_context, ( ref_t ) head );

    if( unlikely( curr == NULL ) )
        return NULLREF;

    while( *str )
    {
        if( unlikely( curr->character[*str - ' '] == NULLREF ) )
            return NULLREF;

        curr = ( struct trie * ) to_ptr( trie_context, ( ref_t ) curr->character[*str - ' '] );

        if( unlikely( curr == NULL ) )
            return NULLREF;

        str++;
    }

    if( likely( curr->is_leaf ) )
    {
        return ( ref_t ) curr->data;
    }

    return NULLREF;
}

ref_t trie_delete( trie_ref_t * head, char * str )
{
    ref_t         data = NULLREF;
    struct trie * curr = NULL;

    curr = ( struct trie * ) to_ptr( trie_context, ( ref_t ) *head );

    if(
        unlikely(
            ( head == NULL )
         || ( *head == ( trie_ref_t )  NULLREF )
         || ( curr == NULL )
        )
      )
        return NULLREF;

    if( *str )
    {
        if(
               *head != ( trie_ref_t ) NULLREF
            && curr->character[*str - ' '] != ( trie_ref_t ) NULLREF
            && trie_delete( &(curr->character[*str - ' '] ), str + 1 )
            && !curr->is_leaf
          )
        {
            if( !_trie_has_children( *head ) )
            {
                data = curr->data;
                rsfree( trie_context, *head );
                *head = ( trie_ref_t ) NULLREF;
                return data;
            }

            return NULLREF;
        }
    }

    if( *str == '\0' && curr->is_leaf == false )
    {
        if( !_trie_has_children( *head ) )
        {
            data = curr->data;
            rsfree( trie_context, ( ref_t ) *head );
            *head = ( trie_ref_t ) NULLREF;
            return data;
        }

        curr->is_leaf = false;
        return NULLREF;
    }

    return NULLREF;
}

void trie_free( trie_ref_t * node )
{
    if( unlikely( ( node == NULL ) || ( *node == ( trie_ref_t ) NULLREF ) ) )
        return;

    _trie_free( *node );
    rsfree( trie_context, ( ref_t ) *node );
    *node = ( trie_ref_t ) NULLREF;
    return;
}

static bool _trie_has_children( trie_ref_t node )
{
    uint32_t      i    = 0;
    struct trie * curr = NULL;

    curr = ( struct trie * ) to_ptr( trie_context, ( ref_t ) node );
    if( curr == NULL )
        return false;

    for( i = 0; i < TRIE_SIZE; i++ )
    {
        if( curr->character[i] != NULLREF )
        {
            return true;
        }
    }

    return false;
}

static trie_ref_t _new_trie_node( void )
{
    trie_ref_t    node = ( trie_ref_t ) NULLREF;
    struct trie * n    = NULL;
    unsigned int  i    = 0;

    node = ( trie_ref_t ) rsmalloc( trie_context, sizeof( struct trie ) );

    if( unlikely( node == ( trie_ref_t ) NULLREF ) )
        return NULLREF;

    n = ( struct trie * ) to_ptr( trie_context, ( ref_t ) node );

    if( unlikely( n == NULL ) )
        return NULLREF;

    n->is_leaf = false;

    for( i = 0; i < TRIE_SIZE; i++ )
    {
        n->character[i] = ( trie_ref_t ) NULLREF;
    }

    return node;
}

static bool _trie_string_safety_check( char * str )
{
    unsigned int i = 0;

    for( i = 0; i < strlen( str ); i++ )
    {
        if( unlikely( memchr( _trie_search_chars, str[i], TRIE_SIZE ) == NULL ) )
        {
            return false;
        }
    }

    return true;
}

static void _trie_free( trie_ref_t node )
{
    uint32_t      i    = 0;
    struct trie * curr = NULL;

    if( unlikely( node == ( trie_ref_t ) NULLREF ) )
        return;

    curr = ( struct trie * ) to_ptr( trie_context, ( ref_t ) node );

    if( unlikely( curr == NULL ) )
        return;

    for( i = 0; i < TRIE_SIZE; i++ )
    {
        if( curr->character[i] != ( trie_ref_t ) NULLREF )
        {
            _trie_free( curr->character[i] );
            rsfree( trie_context, ( ref_t ) curr->character[i] );
            curr->character[i] = ( trie_ref_t ) NULLREF;
        }
    }

    return;
}
