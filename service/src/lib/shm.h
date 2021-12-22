/*------------------------------------------------------------------------
 *
 * shm.h
 *     Shared Memory function prototypes
 *     This includes an allocator and uses the underlying APIs:
 *     - System V (shm.h / ipc.h)
 *     - POSIX (mman.h)
 *     - mmap
 *     The goal of this library is to present a simplified (or as close to)
 *     malloc/calloc/free interface for shared memory allocation
 *
 * Copyright (c) 2021, MerchLogix Inc.
 *
 * IDENTIFICATION
 *        service/src/lib/shm.h
 *
 *------------------------------------------------------------------------
 */
#ifndef _SHM_H
#define _SHM_H

//#define __TESTING__ // code coverage
#define SHM_DEBUG 1

#ifdef __TESTING__
 #include <unistd.h>
 #define SHM_USE_SYSV
 #define SHM_USE_POSIX
 #define SHM_USE_MMAP
#else
 #ifdef __unix__
  #include <unistd.h>
  #if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
   #define SHM_USE_POSIX
  #else
   #define SHM_USE_MMAP
  #endif // _POSIX_C_SOURCE
 #else
  #define SHM_USE_SYSV
 #endif // __unix__
#endif // __TESTING__

#ifdef SHM_USE_POSIX
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <sys/mman.h>
#endif // SHM_USE_POSIX
#ifdef SHM_USE_SYSV
 #include <sys/ipc.h>
 #include <sys/shm.h>
 #include <sys/types.h>
 #ifdef SHM_SHARE_MMU
  #define SYSV_SHM_FLAGS SHM_SHARE_MMU
 #else
  #define SYSV_SHM_FLAGS 0
 #endif // SHM_SHARE_MMU
#endif // SHM_USE_SYSV

#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <stdarg.h>
#include <sys/time.h>

#ifdef _POSIX_C_SOURCE
 #include <signal.h>
#endif // _POSIX_C_SOURCE

#include "barrier.h"

#define SHM_ENABLE_RUNTIME_SANITY_CHECK 1
#define SHM_ENABLE_STRUCT_PACKING 1
#define DEFAULT_PAGE_SIZE 8192 // bytes

/* likely/unlikely are branch hints, we may be using an older Cxx without atomic primitives or branch hinting */
#ifdef __builtin_expect
 #ifndef likely
  #define likely(x) __builtin_expect( !!(x), 1 )
 #endif // likely
 #ifndef unlikely
  #define unlikely(x) __builtin_expect( !!(x), 0 )
 #endif // unlikely
#else
 #define likely(x) ( !!(x) )
 #define unlikely(x) ( !!(x) )
#endif // __builtin_expect

/*
 * HEAP directionality logic
 *  We assume the heap grows in the opposite direction from the stack.
 *  This is not a standard, but a convention. Stack direction is more standardized
 *  and is determined by the architecture and ABI, as stack pointer manipulation is
 *  implemented in hardware. As an example, here's a schematic layout for x86 / x86_64
 *  process mapping in the Linux ABI:
 *
 *   +-------------------------------------------------------------+ 0xFFFFFFFF[FFFFFFFF]
 *   |                                                             |       .
 *   |                                                             |       .
 *   +-------------------------------------------------------------+ High Virtual Address
 *   |                                                             |
 *   |                       ARGV[] / environ                      |
 *   |                                                             |
 *   +-------------------------------------------------------------+
 *   |                                                             |
 *   |                            Stack                            |
 *   |                                                             |
 *   +-------------------------------------------------------------+
 *   |                              |                              |
 *   |                              |                              |
 *   |                              v                              |
 *   |                                                             |
 *   |                      Unallocated space                      |
 *   |                                                             |
 *   |                              ^                              |
 *   |                              |                              |
 *   |                              |                              |
 *   +-------------------------------------------------------------+
 *   |                                                             |
 *   |                            Heap                             |
 *   |                                                             |
 *   +-------------------------------------------------------------+
 *   |                                                             |
 *   |                   .bss (uninitialized data)                 |
 *   |                                                             |
 *   +-------------------------------------------------------------+
 *   |                                                             |
 *   |                   .data (initialized data)                  |
 *   |                                                             |
 *   +-------------------------------------------------------------+
 *   |                                                             |
 *   |                 .text (program code segments)               |
 *   |                                                             |
 *   +-------------------------------------------------------------+ Low Virtual Address
 *   |                                                             |      .
 *   |                                                             |      .
 *   +-------------------------------------------------------------+ 0x00000000[00000000]
 *
 * While it's quite rare, some archs have the stack growing upward,
 * resulting in a different ABI as well. We'll assume (as stated previously)
 * that the heap grows in the opposite direction to minimize memory fragmentation,
 * but this may be a bad assumption
 */

