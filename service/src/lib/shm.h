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
 *  There are multiple ways to share memory between processes. A naive
 *  implementation, but inflexible, is to mmap a memory segment
 *  and treating mmap like a call to malloc(). This segment can be read,
 *  written, and executed from for the calling processes and any process
 *  forked after that point (the forked process has the mapping included
 *  in its memory map). The drawback is that any allocations, extensions,
 *  or 'frees' after that point are only visible to the processor making
 *  that call. Processes, if they really wanted to get this change,
 *  would need to re-map or perform the appropriate action to mirror
 *  these changes. That's all well and good, but pitfalls arise where
 *  mmap may not consistently map a segment, therefore shared pointers
 *  between processes can become invalid or point to the incorrect data.
 *  This effectively leaves dangling pointers in every process excluding
 *  the one calling mmap.
 *
 *  To make this work correctly, a few mechanisms are needed:
 *  - Processes need to be aware of the offset in mapping (from mmap()
 *    mapping memory to different virtual addresses in the memory map of
 *    each process).
 *  - Processes need to be aware of the 'globally desired' size of a
 *    given segment, I.E. if a single process extends or shrinks a
 *    segment, the other processes must be made aware of this change in
 *    size.
 *  - Account for the typical issues that arise in SMP or multithreaded
 *    environments, such as the need for synchronization (of important
 *    book-keeping information laid out above), and locking to prevent data
 *    hazards or ensure the safety of atomic operations).
 *  - Perform all the above as efficiently and transparently as possible
 *    with minimal processing and memory overhead.
 * 
 * UNDERLYING LIBRARIES:
 *  This library abstracts POSIX, mmap, and SystemV shared memory
 *  functionalities to provide a consistent, shared, file-backed memory
 *  store.
 *
 * DATA STRUCTURES:
 *  Storage for book-keeping information is provided by the control_header
 *  and segment_header structs. These reside at the top of each mapped
 *  segment and constitute shm's global state. Each mapped segment (and header)
 *  is backed by a physical file in an implementation-specific location
 *  ( e.g. /dev/shm/ for POSIX )
 *
 *  Local state is stored in the shm_segment struct, along with static
 *  variables in the top of shm.c:
 *   seg_header __segment_lut[]
 *  These serve to indicate if, and where, each segment is mapped in our
 *  local processes' memory map.
 *
 * LOCKING:
 *  These structures use three classes of lock which indicates the importance
 *  ( but not the nature ) of the operation being performed.
 *  Locking is backed by primitives proveded by barrier.h in the form of
 *  __TNS_MUTEX and __C_MUTEX, or test and set mutex and clear mutex,
 *  respectively. These default to basic spin locks but can use compiler-
 *  provided atomics, or atomic assembly instructions, if supported.
 *
 * SYNCHRONIZATION:
 *  Synchronization is NOT provided by this library. This can be implemented
 *  using barrier.h as well as supporting information (such as checking that
 *  all processes have arrived at the synchronization point), but that can
 *  be handled in an use case specific manner.
 *
 * POINTERS:
 *  Pointers are provided by the __ref typedef and helper functions get_ptr(),
 *  get_ptr_fast() and get_ref(). These store and interpret the segment and
 *  offset into that segment (to whuch the __ref points). Depending on
 *  compilation settings, this can be a packed structure or encoded into a
 *  wide integer. The choice of layout is dependent on the settings for maximum
 *  segment size and maximum number of segments, or SHM_SEGMENT_MAX_SIZE and
 *  SHM_MAX_SEGMENTS, respectively. This functionality can be disabled entirely
 *  with SHM_ENABLE_STRUCT_PACKING. The __ref function should be substituted
 *  in user data structures and wrapped with get_ptr() calls to allow the
 *  program to resolve to a locally mapped pointer.
 *
 *  To provide a consistent interface (compared to other standard memory manipulation
 *  functions), ref_is_null() and get_null_ref() are provided as a canonical NULL and
 *  NULL checking.
 *
 * PROPAGATING CHANGES:
 *  New segments and segment size changes need to be propagated. These are
 *  done automatically by get_ptr() when:
 *  - A __ref for an unmapped segment is resolved to a local pointer.
 *  - get_ptr() detects an inconsistency between the local state
 *    (__segment_lut[]) and global state (seg_header).
 *  This functionality is enabled with SHM_AUTO_MAP
 * 
 * Copyright (c) 2021-2022, MerchLogix Inc.
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

