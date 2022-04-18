/*--------------------------------------------------------------------------
 *
 * buffer.c
 *     Internal WAL buffer data structure collection / control
 *
 * This library presents a shared memory backed data structure consisting of
 * a trie which stores buffer pins by relation name. Each buffer pin contains
 * the head of a single-linked priority queue containing WAL entries streamed
 * over logical replication.
 *
 * The parent process is responsible for filling and maintaining the queues,
 * while the child processes are responsible for dequeuing the SLPQs and
 * processing the impact of each WAL change with respect to the table under
 * their responsibility.
 *
 * Copyright (c) 2019-2022, MerchLogix, Inc.
 *
 * IDENTIFICATION
 *          service/src/lib/buffer.c
 *--------------------------------------------------------------------------
 */

#include "buffer.h"

static buffer_pin_ref_t _new_buffer_pin( void );
static buffer_ref_t _new_buffer( void );

static context_t buffer_context = INVALID_CONTEXT;
static context_t buffer_pin_context = INVALID_CONTEXT;
static context_t slpq_context = INVALID_CONTEXT;

bool initialize_contexts( void )
{
    context_t _slpq_context       = INVALID_CONTEXT;
    context_t _slpq_node_context  = INVALID_CONTEXT;
    context_t _buffer_context     = INVALID_CONTEXT;
    context_t _buffer_pin_context = INVALID_CONTEXT;
    context_t _trie_context       = INVALID_CONTEXT;

    _slpq_context       = new_slab( SLPQ_CONTEXT_NAME, sizeof( struct slpq ) );
    _slpq_node_context  = new_slab( SLPQ_NODE_CONTEXT_NAME, sizeof( struct slpq_node ) );
    _buffer_context     = new_slab( BUFFER_CONTEXT_NAME, sizeof( struct buffer ) );
    _buffer_pin_context = new_slab( BUFFER_PIN_CONTEXT_NAME, sizeof( struct buffer_pin ) );
    _trie_context       = new_slab( TRIE_CONTEXT_NAME, sizeof( struct trie ) );
    set_trie_context( _trie_context );

    buffer_pin_context = _buffer_pin_context;
    buffer_context     = _buffer_context;
    slpq_context       = _slpq_context;

    if( !set_slpq_context( _slpq_context ) )
        return false;
    if( !set_slpq_node_context( _slpq_node_context ) )
        return false;

    if(
          _slpq_context == INVALID_CONTEXT
       || _slpq_node_context == INVALID_CONTEXT
       || _buffer_pin_context == INVALID_CONTEXT
       || _buffer_context == INVALID_CONTEXT
       || _trie_context == INVALID_CONTEXT
      )
    {
        return false;
    }

    return true;
}

context_t get_buffer_pin_context( void )
{
    return buffer_pin_context;
}

context_t get_buffer_context( void )
{
    return buffer_context;
}

void buffer_populate_trie(
    buffer_ref_t * b,
    char **        qual_name,
    unsigned int   n_quals
)
{
    ref_t               data = NULLREF;
    buffer_pin_ref_t    bp   = NULLREF;

    //struct buffer_pin * bp   = NULL;
    unsigned int        i    = 0;
    struct buffer * buff     = NULL;
    if( *b == NULLREF )
        *b = _new_buffer();

    if( qual_name == NULL )
    {
        _log( LOG_LEVEL_DEBUG, "qual name is null" );
        return;
    }

    buff = ( struct buffer * ) to_ptr( buffer_context, ( ref_t ) *b );

    if( buff == NULL )
    {
        _log( LOG_LEVEL_ERROR, "!!Buffer dereferenced to NULL" );
        return;
    }

    if( !__TNS_MUTEX( (&(buff->in_use)) ) )
    {
        _log( LOG_LEVEL_ERROR, "Failed to acquire buffer mutex" );
        return;
    }

    for( i = 0; i < n_quals; i++ )
    {
        data = trie_search( buff->trie, qual_name[i] );

        if( data == NULLREF )
        {
            bp = _new_buffer_pin();

            if( bp == NULLREF )
            {
                __C_MUTEX( &(buff->in_use) );
                _log( LOG_LEVEL_ERROR, "Failed to retreive new pin" );
                return;
            }

            if( !trie_insert( &(buff->trie), qual_name[i], ( ref_t ) bp ) )
            {
                __C_MUTEX( (&(buff->in_use)) );
                _log( LOG_LEVEL_DEBUG, "Trie insert failed for qual %s", qual_name[i] );
                return;
            }
        }
        else
        {
            _log(
                LOG_LEVEL_DEBUG,
                "Trie position already exists at %s (%p)",
                qual_name[i],
                to_ptr( buffer_pin_context, ( ref_t ) data )
            );
        }
    }

    __C_MUTEX( (&(buff->in_use)) );
    return;
}

