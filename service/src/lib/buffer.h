/*--------------------------------------------------------------------------
 *
 * buffer.h
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
 * Structure (simplified visual layout) is as follows
 *
 * +---------------------------------------------------------------------+
 * | BUFFER                                                              |
 * |                                                                     |
 * |                              trie                                   |
 * |                               |                                     |
 * |                       ( schema sub trie )                           |
 * |                               |                                     |
 * |                              't'                                    |
 * |                               |                                     |
 * |                              'b'                                    |
 * |                               |                                     |
 * |                              '_'                                    |
 * |                             /   \                                   |
 * |                            /     \                                  |
 * |                          'f'     'b'                                |
 * |                           |       |                                 |
 * |                          'o'     'a'                                |
 * |                           |      / \                                |
 * |                          'o'  'r'   'z'                             |
 * |                           |    |     |                              |
 * |                           v    v     v                              |
 * |                          pin  pin   pin -> SLPQ -> WAL -> WAL       |
 * |                           |    |                                    |
 * |                           |    +--> SLPQ -> WAL -> WAL -> WAL       |
 * |                           |                                         |
 * |                           +--> SLPQ -> WAL -> WAL -> WAL -> WAL     |
 * |                                                                     |
 * +---------------------------------------------------------------------+
 *
 * Copyright (c) 2019-2022, MerchLogix, Inc.
 *
 * IDENTIFICATION
 *          service/src/lib/buffer.h
 *--------------------------------------------------------------------------
 */
#ifndef _BUFFER_H
#define _BUFFER_H

#include <stdbool.h>
#include "barrier.h"
#include "slab.h"
#include "slpq.h"
#include "trie.h"
#include "util.h"

#define BUFFER_PIN_CONTEXT_NAME "BUFFER_PIN"
#define BUFFER_CONTEXT_NAME "BUFFER"

typedef ref_t buffer_pin_ref_t;
typedef ref_t buffer_ref_t;
typedef ref_t slpq_ref_t;

struct buffer
{
    trie_ref_t    trie;
    size_t        entries;
#ifdef __BUF_NO_ATOMICS__
    volatile bool in_use;
#else
    atomic_bool   in_use;
#endif // __BUF_NO_ATOMICS__
};

struct buffer_pin
{
    slpq_ref_t    slpq;
    pid_t         owner;
#ifdef __BUF_NO_ATOMICS__
    volatile bool in_use;
#else
    atomic_bool   in_use;
#endif // __BUF_NO_ATOMICS__
};

extern bool initialize_contexts( void );

extern void buffer_populate_trie( buffer_ref_t *, char **, unsigned int );
extern void new_buffer( buffer_ref_t *, char *, ref_t );
extern buffer_pin_ref_t buffer_get_pin_by_name( buffer_ref_t, char * );
extern bool buffer_add( buffer_ref_t, char *, ref_t );
extern ref_t buffer_pin_pop( buffer_pin_ref_t );
extern bool buffer_pin_push( buffer_pin_ref_t, ref_t );
extern bool remove_buffer_pin_by_name( buffer_pin_ref_t, char * );
extern ref_t buffer_pop( buffer_ref_t, char * );
#endif // _BUFFER_H
