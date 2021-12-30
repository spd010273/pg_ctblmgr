/*------------------------------------------------------------------------
 *
 * shm.c
 *     Shared Memory function primitives
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
 *        service/src/lib/shm.c
 *
 *------------------------------------------------------------------------
 */
#include "shm.h"

/* Static definitions */
#ifdef SHM_USE_POSIX
static bool _shm_posix( shm_op, shm_handle, size_t, void **, size_t * );
static int _shm_posix_resize( int, size_t );
#endif // SHM_USE_POSIX
#ifdef SHM_USE_MMAP
static bool _shm_mmap( shm_op, shm_handle, size_t, void **, size_t * );
static int _shm_mmap_resize( int, size_t );
#endif // SHM_USE_MMAP
#ifdef SHM_USE_SYSV
static bool _shm_sysv( shm_op, shm_handle, size_t, void **, void **, size_t * );
static void * sysv_private = NULL;
#endif // SHM_USE_SYSV

static bool _shm_wrapper( shm_op, shm_handle, size_t, void **, size_t * );

#if defined( SHM_USE_POSIX ) || defined( SHM_USE_MMAP )
static bool _close_segment_descriptor( int, char *, bool );
#endif // SHM_USE_POSIX || SHM_USE_MMAP

static size_t _get_system_page_size( void );
static size_t _round_to_multiple_of_page_size( size_t );
static size_t _get_ctrl_header_size( uint32_t );
static __inline__ shm_handle _get_handle_from_ptr( void * ) __attribute__((always_inline));
static void _free_segment( shm_handle );
static void _append_to_cleanup_list( shm_handle );
static void _cleanup_old_segments( void );

static inline bool _shm_check_owner( ctrl_header * );
static inline bool _shm_check_control( ctrl_header * );
static inline bool _shm_check_seg_owner( seg_header * );
static inline bool _shm_check_segment( seg_header * );

#ifdef SHM_DEBUG
static void __dump_ctrl_header( ctrl_header * ) __attribute__((unused));
static void __dump_seg_header( seg_header * ) __attribute__((unused));
#endif // SHM_DEBUG

#ifdef SHM_ENABLE_RUNTIME_SANITY_CHECK
static bool directionality_check( void );
static bool _dir_check_b( uint64_t * );
#endif // SHM_ENABLE_RUNTIME_SANITY_CHECK

static void _shm_log( shm_ll, char *, ... ) __attribute__ ((format (gnu_printf, 2, 3)));

static shm_handle    control_handle      = ( shm_handle ) CONTROL_HANDLE_INVALID;
static ctrl_header * control_header      = NULL;
static size_t        control_header_size = 0;
static pid_t         p_pid               = ( pid_t ) 0;
static bool          shm_inited          = false;
static shm_handle *  cleanup_list        = NULL;
static uint16_t      cleanup_list_len    = 0;

// Given a segment ID, lets us get the mapping info
static shm_segment   __segment_lut[SHM_MAX_SEGMENTS] = {{0}};

static __inline__ offset_t _ref_get_offset( __ref ) __attribute__((always_inline));
static __inline__ shm_handle _ref_get_segment( __ref ) __attribute__((always_inline));
static __inline__ __ref _ref_set_offset( __ref, offset_t ) __attribute__((always_inline));
static __inline__ __ref _ref_set_segment( __ref, shm_handle ) __attribute__((always_inline));

// External-facing getters for test harness
offset_t ref_get_offset( __ref ref )
{
    return _ref_get_offset( ref );
}

shm_handle ref_get_segment( __ref ref )
{
    return _ref_get_segment( ref );
}

__ref ref_set_segment( __ref ref, shm_handle segment )
{
    return _ref_set_segment( ref, segment );
}

__ref ref_set_offset( __ref ref, offset_t offset )
{
    return _ref_set_offset( ref, offset );
}
/*
 * Setters and getters for __ref type. Depending on optimizations, this may be
 * a struct or crammed into a uint32_t or uint64_t.
 */
static __inline__ offset_t _ref_get_offset( __ref ref )
{
    #ifdef __SHM_NO_STRUCT
    /*
     * Shift out any right-side padding, then mask off the offset then trim
     * to the appropriate length
     */
    return ( offset_t ) ( ref >> ( __SHM_RPAD_WIDTH ) ) & SHM_OFFSET_MASK;
    #else
    return ( offset_t ) ( ref._offset );
    #endif // __SHM_NO_STRUCT
}

static __inline__ shm_handle _ref_get_segment( __ref ref )
{
    #ifdef __SHM_NO_STRUCT
    /*
     * Shift out the right-side padding and offset, masking the segment
     * and trimming with a final cast
     */
    return ( shm_handle ) ( ref >> ( ( __SHM_RPAD_WIDTH ) + SHM_OFFSET_SIZE ) ) & SHM_HANDLE_MASK;
    #else
    return ( shm_handle ) ( ref._segment );
    #endif // __SHM_NO_STRUCT
}

static __inline__ __ref _ref_set_offset( __ref ref, offset_t offset )
{
    #ifdef __SHM_NO_STRUCT
    /*
     * Mask off and clear the offset portion of __ref. For a 64-bit example
     * with 16-bit segments and 32-bit offsets, we're targeting the 0xFF'd
     * portion: 0x0000FFFFFFFF0000
     */
    ref = ref & ~( ( ( __ref ) SHM_OFFSET_MASK << ( __SHM_RPAD_WIDTH ) ) );
    ref = ref | ( ( ( __ref )  offset << ( __SHM_RPAD_WIDTH ) ) );
    #else
    ref._offset = offset;
    #endif // _SHM_NO_STRUCT
    return ref;
}

static __inline__ __ref _ref_set_segment( __ref ref, shm_handle segment )
{
    #ifdef __SHM_NO_STRUCT
    /*
     * Mask off and clear the segment portion of __ref. For a 64-bit example
     * with 16-bit segments and 32-bit offsets, we're targeting the 0xFF'd
     * portion: 0xFFFF000000000000
     */
    ref = ref & ~( ( ( __ref ) SHM_HANDLE_MASK << ( ( __SHM_RPAD_WIDTH ) + SHM_OFFSET_SIZE ) ) );
    ref = ref | ( ( ( __ref ) segment << ( ( __SHM_RPAD_WIDTH ) + SHM_OFFSET_SIZE ) ) );
    #else
    ref._segment = segment;
    #endif // _SHM_NO_STRUCT
    return ref;
}

__inline__ void * get_ptr( __ref ref )
{
    void *   mapped_address     = NULL;
    void *   ret                = NULL;
    size_t   mapped_size        = 0;
    register shm_handle segment = SEGMENT_HANDLE_INVALID;

    if( ref_is_null( ref ) )
        return NULL;

    segment = _ref_get_segment( ref );

    // Check that segment is initialized and valid
    if( unlikely( segment == SEGMENT_HANDLE_INVALID ) )
        return NULL;
    // Check that the requested segment is within the bounds of the LUT
    if( unlikely( segment > ( shm_handle ) SHM_MAX_SEGMENTS ) )
        return NULL;

    mapped_address = __segment_lut[segment].mapped_address;

    if( unlikely( mapped_address == NULL ) )
    #ifndef SHM_AUTO_MAP
        return NULL;
    #else
    {
        // Segment not mapped
        _shm_log(
            LL_SHM_DEBUG,
            "get_ptr() attempting to map a segment %lu",
            ( uint64_t ) segment
        );

        if( unlikely( map_segment( segment ) == NULL ) )
        {
            _shm_log(
                LL_SHM_ERROR,
                "get_ptr() failed to automap segment %lu",
                ( uint64_t ) segment
            );
            return NULL;
        }

        mapped_address = __segment_lut[segment].mapped_address;
    }
    #endif // SHM_AUTO_MAP

    mapped_size = __segment_lut[segment].mapped_size;
    ret         = _PTR_ADD_OFFSET(
        GET_USER_PTR( mapped_address ),
        ( size_t ) _ref_get_offset( ref )
    );

    if( unlikely( !_PTR_BOUND_CHECK( ret, mapped_address, mapped_size ) ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "return address %p failed bounds check",
            ret
        );
        return NULL;
    }

    return ret;
}