/* ----------------------------- TUNABLES -------------------------------
 * Important setup parameters.
 *   SHM_ENABLE_RUNTIME_SANITY_CHECK: Verifies stack and heap growth directions
 *     at initialization. Some assumptions / conventions are used but we will
 *     not know if they are correct until runtime.
 *   SHM_EXTRA_SANE: Mostly for finding bugs - uses alternative methods to return
 *     segment handles or different code paths to perform checks. This ensures
 *     state is correct at runtime at the cost of extra instructions within
 *     inlined routines.
 *   SHM_ENABLE_STRUCT_PACKING: Allows struct packing, which is used if the
 *     architecture word size is sufficient to justify struct packing for
 *     things like offset-based references and headers. This can save a decent
 *     amount of memory but is only useful under certain conditions
 *   DEFAULT_PAGE_SIZE: Default page size in cases where the OS does not
 *     provide a method for determining it at runtime.
 *   SHM_MAX_SEGMENTS: The maximum number of segments allowed to be attached to
 *     a single control header. This limits the header's size, and being
 *     statically defined, limits code complexity. This also controls the
 *     typedefs for segment handles and can, if sufficiently small, allow
 *     for efficient dereferencing
 *   SHM_SEGMENT_MAX_SIZE: The maximum size of a single segment, in pages.
 *     This controls the maximum memory allocation to
 *     SHM_SEGMENT_MAX_SIZE * PAGE_SIZE
 *   SHM_AUTO_MAP: Allows automatic mapping when dereferencing a __ref pointing
 *     to an as-of-not-yet-mapped segment. Otherwise dereferencing will return
 *     a NULL pointer
 *   SHM_ENABLE_HUGETLB: TODO Attempt to use the system's hugepage settings to
 *     fulfill requests to allocate large segments. For typical x86
 *     applications, this can be 2MB, and up to 1GB iff PDPE1GB is supported.
 *     I hope to include PSE support as well
 */
// TODO: control_handle[segment] seems not being set appropriately - can be located by changing how _ref_get_segment words in extra sane mode
#define SHM_EXTRA_SANE 1
#define SHM_ENABLE_RUNTIME_SANITY_CHECK 1
#define SHM_ENABLE_STRUCT_PACKING 1
#define SHM_MAX_SEGMENTS 255
#define SHM_SEGMENT_MAX_SIZE 256 // In pages
#define SHM_AUTO_MAP 1
//#define SHM_ENABLE_HUGETLB

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

#ifdef __ultrasparc__
 #define DEFAULT_PAGE_SIZE 8192
#else
 #define DEFAULT_PAGE_SIZE 4096
#endif // __ultrasparc__
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

// There's a chance this could all be handled by the compiler and I am drastically overthinking things,
// in which case it's really easy to remove the extra crap
#ifdef SHM_HEAP_GROWS_DOWNWARD // When the heap grows 'downward' - towards a lower virtual address
 // _PTR_ADD_OFFSET( pointer, offset )
 #define _PTR_ADD_OFFSET(y,z) ( (uint8_t *) y - (size_t) z )
 // _PTR_REMOVE_OFFSET( pointer, offset )
 #define _PTR_REMOVE_OFFSET(y,z) ( (uint8_t *) y + (size_t)z )
 // _PTR_GET_OFFSET( base_address, target )
 #define _PTR_GET_OFFSET(b,o) ( (uint8_t *) b - (uint8_t *) o )
#else // When the heap grows 'upwards' - towards larger virtual addresses. This is the default for most archs
 // _PTR_ADD_OFFSET( pointer, offset )
 #define _PTR_ADD_OFFSET(y,z) ( (uint8_t *) y + (size_t) z )
 // _PTR_REMOVE_OFFSET( pointer, offset )
 #define _PTR_REMOVE_OFFSET(y,z) ( (uint8_t *) y - (size_t) z )
 // _PTR_GET_OFFSET( base_address, target )
 #define _PTR_GET_OFFSET(b,o) ( (uint8_t *) o - (uint8_t *) b )