#if defined( __amd64__ ) || defined( __x86_64__ ) || defined( __ppc64__ ) || defined( __powerpc64__ )
 #define __sys64
#elif defined( __i386__ ) || defined( __x86__ ) || defined( __ppc__ ) || defined( __powerpc__ )
 #define __sys32
#endif // __sysXX

#ifndef STACK_GROWS_DOWNWARD
 #if defined( __i386__ ) || defined( __x86__ )|| defined( __amd64__ ) || defined( __x86_64__ )
    #define STACK_GROWS_DOWNWARD
 #endif // x86 + x86-64
 #if defined( __ppc__ ) || defined( __ppc64__ ) || defined( __powerpc__ ) || defined( __powerpc64__ )
    #define STACK_GROWS_DOWNWARD
 #endif // PowerPC
 #if defined( __arm__ )
    #define STACK_GROWS_DOWNWARD
 #endif // Arm - Stack dir is configurable.
#endif // STACK_GROWS_DOWNWARD

#ifdef STACK_GROWS_DOWNWARD
 #define SHM_HEAP_GROWS_UPWARD 1
 #undef SHM_HEAP_GROWS_DOWNWARD
#else
 #define SHM_HEAP_GROWS_DOWNWARD 1
 #undef SHM_HEAP_GROWS_UPWARD
#endif // STACK_GROWS_DOWNWARD

#ifdef SHM_HEAP_GROWS_DOWNWARD // When the heap grows 'downward' - towards a lower virtual address
 // _PTR_ADD_OFFSET( pointer, offset )
 #define _PTR_ADD_OFFSET(y,z) ( (char *) y + (size_t) z )
 // _PTR_REMOVE_OFFSET( pointer, offset )
 #define _PTR_REMOVE_OFFSET(y,z) ( (char *) y - (size_t)z )
 // _PTR_GET_OFFSET( base_address, target )
 #define _PTR_GET_OFFSET(b,o) ( (char *) o - (char *) b )
#else // When the heap grows 'upwards' - towards larger virtual addresses. This is the default for most archs
 // _PTR_ADD_OFFSET( pointer, offset )
 #define _PTR_ADD_OFFSET(y,z) ( (char *) y - (size_t) z )
 // _PTR_REMOVE_OFFSET( pointer, offset )
 #define _PTR_REMOVE_OFFSET(y,z) ( (char *) y + (size_t) z )
 // _PTR_GET_OFFSET( base_address, target )
 #define _PTR_GET_OFFSET(b,o) ( (char *) b - (char *) o )
#endif // SHM_HEAP_GROWS_DOWNWARD
// This should be agnostic of all archs

#define _PTR_BOUND_CHECK(p,b,s) ( (p!=NULL) && (b!=NULL) && ((char *) p >= (char *) b) && ((char *) p <= ((char *) b + (size_t) s)) )

#define ZERO_BUFFER_SIZE DEFAULT_PAGE_SIZE