__inline__ __ref get_ref( void * ptr )
{
    __ref      ret    = {0};
    shm_handle handle = 0;

    // Initialize as a canonical NULL ref
    ret = _ref_set_segment( ret, SEGMENT_HANDLE_INVALID );
    ret = _ref_set_offset( ret, __OFFSET_MAX );

    if( unlikely( ptr == NULL ) )
        return ret;

    handle = _get_handle_from_ptr( ptr );

    if( unlikely( handle == SEGMENT_HANDLE_INVALID ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Failed to find mapped segment containing %p",
            ptr
        );
        return ret;
    }

    if(
        unlikely(
            !_PTR_BOUND_CHECK(
                ptr,
                __segment_lut[handle].mapped_address,
                __segment_lut[handle].mapped_size
            )
        )
      )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Pointer failed bounds check:\n"
            "  %p not in range for\n"
            "  %p of size %zu",
            ptr,
            __segment_lut[handle].mapped_address,
            __segment_lut[handle].mapped_size
        );
        return ret;
    }

    if( unlikely( __segment_lut[handle].mapped_address == NULL ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Mapped address for segment handle %lu is null",
            ( uint64_t ) handle
        );
        return ret;
    }

    ret = _ref_set_offset(
        ret,
        _PTR_GET_OFFSET(
            GET_USER_PTR( __segment_lut[handle].mapped_address ),
            ptr
        )
    );

    ret = _ref_set_segment( ret, handle );

    return ret;
}

void shm_init( void )
{
    void *      mapped_address   = NULL;
    size_t      mapped_size      = 0;
    size_t      ctrl_header_size = 0;
    shm_handle  ctrl_handle      = CONTROL_HANDLE_INVALID;
    handle_iter i                = 0;

    if( shm_inited == true )
        return;
    #ifdef SHM_ENABLE_RUNTIME_SANITY_CHECK
    /*
     * sanity check for stack / heap growth directions.
     * This gives us the opportunity to fail in development
     * or testing cleanly rather than in production with a
     * SIGSEGV
     */
    if( !directionality_check() )
        exit( 1 );
    #endif // SHM_ENABLE_RUNTIME_SANITY_CHECK

    #ifdef SHM_DEBUG
    _shm_log( LL_SHM_DEBUG, "SHM DEBUG ENABLED:" );
     #ifdef SHM_HEAP_GROWS_DOWNWARD
    _shm_log(
        LL_SHM_DEBUG,
        "Heap Growth Direction: DOWN (Towards lower virtual addresses)"
    );
     #else
    _shm_log(
        LL_SHM_DEBUG,
        "Heap Growth Direction: UP (Towards higher virtual addresses)"
    );
     #endif // SHM_HEAP_GROWS_DOWNWARD
     #ifdef STACK_GROWS_DOWNWARD
    _shm_log(
        LL_SHM_DEBUG,
        "Stack Growth Direction: DOWN (Towards lower virtual addresses)"
    );
     #else
    _shm_log(
        LL_SHM_DEBUG,
        "Stack Growth Direction: UP (Towards higher virtual addresses)"
    );
     #endif // STACK_GROWS_DOWNWARD
    _shm_log(
        LL_SHM_DEBUG,
        "Compiled configuration:\n"
        "  Max Segments: %lu\n"
        "  Segment Max Pages: %lu\n"
        "  Max offset: %lu\n"
        "  Default page size %lu bytes\n"
        "  System page size %lu bytes",
        ( uint64_t ) SHM_MAX_SEGMENTS,
        ( uint64_t ) SHM_SEGMENT_MAX_SIZE,
        ( uint64_t ) SHM_MAX_OFFSET,
        ( uint64_t ) DEFAULT_PAGE_SIZE,
        ( uint64_t ) _get_system_page_size()
    );

    _shm_log(
        LL_SHM_DEBUG,
        "MAGIC BYTES:\n"
        "  control header: %x\n"
        "  segment header: %x",
        CONTROL_HEADER_MAGIC,
        SEGMENT_HEADER_MAGIC
    );

    _shm_log(
        LL_SHM_DEBUG,
        "HANDLE/OFFSETS:\n"
        "  HANDLE SIZE: %zu bytes\n"
        "  OFFSET SIZE: %zu bytes",
        sizeof( shm_handle ),
        sizeof( offset_t )
    );
     #ifdef _SHM_PACK_STRUCT
    _shm_log(
        LL_SHM_DEBUG,
        "Using packed structures\n"
        "  __ref size: %lu bytes\n"
        "  __segment_lut[] element size: %lu bytes",
        sizeof( __ref ),
        sizeof( shm_segment )
    );
     #else
    _shm_log(
        LL_SHM_DEBUG,
        "Using word-aligned structures\n"
        "  __ref size: %lu bytes\n"
        "  __segment_lut[] element size: %lu bytes",
        sizeof( __ref ),
        sizeof( shm_segment )
    );
     #endif // _SHM_PACK_STRUCT
    #endif // SHM_DEBUG

    p_pid = ( pid_t ) getpid();

    ctrl_header_size = _get_ctrl_header_size( ( uint32_t ) SHM_MAX_SEGMENTS );
    //ctrl_header_size = _round_to_multiple_of_page_size( ctrl_header_size );
    #ifdef SHM_DEBUG
    _shm_log(
        LL_SHM_DEBUG,
        "Attempting to map control segment, header size %zu,"
        " rounded-to-page-size %zu",
        _get_ctrl_header_size( ( uint32_t ) SHM_MAX_SEGMENTS ),
        ctrl_header_size
    );
    #endif // SHM_DEBUG

    while( mapped_address == NULL && mapped_size == 0 )
    {
        ctrl_handle = ( shm_handle ) random();

        if( unlikely( ctrl_handle == CONTROL_HANDLE_INVALID ) )
            continue;
        _shm_log(
            LL_SHM_DEBUG,
            "Attemtping initialization with handle %lu",
            ( uint64_t ) ctrl_handle
        );

        if(
            likely(
                _shm_wrapper(
                    SHM_CREATE,
                    ctrl_handle,
                    ctrl_header_size,
                    ( void ** ) &mapped_address,
                    ( size_t * ) &mapped_size
                )
            )
          )
        {
            _shm_log(
                LL_SHM_DEBUG,
                "Mapped control handle %lu to %p",
                ( uint64_t ) ctrl_handle,
                mapped_address
            );
            break;
        }
        else
        {
            _append_to_cleanup_list( ctrl_handle );
        }
    }

    control_handle              = ctrl_handle;
    control_header              = ( ctrl_header * ) mapped_address;
    control_header_size         = ( size_t ) mapped_size;
    control_header->magic       = CONTROL_HEADER_MAGIC;
    control_header->owner       = getpid();
    control_header->entry_count = ( handle_iter ) 0;
    control_header->max_entries = ( handle_iter ) SHM_MAX_SEGMENTS;
    control_header->locked      = false;

    _cleanup_old_segments();

    // Blank out the allocations
    for( i = 0; i < control_header->max_entries; i++ )
    {
        control_header->segments[i]     = (shm_handle) SEGMENT_HANDLE_INVALID;
        __segment_lut[i].mapped_address = NULL;
        __segment_lut[i].mapped_size    = 0;
        __segment_lut[i].handle         = (shm_handle) SEGMENT_HANDLE_INVALID;
    }

    _shm_log(
        LL_SHM_DEBUG,
        "SHM INITED: mapped control segment to %p, handle %lu",
        control_header,
        ( uint64_t ) control_handle
    );

    shm_inited = true;
    return;
}