void new_buffer( buffer_ref_t * b, char * qual_name, ref_t wal_data )
{
    if( *b == NULLREF )
        *b = ( buffer_ref_t ) _new_buffer();

    _log( LOG_LEVEL_DEBUG, "Got buffer %lu", ( uint64_t ) *b );
    if(
            qual_name != NULL
         && wal_data != NULLREF
         && !buffer_add( *b, qual_name, wal_data )
      )
    {
        return;
    }

    return;
}

buffer_pin_ref_t buffer_get_pin_by_name(
    buffer_ref_t b,
    char *       qual_name
)
{
    buffer_pin_ref_t bp   = NULLREF;
    ref_t            data = NULLREF;
    struct buffer *  buff = NULL;

    if( unlikely( b == NULLREF || qual_name == NULL ) )
    {
        _log( LOG_LEVEL_DEBUG, "buffer or qual is null" );
        return NULLREF;
    }

    buff = ( struct buffer * ) to_ptr( buffer_context, ( ref_t ) b );

    if( unlikely( !__TNS_MUTEX( (&(buff->in_use)) ) ) )
    {
        _log( LOG_LEVEL_DEBUG, "failed to acquire lock" );
        return NULLREF;
    }

    data = trie_search( buff->trie, qual_name );
    __C_MUTEX( (&(buff->in_use)) );

    if( likely( data != NULLREF ) )
    {
        bp = ( buffer_pin_ref_t ) data;
        //_log( LOG_LEVEL_DEBUG, "Got pin %lu", ( uint64_t ) bp );
        return bp;
    }

    _log( LOG_LEVEL_DEBUG, "No pin for qual %s", qual_name );
    return NULLREF;
}

bool buffer_add( buffer_ref_t b, char * qual_name, ref_t wal_data )
{
    buffer_pin_ref_t    bp       = NULLREF;
    struct buffer *     buff     = NULL;
    ref_t               data     = NULLREF;
    struct buffer_pin * buff_pin = NULL;

    if( b == NULLREF || qual_name == NULL || wal_data == NULLREF )
        return false;

    buff = ( struct buffer * ) to_ptr( buffer_context, ( ref_t ) b );

    if( buff == NULL )
        return false;

    if( !__TNS_MUTEX( (&(buff->in_use)) ) )
        return false;

    data = trie_search( buff->trie, qual_name );

    if( data == NULLREF )
    {
        bp = _new_buffer_pin();

        if( bp == NULLREF )
        {
            __C_MUTEX( (&(buff->in_use)) );
            return false;
        }

        if( !trie_insert( &(buff->trie), qual_name, ( ref_t ) bp ) )
        {
            __C_MUTEX( (&(buff->in_use)) );
            return false;
        }

        buff->entries++;
    }
    else
    {
        bp = ( buffer_pin_ref_t ) data;
    }

    buff_pin = ( struct buffer_pin * ) to_ptr( buffer_pin_context, ( ref_t ) bp );
    __C_MUTEX( (&(buff->in_use)) );

    if( buff_pin == NULL )
        return false;

    if( !__TNS_MUTEX( (&(buff_pin->in_use)) ) )
        return false;

    if( !slpq_push( buff_pin->slpq, wal_data ) )
    {
        __C_MUTEX( (&(buff_pin->in_use)) );
        return false;
    }

    __C_MUTEX( (&(buff_pin->in_use)) );
    return true;
}

ref_t buffer_pin_pop( buffer_pin_ref_t bp )
{
    ref_t data = NULLREF;
    struct buffer_pin * buff_pin = NULL;

    if( bp == NULLREF )
        return NULLREF;

    buff_pin = ( struct buffer_pin * ) to_ptr( buffer_pin_context, ( ref_t ) bp );

    if( buff_pin == NULL )
        return NULLREF;

    if( !__TNS_MUTEX( (&(buff_pin->in_use)) ) )
        return NULLREF;

    data = slpq_pop( buff_pin->slpq );
    __C_MUTEX( (&(buff_pin->in_use)) );
    return data;
}

