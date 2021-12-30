/*------------------------------------------------------------------------
 *
 * slab.h
 *     Shared memory slab allocator
 *
 * Copyright (c) 2021, MerchLogix Inc.
 *
 * IDENTIFICATION
 *        service/src/lib/slab.h
 *
 *------------------------------------------------------------------------
 */
#ifndef _SLAB_H
#define _SLAB_H

#define SLAB_DEBUG 1
#define SLAB_LAZY_LOAD 1
#define SLAB_DEFAULT_ALLOCATION 32
#include <stdint.h>
#include <stdbool.h>
#include "barrier.h"
#include "shm.h"

#define _SHALLOC_MAX_SLABS 16
#define _SHALLOC_MAX_IDENT 64

#define _SHALLOC_MAX_ALLOCS_PER_SLAB 2048
#define _SHALLOC_CONTROL_MAGIC 0xF0042069
#define _SHALLOC_HEADER_MAGIC 0xDEED144A
#define _INVALID_CONTEXT 0xB16F00FE
#if defined( _SHALLOC_MAX_SLABS ) && ( _SHALLOC_MAX_SLABS <= UCHAR_MAX )
typedef uint8_t header_iter;
typedef uint8_t context_t;
 #define INVALID_CONTEXT ( uint8_t ) _INVALID_CONTEXT
#elif defined( _SHALLOC_MAX_SLABS ) && ( _SHALLOC_MAX_SLABS > UCHAR_MAX ) && ( _SHALLOC_MAX_SLABS <= USHRT_MAX )
typedef uint16_t header_iter;
typedef uint16_t context_t;
 #define INVALID_CONTEXT ( uint16_t ) _INVALID_CONTEXT
#elif defined( _SHALLOC_MAX_SLABS ) && ( _SHALLOC_MAX_SLABS > USHRT_MAX ) && ( _SHALLOC_MAX_SLABS <= UINT_MAX )
typedef uint32_t header_iter;
typedef uint32_t context_t;
 #define INVALID_CONTEXT ( uint32_t ) _INVALID_CONTEXT
#else
typedef uint64_t header_iter;
typedef uint64_t context_t;
 #define INVALID CONTEXT ( uint64_t ) _INVALID_CONTEXT
#endif // iter setup

// context_t is used to identify which slab is used for a given compilation unit.
// IE the unit will initialize the slab with some string identifier, and use the
// static context returned when doing allocs/frees. It creates a little boilerplate
// for the caller but saves the callee some time when resolving stuff

typedef struct shalloc_header {
    uint64_t       magic;
    shm_handle     segment;
    size_t         object_size;
    void **        allocs; // may need to be converted to __ref
    uint64_t       n_allocs;
    void **        freelist; // may need to be converted to __ref
    uint64_t       n_freelist;
    char           object_id[_SHALLOC_MAX_IDENT];
    context_t      self;
    bool           locked;
} shalloc_header;

typedef struct shalloc_control {
    uint64_t       magic;
    shalloc_header headers[_SHALLOC_MAX_SLABS];
    header_iter    next_header; //next free header
    bool           locked; 
} shalloc_control;

extern bool slab_init( void );
extern context_t new_slab( const char *, size_t );
extern __ref scalloc( context_t, size_t, uint64_t );
extern __ref smalloc( context_t, size_t );
extern void sfree( context_t, __ref );
#endif // _SLAB_H