bool shm_is_init( void )
{
    return shm_inited;
}

void shm_child_init( void )
{
    void *     ctrl_header_address = NULL;
    size_t     ctrl_header_size    = 0;
    shm_handle ctrl_handle         = 0;

    if( unlikely( !shm_inited ) )
        return;

    if(
        unlikely(
            control_handle == 0
         || control_handle == CONTROL_HANDLE_INVALID
        )
      )
        return;

    // Store the control handle - this needs to be empty for local
    // initialization of that segment, after which we place it back to
    // initialize the regular segments.
    ctrl_handle    = control_handle;
    control_handle = 0;

    if(
        likely(
            _shm_wrapper(
                SHM_ATTACH,
                ctrl_handle,
                0,
                ( void ** ) &ctrl_header_address,
                ( size_t * ) &ctrl_header_size
            )
        )
      )
    {
        control_header      = ( ctrl_header * ) ctrl_header_address;
        control_header_size = ( size_t ) ctrl_header_size;
    }

    control_handle = ctrl_handle;

    _shm_log(
        LL_SHM_DEBUG,
        "Found control header %lu - %p (size %zu)",
        ( uint64_t ) control_handle,
        control_header,
        control_header_size
    );

    if( unlikely( !_shm_check_control( control_header ) ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Child initialized on an invalid"
            " control header in shared memory %lu",
            ( uint64_t ) control_handle
        );
        return;
    }

    return;
}

void * map_segment( shm_handle handle )
{
    void * mapped_address = NULL;
    size_t mapped_size    = 0;

    if(
        unlikely(
            handle == SEGMENT_HANDLE_INVALID
         || handle == CONTROL_HANDLE_INVALID
        )
      )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Map failed: INVALID handle"
        );
        return NULL;
    }

    if( unlikely( handle > SHM_MAX_SEGMENTS ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Map failed: handle is out-of-bounds"
        );
        return NULL;
    }

    // Already mapped
    if( __segment_lut[handle].handle == handle )
        return ( void * ) GET_USER_PTR( __segment_lut[handle].mapped_address );

    // Map an existing handle
    if(
        likely(
            _shm_wrapper(
                SHM_ATTACH,
                handle,
                0,
                ( void ** ) &mapped_address,
                ( size_t * ) &mapped_size
            )
        )
      )
    {
        if( likely( _shm_check_seg_owner( ( seg_header * ) mapped_address ) ) )
        {
            __segment_lut[handle].handle         = handle;
            __segment_lut[handle].mapped_address = mapped_address;
            __segment_lut[handle].mapped_size    = mapped_size;
            ( ( seg_header * ) mapped_address )->ref_count++;
            return ( void * ) GET_USER_PTR( mapped_address );
        }
        else
        {
            _shm_log(
                LL_SHM_ERROR,
                "Attempt to map invalid shared memory segment %lu",
                ( uint64_t ) handle
            );

            return NULL;
        }
    }

    _shm_log(
        LL_SHM_ERROR,
        "Shared memory segment %lu not found",
        ( uint64_t ) handle
    );

    return NULL;
}

void * new_segment( size_t size )
{
    shm_handle   new_handle     = ( shm_handle ) SEGMENT_HANDLE_INVALID;
    void *       mapped_address = NULL;
    size_t       mapped_size    = 0;
    size_t       real_size      = 0;
    seg_header * header         = NULL;

    if( unlikely( control_handle == CONTROL_HANDLE_INVALID ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Cannot create new segment: INVALID control handle"
        );
        return NULL;
    }

    if( unlikely( control_header == NULL ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Cannot create new segment: NULL control header"
        );
        return NULL;
    }

    // Begin critical section
    if( !__TNS_MUTEX( &(control_header->locked) ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Cannot create new segment: Failed to acquire control lock"
        );
        return NULL;
    }

    if(
            control_header->entry_count + 1 > SHM_MAX_SEGMENTS
         && ( control_header->entry_count + 1 > control_header->entry_count )
      )
    {
        __C_MUTEX( &(control_header->locked) );
        _shm_log(
            LL_SHM_ERROR,
            "Out of shared memory (max allocations %d made)",
            SHM_MAX_SEGMENTS
        );
        return NULL;
    }

    new_handle = control_header->entry_count;
    control_header->segments[control_header->entry_count] = new_handle;
    control_header->entry_count++;
    __C_MUTEX( &(control_header->locked) );

    // End critical section

    real_size = _round_to_multiple_of_page_size( size );

    if(
        likely(
            _shm_wrapper(
                SHM_CREATE,
                new_handle,
                real_size,
                ( void ** ) &mapped_address,
                ( size_t * ) &mapped_size
            )
        )
      )
    {
        __segment_lut[new_handle].mapped_address = mapped_address;
        __segment_lut[new_handle].mapped_size    = mapped_size;
        __segment_lut[new_handle].handle         = new_handle;

        header = ( seg_header * ) mapped_address;
        header->magic       = SEGMENT_HEADER_MAGIC;
        header->owner       = p_pid;
        header->locked      = false;
        header->entry_count = 0;
        header->ref_count   = 1;
        header->control     = control_handle;
#ifdef SHM_DEBUG
        _shm_log(
            LL_SHM_DEBUG,
            "Mapped new segment %lu at %p:\n"
            "Requested size: %zu\n"
            "real_size: %zu\n"
            "mapped_size: %zu\n"
            "User address: %p\n"
            "end address: %p",
            ( uint64_t ) new_handle,
            mapped_address,
            size,
            real_size,
            mapped_size,
            GET_USER_PTR( mapped_address ),
            _PTR_ADD_OFFSET( mapped_address, size )
        );
#endif // SHM_DEBUG

        return ( void * ) GET_USER_PTR( mapped_address );
    }

    _shm_log(
        LL_SHM_ERROR,
        "Failed to create new shm segment at %lu",
        ( uint64_t ) new_handle
    );

    return NULL;
}

static __inline__ shm_handle _get_handle_from_ptr( void * ptr )
{
    seg_header * header = NULL;
    shm_handle   handle = SEGMENT_HANDLE_INVALID;
    handle_iter  i      = 0;

    if( unlikely( ptr == NULL ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Failed to resolve handle from NULL pointer"
        );
        return ( shm_handle ) SEGMENT_HANDLE_INVALID;
    }

    if( unlikely( control_header == NULL ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Handle lookup failed: NULL control header"
        );
        return ( shm_handle ) SEGMENT_HANDLE_INVALID;
    }

    if( unlikely( control_handle == ( shm_handle ) CONTROL_HANDLE_INVALID ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Handle lookup failed: INVALID control handle"
        );
        return ( shm_handle ) SEGMENT_HANDLE_INVALID;
    }

    header = ( seg_header * ) GET_HDR_PTR( ptr );

    // TODO: Implement reverse lookup for header pointers (locally mapped) to shm_handle,
    // This is exhaustive but safe as we don't have to dereference the header pointer, just do
    // comparisons
    for( i = 0; i < ( handle_iter ) SHM_MAX_SEGMENTS; i++ )
    {
        if(
               ( void * ) __segment_lut[i].mapped_address == ( void * ) header
            || _PTR_BOUND_CHECK(
                   ptr,
                   __segment_lut[i].mapped_address,
                   __segment_lut[i].mapped_size
               )
          )
        {
            header = ( seg_header * ) __segment_lut[i].mapped_address;
            handle = __segment_lut[i].handle;
            break;
        }
    }

    if( unlikely( handle == SEGMENT_HANDLE_INVALID ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Address %p could not be resolved to a segment handle"
            " (was given %p)",
            header,
            ptr
        );
        return ( shm_handle ) SEGMENT_HANDLE_INVALID;
    }

    if( unlikely( header->magic != SEGMENT_HEADER_MAGIC ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Invalid header magic for segment %lu",
            ( uint64_t ) handle
        );
        return ( shm_handle ) SEGMENT_HANDLE_INVALID;
    }

    return handle;
}