bool buffer_pin_push( buffer_pin_ref_t bp, ref_t data )
{
    struct buffer_pin * buff_pin = NULL;
    if( bp == NULLREF || data == NULLREF )
        return false;

    buff_pin = ( struct buffer_pin * ) to_ptr( buffer_pin_context, ( ref_t ) bp );

    if( buff_pin == NULL )
        return false;

    if( !__TNS_MUTEX( (&(buff_pin->in_use)) ) )
        return false;

    if( !slpq_push( buff_pin->slpq, data ) )
    {
        __C_MUTEX( (&(buff_pin->in_use)) );
        return false;
    }

    __C_MUTEX( (&(buff_pin->in_use)) );
    return true;
}

bool remove_buffer_pin_by_name( buffer_ref_t b, char * qual_name )
{
    struct buffer *     buff     = NULL;
    buffer_pin_ref_t    bp       = NULLREF;
    struct buffer_pin * buff_pin = NULL;
    struct slpq * slpq = NULL;

    if( b == NULLREF || qual_name == NULL )
        return false;

    buff = ( struct buffer * ) to_ptr( buffer_context, ( ref_t ) b );

    if( buff == NULL )
        return false;

    if( !__TNS_MUTEX( (&(buff->in_use)) ) )
        return false;

    bp = ( buffer_pin_ref_t ) trie_search( buff->trie, qual_name );

    if( bp == NULLREF )
    {
        __C_MUTEX( (&(buff->in_use)) );
        return true;
    }
    else
    {
        buff_pin = ( struct buffer_pin * ) to_ptr( buffer_pin_context, ( ref_t ) bp );

        if( buff_pin == NULL )
        {
            __C_MUTEX( &(buff->in_use ) );
            return false;
        }

        if( !__TNS_MUTEX( (&(buff_pin->in_use)) ) )
        {
            __C_MUTEX( (&(buff->in_use)) );
            return false;
        }

        slpq = ( struct slpq * ) to_ptr( slpq_context, ( ref_t ) buff_pin->slpq );

        if( unlikely( slpq == NULL ) )
        {
            __C_MUTEX( &(buff_pin->in_use) );
            __C_MUTEX( &(buff->in_use) );
            return true;
        }

        if( slpq->size != 0 )
        {
            __C_MUTEX( (&(buff_pin->in_use)) );
            __C_MUTEX( (&(buff->in_use)) );
            return false;
        }

        bp = ( buffer_pin_ref_t ) trie_delete( ( trie_ref_t * ) &(buff->trie), qual_name );

        if( bp != NULLREF )
            rsfree( buffer_pin_context, ( ref_t ) bp );

        buff->entries--;
        __C_MUTEX( (&(buff->in_use)) );
        return true;
    }
}

ref_t buffer_pop( buffer_ref_t b, char * qual )
{
    buffer_pin_ref_t bp = NULLREF;
    struct buffer * buff = NULL;

    if( unlikely( b == NULLREF || qual == NULL ) )
        return NULLREF;

    buff = ( struct buffer * ) to_ptr( buffer_context, ( ref_t ) b );

    if( unlikely( buff == NULL ) )
        return NULLREF;

    if( unlikely( !__TNS_MUTEX( (&(buff->in_use)) ) ) )
        return NULLREF;

    bp = ( buffer_pin_ref_t ) trie_search( buff->trie, qual );

    if( likely( bp != NULLREF ) )
    {
        __C_MUTEX( (&(buff->in_use)) );
        return buffer_pin_pop( bp );
    }

    __C_MUTEX( (&(buff->in_use)) );
    return NULLREF;
}

static buffer_pin_ref_t _new_buffer_pin( void )
{
    struct buffer_pin * buff_pin = NULL;
    buffer_pin_ref_t bp = NULLREF;

    bp = ( buffer_pin_ref_t ) rsmalloc( buffer_pin_context, sizeof( struct buffer_pin ) );

    if( unlikely( bp == NULLREF ) )
        return NULLREF;

    buff_pin = ( struct buffer_pin * ) to_ptr( buffer_pin_context, ( ref_t ) bp );

    if( buff_pin == NULL )
        return NULLREF;

    buff_pin->in_use = false;
    buff_pin->owner  = 0;
    buff_pin->slpq   = new_slpq();

    return bp;
}

static buffer_ref_t _new_buffer( void )
{
    struct buffer * buff = NULL;
    buffer_ref_t    b    = NULLREF;

    b = ( buffer_ref_t ) rsmalloc( buffer_context, sizeof( struct buffer ) );

    if( b == NULLREF )
        return NULLREF;

    buff = ( struct buffer * ) to_ptr( buffer_context, ( ref_t ) b );
    if( buff == NULL )
        return NULLREF;

    buff->entries = 0;
    buff->trie    = NULLREF;
    return b;
}
