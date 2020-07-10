#ifndef _TRIE_H
#define _TRIE_H
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define _TRIE_ALLOC(sz) create_shared_memory(sz)
#define _TRIE_FREE(ptr,sz) free_shared_memory(ptr,sz)
#define TRIE_SIZE 96

struct trie
{
    void *        data;
    struct trie * character[TRIE_SIZE];
    bool          is_leaf;
};

bool trie_insert( struct trie **, char *, void * );
void * trie_search( struct trie *, char * );
void * trie_delete( struct trie **, char * );
void trie_free( struct trie ** );
#endif // _TRIE_H