shm_handle get_handle_from_ptr( void * ptr )
{
    return _get_handle_from_ptr( ptr );
}

void unmap_segment( void * ptr )
{
    seg_header * header = NULL;
    shm_handle   handle = SEGMENT_HANDLE_INVALID;

    handle = _get_handle_from_ptr( ptr );

    if( unlikely( handle == ( shm_handle ) SEGMENT_HANDLE_INVALID ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Unmap failed: Invalid handle returned for %p",
            ptr
        );
        return;
    }

    header = ( seg_header * ) GET_HDR_PTR( ptr );

    // Critical section - decrement ref count
    if( !__TNS_MUTEX( &(header->locked) ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Unmap failed: Failed to acquire lock on segment header"
        );
        return;
    }

    if( unlikely( header->ref_count < 2 ) )
    {
        __C_MUTEX( &(header->locked ) );
        _free_segment( handle );
        _shm_log(
            LL_SHM_DEBUG,
            "Segment %lu auto-freed due to low reference count",
            ( uint64_t ) handle
        );
        return;
    }
    else
    {
        header->ref_count--;
    }

    __C_MUTEX( &(header->locked) );
    // End critical section - decrement ref count
    // Now we can unmap
    if(
        likely(
            _shm_wrapper(
                SHM_DETACH,
                handle,
                0,
                ( void ** ) &__segment_lut[handle].mapped_address,
                ( size_t * ) &__segment_lut[handle].mapped_size
            )
        )
      )
    {
        __segment_lut[handle].handle         = SEGMENT_HANDLE_INVALID;
        __segment_lut[handle].mapped_address = NULL;
        __segment_lut[handle].mapped_size    = 0;

        return;
    }

    _shm_log(
        LL_SHM_ERROR,
        "Failed to unmap segment %lu",
        ( uint64_t ) handle
    );

    return;
}

static void _free_segment( shm_handle handle )
{
    // Internal - we're relying on things like the handle already being vetted
    // Critical section - free segment
    if( !__TNS_MUTEX( &(control_header->locked) ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "free segment failed: Failed to acquire control lock"
        );
        return;
    }

    control_header->segments[handle] = SEGMENT_HANDLE_INVALID;
    // XXX - may need to compactify the segments array to prevent fragmentation
    // The issue is we'll need a mechanism to locate __refs and update their
    // reference on a segment move

    __C_MUTEX( &(control_header->locked) );
    // End critical section - free segment

    if(
        likely(
            _shm_wrapper(
                SHM_DESTROY,
                handle,
                0,
                NULL,
                0
            )
        )
      )
    {
        return;
    }

    _shm_log(
        LL_SHM_ERROR,
        "Failed to invoke SHM_DESTROY on segment %lu",
        ( uint64_t ) handle
    );

    return;
}

void free_segment( void * ptr )
{
    seg_header * header = NULL;
    shm_handle   handle = SEGMENT_HANDLE_INVALID;

    handle = _get_handle_from_ptr( ptr );

    if( unlikely( handle == ( shm_handle ) SEGMENT_HANDLE_INVALID ) )
        return;

    // See if we're still mapped, otherwise we need to unmap first
    if( unlikely( __segment_lut[handle].handle == handle ) )
    {
        // Sanity check ref count
        header = ( seg_header * ) __segment_lut[handle].mapped_address;

        if( unlikely( header->ref_count >= 2 ) )
            return;

        unmap_segment( header );

        if( unlikely( __segment_lut[handle].handle != SEGMENT_HANDLE_INVALID ) )
            return;
    }

    _free_segment( handle );
    return;
}

// Unmaps all segments, including control
void unmap_all( void )
{
    shm_handle  ctrl           = ( shm_handle ) CONTROL_HANDLE_INVALID;
    shm_handle  seg            = ( shm_handle ) SEGMENT_HANDLE_INVALID;
    void *      mapped_address = NULL;
    handle_iter i              = 0;

    if( control_header == NULL )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Unmap all failed: NULL control header"
        );
        return;
    }

    if( !_shm_check_owner( control_header ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Unmap all failed: header does not pass ownership tests"
        );
        return;
    }

    for( i = 0; i < control_header->max_entries; i++ )
    {
        if( control_header->segments[i] == CONTROL_HANDLE_INVALID )
            continue;

        seg = control_header->segments[i];

        if( seg == SEGMENT_HANDLE_INVALID )
            continue;

        mapped_address = ( void * ) __segment_lut[seg].mapped_address;

        if( mapped_address == NULL )
            continue;

        unmap_segment( mapped_address );

        __segment_lut[seg].mapped_address = NULL;
        __segment_lut[seg].mapped_size    = 0;
        __segment_lut[seg].handle         = SEGMENT_HANDLE_INVALID;
    }

    ctrl           = control_handle;
    control_handle = CONTROL_HANDLE_INVALID;

    if(
        !_shm_wrapper(
            SHM_DETACH,
            ctrl,
            0,
            NULL,
            NULL
        )
      )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Failed to detach control handle %lu",
            ( uint64_t ) ctrl
        );
    }

    control_handle      = CONTROL_HANDLE_INVALID;
    control_header      = NULL;
    control_header_size = 0;
    shm_inited          = false;

    return;
}

void zero_segment( shm_handle segment )
{
    if( unlikely( __segment_lut[segment].mapped_address == NULL ) )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Cannot zero-fill unmapped segment %lu",
            ( uint64_t ) segment
        );
        errno = EINVAL;
        return;
    }

    memset(
        ( void * ) GET_USER_PTR(
            __segment_lut[segment].mapped_address
        ),
        0,
        __segment_lut[segment].mapped_size - offsetof( seg_header, data )
    );

    return;
}

size_t get_segment_size( shm_handle segment )
{
    if( unlikely( segment > SHM_MAX_SEGMENTS ) )
        return 0;
    if( unlikely( __segment_lut[segment].mapped_address == NULL ) )
        return 0;

    return __segment_lut[segment].mapped_size - offsetof( seg_header, data ); 
}

void map_all( void )
{
    uint32_t seg_index = 0;

    if( control_header == NULL || control_handle == CONTROL_HANDLE_INVALID )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Cannot map_all - control handle is empty"
        );
        return;
    }

    for( seg_index = 0; seg_index < control_header->max_entries; seg_index++ )
    {
        if(
               __segment_lut[seg_index].handle == SEGMENT_HANDLE_INVALID
            && control_header->segments[seg_index] != SEGMENT_HANDLE_INVALID
          )
        {
            if( map_segment( ( shm_handle ) seg_index ) == NULL )
            {
                _shm_log(
                    LL_SHM_ERROR,
                    "Failed to map segment handle %u from control handle %lu",
                    seg_index,
                    ( uint64_t ) control_handle
                );
            }
        }
    }

    return;
}

