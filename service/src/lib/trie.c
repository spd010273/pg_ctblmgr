#include "trie.h"

static bool _trie_has_children( struct trie * );
static struct trie * _new_trie_node( void );
static bool _trie_string_safety_check( char * );
static void _trie_free( struct trie * );

/*
 * List of valid characters for a trie span, listed in order
 * of their appearance on the ASCII table
 */
static const char _trie_search_chars[TRIE_SIZE] = "\
 !\"#$%&'()*+,-./0123456789:;<=\
>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\
\\]^_`abcdefghijklmnopqrstuvwxy\
z{|}~'";

bool trie_insert( struct trie ** head, char * str, void * data )
{
    struct trie * curr = NULL;
    struct trie * temp = NULL;

    if( str == NULL )
    {
        return false;
    }

    if( !_trie_string_safety_check( str ) )
    {
        return false;
    }

    if( *head == NULL )
    {
        *head = _new_trie_node();
        trie_insert( head, str, data );
        return true;
    }

    curr = *head;

    while( *str )
    {
        if( curr->character[*str - ' '] == NULL )
        {
            temp = _new_trie_node();

            if( temp == NULL )
            {
                return false;
            }

            curr->character[*str - ' '] = temp;
        }

        curr = curr->character[*str - ' '];
        str++;
    }

    curr->is_leaf = true;
    curr->data    = data;
    return true;
}

void * trie_search( struct trie * head, char * str )
{
    struct trie * curr = NULL;

    if( head == NULL || str == NULL )
    {
        return NULL;
    }

    curr = head;

    while( *str )
    {
        curr = curr->character[*str - ' '];

        if( curr == NULL )
        {
            return NULL;
        }

        str++;
    }

    if( curr->is_leaf )
    {
        return curr->data;
    }

    return NULL;
}

void * trie_delete( struct trie ** head, char * str )
{
    void * data = NULL;

    if( *head == NULL )
    {
        return NULL;
    }

    if( *str )
    {
        if(
               *head != NULL
            && (*head)->character[*str - ' '] != NULL
            && trie_delete( &((*head)->character[*str - ' '] ), str + 1 )
            && !(*head)->is_leaf
          )
        {
            if( !_trie_has_children( *head ) )
            {
                data = (*head)->data;
                _TRIE_FREE( *head, sizeof( struct trie ) );
                *head = NULL;
                return data;
            }

            return NULL;
        }
    }

    if( *str == '\0' && (*head)->is_leaf == false )
    {
        if( !_trie_has_children( *head ) )
        {
            data = (*head)->data;
            _TRIE_FREE( *head, sizeof( struct trie ) );
            *head = NULL;
            return data;
        }

        (*head)->is_leaf = false;
        return NULL;
    }

    return NULL;
}

void trie_free( struct trie ** node )
{
    if( node == NULL || *node == NULL )
    {
        return;
    }

    _trie_free( *node );
    _TRIE_FREE( *node, sizeof( struct trie ) );
    *node = NULL;
    return;
}

static bool _trie_has_children( struct trie * node )
{
    unsigned int i = 0;

    for( i = 0; i < TRIE_SIZE; i++ )
    {
        if( node->character[i] )
        {
            return true;
        }
    }

    return false;
}

static struct trie * _new_trie_node( void )
{
    struct trie * node = NULL;
    unsigned int i = 0;
    node = ( struct trie * ) _TRIE_ALLOC( sizeof( struct trie ) );

    if( node == NULL )
    {
        return NULL;
    }

    node->is_leaf = false;

    for( i = 0; i < TRIE_SIZE; i++ )
    {
        node->character[i] = NULL;
    }

    return node;
}

static bool _trie_string_safety_check( char * str )
{
    unsigned int i = 0;

    for( i = 0; i < strlen( str ); i++ )
    {
        if( memchr( _trie_search_chars, str[i], TRIE_SIZE ) == NULL )
        {
            return false;
        }
    }

    return true;
}

static void _trie_free( struct trie * node )
{
    unsigned int i = 0;

    if( node == NULL )
    {
        return;
    }

    for( i = 0; i < TRIE_SIZE; i++ )
    {
        if( node->character[i] != NULL )
        {
            _trie_free( node->character[i] );
            _TRIE_FREE( node->character[i], sizeof( struct trie * ) );
            node->character[i] = NULL;
        }
    }

    return;
}
