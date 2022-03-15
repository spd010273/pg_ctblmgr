/*--------------------------------------------------------------------------
 *
 * trie.h
 *     Trie data structure
 *
 * Provides a prefix triee for fast key search of relation names.
 * This stores the fully qualified relation name, schema included,
 * so adding the table 'public.tb_foobarbaz' would result in the creation
 * of 19 trie nodes. The leaf node will have the is_leaf bit set, and the
 * data segment will contain a reference to a buffer pin, containing the
 * single-linked priority queue of WAL changes.
 *
 * Details about the trie structure are in buffer.h's header
 *
 * Copyright (c) 2019-2022, MerchLogix, Inc.
 *
 * IDENTIFICATION
 *          service/src/lib/trie.h
 *--------------------------------------------------------------------------
 */

#ifndef _TRIE_H
#define _TRIE_H
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "slab.h"

#define TRIE_CONTEXT_NAME "TRIE"

#define TRIE_SIZE 96

typedef ref_t trie_ref_t;

struct trie
{
    ref_t data;
    trie_ref_t character[TRIE_SIZE];
    bool  is_leaf;
};

extern void set_trie_context( context_t );
extern bool trie_insert( trie_ref_t *, char *, ref_t ); // 1st arg is pass by ref
extern ref_t trie_search( trie_ref_t, char * );
extern ref_t trie_delete( trie_ref_t *, char * );
extern void trie_free( trie_ref_t * );
#endif // _TRIE_H