static bool _shm_wrapper(
    shm_op     op,
    shm_handle handle,
    size_t     size,
    void **    mapped_address,
    size_t *   mapped_size
)
{
#ifdef SHM_DEBUG
    _shm_log(
        LL_SHM_DEBUG,
        "Entry: _shm_wrapper( %s, %lu, %zu, %p, %zu ) CTRL:%s (%lu) ",
        op == SHM_ATTACH ? "ATTACH" :
        op == SHM_CREATE ? "CREATE" :
        op == SHM_DETACH ? "DETACH" :
        op == SHM_DESTROY ? "DESTROY" : "INVALID",
        ( uint64_t ) handle,
        size,
        mapped_address != NULL ? *mapped_address : NULL,
        mapped_size != NULL ? *mapped_size : 0,
        control_handle == CONTROL_HANDLE_INVALID ? " INVALID" : "",
        ( uint64_t ) control_handle
    );
#endif // SHM_DEBUG

#ifdef SHM_USE_POSIX
    return _shm_posix( op, handle, size, mapped_address, mapped_size );
#endif // SHM_USE_POSIX
#ifdef SHM_USE_MMAP
    return _shm_mmap( op, handle, size, mapped_address, mapped_size );
#endif // SHM_USE_MMAP
#ifdef SHM_USE_SYSV
    return _shm_sysv( op, handle, size, &sysv_private, mapped_address, mapped_size );
#endif // SHM_USE_SYSV
    return false;
}

#ifdef SHM_USE_MMAP
static bool _shm_mmap(
    shm_op     op,
    shm_handle handle,
    size_t     size,
    void **    mapped_address,
    size_t *   mapped_size
)
{
    char        name[SHM_ID_NAME_SIZE] = {0};
    int         flags                  = 0;
    int         save_errno             = 0;
    int         descriptor             = 0;
    struct stat statbuff               = {0};
    char *      address                = NULL;

    if( mapped_address == NULL || mapped_size == NULL )
    {
        errno = EINVAL;
        return false;
    }

    snprintf(
        name,
        SHM_ID_NAME_SIZE,
        "%s/%s%lu.%lu",
        SHM_FILE_MMAP_DIR,
        SHM_FILE_MMAP_PREFIX,
        ( uint64_t ) ( control_handle == CONTROL_HANDLE_INVALID ) ? 0 : control_handle,
        ( uint64_t ) handle
    );

    if( op == SHM_DETACH || op == SHM_DESTROY )
    {
        save_errno = errno;

        if(
              *mapped_address != NULL
           && munmap( *mapped_address, *mapped_size ) != 0
          )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Failed to unmap shared memory segment %s: %s",
                name,
                strerror( errno )
            );
            errno = save_errno;
            return false;
        }

        *mapped_address = NULL;
        *mapped_size    = 0;

        if( op == SHM_DESTROY && unlink( name ) != 0 )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Failed to remove shared memory segment %s: %s",
                name,
                strerror( errno )
            );
            errno = save_errno;
            return false;
        }

        return true;
    }

    flags = O_RDWR;

    if( op == SHM_CREATE )
    {
        flags |= O_CREAT | O_EXCL;
    }

    save_errno = errno;
    descriptor = open( name, flags, SHM_FILE_PERMS );

    if( descriptor < 0 )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Failed to create shared memory segment %s: %s",
            name,
            strerror( errno )
        );
        errno = save_errno;
        return false;
    }

    if( op == SHM_ATTACH )
    {
        if( fstat( descriptor, &statbuff ) != 0 )
        {
            _close_segment_descriptor( descriptor, name, false );

            _shm_log(
                LL_SHM_ERROR,
                "Failed to stat shared memory segment %s: %s",
                name,
                strerror( errno )
            );
            return false;
        }

        if( statbuff.st_size < size )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Mismatch in shared memory segment %s. Loaded %zu, expected %zu",
                name,
                statbuff.st_size,
                size
            );
            _close_segment_descriptor( descriptor, name, false );
            return false;
        }

        size = statbuff.st_size;
    }
    else if( _shm_mmap_resize( descriptor, size ) != 0 )
    {
        _close_segment_descriptor( descriptor, name, true );
        _shm_log(
            "Failed to resize shared memory segment %s to %zu bytes: %s",
            name,
            size,
            strerror( errno )
        );
        return false;
    }

    address = ( char * ) mmap(
        NULL,
        size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MMAP_FLAGS,
        descriptor,
        0
    );

    if( unlikely( address == MAP_FAILED ) )
    {
        if( op == SHM_CREATE )
            _close_segment_descriptor( descriptor, name, true );
        else
            _close_segment_descriptor( descriptor, name, false );

        _shm_log(
            "Could not map shared memory segmnet %s: %s",
            name,
            strerror( errno )
        );

        return false;
    }

    *mapped_address = ( void * ) address;
    *mapped_size    = size;

    if( !_close_segment_descriptor( descriptor, name, false ) )
        return false;

    return true;
}

static int _shm_mmap_resize( int descriptor, size_t size )
{
    char *      zero_buffer = NULL;
    uint32_t    remaining   = 0;
    size_t      goal        = 0;
    size_t      written     = 0;
    bool        success     = false;

    /*
     * Fill the file with zeros. We want to do this ahead of time to ensure
     * that the space has actually been allocated. In a similare vein to the
     * prevention of SIGBUS some time after the initial allocation, this
     * page-zeroing prevents an errant SIGSEGV from occuring if an
     * unallocated portion of the mapping is accessed later.
     */
    zero_buffer = ( char * ) calloc( ZERO_BUFFER_SIZE, 1 );
    remaining   = ( uint32_t ) size;
    success     = true;

    if( unlikely( zero_buffer == NULL ) )
    {
        errno = ENOMEM;
        return -1;
    }

    while( success && remaining > 0 )
    {
        goal = ( size_t ) remaining;

        if( goal > ZERO_BUFFER_SIZE )
            goal = ZERO_BUFFER_SIZE;
        errno = 0;
        do {
            written = write( descriptor, zero_buffer, goal );
        } while( errno == EINTR );

        if( written == goal )
            remaining -= goal;
        else
            success = false;
    }

    free( zero_buffer );

    if( !success )
    {
        if( errno == 0 )
            errno = ENOSPC;
        return -1;
    }

    return 0;
}
#endif // SHM_USE_MMAP

