#ifndef _BUFFER_H
#define _BUFFER_H

#include <stdbool.h>

#if __STDC_VERSION__ >= 201112L
# ifdef __STDC_NO_ATOMICS__
#  define __BUF_NO_ATOMICS__
# else
#  include <stdatomic.h>
#  define __TNS_MUTEX(val) atomic_test_and_set(val)
#  define __C_MUTEX(val) atomic_flag_clear(val)
# endif // __STDC_NO_ATOMICS__
#else
#define __BUF_NO_ATOMICS__
#endif // __STDC_VERSION__

#include "slpq.h"
#include "trie.h"
#include "util.h"

#define _BUFFER_ALLOC(sz) create_shared_memory(sz)
#define _BUFFER_FREE(ptr,sz) free_shared_memory(ptr,sz)

struct buffer
{
    struct trie * trie;
    size_t        entries;
#ifdef __BUF_NO_ATOMICS__
    bool          in_use;
#else
    atomic_bool   in_use;
#endif // __BUF_NO_ATOMICS__
};

struct buffer_pin
{
    struct slpq * slpq;
    pid_t         owner;
#ifdef __BUF_NO_ATOMICS__
    bool          in_use;
#else
    atomic_bool   in_use;
#endif // __BUF_NO_ATOMICS__
};

void buffer_populate_trie( struct buffer **, char **, unsigned int );
void new_buffer( struct buffer **, char *, void * );
struct buffer_pin * buffer_get_pin_by_name( struct buffer *, char * );
bool buffer_add( struct buffer *, char *, void * );
void * buffer_pin_pop( struct buffer_pin * );
bool buffer_pin_push( struct buffer_pin *, void * );
bool remove_buffer_pin_by_name( struct buffer *, char * );
void * buffer_pop( struct buffer *, char * );
#endif // _BUFFER_H
