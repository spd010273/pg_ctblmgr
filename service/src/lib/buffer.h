#ifndef _BUFFER_H
#define _BUFFER_H

#include "slpq.h"
#include "trie.h"

#define _BUFFER_ALLOC(sz) calloc(1,sz)
#define _BUFFER_FREE(ptr) free(ptr)

struct buffer
{
    struct trie * trie;
    size_t        entries;
    bool          in_use;
};

struct buffer_pin
{
    struct slpq * slpq;
    pid_t         owner;
    bool          in_use;
};

void new_buffer( struct buffer **, char *, void * );
struct buffer_pin * buffer_get_pin_by_name( struct buffer *, char * );
bool buffer_add( struct buffer *, char *, void * );
void * buffer_pin_pop( struct buffer_pin * );
bool buffer_pin_push( struct buffer_pin *, void * );
bool remove_buffer_pin_by_name( struct buffer *, char * );
#endif // _BUFFER_H