#ifdef SHM_USE_SYSV
static bool _shm_sysv(
    shm_op     op,
    shm_handle handle,
    size_t     size,
    void **    private,
    void **    mapped_address,
    size_t *   mapped_size
)
{
    key_t           key                    = 0;
    int             save_errno             = 0;
    int             flags                  = 0;
    int             identifier             = 0;
    int *           identifier_cache       = NULL;
    char *          address                = NULL;
    char            name[SHM_ID_NAME_SIZE] = {0};
    size_t          segment_size           = 0;
    struct shmid_ds shm                    = {{0}};

    if( private == NULL )
    {
        errno = EINVAL;
        return false;
    }

    snprintf(
        name,
        SHM_ID_NAME_SIZE,
        "%lu.%lu",
        ( uint64_t ) ( control_handle == CONTROL_HANDLE_INVALID ) ? 0 : control_handle,
        ( uint64_t ) handle
    );

    // Type coersion may involve truncation, so we
    // consistently 'fix' the converted value here
    key = ( key_t ) handle;
    if( key < 1 )
        key = -key;
    if( key == IPC_PRIVATE )
    {
        if( op != SHM_CREATE )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Use of restrictred handle resolved to SystemV "
                "IPC_PRIVATE flag"
            );
        }
        errno = EEXIST;
        return false;
    }

    if( *private == NULL )
    {
        flags = SHM_FILE_OCTAL;

        if( op == SHM_CREATE )
        {
            flags |= IPC_CREAT | IPC_EXCL;
            segment_size = size;
        }

        identifier_cache = ( int * ) calloc( 1, sizeof( int ) );

        if( identifier_cache == NULL )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Failed to allocate identifier cache"
            );
            return false;
        }

        identifier = shmget( key, segment_size, flags );

        if( identifier == -1 )
        {
            if( errno != EEXIST )
            {
                save_errno = errno;
                free( identifier_cache );
                _shm_log(
                    LL_SHM_ERROR,
                    "Failed to get shared memory segment %s: %s",
                    name,
                    strerror( errno )
                );

                return false;
            }
        }

        *identifier_cache = identifier;
        *private          = ( void * ) identifier_cache;
    }
    else
    {
        identifier_cache = ( int * ) *private;
        identifier       = ( int ) *identifier_cache;
    }

    if( op == SHM_DESTROY || op == SHM_DETACH )
    {
        // Cleanup previously allocated ID cache
        save_errno = errno;
        if( identifier_cache != NULL )
        {
            free( identifier_cache );
            *private = NULL;
        }

        if( *mapped_address != NULL && shmdt( *mapped_address ) != 0 )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Could not unmap shared memory segment %s: %s",
                name,
                strerror( errno )
            );
            errno = save_errno;
            return false;
        }

        *mapped_address = NULL;
        *mapped_size    = 0;

        if( op == SHM_DESTROY )
        {
            if( shmctl( identifier, IPC_RMID, NULL ) < 0 )
            {
                _shm_log(
                    LL_SHM_ERROR,
                    "Could not remove shared memory segment %s: %s",
                    name,
                    strerror( errno )
                );
                errno = save_errno;
                return false;
            }

            return true;
        }
    }

    if( op == SHM_ATTACH )
    {
        if( shmctl( identifier, IPC_STAT, &shm ) != 0 )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Failed to stat shared memory segment %s: %s",
                name,
                strerror( errno )
            );

            return false;
        }

        size = shm.shm_segsz;
    }

    address = shmat( identifier, NULL, SYSV_SHM_FLAGS );

    if( unlikely( ( void * ) address == ( void * ) -1 ) )
    {
        save_errno = errno;

        if( op == SHM_CREATE )
        {
            shmctl( identifier, IPC_RMID, NULL );
        }

        errno = save_errno;
        _shm_log(
            LL_SHM_ERROR,
            "Failed to map shared memory segment %s: %s",
            name,
            strerror( errno )
        );

        return false;
    }

    *mapped_address = ( void * ) address;
    *mapped_size    = size;

    return true;
}
#endif // SHM_USE_SYSV

#ifdef SHM_USE_POSIX
static bool _shm_posix(
    shm_op     op,
    shm_handle handle,
    size_t     size,
    void **    mapped_address,
    size_t *   mapped_size
)
{
    char        name[SHM_ID_NAME_SIZE] = {0};
    int         flags                  = 0;
    int         descriptor             = 0;
    int         save_errno             = 0;
    struct stat statbuff               = {0};
    char *      address                = NULL;

    snprintf(
        name,
        SHM_ID_NAME_SIZE,
        "/%s%lu.%lu",
        SHM_FILE_POSIX_PREFIX,
        ( uint64_t ) ( control_handle == CONTROL_HANDLE_INVALID
            ? 0 : control_handle ),
        ( uint64_t ) handle
    );

    if( op == SHM_DESTROY || op == SHM_DETACH )
    {
        if(
               mapped_address != NULL
            && mapped_size != NULL
            && *mapped_address != NULL
            && munmap( *mapped_address, *mapped_size ) != 0
          )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Cannot unmap %p from %s (%lu)",
                mapped_address,
                name,
                ( uint64_t ) handle
            );
            return false;
        }

        // Clear iff provided
        if( mapped_address != NULL )
            *mapped_address = NULL;

        if( mapped_size != NULL )
            *mapped_size = 0;

        if( op == SHM_DESTROY && shm_unlink( name ) != 0 )
        {
            return false;
        }

        return true;
    }

    flags = O_RDWR;

    if( op == SHM_CREATE )
    {
        // Generate an all-or-nothing page
        flags |= O_CREAT | O_EXCL;
    }

    save_errno = errno;
    errno      = 0;
    descriptor = shm_open( name, flags, SHM_FILE_PERMS );

    _shm_log(
        LL_SHM_DEBUG,
        "Opened file %s: descriptor %d",
        name,
        descriptor
    );

    if( descriptor == -1 )
    {
        if( errno != EEXIST )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Failed to open shared memory segment %s: %s",
                name,
                strerror( errno )
            );
        }

        _shm_log(
            LL_SHM_DEBUG,
            "Got errno %d: '%s' on file open",
            errno,
            strerror( errno )
        );

        return false;
    }

    if( op == SHM_ATTACH )
    {
        if( fstat( descriptor, &statbuff ) != 0 )
        {
            _close_segment_descriptor( descriptor, name, false );
            _shm_log(
                LL_SHM_ERROR,
                "Failed to stat shared memory segment %s: %s",
                name,
                strerror( errno )
            );
            return false;
        }

        if( size > statbuff.st_size )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Mismatch in shared memory segment %s: loaded %zu bytes, expected %zu bytes",
                name,
                statbuff.st_size,
                size
            );
            _close_segment_descriptor( descriptor, name, false );
            return false;
        }

        size = statbuff.st_size;
    }
    else if( _shm_posix_resize( descriptor, size ) != 0 )
    {
        _close_segment_descriptor( descriptor, name, false );
        _shm_log(
            LL_SHM_ERROR,
            "Failed to resize shared memory segment %s to %zu bytes: %s",
            name,
            size,
            strerror( errno )
        );
        return false;
    }

    address = ( char * ) mmap(
        NULL,
        size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MMAP_FLAGS,
        descriptor,
        0
    );

    if( address == MAP_FAILED )
    {
        save_errno = errno;
        _close_segment_descriptor( descriptor, name, false );

        if( op == SHM_CREATE )
        {
            shm_unlink( name );
        }

        errno = save_errno;
        _shm_log(
            LL_SHM_ERROR,
            "failed to map shared memory segment %s: %s",
            name,
            strerror( errno )
        );
        return false;
    }

    *mapped_address = ( void * ) address;
    *mapped_size = size;
    _shm_log(
        LL_SHM_DEBUG,
        "Mapped %s (%lu) to %p (size %zu)",
        name,
        ( uint64_t ) handle,
        ( void * ) *mapped_address,
        ( size_t ) *mapped_size
    );

    _close_segment_descriptor( descriptor, name, false );

    return true;
}

static int _shm_posix_resize( int descriptor, size_t size )
{
    int ret = 0;

    /*
     * Handle the case where shm_open is backed by tmpfs. When the size is
     * extended, a hole may occur. When this hole is accessed later - tmpfs
     * will attempt to allocate memory or page in stuff. If we run out of
     * space, this may cause a (very) unexpected SIGBUS. To prevent this,
     * we zero out the file up front, exchanging hard-to-trace SIGBUS with
     * a ENOSPC. This is considered best practice, but oddly enough, is
     * mentioned only in passing in the BSD manpages.
     */

    if( ret == 0 )
    {
        do {
            ret = posix_fallocate( descriptor, 0, size );
        } while( ret == EINTR );

        errno = ret;
    }

    return ret;
}
#endif // SHM_USE_POSIX

#if defined( SHM_USE_POSIX ) || defined( SHM_USE_MMAP )
static bool _close_segment_descriptor( int descriptor, char * name, bool do_unlink )
{
    int save_errno = 0;

    save_errno = errno;

    if( close( descriptor ) != 0 )
    {
        _shm_log(
            LL_SHM_ERROR,
            "Failed to close shared memory segment %s: %s",
            name,
            strerror( errno )
        );
        errno = save_errno;
        return false;
    }

    if( do_unlink && name != NULL )
    {
        errno = 0;
        if( unlink( name ) != 0 )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Failed to remove shared memory segment %s: %s",
                name,
                strerror( errno )
            );
            errno = save_errno;
            return false;
        }
    }

    errno = save_errno;
    return true;
}
#endif // SHM_USE_POSIX || SHM_USE_MMAP

