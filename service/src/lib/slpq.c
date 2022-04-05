/*--------------------------------------------------------------------------
 *
 * slpq.c
 *     single-linked priority queue
 *
 * Creates a singlei-linked priority queue. Integrates with buffer.c to provide
 * a complete data structure for sorting WAL changes and feeding them into
 * subprocesses for change extraction.
 *
 * Copyright (c) 2019-2022, MerchLogix, Inc.
 *
 * IDENTIFICATION
 *          service/src/lib/slpq.c
 *--------------------------------------------------------------------------
 */

#include "slpq.h"
#ifdef SLPQ_DEBUG
static void _dump_node( slpq_node_ref_t );
static void _dump_slpq( slpq_ref_t );
#endif // SLPQ_DEBUG
static context_t slpq_context;
static context_t slpq_node_context;

void set_slpq_context( context_t ctx )
{
    if( unlikely( !check_context( ctx ) ) )
        return;
    if( unlikely( ctx == slpq_node_context ) )
        return;
    slpq_context = ctx;
    return;
}

void set_slpq_node_context( context_t ctx )
{
    if( unlikely( !check_context( ctx ) ) )
        return;
    if( unlikely( ctx = slpq_context ) )
        return;
    slpq_node_context = ctx;
    return;
}

slpq_ref_t new_slpq( void )
{
    slpq_ref_t n = NULLREF;
    struct slpq * new = NULL;

    n = ( slpq_ref_t ) rsmalloc( slpq_context, sizeof( struct slpq ) );

    if( unlikely( n == NULLREF ) )
        return NULLREF;

    new = ( struct slpq * ) to_ptr( slpq_context, ( ref_t ) n );

    if( unlikely( new == NULL ) )
        return NULLREF;

    new->size = 0;
    new->head = NULLREF;
    new->tail = NULLREF;
    return n;
}

bool slpq_push( slpq_ref_t head, ref_t data )
{
    struct slpq *      slpq_head = NULL;
    struct slpq_node * node      = NULL;
    slpq_node_ref_t    n         = NULLREF;

    if( unlikely( ( head == NULLREF ) || ( data == NULLREF ) ) )
        return false;

    n = ( slpq_node_ref_t ) rsmalloc( slpq_node_context, sizeof( struct slpq_node ) );

    if( unlikely( n == NULLREF ) )
        return false;

    node = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) n );
    slpq_head = ( struct slpq * ) to_ptr( slpq_context, ( ref_t ) head );

    if( unlikely( node == NULL ) )
        return false;

    if( unlikely( slpq_head == NULL ) )
        return false;

    node->data = data;
    node->next = NULLREF;

    if( slpq_head->tail == NULLREF && slpq_head->head == NULLREF )
    {
        slpq_head->tail = n;
        slpq_head->head = n;
        slpq_head->size = 1;
        return true;
    }

    node = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) slpq_head->tail );

    if( unlikely( node == NULL ) )
        return false;

    node->next = n;
    slpq_head->tail = n;
    slpq_head->size++;
    return true;
}

ref_t slpq_pop( slpq_ref_t head )
{
    ref_t              data      = NULLREF;
    slpq_node_ref_t    t         = NULLREF;
    struct slpq_node * temp      = NULL;
    struct slpq *      slpq_head = NULL;

    #ifdef SLPQ_DEBUG
    _dump_slpq( head );
    #endif // SLPQ_DEBUG
    if( unlikely( head == NULLREF ) )
        return NULLREF;

    slpq_head = ( struct slpq * ) to_ptr( slpq_context, ( ref_t ) head );

    if( unlikely( slpq_head == NULL ) )
        return NULLREF;

    t = slpq_head->head;

    temp = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) t );

    if( unlikely( temp == NULL ) )
        return NULLREF;

    data            = temp->data;
    slpq_head->head = temp->next;

    if( slpq_head->size == 1 || slpq_head->head == NULLREF )
    {
        slpq_head->tail = NULLREF;
        slpq_head->head = NULLREF;
    }

    rsfree( slpq_node_context, t );
    slpq_head->size--;
    return data;
}