#ifndef MAP_NOSYNC
#define MAP_NOSYNC 0
#endif // MAP_NOSYNC
#ifndef MAP_HASSEMAPHORE
#define MAP_HASSEMAPHORE 0
#endif // MAP_HASSEMAPHORE
#define MMAP_ADDITIONAL 0
#define MMAP_FLAGS ( MAP_HASSEMAPHORE | MAP_NOSYNC | MMAP_ADDITIONAL )

// File prefixes and flags
#define SHM_FILE_MMAP_DIR "shm"
#define SHM_FILE_MMAP_PREFIX "shm_"
#define SHM_FILE_POSIX_PREFIX "pg_ctblmgr_shm_"
#define SHM_FILE_PERMS ( S_IWUSR | S_IRUSR )
#define SHM_FILE_OCTAL 0600

#define SHM_ID_NAME_SIZE 64
#define SHM_MAX_SEGMENTS 1024

#if ( SHM_MAX_SEGMENTS > 0 ) && ( SHM_MAX_SEGMENTS <= UCHAR_MAX )
typedef uint8_t shm_handle;
typedef uint8_t handle_iter;
 #ifdef SHM_ENABLE_STRUCT_PACKING
  #define _SHM_PACK_STRUCT
 #endif // SHM_ENABLE_STRUCT_PACKING
#elif ( SHM_MAX_SEGMENTS > UCHAR_MAX ) && ( SHM_MAX_SEGMENTS <= USHRT_MAX )
typedef uint16_t shm_handle;
typedef uint16_t handle_iter;
 #ifdef SHM_ENABLE_STRUCT_PACKING
  #define _SHM_PACK_STRUCT
 #endif // SHM_ENABLE_STRUCT_PACKING
#elif ( SHM_MAX_SEGMENTS > USHRT_MAX ) && ( SHM_MAX_SEGMENTS <= UINT_MAX )
typedef uint32_t shm_handle;
typedef uint32_t handle_iter;
 #if defined( __sys64 ) && defined( SHM_ENABLE_STRUCT_PACKING )
  #define _SHM_PACK_STRUCT
 #endif // __sys64 && SHM_ENABLE_STRUCT_PACKING
#else
typedef uint64_t shm_handle;
typedef uint64_t handle_iter;
#endif // handle setup

typedef enum {
    SHM_CREATE,
    SHM_DESTROY,
    SHM_ATTACH,
    SHM_DETACH
} shm_op;
// TODO: Need to remove stale segments/control if found on startup
//  - These are easily discovered but we'll need to load them in and kill(0) the PID
//  to see if it's valid
//  Also need a free / unmap all
/* Interface functions / flags */
extern void shm_init( void );
extern void shm_child_init( void );
extern void * map_segment( shm_handle );
extern void * new_segment( size_t );
extern void unmap_segment( void * );
extern void free_segment( void * );
extern void unmap_all( void );
extern void map_all( void );
extern void zero_segment( shm_handle );

/* * * Local mapping of shared objects * * */
/*
 * We need to create local allocations to track the base addresses of objects
 * mapped into our own address space. While the stuff in shared memory will
 * reside in the same absolute location in memory, we are not given /direct/
 * access to those addresses. Instead, after calling mmap(), we get an address
 * in our own memory map which that segment is mapped to. This address cannot
 * be expected to be the same from process to process, so we must keep track of
 * the base address in order to do offset-based indirect memory access to these
 * mapped locations.
 */

#ifdef _SHM_PACK_STRUCT
typedef struct shm_segment {
    shm_handle handle;
    void *     mapped_address;
    size_t     mapped_size;
} __attribute__((packed)) shm_segment;
#else
typedef struct shm_segment {
    shm_handle handle;          // Mapped segment ID
    void *     mapped_address;  // Address it was mapped to in the process' memory map this is the address of the header
    size_t     mapped_size;     // Size mapped in bytes
} shm_segment;
#endif // _SHM_PACK_STRUCT