static size_t _get_ctrl_header_size( uint32_t seg_count )
{
    uint64_t ret = 0;
    // Calculate the number of bytes needed to store given segments

    ret = offsetof( ctrl_header, segments )
        + ( sizeof( shm_handle ) * seg_count ); // Segment array

    return ( size_t ) ret;
}

static size_t _round_to_multiple_of_page_size( size_t size )
{
    size_t page_size = 0;
    size_t result    = 0;

    // Round a given size to a multiple of the system page size, including
    // header overhead
    page_size = _get_system_page_size();
    result    = page_size * (
        ( ( size + offsetof( seg_header, data ) ) / page_size ) + 1
    );
    return result;
}

static size_t _get_system_page_size( void )
{
#ifdef __linux__
    return ( size_t ) sysconf( _SC_PAGESIZE );
#endif // __linux__
#if defined( __FreeBSD__ ) || defined( __APPLE__ ) || defined( __unix__ )
    return ( size_t ) getpagesize();
#endif // __FreeBSD__ || __APPLE__ || __unix__
    return ( size_t ) DEFAULT_PAGE_SIZE;
}

static inline bool _shm_check_owner( ctrl_header * header )
{
    if( unlikely( !_shm_check_control( header ) ) )
#ifdef SHM_DEBUG
    {
        _shm_log(
            LL_SHM_ERROR,
            "Control header failed sanity checks"
        );
#endif // SHM_DEBUG
        return false;
#ifdef SHM_DEBUG
    }

#endif // SHM_DEBUG
    if( likely( header->owner == getpid() || header->owner == getppid() ) )
        return true;

#ifdef SHM_DEBUG
    _shm_log(
        LL_SHM_ERROR,
        "Control header (%d) is not owned by this PID (%d) parent %d",
        header->owner,
        getpid(),
        getppid()
    );
#endif // SHM_DEBUG
    return false;
}

static inline bool _shm_check_control( ctrl_header * header )
{
    if( unlikely( header == NULL ) )
#ifdef SHM_DEBUG
    {
        _shm_log(
            LL_SHM_ERROR,
            "Control header check failed:"
            " Header pointer is null"
        );
#endif // SHM_DEBUG
        return false;
#ifdef SHM_DEBUG
    }
#endif // SHM_DEBUG
    if( unlikely( header->magic != CONTROL_HEADER_MAGIC ) )
#ifdef SHM_DEBUG
    {
        _shm_log(
            LL_SHM_ERROR,
            "Control header check failed:"
            " Bad magic %x in header, %x in macro\n",
            header->magic,
            CONTROL_HEADER_MAGIC
        );
#endif // SHM_DEBUG
        return false;
#ifdef SHM_DEBUG
    }
#endif // SHM_DEBUG
    if( unlikely( header->entry_count > header->max_entries ) )
#ifdef SHM_DEBUG
    {
        _shm_log(
            LL_SHM_ERROR,
            "Control header check failed:"
            " entry count (%lu) exceeds max entries (%lu)",
            ( uint64_t ) header->entry_count,
            ( uint64_t ) header->max_entries
        );
#endif // SHM_DEBUG
        return false;
#ifdef SHM_DEBUG
    }
#endif // SHM_DEBUG

    return true;
}

static inline bool _shm_check_seg_owner( seg_header * header )
{
    if( unlikely( !_shm_check_segment( header ) ) )
        return false;
    if( likely( header->owner == getpid() || header->owner == getppid() ) )
        return true;

    return false;
}

static inline bool _shm_check_segment( seg_header * header )
{
    if( unlikely( header == NULL ) )
        return false;
    if( unlikely( header->magic != SEGMENT_HEADER_MAGIC ) )
        return false;
    if( unlikely( header->control != control_handle ) )
        return false;
    return true;
}

static void _append_to_cleanup_list( shm_handle ctrl_handle )
{
    if( cleanup_list == NULL )
    {
        cleanup_list = ( shm_handle * ) calloc( sizeof( shm_handle ), 1 );

        if( cleanup_list == NULL )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Failed to allocate prune list for old segments"
            );
            errno = ENOSPC;
            return;
        }
    }
    else
    {
        cleanup_list = ( shm_handle * ) realloc(
            ( void * ) cleanup_list,
            sizeof( shm_handle ) * ( cleanup_list_len + 1 )
        );

        if( cleanup_list == NULL )
        {
            _shm_log(
                LL_SHM_ERROR,
                "Failed to resize prune list for old segments"
            );
            errno = ENOSPC;
            return;
        }
    }

    cleanup_list[cleanup_list_len] = ctrl_handle;
    cleanup_list_len++;
    return;
}

static void _cleanup_old_segments( void )
{
    shm_handle    current_seg    = ( shm_handle ) SEGMENT_HANDLE_INVALID;
    shm_handle    current_ctrl   = ( shm_handle ) CONTROL_HANDLE_INVALID;
    shm_handle    save_ctrl      = ( shm_handle ) CONTROL_HANDLE_INVALID;
    void *        mapped_address = NULL;
    ctrl_header * header         = NULL;
    uint16_t      i              = 0;
    uint16_t      j              = 0;
    size_t        mapped_size    = 0;
    int           save_errno     = 0;
    pid_t         owner_pid      = 0;
    bool          can_remove     = false;

    if( cleanup_list == NULL || cleanup_list_len == 0 )
        return;

    save_ctrl = control_handle;

    for( i = 0; i < cleanup_list_len; i++ )
    {
        control_handle = CONTROL_HANDLE_INVALID;
        current_ctrl = cleanup_list[i];
        _shm_log(
            LL_SHM_DEBUG,
            "Performing cleanup for %lu",
            ( uint64_t ) current_ctrl
        );

        if(
            likely(
                _shm_wrapper(
                    SHM_ATTACH,
                    current_ctrl,
                    0,
                    ( void ** ) &mapped_address,
                    ( size_t * ) &mapped_size
                )
            )
          )
        {
            can_remove = false;
            if( unlikely( mapped_address == NULL ) )
            {
                _shm_log(
                    LL_SHM_ERROR,
                    "Mapping failed for control segment %lu",
                    ( uint64_t ) current_ctrl
                );
                continue;
            }

            header = ( ctrl_header * ) mapped_address;

            if( header->magic != CONTROL_HEADER_MAGIC )
            {
                // Doesn't belong to pg_ctblmgr?
                _shm_log(
                    LL_SHM_ERROR,
                    "Bad magic, expected %x, got %x",
                    CONTROL_HEADER_MAGIC,
                    header->magic
                );
                continue;
            }

            owner_pid = header->owner;

            if( owner_pid <= 1 )
            {
                _shm_log(
                    LL_SHM_ERROR,
                    "Invalid pid %d in stale control handle",
                    owner_pid
                );
                continue;
            }

#ifdef _POSIX_C_SOURCE
            // Indicates we have access to kill
            save_errno = errno;

            if( kill( owner_pid, 0 ) < 0 )
            {
                _shm_log(
                    LL_SHM_DEBUG,
                    "Kill( 0 ) to PID %u gave %s",
                    owner_pid,
                    strerror( errno )
                );

                if( errno != ESRCH )
                    continue;

                can_remove = true;
            }

            errno = save_errno;
#endif // _POSIX_C_SOURCE
            if( can_remove )
            {
                control_handle = current_ctrl;
                _shm_log(
                    LL_SHM_DEBUG,
                    "Pruning segments belonging to control handle %lu",
                    ( uint64_t ) current_ctrl
                );
#ifdef SHM_DEBUG
                __dump_ctrl_header( header );
#endif // SHM_DEBUG
                for( j = 0; j <  header->max_entries; j++ )
                {
                    current_seg = header->segments[j];

                    if( current_seg == SEGMENT_HANDLE_INVALID )
                        continue;

                    if(
                        unlikely(
                            !_shm_wrapper(
                                SHM_DESTROY,
                                current_seg,
                                0,
                                NULL,
                                NULL
                            )
                        )
                      )
                    {
                        _shm_log(
                            LL_SHM_ERROR,
                            "Failed to destroy stale segment %lu"
                            " for control handle %lu",
                            ( uint64_t ) current_seg,
                            ( uint64_t ) control_handle
                        );
                        continue;
                    }
                }

                // Done removing segments, unmap & remove control handle
                // we invalidate the control handle to represent uninitialized state
                control_handle = CONTROL_HANDLE_INVALID;
                if(
                    unlikely(
                        !_shm_wrapper(
                            SHM_DETACH,
                            current_ctrl,
                            0,
                            NULL,
                            NULL
                        )
                    )
                  )
                {
                    _shm_log(
                        LL_SHM_ERROR,
                        "Failed to detach control segment %lu after cleanup",
                        ( uint64_t ) current_ctrl
                    );

                    continue;
                }

                if(
                    unlikely(
                        !_shm_wrapper(
                            SHM_DESTROY,
                            current_ctrl,
                            0,
                            NULL,
                            NULL
                        )
                    )
                  )
                {
                    _shm_log(
                        LL_SHM_ERROR,
                        "Failed to destroy control segment %lu after cleanup",
                        ( uint64_t ) current_ctrl
                    );

                    continue;
                }

            }
        }
        else
        {
            _shm_log(
                LL_SHM_ERROR,
                "Failed to attach control segment %lu for inspection",
                ( uint64_t ) current_ctrl
            );
        }
    }

    control_handle = save_ctrl;

    free( cleanup_list );

    cleanup_list_len = 0;
    cleanup_list     = NULL;

    return;
}