ref_t slpq_unshift( slpq_ref_t head )
{
    ref_t              data      = NULLREF;
    struct slpq *      slpq_head = NULL;
    slpq_node_ref_t    t         = NULLREF;
    struct slpq_node * temp      = NULL;

    if( unlikely( head == NULLREF ) )
        return NULLREF;

    slpq_head = ( struct slpq * ) to_ptr( slpq_context, ( ref_t ) head );
    if(
        unlikely(
            ( slpq_head == NULL )
         || ( slpq_head->tail == NULLREF )
         || ( slpq_head->head == NULLREF )
        )
      )
        return NULLREF;

    t = slpq_head->head;
    temp = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) t );
    if( unlikely( temp == NULL ) )
        return NULLREF;

    // Iterate through the linked list because we need node n-1
    while( temp != NULL && temp->next != slpq_head->tail )
    {
        t = temp->next;
        temp = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) t );
    }

    if( temp == NULL )
        return NULLREF;
    // Lazy assert :|
    if( temp->next != slpq_head->tail )
    {
        // Ruh roh, we aren't at the tail?!?
        return NULLREF;
    }

    // t / temp is node n-1 where node n is the tail,
    // pull data from the tail and set t as the new tail
    slpq_head->tail = t;
    t               = temp->next;
    temp = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) t );

    if( unlikely( temp == NULL ) )
        return NULLREF;

    data            = temp->data;
    rsfree( slpq_node_context, t );
    slpq_head->size--;
    return data;
}

bool slpq_shift( slpq_ref_t head, ref_t data )
{
    struct slpq *      slpq_head = NULL;
    slpq_node_ref_t    n         = NULLREF;
    struct slpq_node * node      = NULL;

    if( unlikely( head == NULLREF || data == NULLREF ) )
        return false;

    slpq_head = ( struct slpq * ) to_ptr( slpq_context, ( ref_t ) head );

    if( unlikely( slpq_head == NULL ) )
        return false;

    n = ( slpq_node_ref_t ) rsmalloc( slpq_node_context, sizeof( struct slpq_node ) );

    if( unlikely( n == ( slpq_node_ref_t ) NULLREF ) )
        return false;

    node = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) n );

    if( unlikely( node == NULL ) )
        return false;

    node->data = data;
    node->next = NULLREF;

    if( slpq_head->head == NULLREF && slpq_head->tail == NULLREF )
    {
        slpq_head->tail = n;
        slpq_head->head = n;
        slpq_head->size = 1;
        return true;
    }

    node->next = slpq_head->head;
    slpq_head->head = n;
    slpq_head->size++;
    return true;
}

void slpq_free( slpq_ref_t head )
{
    struct slpq * slpq_head = NULL;
    struct slpq_node * node = NULL;
    slpq_node_ref_t n = NULLREF;
    slpq_node_ref_t l = NULLREF;

    if( unlikely( head == NULLREF ) )
       return;

    slpq_head = ( struct slpq * ) to_ptr( slpq_context, ( ref_t ) head );

    if( unlikely( slpq_head == NULL ) )
        return;

    n = slpq_head->head;

    if( unlikely( n == NULLREF ) )
        return;

    node = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) n );

    while( node != NULL )
    {
        l = n;
        n = node->next;
        rsfree( slpq_node_context, l );
        node = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) n );
    }

    slpq_head->head = NULLREF;
    slpq_head->size = 0;
    slpq_head->tail = NULLREF;
    rsfree( slpq_context, head );
    return;
}

#ifdef SLPQ_DEBUG
static void _dump_node( slpq_node_ref_t n )
{
    struct slpq_node * node = NULL;

    if( unlikely( n == NULLREF ) )
    {
        _log( LOG_LEVEL_DEBUG, "null node" );
        return;
    }

    node = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) n );

    if( unlikely( node == NULL ) )
    {
        _log( LOG_LEVEL_DEBUG, "null node" );
        return;
    }

    _log(
        LOG_LEVEL_DEBUG,
        "Node %p, data_ref %lu, next %p",
        node,
        ( uint64_t ) node->data, // need the right context to deref the ptr
        to_ptr( slpq_node_context, ( ref_t ) node->next )
    );
    return;
}

static void _dump_slpq( slpq_ref_t head )
{
    struct slpq *      slpq_head = NULL;
    struct slpq_node * n         = NULL;
    slpq_node_ref_t    node      = NULLREF;

    if( unlikely( head == NULLREF ) )
    {
        _log( LOG_LEVEL_DEBUG, "null slpq" );
        return;
    }

    slpq_head = ( struct slpq * ) to_ptr( slpq_context, ( ref_t ) head );

    if( unlikely( slpq_head == NULL ) )
    {
        _log( LOG_LEVEL_DEBUG, "null slpq" );
        return;
    }

    node = slpq_head->head;
    n = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) node );
    _log(
        LOG_LEVEL_DEBUG,
        "SLPQ %p size %u, H: %p, T: %p",
        slpq_head,
        ( uint32_t ) slpq_head->size,
        n,
        to_ptr( slpq_node_context, ( ref_t ) slpq_head->tail )
    );

    while( n != NULL )
    {
        _dump_node( node );
        node = n->next;
        n = ( struct slpq_node * ) to_ptr( slpq_node_context, ( ref_t ) node );
    }

    return;
}
#endif // SLPQ_DEBUG