#endif // SHM_HEAP_GROWS_DOWNWARD
// This should be agnostic of all archs

#define _PTR_BOUND_CHECK(p,b,s) ( (p!=NULL) && (b!=NULL) && ((char *) p >= (char *) b) && ((char *) p <= ((char *) b + (size_t) s)) )
#define _PTR_BOUND_CHECK_NULL(p,b,s) ( ( (p!=NULL) && (b!=NULL) && ((char *) p >= (char *) b) && ((char *) p <= ((char *) b + (size_t) s)) ) ? p : NULL )
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

// typedef our handles and iterator into the smallest possible size to fit them
// This may allow the handle and offset to be packed into a single uint
#if defined( SHM_MAX_SEGMENTS ) && ( SHM_MAX_SEGMENTS > 0 ) && ( SHM_MAX_SEGMENTS <= UCHAR_MAX )
typedef uint8_t shm_handle;
typedef uint8_t handle_iter;
 #define SHM_HANDLE_ITER_MAX UCHAR_MAX
 #define SHM_HANDLE_SIZE 8
 #define SHM_HANDLE_MASK 0xFF
 #ifdef SHM_ENABLE_STRUCT_PACKING
  #define _SHM_PACK_STRUCT
 #endif // SHM_ENABLE_STRUCT_PACKING
#elif defined( SHM_MAX_SEGMENTS ) && ( SHM_MAX_SEGMENTS > UCHAR_MAX ) && ( SHM_MAX_SEGMENTS <= USHRT_MAX )
typedef uint16_t shm_handle;
typedef uint16_t handle_iter;
 #define SHM_HANDLE_ITER_MAX USHRT_MAX
 #define SHM_HANDLE_SIZE 16
 #define SHM_HANDLE_MASK 0xFFFF
 #ifdef SHM_ENABLE_STRUCT_PACKING
  #define _SHM_PACK_STRUCT
 #endif // SHM_ENABLE_STRUCT_PACKING
#elif defined( SHM_MAX_SEGMENTS ) && ( SHM_MAX_SEGMENTS > USHRT_MAX ) && ( SHM_MAX_SEGMENTS <= UINT_MAX )
typedef uint32_t shm_handle;
typedef uint32_t handle_iter;
 #define SHM_HANDLE_ITER_MAX UINT_MAX
 #define SHM_HANDLE_SIZE 32
 #define SHM_HANDLE_MASK 0xFFFFFFFF
 #if defined( __sys64 ) && defined( SHM_ENABLE_STRUCT_PACKING )
  #define _SHM_PACK_STRUCT
 #endif // __sys64 && SHM_ENABLE_STRUCT_PACKING
#else
typedef uint64_t shm_handle;
typedef uint64_t handle_iter;
 #define SHM_HANDLE_ITER_MAX ULONG_MAX
 #define SHM_HANDLE_SIZE 64
 #define SHM_HANDLE_MASK 0xFFFFFFFFFFFFFFFF
 #ifndef SHM_MAX_SEGMENTS
  #define SHM_MAX_SEGMENTS UINT_MAX
 #endif // SHM_MAX_SEGMENTS
#endif // handle setup

#ifdef SHM_SEGMENT_MAX_SIZE
 #define SHM_MAX_OFFSET SHM_SEGMENT_MAX_SIZE * DEFAULT_PAGE_SIZE
#else
 #define SHM_MAX_OFFSET UINT_MAX
 #define SHM_SEGMENT_MAX_SIZE ( SHM_MAX_OFFSET / DEFAULT_PAGE_SIZE )
#endif // SHM_SEGMENT_MAX_SIZE

