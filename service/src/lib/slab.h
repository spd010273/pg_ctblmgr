/*------------------------------------------------------------------------
 *
 * slab.h
 *     Shared memory slab allocator
 *
 * This library, together with shm.h, provides a simplified malloc/calloc/
 * realloc/free-like interface with shared memory. Shared memory segments, in
 * most implementations, come in multiples of 4KiB in size. This library
 * abstracts the management of the segments with allocation functions that
 * let the user allocate multiples of a given object in what is referred to
 * as a context. Shared memory contexts are used to differentiate between
 * other translation unit's uses of the same library. This handles use cases
 * where different translation units allocate for objects of different sizes.
 *
 * The by-default large segment size is a perfect fit for a slab allocator.
 * On the first allocation, the allocator will create two segments, one for
 * data and another (sparse) segment for what is referred to as allocset.
 * The allocset[] stores the size of each allocation. The data segment is
 * laid out as:
 *
 * +--------------------------------------------------------+ lower virtual addresses
 * |                        Canary                          |
 * +--------------------------------------------------------+
 * |                                                        |
 * |                                                        |
 * |                    Allocation Area                     |
 * |                                                        |
 * |                                                        |
 * +--------------------------------------------------------|
 * |                        Canary                          |
 * +--------------------------------------------------------+
 * |                    Free Space Map                      |
 * +--------------------------------------------------------|
 * |                        Canary                          |
 * +--------------------------------------------------------+ higher virtual address
 *
 * Here, the Free Space Map (FSM)'s bit positions coincide with
 * positions in the allocation area. The FSM bitmap, and as a
 * consequence, the allocation area, is biased towards making
 * single allocations towards the front (lower address) of the
 * array, and larger consecutive allocations towards the rear
 * of the array.
 *
 * Segment resizes leave existing allocations referentially intact
 * while only requiring the movement of the FSM to the end of the
 * resized segment.
 *
 * Similar to malloc()/realloc()/calloc(), memory handling speed is improved by
 * getting an allocation up-front, for the correct size, and working from that.
 * Calls to realloc type functions are a little hazardous as the distributed
 * references are not updated automatically in other processes. This can be
 * worked around by creating a separate slab to store references to a reference
 * that can be reallocated (akin to a double pointer). This works well but
 * wastes a little space (both memory and source files).
 *
 * This library provides additional abstraction of references that are safe to use
 * when another process performs a reallocation on the same memory allocation.
 * These references are generated with to_ref and to_ptr routines and revolve
 * around the ref_t type
 *
 * TODO:
 * - Complete compactify functionality (release space back to kernel when
 *   segments shrink)
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
#define SLAB_FSM_DEBUG 0

#if defined( SLAB_DEBUG ) && SLAB_DEBUG >= 1
 #define _SLAB_DEBUG
#endif // SLAB_DEBUG
#if defined( SLAB_FSM_DEBUG ) && SLAB_FSM_DEBUG >= 1
 #define _SLAB_FSM_DEBUG
#endif // SLAB_FSM_DEBUG

// since we're wrapping shm.c, we can control whether map_all() is called
// by a forkee upon initialization. By lazy loading - we defer loading in
// and mapping a segment until a reference to that segment is dereferenced
#define SLAB_LAZY_LOAD 1

// When the slab is initialized, we allocate for this many objects,
// This can be overridden at runtime with slab_set_count_hint()
#define SLAB_DEFAULT_ALLOCATION 32
#define ALLOCSET_DEFAULT_ALLOC_BLOCK SLAB_DEFAULT_ALLOCATION
// If the FSM is > 75% full, do exhaustive search
#define FSM_INDEX_FILL_CUTOFF ( ( double ) 0.75 )
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "barrier.h"
#include "shm.h"
#include "compiler.h"
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#if defined __GLIBC__ && defined __linux__
 #define _SLAB_HAS_GETRANDOM
 #if __GLIBC__ > 2 || __GLIBC_MINOR__ > 24
#include <sys/random.h>
 #else
  #define _SLAB_RAND_USE_SYSCALL
#include <sys/syscall.h>
#include <linux/random.h>
 #endif // GLIBC version
#else
 #undef _SLAB_HAS_GETRANDOM
#endif // GLIBC / linux
#define _SLAB_MAX_SLABS 16

#if defined( _SLAB_MAX_SLABS ) && ( _SLAB_MAX_SLABS * 2 > SHM_MAX_SEGMENTS - 1 )
#warning "MAX SLABS is dangerously close to the compiled max segments allowed"
#endif // _SLAB_MAX_SLABS check

#define _SLAB_MAX_IDENT 64
#define _SLAB_EXTRA_SANE SHM_EXTRA_SANE // Enable extra sanity checks
#define _SLAB_REALLOC_MULTIPLE 2 // unused: IFF a slab realloc occurs-  how aggressively do we overallocate?
#undef  _SLAB_CONTROL_IN_OWN_SEGMENT
#define _SLAB_CONTROL_MAGIC 0xF0042069
#define _SLAB_HEADER_MAGIC 0xDEED144A
#define _INVALID_CONTEXT ( ( uint64_t ) 0 - 1 )
#define _ZERO_FILL_BYTE 0xEA // Sports. It's in the game.
//#define _FORCE_SIGSEGV_ON_CANARY_FAILURE 1

typedef uint32_t _as_ind_t;
typedef _as_ind_t ref_t;
#define ALLOCSET_ITEM_INVALID ( _as_ind_t ) ( UINT_MAX )
#define NULLREF ( ref_t ) ALLOCSET_ITEM_INVALID

#if defined( _SLAB_MAX_SLABS ) && ( _SLAB_MAX_SLABS <= UCHAR_MAX )
typedef uint8_t header_iter;
typedef uint8_t context_t;
 #define INVALID_CONTEXT ( uint8_t ) _INVALID_CONTEXT
#elif defined( _SLAB_MAX_SLABS ) && ( _SLAB_MAX_SLABS > UCHAR_MAX ) && ( _SLAB_MAX_SLABS <= USHRT_MAX )
typedef uint16_t header_iter;
typedef uint16_t context_t;
 #define INVALID_CONTEXT ( uint16_t ) _INVALID_CONTEXT
#elif defined( _SLAB_MAX_SLABS ) && ( _SLAB_MAX_SLABS > USHRT_MAX ) && ( _SLAB_MAX_SLABS <= UINT_MAX )
typedef uint32_t header_iter;
typedef uint32_t context_t;
 #define INVALID_CONTEXT ( uint32_t ) _INVALID_CONTEXT
#else
typedef uint64_t header_iter;
typedef uint64_t context_t;
 #define INVALID CONTEXT ( uint64_t ) _INVALID_CONTEXT
#endif // iter setup

#if defined( __sys32 )
typedef uint32_t canary_t;
typedef uint32_t fsm_t;
 #define FSM_WIDTH 32
 #define FSM_SHIFT_WIDTH 8
 #define FSM_LAST_WORD_MASK 0x80
typedef uint8_t fsm_cmp_t;
#else
typedef uint64_t canary_t;
typedef uint64_t fsm_t;
 #define FSM_WIDTH 64
 #define FSM_SHIFT_WIDTH 16
 #define FSM_LAST_WORD_MASK 0x8000
typedef uint16_t fsm_cmp_t;
#endif // canary

#define FSM_RATIO ( FSM_WIDTH / FSM_SHIFT_WIDTH )

// context_t is used to identify which slab is used for a given compilation unit.
// IE the unit will initialize the slab with some string identifier, and use the
// static context returned when doing allocs/frees. It creates a little boilerplate
// for the caller but saves the callee some time when resolving stuff

/*
 *  slab layout:
 *  +---------------+     +---------------+     +---------------+
 *  |               |-->  |ALLOCS[]       |     |ALLOCSET[]     |
 *  |               |     |               |     |               |
 *  |               |     |     data      |     |               |
 *  |    control    |     |               |     |               |
 *  |    segment    |     |               |     |               |
 *  |   (headers)   |     |               |     |               |
 *  |               |     +---------------+     |               |
 *  |               |     |      FSM      |     |               |
 *  +---------------+     +---------------+     +---------------+
 *              |                                   ^
 *              +-----------------------------------+
 *
 *  The slab_control and slab_headers are stored together in the data
 *  section of the shm.c control header. Each initialized slab (slab header)
 *  constitutes the data section, referenced by allocs[] and FSM refs. These reside
 *  on the same shm.c segment. allocsets live in their own shm.c segment.
 *
 *  When segment resizes are needed, these happen in-place. Our internal
 *  headers are updated with the correct size, and when a process goes to execute
 *  get_ptr(), it can detect a mismatch between its own __segment_lut[]'s mapped_size
 *  and the header's defined size, indicating that the calling process needs to perform
 *  a remap prior to dereferencing the pointer
 *
 *  NOTES: There is an issue where when a given process performs a reallocation, the old
 *  __refs pointing to it (in processes other than the one performing the reallocation)
 *  become stale. This can be resolved by the defeloper storing their references in a slab,
 *  and passing references around in that manner ( for now ).
 */

