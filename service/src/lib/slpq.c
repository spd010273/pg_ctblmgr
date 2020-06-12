#include "slpq.h"

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
    {
        return false;
    }

    node = ( struct slpq_node * ) _SLPQ_ALLOC( sizeof( struct slpq_node ) );

    if( node == NULL )
    {
        return false;
    }

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

    if( head == NULL )
    {
        return NULL;
    }

    temp = head->head;

    if( temp == NULL )
    {
        return NULL;
    }

    data = temp->data;
    head->head = temp->next;
    _SLPQ_FREE( temp );
    head->size--;
    return data;
}

void * slpq_unshift( struct slpq * head )
{
    void *             data = NULL;
    struct slpq_node * temp = NULL;

    if( head == NULL )
    {
        return NULL;
    }

    if( head->tail == NULL || head->head == NULL )
    {
        return NULL;
    }

    temp = head->head;

    while( temp != NULL && temp->next != head->tail )
    {
        temp = temp->next;
    }

    // Lazy assert :|
    if( temp->next != head->tail )
    {
        return NULL;
    }

    head->tail = temp;
    temp       = temp->next;
    data       = temp->data;
    _SLPQ_FREE( temp );
    head->size--;
    return data;
}

bool slpq_shift( struct slpq * head, void * data )
{
    struct slpq_node * node = NULL;

    if( head == NULL || data == NULL )
    {
        return false;
    }

    node = ( struct slpq_node * ) _SLPQ_ALLOC( sizeof( struct slpq_node * ) );

    if( node == NULL )
    {
        return false;
    }

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
    {
        return;
    }

    node = head->head;

    while( node != NULL )
    {
        last = node;
        node = node->next;
        _SLPQ_FREE( last );
    }

    head->head = NULL;
    head->size = 0;
    head->tail = NULL;
    _SLPQ_FREE( head );

    return;
}