// Like the handle, size the offset typedef to the smallest possible type
#if defined( SHM_MAX_OFFSET ) && ( SHM_MAX_OFFSET > 0 ) && ( SHM_MAX_OFFSET <= UCHAR_MAX )
typedef uint8_t offset_t;
#define SHM_OFFSET_SIZE 8
#define __OFFSET_MAX UCHAR_MAX
#define SHM_OFFSET_MASK 0xFF
#elif defined( SHM_MAX_OFFSET ) && ( SHM_MAX_OFFSET > UCHAR_MAX ) && ( SHM_MAX_OFFSET <= USHRT_MAX )
typedef uint16_t offset_t;
#define SHM_OFFSET_SIZE 16
#define __OFFSET_MAX USHRT_MAX
#define SHM_OFFSET_MASK 0xFFFF
#elif defined( SHM_MAX_OFFSET ) && ( SHM_MAX_OFFSET > USHRT_MAX ) && ( SHM_MAX_OFFSET <= UINT_MAX )
typedef uint32_t offset_t;
#define SHM_OFFSET_SIZE 32
#define __OFFSET_MAX UINT_MAX
#define SHM_OFFSET_MASK 0xFFFFFFFF
#else
typedef uint64_t offset_t;
#define SHM_OFFSET_SIZE 64
#define __OFFSET_MAX ULONG_MAX;
#define SHM_OFFSET_MASK 0xFFFFFFFFFFFFFFFF
#endif // offset setup

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
extern void shm_init( void ); // Initialize control header
extern void shm_init_extra( size_t ); // Initialize control header with extra space for data
extern bool shm_is_init( void );
extern void shm_child_init( void );
extern void * map_segment( shm_handle );
extern void * new_segment( size_t );
extern shm_handle get_handle_from_ptr( void * );
extern void unmap_segment( void * );
extern void free_segment( void * );
extern void unmap_all( void );
extern void map_all( void );
extern void zero_segment( shm_handle );
extern bool shm_resize_segment( shm_handle, size_t );
extern size_t get_segment_size( shm_handle ); // Returns the size available to the user
                                              // IE: mapped_size - sizeof( seg_header )
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
    size_t     mapped_size; // This is not the user requested size, but the actual size of the segment. Typically a
} __attribute__((packed)) shm_segment;
#else
typedef struct shm_segment {
    shm_handle handle;          // Mapped segment ID
    void *     mapped_address;  // Address it was mapped to in the process' memory map this is the address of the header
    size_t     mapped_size;     // Size mapped in bytes incl headers
} shm_segment;
#endif // _SHM_PACK_STRUCT

// Global stuff
typedef struct ctrl_header {
    uint32_t      magic;           // Should be CONTROL_HEADER_MAGIC at all times
    pid_t         owner;           // Parent process owning this segment
    volatile bool locked;          // Indicates a PID is modifying accounting info (this is SHM_EXCLUSIVE)
    handle_iter   entry_count;     // # Allocated segments
    handle_iter   max_entries;     // SHM_MAX_SEGMENTS
    shm_handle    segments[SHM_MAX_SEGMENTS]; // shm_handles, indexed as 0-SHM_MAX_SEGMENTS,
                                           // with entry_count indexing into the next available
    size_t        sizes[SHM_MAX_SEGMENTS];
    volatile bool hwlocks[SHM_MAX_SEGMENTS]; // TODO: Need to relocate segment header locks here. (this is SHM_HWLOCK)
    uint8_t *     data;
} ctrl_header;

typedef struct seg_header {
    uint32_t        magic;     // Should be SEGMENT_HEADER_MAGIC at all times
    pid_t           owner;     // Parent process owning this segment
    volatile bool   locked;    // Shared between allocator and shm.c. This is SHM_LWLOCK
    volatile size_t size; // XXX this needs to be moved to the control headers sizes[]. The resize's destroy/create might interfere
    uint32_t        ref_count; // Number of processes with this segment mapped
    shm_handle      control;   // ID of control segment
    uint8_t *       data;      // User ( allocator ) data starts here NOTE. NEED TO MAKE SURE THIS ADDRESS IS ALIGNED
} seg_header;

/*
 * Page Headers - these are stored in shared memory
 *  we use different magic numbers with the packing
 *  settings to avoid mis-interpreting the headers
 */
#define SEGMENT_HANDLE_INVALID ( ( shm_handle ) ( ( uint64_t ) 0 - 2 ) )
#ifdef _SHM_PACK_STRUCT
 #define SEGMENT_HEADER_MAGIC ( uint32_t ) 0xC0FFEEBE