// Hash table parameters

typedef enum {
    COMPACT_AGGRESSIVE, // Attempt to reduce segment size after every free
    COMPACT_LAZY,       // DEFAULT: 'Intelligently' reduce segment size when high FSM indexes are freed
    COMPACT_NONE        // Do not compactify segments
} compact_t;

typedef struct allocset_t {
    __ref         set;
    volatile bool locked;
    _as_ind_t     max_allocset;
    _as_ind_t     used;
    _as_ind_t     head; // Form offset-based double linked list
    _as_ind_t     tail;
} PACKED allocset_t;

typedef struct slab_header {
    uint32_t       magic;
    shm_handle     segment;
    // Initialization / boilerplate// NOTE: this is the data segment, not the segment this header is stored in
    size_t         object_size;
    size_t         count_hint;
    __ref          allocs; // This is an array of __refs that has n_allocs positions, with element 0 at this __ref's location
    uint32_t       n_allocs;
    __ref          fsm; // Free Space Map - bitmap of the free allocations slots. 0 = unallocated, 1 = allocated
    uint32_t       max_allocations;
    char           object_id[_SLAB_MAX_IDENT];
    context_t      self; // our index in the headers[]
    volatile bool  locked;
    uint64_t       i_front_fsm_bit;
    __ref          loc_c_allocstart;
    canary_t       c_allocstart;
    __ref          loc_c_fsmstart;
    canary_t       c_fsmstart;
    __ref          loc_c_fsmend;
    canary_t       c_fsmend;
    allocset_t     allocset;
//    compact_t      compact;
} PACKED slab_header;

