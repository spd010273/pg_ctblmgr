#include "buffer.h"

#ifdef __BUF_NO_ATOMICS__
static inline bool _test_and_set( bool * );
static bool _test_and_set_mutex( bool * );
static void _clear_mutex( bool * );
#define __TNS_MUTEX(val) _test_and_set_mutex(val)
#define __C_MUTEX(val) _clear_mutex(val)
#endif // __BUF_NO_ATOMICS__

static struct buffer_pin * _new_buffer_pin( void );
static struct buffer * _new_buffer( void );

void buffer_populate_trie( struct buffer ** b, char ** qual_name, unsigned int n_quals )
{
    void *              data = NULL;
    struct buffer_pin * bp   = NULL;
    unsigned int        i    = 0;

    if( *b == NULL )
        *b = ( struct buffer * ) _new_buffer();

    if( qual_name == NULL )
        return;

    if( !__TNS_MUTEX( (&((*b)->in_use)) ) )
        return;

    for( i = 0; i < n_quals; i++ )
    {
        data = trie_search( (*b)->trie, qual_name[i] );

        if( data == NULL )
        {
            bp = _new_buffer_pin();

            if( bp == NULL )
            {
                __C_MUTEX( (&((*b)->in_use)) );
                return;
            }

            if( !trie_insert( &((*b)->trie), qual_name[i], ( void * ) bp ) )
            {
                __C_MUTEX( (&((*b)->in_use)) );
                return;
            }
        }
    }
    
    __C_MUTEX( (&((*b)->in_use)) );
    return;
}

void new_buffer( struct buffer ** b, char * qual_name, void * wal_data )
{
    if( *b == NULL )
        *b = ( struct buffer * ) _new_buffer();

    if(
            qual_name != NULL
         && wal_data != NULL
         && !buffer_add( *b, qual_name, wal_data )
      )
    {
        return;
    }

    return;
}

struct buffer_pin * buffer_get_pin_by_name( struct buffer * b, char * qual_name )
{
    struct buffer_pin * bp   = NULL;
    void *              data = NULL;

    if( b == NULL || qual_name == NULL )
        return NULL;

    if( !__TNS_MUTEX( (&(b->in_use)) ) )
        return NULL;

    data = trie_search( b->trie, qual_name );
    __C_MUTEX( (&(b->in_use)) );

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

    if( !__TNS_MUTEX( (&(b->in_use)) ) )
        return false;

    data = trie_search( b->trie, qual_name );

    if( data == NULL )
    {
        bp = _new_buffer_pin();

        if( bp == NULL )
        {
            __C_MUTEX( (&(b->in_use)) );
            return false;
        }

        if( !trie_insert( &(b->trie), qual_name, ( void * ) bp ) )
        {
            __C_MUTEX( (&(b->in_use)) );
            return false;
        }

        b->entries++;
    }
    else
    {
        bp = ( struct buffer_pin * ) data;
    }

    __C_MUTEX( (&(b->in_use)) );

    if( !__TNS_MUTEX( (&(bp->in_use)) ) )
        return false;

    if( !slpq_push( bp->slpq, wal_data ) )
    {
        __C_MUTEX( (&(bp->in_use)) );
        return false;
    }

    __C_MUTEX( (&(bp->in_use)) );
    return true;
}

void * buffer_pin_pop( struct buffer_pin * bp )
{
    void * data = NULL;

    if( bp == NULL )
    {
        return NULL;
    }

    if( !__TNS_MUTEX( (&(bp->in_use)) ) )
        return NULL;

    data = slpq_pop( bp->slpq );
    __C_MUTEX( (&(bp->in_use)) );
    return data;
}

bool buffer_pin_push( struct buffer_pin * bp, void * data )
{
    if( bp == NULL || data == NULL )
    {
        return false;
    }

    if( !__TNS_MUTEX( (&(bp->in_use)) ) )
        return false;

    if( !slpq_push( bp->slpq, data ) )
    {
        __C_MUTEX( (&(bp->in_use)) );
        return false;
    }

    __C_MUTEX( (&(bp->in_use)) );
    return true;
}

bool remove_buffer_pin_by_name( struct buffer * b, char * qual_name )
{
    struct buffer_pin * bp = NULL;

    if( b == NULL || qual_name == NULL )
    {
        return false;
    }

    if( !__TNS_MUTEX( (&(b->in_use)) ) )
        return false;

    bp = ( struct buffer_pin * ) trie_search( b->trie, qual_name );

    if( bp == NULL )
    {
        __C_MUTEX( (&(b->in_use)) );
        return true;
    }
    else
    {
        if( !__TNS_MUTEX( (&(bp->in_use)) ) )
        {
            __C_MUTEX( (&(b->in_use)) );
            return false;
        }

        if( bp->slpq->size != 0 )
        {
            __C_MUTEX( (&(bp->in_use)) );
            __C_MUTEX( (&(b->in_use)) );
            return false;
        }

        bp = ( struct buffer_pin * ) trie_delete( &(b->trie), qual_name );

        if( bp != NULL )
            _BUFFER_FREE( bp );

        b->entries--;
        __C_MUTEX( (&(b->in_use)) );
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

#ifdef __BUF_NO_ATOMICS__
static inline bool _test_and_set( bool * mutex )
{
    bool initial = true;
    initial = *mutex;
    *mutex = true;
    return initial;
}

static bool _test_and_set_mutex( bool * mutex )
{
    while( *mutex == true || _test_and_set( mutex ) == true );
    return true;
}

static void _clear_mutex( bool * mutex )
{
    *mutex = false;
    return;
}
#endif // __BUF_NO_ATOMICS__
