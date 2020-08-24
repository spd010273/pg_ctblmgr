#ifndef _SLPQ_H
#define _SLPQ_H
#include <stdbool.h>
#include <stdlib.h>
#include "util.h"
#define _SLPQ_ALLOC(sz) create_shared_memory(sz)
#define _SLPQ_FREE(ptr,sz) free_shared_memory(ptr,sz)

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

extern struct slpq * new_slpq( void );
extern bool slpq_push( struct slpq *, void * );
extern void * slpq_pop( struct slpq * );
extern void * slpq_unshift( struct slpq * );
extern bool slpq_shift( struct slpq *, void * );
extern void slpq_free( struct slpq * );
#endif // _SLPQ_H