typedef struct slab_control {
    uint32_t      magic;
    slab_header   headers[_SLAB_MAX_SLABS];
    header_iter   next_header; //next free header
    volatile bool locked;
} slab_control;

typedef struct allocset_item_t {
    __ref         issued_ref;
    size_t        size;
    uint64_t      index;
    _as_ind_t     last;
    _as_ind_t     next;
} PACKED allocset_item_t;

typedef struct ptr_cache {
    void * as_base;
    size_t as_size;
    void * allocs_base;
    size_t allocs_size;
    void * fsm_base;
    size_t fsm_size;
} ptr_cache;

// TODO - add compactification (segment size reduction) for slabs on free
// Initialization / boilerplate
extern bool slab_init( void );
extern context_t new_slab( const char *, size_t );
extern context_t new_slab_with_hint( const char *, size_t, uint64_t );
extern void slab_set_count_hint( context_t, size_t );
//extern void slab_set_compaction( context_t, compact_t );
extern void destroy_slab( context_t );
extern bool check_context( context_t );
extern bool sync_context( context_t );

// Note for users: *realloc*() functions are dangerous, and you need a method to share
// the updated __ref with other processes. This can be done by setting aside a separate slab
// for __refs (similar to a double pointer). It's a little more boilerplate but works. Sorry :(
// Extra malloc/realloc calls where # of objects requested are used
extern ref_t rscalloc_object_count( context_t, uint64_t );
extern ref_t rsmalloc_object_count( context_t, uint64_t );
extern ref_t rsrealloc_object_count( context_t, ref_t, uint64_t );

// ref_t returning malloc/calloc/realloc/free calls where bytes are specified
extern ref_t rscalloc( context_t, size_t, uint64_t );
extern ref_t rsmalloc( context_t, size_t );
extern ref_t rsrealloc( context_t, ref_t, size_t );
extern void rsfree( context_t, ref_t );

// Extra malloc / realloc calls where the number of objects are used
extern void * scalloc_object_count( context_t, uint64_t );
extern void * smalloc_object_count( context_t, uint64_t );
extern void * srealloc_object_count( context_t, void *, uint64_t );

// pointer returning malloc/calloc/realloc/free calls where bytes are specified
extern void * scalloc( context_t, size_t, uint64_t );
extern void * smalloc( context_t, size_t );
extern void * srealloc( context_t, void *, size_t );
extern void sfree( context_t, void * );

/* Utility functions for moving data between a local allocation and shm / slab managed shared memory allocation */
extern void * move_to_local( context_t, ref_t * ); // Both make changes to the 2nd argument in-place
extern ref_t move_to_shared( context_t, void **, size_t );

// Debugging / testing functions
extern void dump_context( context_t );
extern bool force_canary_check( context_t );
extern slab_header * get_header_by_context( context_t );
extern void print_fsm( slab_header * header );
extern void dump_control( void );
extern void __test_harness( void );

// Reference / dereference subsystem

extern void dump_headers( void );
extern void * to_ptr( context_t, ref_t );
extern ref_t to_ref( context_t, void * );

typedef enum {
    LL_SLAB_ERROR,
    LL_SLAB_DEBUG
} slab_ll;

#endif // _SLAB_H
