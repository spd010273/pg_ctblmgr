#ifndef _BUFFER_H
#define _BUFFER_H

#include <stdbool.h>
#include "util.h"
#include "barrier.h"
#include "slpq.h"
#include "trie.h"

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