// Global stuff
typedef struct ctrl_header {
    uint32_t    magic;           // Should be CONTROL_HEADER_MAGIC at all times
    pid_t       owner;           // Parent process owning this segment
    bool        locked;          // Indicates a PID is modifying accounting info
    handle_iter entry_count;     // # Allocated segments
    handle_iter max_entries;     // SHM_MAX_SEGMENTS
    shm_handle  segments[SHM_MAX_SEGMENTS]; // shm_handles, indexed as 0-SHM_MAX_SEGMENTS,
                                           // with entry_count indexing into the next available
} ctrl_header;

typedef struct seg_header {
    uint32_t   magic;           // Should be SEGMENT_HEADER_MAGIC at all times
    pid_t      owner;           // Parent process owning this segment
    bool       locked;          // Shared between allocator and shm.c
    uint32_t   entry_count;     // FOR ALLOCATOR USE
    uint32_t   ref_count;       // Number of processes with this segment mapped
    shm_handle control;         // ID of control segment
    char *     data;            // User ( allocator ) data starts here NOTE. NEED TO MAKE SURE THIS ADDRESS IS ALIGNED
} seg_header;

/*
 * Page Headers - these are stored in shared memory
 *  we use different magic numbers with the packing
 *  settings to avoid mis-interpreting the headers
 */
#define SEGMENT_HANDLE_INVALID ( ( shm_handle ) ( ( uint64_t ) 0 - 2 ) )
#ifdef _SHM_PACK_STRUCT
 #define SEGMENT_HEADER_MAGIC ( uint32_t ) 0x3FA7B00B
#else
 #define SEGMENT_HEADER_MAGIC ( uint32_t ) 0xE02EA7F3
#endif // _SHM_PACK_STRUCT
#define CONTROL_HANDLE_INVALID ( ( shm_handle ) ( ( uint64_t ) 0 - 1 ) )
#ifdef _SHM_PACK_STRUCT
 #define CONTROL_HEADER_MAGIC ( uint32_t ) 0xBE22420A
#else
 #define CONTROL_HEADER_MAGIC ( uint32_t ) 0x9F0522BE
#endif // _SHM_PACK_STRUCT
#define GET_USER_PTR(x) ( (void *) _PTR_REMOVE_OFFSET( ( ( char * ) x ), ( offsetof( seg_header, data )) ) )
#define GET_HDR_PTR(x) ( (void *) _PTR_ADD_OFFSET( ( ( char * ) x ), ( offsetof( seg_header, data ) ) ) )
/*
 * Pointer dereference helpers / logic
 * __ref:
 *   This, ideally, replaces an absolute pointer.
 *   The segment tells us which SHM segment the data resides
 *   from the segment, we can get the locally mapped address ( base address )
 *   and from that, we add the offset and get the absolute, locally mapped
 *   address
 * __segment_lut[]:
 *   This array stores shm_segment structs, locally defined, which map a segment id
 *   to a base address.
 * get_ptr():
 *   Given a __ref and a populated __segment_lut[], we can resolve an absolute
 *   local address from a base address / segment_id and offset
 */
#ifdef _SHM_PACK_STRUCT
typedef struct __ref {
    shm_handle _segment; // ID of the segment this ref points to
    size_t     _offset;  // Offset into the segment (from the user facing pointer IE mapped_address + offsetof( seg_header, data ) )
} __attribute__((packed)) __ref;
#else
typedef struct __ref {
    shm_handle _segment;
    size_t     _offset;
} __ref;
#endif // _SHM_PACK_STRUCT

extern __inline__ void * get_ptr( __ref );
extern __inline__ __ref get_ref( void * );
extern ctrl_header * get_control_header( void );

// Logging helpers

typedef enum {
    LL_SHM_ERROR,
    LL_SHM_DEBUG
} shm_ll;

#endif // _SHM_H
