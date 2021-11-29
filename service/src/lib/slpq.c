#include "slpq.h"
static void _dump_node( struct slpq_node * );
static void _dump_slpq( struct slpq * );

struct slpq * new_slpq( void )
{
    struct slpq * new = NULL;

    new = ( struct slpq * ) _SLPQ_ALLOC( sizeof( struct slpq ) );

    if( new == NULL )
    {
        return NULL;
    }

    new->size = 0;
    new->head = NULL;
    new->tail = NULL;
    return new;
}

bool slpq_push( struct slpq * head, void * data )
{
    struct slpq_node * node = NULL;

    if( head == NULL || data == NULL )
        return false;

    node = ( struct slpq_node * ) _SLPQ_ALLOC( sizeof( struct slpq_node ) );

    if( node == NULL )
        return false;

    node->data = data;
    node->next = NULL;

    if( head->tail == NULL && head->head == NULL )
    {
        head->tail = node;
        head->head = node;
        head->size = 1;
        return true;
    }

    head->tail->next = node;
    head->tail       = node;
    head->size++;
    return true;
}

void * slpq_pop( struct slpq * head )
{
    void *             data = NULL;
    struct slpq_node * temp = NULL;

    _dump_slpq( head );
    if( head == NULL )
        return NULL;

    temp = head->head;

    if( temp == NULL )
        return NULL;

    data = temp->data;
    head->head = temp->next;

    if( head->size == 1 || head->head == NULL )
    {
        head->tail = NULL;
        head->head = NULL;
    }

    _SLPQ_FREE( temp, sizeof( struct slpq_node ) );
    head->size--;
    return data;
}

void * slpq_unshift( struct slpq * head )
{
    void *             data = NULL;
    struct slpq_node * temp = NULL;

    if( head == NULL )
        return NULL;

    if( head->tail == NULL || head->head == NULL )
        return NULL;

    temp = head->head;

    while( temp != NULL && temp->next != head->tail )
    {
        temp = temp->next;
    }

    // Lazy assert :|
    if( temp->next != head->tail )
        return NULL;

    head->tail = temp;
    temp       = temp->next;
    data       = temp->data;
    _SLPQ_FREE( temp, sizeof( struct slpq_node ) );
    head->size--;
    return data;
}

bool slpq_shift( struct slpq * head, void * data )
{
    struct slpq_node * node = NULL;

    if( head == NULL || data == NULL )
        return false;

    node = ( struct slpq_node * ) _SLPQ_ALLOC( sizeof( struct slpq_node ) );

    if( node == NULL )
        return false;

    node->data = data;
    node->next = NULL;

    if( head->head == NULL && head->tail == NULL )
    {
        head->tail = node;
        head->head = node;
        head->size = 1;
        return true;
    }

    node->next = head->head;
    head->head = node;
    head->size++;
    return true;
}

void slpq_free( struct slpq * head )
{
    struct slpq_node * node = NULL;
    struct slpq_node * last = NULL;

    if( head == NULL )
       return;

    node = head->head;

    while( node != NULL )
    {
        last = node;
        node = node->next;
        _SLPQ_FREE( last, sizeof( struct slpq_node ) );
    }

    head->head = NULL;
    head->size = 0;
    head->tail = NULL;
    _SLPQ_FREE( head, sizeof( struct slpq_node ) );

    return;
}

static void _dump_node( struct slpq_node * n )
{
    if( n == NULL )
    {
        _log( LOG_LEVEL_DEBUG, "null node" );
        return;
    }

    _log( LOG_LEVEL_DEBUG, "Node %p, data: %p, next %p", n, n->data, n->next );
    return;
}

static void _dump_slpq( struct slpq * head )
{
    struct slpq_node * n = NULL;

    if( head == NULL )
    {
        _log( LOG_LEVEL_DEBUG, "null slpq" );
        return;
    }

    _log(
        LOG_LEVEL_DEBUG,
        "SLPQ %p size %u, H: %p, T: %p",
        head,
        ( unsigned int ) head->size,
        head->head,
        head->tail
    );

    n = head->head;
    while( n != NULL )
    {
        _dump_node( n );
        n = n->next;
    }
}
