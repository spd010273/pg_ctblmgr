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

/*
 * This library maps on top of the shm.c's segments to form a basic
 * slab allocator.
 *
 * Segments will be laid out as:
 * +--------------------------------------------------------+ lower virtual addresses
 * |                   SHM segment header                   |
 * +--------------------------------------------------------+
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
 */

#define SLAB_DEBUG 1

// since we're wrapping shm.c, we can control whether map_all() is called
// by a forkee upon initialization. By lazy loading - we defer loading in
// and mapping a segment until a reference to that segment is dereferenced
#define SLAB_LAZY_LOAD 1

// When the slab is initialized, we allocate for this many objects,
// This can be overridden at runtime with slab_set_count_hint()
#define SLAB_DEFAULT_ALLOCATION 32

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "barrier.h"
#include "shm.h"

#define _SHALLOC_MAX_SLABS 16
#define _SHALLOC_MAX_IDENT 64
#define _SHALLOC_EXTRA_SANE 1 // Enable extra sanity checks
#define _SHALLOC_REALLOC_MULTIPLE 2 // IFF a slab realloc occurs-  how aggressively do we overallocate?

#define _SHALLOC_MAX_ALLOCS_PER_SLAB 2048
#define _SHALLOC_CONTROL_MAGIC 0xF0042069
#define _SHALLOC_HEADER_MAGIC 0xDEED144A
#define _INVALID_CONTEXT 0xB16F00FE
#define _ZERO_FILL_BYTE 0xEA // Sports. It's in the game.
#define _FORCE_SIGSEGV_ON_CANARY_FAILURE 1

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

//typedef uint8_t fsm_t;
//#define FSM_WIDTH 8
// context_t is used to identify which slab is used for a given compilation unit.
// IE the unit will initialize the slab with some string identifier, and use the
// static context returned when doing allocs/frees. It creates a little boilerplate
// for the caller but saves the callee some time when resolving stuff

// Note - these are both stored together in the control segment for the slab allocator
// XXX: Need to move this struct to the control_segment for shm.c to avoid wasting a page
typedef struct shalloc_header {
    uint32_t       magic;
    shm_handle     segment; // NOTE: this is the data segment, not the segment this header is stored in
    size_t         object_size;
    size_t         count_hint;
    __ref          allocs; // This is an array of __refs that has n_allocs positions, with element 0 at this __ref's location
    uint32_t       n_allocs;
    __ref          fsm; // Free Space Map - bitmap of the free allocations slots. 0 = unallocated, 1 = allocated
    uint32_t       max_allocations;
    char           object_id[_SHALLOC_MAX_IDENT];
    context_t      self; // our index in the headers[]
    bool           locked;
    uint64_t       i_front_fsm_bit;
    uint64_t       i_rear_fsm_word;
    __ref          loc_c_allocstart;
    canary_t       c_allocstart;
    __ref          loc_c_fsmstart;
    canary_t       c_fsmstart;
    __ref          loc_c_fsmend;
    canary_t       c_fsmend;
    uint16_t       allocset[_SHALLOC_MAX_ALLOCS_PER_SLAB]; // Stores allocation sizes by index - TODO: Maybe make this variable length in its own segment??
} __attribute__((packed)) shalloc_header;

typedef struct shalloc_control {
    uint32_t       magic;
    shalloc_header headers[_SHALLOC_MAX_SLABS];
    header_iter    next_header; //next free header
    bool           locked;
} shalloc_control;

extern bool slab_init( void );
extern context_t new_slab( const char *, size_t );
extern void slab_set_count_hint( context_t, size_t );
extern __ref scalloc( context_t, size_t, uint64_t );
extern __ref smalloc( context_t, size_t );
extern __ref srealloc( context_t, __ref, size_t );
extern void sfree( context_t, __ref );
extern void * move_to_local( context_t, __ref * ); // Both make changes to the 2nd argument in-place
extern __ref move_to_shared( context_t, void **, size_t );

// Debugging / testing functions
extern bool force_canary_check( context_t );
extern shalloc_header * get_header_by_context( context_t );
#endif // _SLAB_H
