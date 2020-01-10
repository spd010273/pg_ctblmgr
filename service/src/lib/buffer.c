#include "buffer.h"

static struct buffer_pin * _new_buffer_pin( void );
static struct buffer * _new_buffer( void );
static inline bool _test_and_set( bool * );
static bool _test_and_set_mutex( bool * );

void new_buffer( struct buffer ** b, char * qual_name, void * wal_data )
{
    if( b == NULL || qual_name == NULL || wal_data == NULL )
        return;

    if( *b == NULL )
    {
        *b = ( struct buffer * ) _new_buffer();
    }

    if( !buffer_add( *b, qual_name, wal_data ) )
        return;

    return;
}

struct buffer_pin * buffer_get_pin_by_name( struct buffer * b, char * qual_name )
{
    struct buffer_pin * bp   = NULL;
    void *              data = NULL;

    if( b == NULL || qual_name == NULL )
        return NULL;

    if( !_test_and_set_mutex( &(b->in_use) ) )
        return NULL;

    data = trie_search( b->trie, qual_name );
    b->in_use = false;

    if( data != NULL )
    {
        bp = ( struct buffer_pin * ) data;
        return bp;
    }

    return NULL;
}

bool buffer_add( struct buffer * b, char * qual_name, void * wal_data )
{
    void *              data = NULL;
    struct buffer_pin * bp   = NULL;

    if( b == NULL || qual_name == NULL || wal_data == NULL )
        return false;

    if( !_test_and_set_mutex( &(b->in_use) ) )
        return false;

    data = trie_search( b->trie, qual_name );

    if( data == NULL )
    {
        bp = _new_buffer_pin();

        if( bp == NULL )
        {
            b->in_use = false;
            return false;
        }

        if( !trie_insert( &(b->trie), qual_name, ( void * ) bp ) )
        {
            b->in_use = false;
            return false;
        }

        b->entries++;
    }
    else
    {
        bp = ( struct buffer_pin * ) data;
    }

    b->in_use = false;

    if( !_test_and_set_mutex( &(bp->in_use) ) )
        return false;

    if( !slpq_push( bp->slpq, wal_data ) )
    {
        bp->in_use = false;
        return false;
    }

    bp->in_use = false;
    return true;
}

void * buffer_pin_pop( struct buffer_pin * bp )
{
    void * data = NULL;

    if( bp == NULL )
    {
        return NULL;
    }

    if( !_test_and_set_mutex( &(bp->in_use) ) )
        return NULL;

    data = slpq_pop( bp->slpq );
    bp->in_use = false;
    return data;
}

bool buffer_pin_push( struct buffer_pin * bp, void * data )
{
    if( bp == NULL || data == NULL )
    {
        return false;
    }

    if( !_test_and_set_mutex( &(bp->in_use) ) )
        return false;

    if( !slpq_push( bp->slpq, data ) )
    {
        bp->in_use = false;
        return false;
    }

    bp->in_use = false;
    return true;
}

bool remove_buffer_pin_by_name( struct buffer * b, char * qual_name )
{
    struct buffer_pin * bp = NULL;

    if( b == NULL || qual_name == NULL )
    {
        return false;
    }

    if( !_test_and_set_mutex( &(b->in_use) ) )
        return false;

    bp = ( struct buffer_pin * ) trie_search( b->trie, qual_name );

    if( bp == NULL )
    {
        b->in_use = false;
        return true;
    }
    else
    {
        if( !_test_and_set_mutex( &(bp->in_use) ) )
        {
            b->in_use = false;
            return false;
        }

        if( bp->slpq->size != 0 )
        {
            bp->in_use = false;
            b->in_use  = false;
            return false;
        }

        bp = ( struct buffer_pin * ) trie_delete( &(b->trie), qual_name );

        if( bp != NULL )
            _BUFFER_FREE( bp );

        b->entries--;
        b->in_use = false;
        return true;
    }
}

static struct buffer_pin * _new_buffer_pin( void )
{
    struct buffer_pin * bp = NULL;

    bp = ( struct buffer_pin * ) _BUFFER_ALLOC( sizeof( struct buffer_pin ) );

    if( bp == NULL )
        return NULL;

    bp->in_use = false;
    bp->owner  = 0;
    bp->slpq   = new_slpq();
    return bp;
}

static struct buffer * _new_buffer( void )
{
    struct buffer * b = NULL;

    b = ( struct buffer * ) _BUFFER_ALLOC( sizeof( struct buffer ) );

    if( b == NULL )
        return NULL;

    b->entries = 0;
    b->trie    = NULL;
    return b;
}

static inline bool _test_and_set( bool * mutex )
{
    bool initial = true;
    initial = *mutex;
    *mutex  = true;
    return initial;
}

static bool _test_and_set_mutex( bool * mutex )
{
    while( *mutex == true || _test_and_set( mutex ) == true );
    *mutex = true;
    return true;
}
