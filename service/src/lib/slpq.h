#ifndef _SLPQ_H
#define _SLPQ_H
#include <stdbool.h>
#include <stdlib.h>

#define _SLPQ_ALLOC(sz) calloc(1,sz)
#define _SLPQ_FREE(ptr) free(ptr)

/*
 *  Form a single-linked priority queue with the following structure:
 *
 *            +---- head ----------------tail--------+
 *            |                                      |
 *            v                                      v
 *          +---+        +---+        +---+        +---+
 *  pop  <- |   | -nxt-> |   | -nxt-> |   | -nxt-> |   | <- push
 * shift -> +---+        +---+        +---+        +---+ -> unshift
 *
 */

struct slpq_node
{
    void *             data;
    struct slpq_node * next;
};

struct slpq
{
    size_t             size;
    struct slpq_node * head;
    struct slpq_node * tail;
};

struct slpq * new_slpq( void );
bool slpq_push( struct slpq *, void * );
void * slpq_pop( struct slpq * );
void * slpq_unshift( struct slpq * );
bool slpq_shift( struct slpq *, void * );
void slpq_free( struct slpq * );
#endif // _SLPQ_H