#ifdef SHM_DEBUG
static void __dump_ctrl_header( ctrl_header * header )
{
    handle_iter i = 0;

    if( header == NULL )
        return;
    _shm_log(
        LL_SHM_DEBUG,
        "Header data:\n  " \
          "MAGIC: %x\n  " \
          "OWNER: %d\n  " \
          "LOCKED: %s\n  " \
          "ENTRY_COUNT: %lu\n  " \
          "MAX_ENTRIES: %lu\n  " \
          "SEGMENTS[]:",
        header->magic,
        header->owner,
        header->locked ? "TRUE" : "FALSE",
        ( uint64_t ) header->entry_count,
        ( uint64_t ) header->max_entries
    );

    for( i = 0; i < header->max_entries; i++ )
    {
        if( header->segments[i] == SEGMENT_HANDLE_INVALID )
            continue;
        _shm_log(
            LL_SHM_DEBUG,
            "    [%lu]: %lu",
            ( uint64_t ) i,
            ( uint64_t ) header->segments[i]
        );
    }

    return;
}

static void __dump_seg_header( seg_header * header )
{
    if( header == NULL )
        return;

    _shm_log(
        LL_SHM_DEBUG,
        "Header data:\n  " \
          "MAGIC: %x\n  " \
          "OWNER: %d\n  " \
          "LOCKED: %s\n  " \
          "ENTRY_COUNT: %u\n  " \
          "REF_COUNT: %u\n  " \
          "CONTROL: %lu\n  " \
          "DATA: %p",
        header->magic,
        header->owner,
        header->locked ? "TRUE" : "FALSE",
        header->entry_count,
        header->ref_count,
        ( uint64_t ) header->control,
        header->data
    );

    return;
}
#endif // SHM_DEBUG

ctrl_header * get_control_header( void )
{
    ctrl_header * header = NULL;
    header = control_header;

    return header;
}

__inline__ bool ref_is_null( __ref ref )
{
    return ( _ref_get_segment( ref ) == SEGMENT_HANDLE_INVALID );
}

__inline__ __ref get_null_ref( void )
{
    __ref nullref = {0};
    return ref_set_segment( nullref, SEGMENT_HANDLE_INVALID );
}

#ifdef SHM_ENABLE_RUNTIME_SANITY_CHECK
static bool directionality_check( void )
{
    bool       is_up  = false;
    uint64_t   a      = 0;
    uint64_t * heap_a = NULL;
    uint64_t * heap_b = NULL;

    is_up = _dir_check_b( &a );

    heap_a = ( uint64_t * ) malloc( sizeof( uint64_t ) );
    heap_b = ( uint64_t * ) malloc( sizeof( uint64_t ) );
    if( heap_a == NULL || heap_b == NULL )
    {
        if( heap_a != NULL )
            free( heap_a );

        if( heap_b != NULL )
            free( heap_b );
        return false;
    }

    // just need the addresses
    free( heap_a );
    free( heap_b );

    // Emit some warnings if the reality of our allocation situation
    // does not align with our compiled flags. Incorrect assumptions
    // can lead to bad pointer math
#ifdef STACK_GROWS_DOWNWARD
    if( is_up )
    {
        _shm_log(
            LL_SHM_ERROR,
            "ERROR: Stack growth detected as growing upward, but "
            "program compiled with STACK_GROWS_DOWNWARD"
        );
        return false;
    }

    if( heap_a > heap_b )
    {
        _shm_log(
            LL_SHM_ERROR,
            "ERROR: Heap growth detected as growing downward, but "
            "program compiled with STACK_GROWS_DOWNWARD\n"
            "(This conflicts with stack growth direction by convention)"
        );
        return false;
    }
#else
    if( !is_up )
    {
        _shm_log(
            LL_SHM_ERROR,
            "ERROR: Stack growth detected as growing downward, but "
            "program wasn't compiled with STACK_GROWS_DOWNWARD"
        );
        return false;
    }

    if( heap_a < heap_b )
    {
        _shm_log(
            LL_SHM_ERROR,
            "ERROR: Heap growth detected as growing upward, but "
            "program wasn't compiled with STACK_GROWS_DOWNWARD\n"
            "(This conflicts with stack growth direction by convention)"
        );
        return false;
    }
#endif // STACK_GROWS_DOWNWARD

    return true;
}

static bool _dir_check_b( uint64_t * a )
{
    uint64_t b = 0;

    if( a < &b )
        return true;

    return false;
}
#endif // SHM_ENABLE_RUNTIME_SANITY_CHECK

static void _shm_log( shm_ll log_level, char * message, ... )
{
    va_list        args          = {{0}};
    FILE *         output_handle = NULL;
    struct timeval tv            = {0};
    char           buff_time[28] = {0};

    if( unlikely( message == NULL ) )
        return;

#ifndef SHM_DEBUG
    if( log_level == LL_SHM_DEBUG )
        return;
#endif // !SHM_DEBUG

    gettimeofday( &tv, NULL );

    strftime(
        buff_time,
        sizeof( buff_time ) / sizeof( *buff_time ),
        "%Y-%m-%d %H:%M:%S",
        gmtime( &tv.tv_sec )
    );

    output_handle = stdout;
    if( log_level == LL_SHM_ERROR )
        output_handle = stderr;

    va_start( args, message );

    fprintf(
        output_handle,
        "%s.%05d [%d] %s: ",
        buff_time,
        ( int ) ( tv.tv_usec / 1000 ),
        getpid(),
        log_level == LL_SHM_DEBUG ?
            "DEBUG" : log_level == LL_SHM_ERROR ?
            "ERROR" : "INFO"
    );

    vfprintf(
        output_handle,
        message,
        args
    );

    fprintf(
        output_handle,
        "\n"
    );
    return;
}
