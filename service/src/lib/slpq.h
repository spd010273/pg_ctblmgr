/*--------------------------------------------------------------------------
 *
 * slpq.h
 *     single-linked priority queue
 *
 *  Creates a single-linked priority queue data structure which stores WAL
 *  changes.
 *  Priority is FIFO, though alternative push/pop/shift/unshift mechanisms
 *  are provided. Priority is determined by the user. In this specific
 *  application, growth is from the head towards the tail, with the tail
 *  being the most recently queued item, and the head being the first to be
 *  dequeued.
 *
 *  The SLPQ structure forms the handle for the whole queue, and is stored
 *  ( by reference ) in the buffer pin.
 *  Buffer pins represent changes for a specific relation, with the SLPQ
 *  being an temporally ordered list of changes to-be-processed.
 *
 *            +---- head ----------------tail--------+
 *            |                                      |
 *            v                                      v
 *          +---+        +---+        +---+        +---+
 *  pop  <- |   | -nxt-> |   | -nxt-> |   | -nxt-> |   | <- push
 * shift -> +---+        +---+        +---+        +---+ -> unshift
 *
 * Copyright (c) 2019-2022, MerchLogix, Inc.
 *
 * IDENTIFICATION
 *          service/src/lib/slpq.h
 *--------------------------------------------------------------------------
 */

#ifndef _SLPQ_H
#define _SLPQ_H

#include <stdbool.h>
#include <stdlib.h>
#include "slab.h"
#include "util.h"

#define SLPQ_CONTEXT_NAME "SLPQ"
#define SLPQ_NODE_CONTEXT_NAME "SLPQ_NODE"

typedef ref_t slpq_ref_t;
typedef ref_t slpq_node_ref_t;

struct slpq_node
{
    ref_t           data;
    slpq_node_ref_t next;
};

struct slpq
{
    size_t           size;
    slpq_node_ref_t  head;
    slpq_node_ref_t  tail;
    volatile bool    locked;
};

extern void set_slpq_context( context_t );
extern void set_slpq_node_context( context_t );

extern slpq_ref_t new_slpq( void );
extern bool slpq_push( slpq_ref_t, ref_t );
extern ref_t slpq_pop( slpq_ref_t );
extern ref_t slpq_unshift( slpq_ref_t );
extern bool slpq_shift( slpq_ref_t, ref_t );
extern void slpq_free( slpq_ref_t );
#endif // _SLPQ_H