#else
 #define SEGMENT_HEADER_MAGIC ( uint32_t ) 0xF00DFACE
#endif // _SHM_PACK_STRUCT
#define CONTROL_HANDLE_INVALID ( ( shm_handle ) ( ( uint64_t ) 0 - 1 ) )
#ifdef _SHM_PACK_STRUCT
 #define CONTROL_HEADER_MAGIC ( uint32_t ) 0xC0DEDEAD
#else
 #define CONTROL_HEADER_MAGIC ( uint32_t ) 0x1337C0DE
#endif // _SHM_PACK_STRUCT
#define GET_USER_PTR(x) ( (void *) _PTR_ADD_OFFSET( ( ( char * ) x ), ( offsetof( seg_header, data )) ) )
#define GET_HDR_PTR(x) ( (void *) _PTR_REMOVE_OFFSET( ( ( char * ) x ), ( offsetof( seg_header, data ) ) ) )

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
 #ifdef __sys64
  #if SHM_OFFSET_SIZE + SHM_HANDLE_SIZE < 64
   #define __SHM_NO_STRUCT
   #define __SHM_RPAD_WIDTH 64 - SHM_OFFSET_SIZE - SHM_HANDLE_SIZE
  #endif // SHM_OFFSET_SIZE + SHM_HANDLE_SIZE
 #elif defined( __sys32 )
  #if SHM_OFFSET_SIZE + SHM_HANDLE_SIZE < 32
   #define __SHM_NO_STRUCT
   #define __SHM_RPAD_WIDTH 32 - SHM_OFFSET_SIZE - SHM_HANDLE_SIZE
  #endif // SHM_OFFSET_SIZE + SHM_HANDLE_SIZE
 #endif // arch width
#endif // SHM_PACK_STRUCT
#ifdef _SHM_PACK_STRUCT
 #ifndef __SHM_NO_STRUCT
typedef struct __ref {
    shm_handle _segment; // ID of the segment this ref points to
    offset_t   _offset;  // Offset into the segment (from the user facing pointer IE mapped_address + offsetof( seg_header, data ) )
} __attribute__((packed)) __ref;
 #else
  #ifdef __sys32
typedef uint32_t __ref;
  #elif defined( __sys64 )
typedef uint64_t __ref;
  #endif // arch
 #endif // __SHM_NO_STRUCT
#else
typedef struct __ref {
    shm_handle _segment;
    offset_t   _offset;
} __ref;
#endif // _SHM_PACK_STRUCT

// This lock pertains to the bit being set - with the following order of precedence
// control_header->locked         = SHM_EXCLUSIVE - hardest to obtain, indicates the header is being manipulated & conflicts with all other locks
// control_header->locks[segment] = SHM_HWLOCK - Indicates that the segment is in a resize / maintenance
// seg_header->locked             = SHM_LWLOCK - User lock
//
typedef enum {
    SHM_EXCLUSIVE,
    SHM_HWLOCK,
    SHM_LWLOCK,
} shm_lock;

/*
 * get_ptr( __ref )
 */
extern __inline__ void * get_ptr( __ref ); // Get local pointer to mapping
extern __inline__ void * get_ptr_fast( __ref ) __attribute__((flatten)); // above but only for contexts where the segment will not chang
extern __inline__ __ref get_ref( void * ); // Get absolute ref
extern ctrl_header * get_control_header( void );
extern shm_handle get_control_segment( void );
extern uint8_t * get_control_data_section( void );
extern __inline__ bool ref_is_null( __ref ); // check if ref is null
extern __inline__ __ref get_null_ref( void ); // Get a reference null
extern bool shm_remap( shm_handle );

extern offset_t ref_get_offset( __ref );
extern shm_handle ref_get_segment( __ref );
extern __ref ref_set_segment( __ref, shm_handle );
extern __ref ref_set_offset( __ref, offset_t );

extern bool is_locked( shm_handle, shm_lock );
extern bool get_lock( shm_handle, shm_lock );
extern bool release_lock( shm_handle, shm_lock );

typedef enum {
    LL_SHM_ERROR, // Critical error
    LL_SHM_DEBUG  // Only enabled iff SHM_DEBUG is defined
} shm_ll;

#endif // _SHM_H
