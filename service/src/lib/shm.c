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
static size_t _round_to_multiple_of_page_size( size_t ) __attribute__((unused));
static size_t _get_ctrl_header_size( uint32_t );
static inline shm_handle _get_handle_from_ptr( void * ) __attribute__((always_inline));
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

static shm_handle    control_handle                  = ( shm_handle ) CONTROL_HANDLE_INVALID;
static ctrl_header * control_header                  = NULL;
static size_t        control_header_size             = 0;
static pid_t         p_pid                           = ( pid_t ) 0;
static bool          shm_inited                      = false;
static shm_segment   __segment_lut[SHM_MAX_SEGMENTS] = {{0}}; // Given a segment ID, lets us get the mapping info
static shm_handle *  cleanup_list                    = NULL;
static uint16_t      cleanup_list_len                = 0;

inline void * get_ptr( __ref ref )
{
    void * mapped_address = NULL;
    void * ret            = NULL;
    size_t offset         = 0;
    size_t mapped_size    = 0;

    // Check that segment is initialized and valid
    if( unlikely( ref._segment == SEGMENT_HANDLE_INVALID ) )
        return NULL;
    // Check that the requested segment is within the bounds of the LUT
    if( unlikely( ref._segment > ( shm_handle ) SHM_MAX_SEGMENTS ) )
        return NULL;

    offset         = ref._offset;
    mapped_address = __segment_lut[ref._segment].mapped_address;

    if( unlikely( mapped_address == NULL ) )
    {
        // Segment not mapped
#ifdef SHM_DEBUG
        fprintf(
            stderr,
            "get_ptr() attempting to map a segment %lu\n", ref._segment
        );
#endif // SHM_DEBUG
        if( unlikely( map_segment( ref._segment ) == NULL ) )
            return NULL;
        mapped_address = __segment_lut[ref._segment].mapped_address;
    }

    mapped_size = __segment_lut[ref._segment].mapped_size;
    ret         = _PTR_ADD_OFFSET( GET_USER_PTR( mapped_address ), offset );

    // Check that our computed address remains within the bounds of the page
    if( unlikely( !_PTR_BOUND_CHECK( ret, mapped_address, mapped_size ) ) )
        return NULL;

    return ret;
}

inline __ref get_ref( void * ptr )
{
    __ref       ret    = {0};
    shm_handle  handle = 0;

    ret._segment = SEGMENT_HANDLE_INVALID;
    if( unlikely( ptr == NULL ) )
        return ret;

    handle = _get_handle_from_ptr( ptr );
    if( unlikely( handle == SEGMENT_HANDLE_INVALID ) )
#ifdef SHM_DEBUG
    {
        fprintf(
            stderr,
            "Failed to find mapped segment containing %p\n",
            ptr
        );
#endif // SHM_DEBUG
        return ret;
#ifdef SHM_DEBUG
    }
#endif //SHM_DEBUG
    if(
        unlikely(
            !_PTR_BOUND_CHECK(
                ptr,
                __segment_lut[handle].mapped_address,
                __segment_lut[handle].mapped_size
            )
        )
      )
#ifdef SHM_DEBUG
    {
        fprintf(
            stderr,
            "Pointer failed bounds check:\n  %p not in range for\n  %p of size %zu\n",
            ptr,
            __segment_lut[handle].mapped_address,
            __segment_lut[handle].mapped_size
        );
#endif // SHM_DEBUG
        return ret;
#ifdef SHM_DEBUG
    }
#endif // SHM_DEBUG

    if( unlikely( __segment_lut[handle].mapped_address == NULL ) )
#ifdef SHM_DEBUG
    {
        fprintf(
            stderr,
            "Mapped address for segment handle %lu is null\n",
            handle
        );
#endif // SHM_DEBUG
        return ret;
#ifdef SHM_DEBUG
    }
#endif // SHM_DEBUG
    // TODO
    // Figure out (more efficiently) shm_handle by base address??
    ret._offset  = ( size_t ) ( _PTR_GET_OFFSET( GET_USER_PTR( __segment_lut[handle].mapped_address ), ptr ) );
    ret._segment = handle;
    return ret;
}

void shm_init( void )
{
    void *     mapped_address   = NULL;
    size_t     mapped_size      = 0;
    size_t     ctrl_header_size = 0;
    shm_handle ctrl_handle      = CONTROL_HANDLE_INVALID;
    uint32_t   i                = 0;

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
    fprintf(
        stdout,
        "SHM DEBUG ENABLED:\n  Heap Growth Direction: "
    );
#ifdef SHM_HEAP_GROWS_DOWNWARD
    fprintf( stdout, "DOWN (Towards lower virtual addresses)\n" );
#else
    fprintf( stdout, "UP (Towards higher virtual addresses)\n" );
#endif // SHM_HEAP_GROWS_DOWNWARD
    fprintf( stdout, "  Stack Growth Direction: " );
#ifdef STACK_GROWS_DOWNWARD
    fprintf( stdout, "DOWN (Towards lower virtual addresses)\n" );
#else
    fprintf( stdout, "UP (Towards higher virtual addresses)\n" );
#endif // STACK_GROWS_DOWNWARD
#endif // SHM_DEBUG
    p_pid = ( pid_t ) getpid();

    ctrl_header_size = _get_ctrl_header_size( ( uint32_t ) SHM_MAX_SEGMENTS );
    //ctrl_header_size = _round_to_multiple_of_page_size( ctrl_header_size );
#ifdef SHM_DEBUG
    fprintf(
        stderr,
        "Attempting to map control segment, header size %zu, rounded-to-page-size %zu, page_size %zu\n",
        _get_ctrl_header_size( ( uint32_t ) SHM_MAX_SEGMENTS ),
        ctrl_header_size,
        _get_system_page_size()
    );
#endif // SHM_DEBUG
    while( mapped_address == NULL && mapped_size == 0 )
    {
        ctrl_handle = ( shm_handle ) random();

        if( unlikely( ctrl_handle == CONTROL_HANDLE_INVALID ) )
            continue;
#ifdef SHM_DEBUG
        fprintf(
            stderr,
            "Attemtping handle %lu\n",
            ctrl_handle
        );
#endif // SHM_DEBUG
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
#ifdef SHM_DEBUG
            fprintf(
                stderr,
                "Mapped control handle %lu to %p\n",
                ctrl_handle,
                mapped_address
            );
#endif // SHM_DEBUG
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
    control_header->magic       = ( uint32_t ) CONTROL_HEADER_MAGIC;
    control_header->owner       = getpid();
    control_header->entry_count = 0;
    control_header->max_entries = SHM_MAX_SEGMENTS;
    control_header->locked      = false;

    _cleanup_old_segments();

    // Blank out the allocations
    for( i = 0; i < ( uint32_t ) SHM_MAX_SEGMENTS; i++ )
    {
        control_header->segments[i]     = (shm_handle) SEGMENT_HANDLE_INVALID;
        __segment_lut[i].mapped_address = NULL;
        __segment_lut[i].mapped_size    = 0;
        __segment_lut[i].handle         = (shm_handle) SEGMENT_HANDLE_INVALID;
    }

#ifdef SHM_DEBUG
    fprintf(
        stderr,
        "SHM INITED: mapped control segment to %p, handle %lu\n",
        control_header,
        control_handle
    );
#endif // SHM_DEBUG

    shm_inited = true;
    return;
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
#ifdef SHM_DEBUG
    fprintf(
        stderr,
        "Found control header %lu - %p (size %zu)\n",
        ( uint64_t ) control_handle,
        control_header,
        control_header_size
    );
#endif // SHM_DEBUG

    if( unlikely( !_shm_check_control( control_header ) ) )
    {
        fprintf(
            stderr,
            "Child initialized on an invalid control header in shared memory %lu\n",
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
        return NULL;
    if( unlikely( handle > SHM_MAX_SEGMENTS ) )
        return NULL;

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
#ifdef SHM_DEBUG
            fprintf(
                stderr,
                "SEGMENT addrs:\n  header base: %p\n  magic: %p\n  owner: %p\n  locked: %p\n  entry_count: %p\n  ref_count: %p\n  control: %p\n  data: %p\n",
                mapped_address,
                &( ( ( seg_header * ) mapped_address )->magic ),
                &( ( ( seg_header * ) mapped_address )->owner ),
                &( ( ( seg_header * ) mapped_address )->locked ),
                &( ( ( seg_header * ) mapped_address )->entry_count ),
                &( ( ( seg_header * ) mapped_address )->ref_count ),
                &( ( ( seg_header * ) mapped_address )->control ),
                &( ( ( seg_header * ) mapped_address )->data )
            );
#endif // SHM_DEBUG
            return ( void * ) GET_USER_PTR( mapped_address );
        }
        else
        {
            fprintf(
                stderr,
                "Attempt to map invalid shared memory segment %lu\n",
                ( uint64_t ) handle
            );

            return NULL;
        }
    }

    fprintf(
        stderr,
        "Shared memory segment %lu not found\n",
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
        return NULL;

    if( unlikely( control_header == NULL ) )
        return NULL;

    // Begin critical section
    if( !__TNS_MUTEX( &(control_header->locked) ) )
        return NULL;

    if( control_header->entry_count + 1 > SHM_MAX_SEGMENTS )
    {
        __C_MUTEX( &(control_header->locked) );
        fprintf(
            stderr,
            "Out of shared memory (max allocations %d made)\n",
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
        header->magic       = ( uint32_t ) SEGMENT_HEADER_MAGIC;
        header->owner       = p_pid;
        header->locked      = false;
        header->entry_count = 0;
        header->ref_count   = 1;
        header->control     = control_handle;
#ifdef SHM_DEBUG
        fprintf(
            stderr,
            "Mapped new segment %lu at %p of size %zu (req'd: %zu, round'd: %zu)\n",
            new_handle,
            mapped_address,
            mapped_size,
            size,
            real_size
        );

        fprintf(
            stderr,
            "Mapped address %p getting returned as %p\n", mapped_address, GET_USER_PTR( mapped_address )
        );

        fprintf(
            stderr,
            "SEGMENT %lu details:\n  START ADDR: %p\n  LENGTH: %lu\n  USR_START: %p\n  END: %p\n",
            new_handle,
            mapped_address,
            mapped_size,
            GET_USER_PTR( mapped_address ),
            ( void * ) _PTR_ADD_OFFSET( mapped_address, mapped_size )
        );

        fprintf(
            stderr,
            "SEGMENT addrs:\n  header base: %p\n  magic: %p\n  owner: %p\n  locked: %p\n  entry_count: %p\n  ref_count: %p\n  control: %p\n  data: %p\n",
            mapped_address,
            &( ( ( seg_header * ) mapped_address )->magic ),
            &( ( ( seg_header * ) mapped_address )->owner ),
            &( ( ( seg_header * ) mapped_address )->locked ),
            &( ( ( seg_header * ) mapped_address )->entry_count ),
            &( ( ( seg_header * ) mapped_address )->ref_count ),
            &( ( ( seg_header * ) mapped_address )->control ),
            &( ( ( seg_header * ) mapped_address )->data )
        );
#endif // SHM_DEBUG
        return ( void * ) GET_USER_PTR( mapped_address );
    }

    fprintf(
        stderr,
        "Failed to create new shm segment at %lu\n",
        ( shm_handle ) new_handle
    );

    return NULL;
}

static inline shm_handle _get_handle_from_ptr( void * ptr )
{
    seg_header *      header = NULL;
    shm_handle        handle = SEGMENT_HANDLE_INVALID;
    register uint16_t i      = 0;

    if( unlikely( ptr == NULL ) )
        return ( shm_handle ) SEGMENT_HANDLE_INVALID;

    if( unlikely( control_header == NULL ) )
        return ( shm_handle ) SEGMENT_HANDLE_INVALID;

    if( unlikely( control_handle == ( shm_handle ) CONTROL_HANDLE_INVALID ) )
        return ( shm_handle ) SEGMENT_HANDLE_INVALID;

    header = ( seg_header * ) GET_HDR_PTR( ptr );

    // TODO: Implement reverse lookup for header pointers (locally mapped) to shm_handle,
    // This is exhaustive but safe as we don't have to dereference the header pointer, just do
    // comparisons
    for( i = 0; i < ( uint16_t ) SHM_MAX_SEGMENTS; i++ )
    {
        if(
               ( void * ) __segment_lut[i].mapped_address == ( void * ) header
            || _PTR_BOUND_CHECK( ptr, __segment_lut[i].mapped_address, __segment_lut[i].mapped_size )
          )
        {
            header = ( seg_header * ) __segment_lut[i].mapped_address;
            handle = __segment_lut[i].handle;
#ifdef SHM_DEBUG
            fprintf(
                stderr,
                "LUT[%u] match on %p\n",
                i, ( void * ) header
            );
#endif // SHM_DEBUG
            break;
        }
    }

    if( unlikely( handle == SEGMENT_HANDLE_INVALID ) )
    {
#ifdef SHM_DEBUG
        fprintf(
            stderr,
            "Address %p could not be resolved to a segment handle (was given %p)\n",
            header,
            ptr
        );
#endif // SHM_DEBUG
        return ( shm_handle ) SEGMENT_HANDLE_INVALID;
    }

    if( unlikely( header->magic != SEGMENT_HEADER_MAGIC ) )
        return ( shm_handle ) SEGMENT_HANDLE_INVALID;

#ifdef SHM_DEBUG
    fprintf(
        stderr,
        "Resolved %p to shm_handle %lu\n",
        header,
        ( uint64_t ) handle
    );
#endif // SHM_DEBUG

    return handle;
}

void unmap_segment( void * ptr )
{
    seg_header * header = NULL;
    shm_handle   handle = SEGMENT_HANDLE_INVALID;

    handle = _get_handle_from_ptr( ptr );

    if( unlikely( handle == ( shm_handle ) SEGMENT_HANDLE_INVALID ) )
        return;

    header = ( seg_header * ) GET_HDR_PTR( ptr );

    // Critical section - decrement ref count
    if( !__TNS_MUTEX( &(header->locked) ) )
        return;

    if( unlikely( header->ref_count < 2 ) )
    {
        __C_MUTEX( &(header->locked ) );
        _free_segment( handle );
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

    fprintf(
        stderr,
        "Failed to detach segment %lu\n",
        handle
    );

    return;
}

static void _free_segment( shm_handle handle )
{
    // Internal - we're relying on things like the handle already being vetted
    // Critical section - free segment
    if( !__TNS_MUTEX( &(control_header->locked) ) )
        return;

    control_header->segments[handle] = SEGMENT_HANDLE_INVALID;
    // XXX - may need to compactify the segments array to prevent fragmentation
    // The issue is we'll need a mechanism to locate __refs and update their reference on a segment move

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

    fprintf(
        stderr,
        "Failed to destroy segment %lu\n",
        handle
    );

    return;
}

void free_segment( void * ptr )
{
    seg_header * header = NULL;
    shm_handle   handle = SEGMENT_HANDLE_INVALID;
    void *       base   = NULL;

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
    uint32_t   i              = 0;
    shm_handle seg            = ( shm_handle ) SEGMENT_HANDLE_INVALID;
    void *     mapped_address = NULL;
    shm_handle ctrl           = ( shm_handle ) CONTROL_HANDLE_INVALID;

    if( control_header == NULL )
        return;

    if( !_shm_check_owner( control_header ) )
        return;

    for( i = 0; i < control_header->max_entries; i++ )
    {
        if( control_header->segments[i] == CONTROL_HANDLE_INVALID )
            continue;

        seg = control_header->segments[i];

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
        fprintf(
            stderr,
            "Failed to detach control handle %lu\n",
            ctrl
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

void map_all( void )
{
    uint64_t seg_index = 0;

    if( control_header == NULL || control_handle == CONTROL_HANDLE_INVALID )
#ifdef SHM_DEBUG
    {
        fprintf(
            stderr,
            "Cannot map_all - control handle is empty\n"
        );
#endif // SHM_DEBUG
        return;
#ifdef SHM_DEBUG
    }
#endif // SHM_DEBUG

    for( seg_index = 0; seg_index < control_header->max_entries; seg_index++ )
    {
        if(
               __segment_lut[seg_index].handle == SEGMENT_HANDLE_INVALID
            && control_header->segments[seg_index] != SEGMENT_HANDLE_INVALID
          )
        {
            if( map_segment( ( shm_handle ) seg_index ) == NULL )
            {
                fprintf(
                    stderr,
                    "Failed to map segment handle %lu from control handle %lu\n",
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
    fprintf(
        stderr,
        "Entry: _shm_wrapper( %s, %lu, %zu, %p, %zu )  ",
        op == SHM_ATTACH ? "ATTACH" :
        op == SHM_CREATE ? "CREATE" :
        op == SHM_DETACH ? "DETACH" :
        op == SHM_DESTROY ? "DESTROY" : "INVALID",
        handle,
        size,
        mapped_address != NULL ? *mapped_address : NULL,
        mapped_size != NULL ? *mapped_size : 0
    );
    if( control_handle == CONTROL_HANDLE_INVALID )
    {
        fprintf(
            stderr,
            "CTRL: INVALID (%lu)\n",
            control_handle
        );
    }
    else
    {
        fprintf(
            stderr,
            "CTRL: %lu\n",
            control_handle
        );
    }
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
            fprintf(
                stderr,
                "Failed to unmap shared memory segment %s: %s\n",
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
            fprintf(
                stderr,
                "Failed to remove shared memory segment %s: %s\n",
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
        fprintf(
            stderr,
            "Failed to create shared memory segment %s: %s\n",
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

            fprintf(
                stderr,
                "Failed to stat shared memory segment %s: %s\n",
                name,
                strerror( errno )
            );
            return false;
        }

        if( statbuff.st_size < size )
        {
            fprintf(
                stderr,
                "Mismatch in shared memory segment %s. Loaded %zu, expected %zu\n",
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
        fprintf(
            stderr,
            "Failed to resize shared memory segment %s to %zu bytes: %s\n",
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

        fprintf(
            stderr,
            "Could not map shared memory segmnet %s: %s\n",
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

    // Type coersion may involve truncation, so we consistently 'fix' the converted value here
    key = ( key_t ) handle;
    if( key < 1 )
        key = -key;
    if( key == IPC_PRIVATE )
    {
        if( op != SHM_CREATE )
        {
            fprintf(
                stderr,
                "Use of restrictred handle resolved to SystemV IPC_PRIVATE flag\n"
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
            fprintf(
                stderr,
                "Failed to allocate identifier cache\n"
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
                fprintf(
                    stderr,
                    "Failed to get shared memory segment %s: %s\n",
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
            fprintf(
                stderr,
                "Could not unmap shared memory segment %s: %s\n",
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
                fprintf(
                    stderr,
                    "Could not remove shared memory segment %s: %s\n",
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
            fprintf(
                stderr,
                "Failed to stat shared memory segment %s: %s\n",
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
        fprintf(
            stderr,
            "Failed to map shared memory segment %s: %s\n",
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
        ( uint64_t ) ( control_handle == CONTROL_HANDLE_INVALID ) ? 0 : control_handle,
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
#ifdef SHM_DEBUG
            fprintf(
                stderr,
                "Cannot unmap %p from %s (%lu)\n",
                mapped_address,
                name,
                handle
            );
#endif // SHM_DEBUG
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
    errno = 0;

    descriptor = shm_open( name, flags, SHM_FILE_PERMS );
#ifdef SHM_DEBUG
    fprintf(
        stderr,
        "Opened file %s: descriptor %d\n",
        name,
        descriptor
    );
#endif // SHM_DEBUG
    if( descriptor == -1 )
    {
        if( errno != EEXIST )
        {
            fprintf(
                stderr,
                "Failed to open shared memory segment %s: %s\n",
                name,
                strerror( errno )
            );
        }
#ifdef SHM_DEBUG
        fprintf(
            stderr,
            "Got errno %d: '%s' on file open\n",
            errno,
            strerror( errno )
        );
#endif // SHM_DEBUG
        return false;
    }

    if( op == SHM_ATTACH )
    {
        if( fstat( descriptor, &statbuff ) != 0 )
        {
            _close_segment_descriptor( descriptor, name, false );
            fprintf(
                stderr,
                "Failed to stat shared memory segment %s: %s\n",
                name,
                strerror( errno )
            );
            return false;
        }

        if( size != statbuff.st_size )
        {
            fprintf(
                stderr,
                "Mismatch in shared memory segment %s: loaded %zu bytes, expected %zu bytes\n",
                name,
                statbuff.st_size,
                size
            );
            // size mismatch?
        }

        size = statbuff.st_size;
    }
    else if( _shm_posix_resize( descriptor, size ) != 0 )
    {
        _close_segment_descriptor( descriptor, name, false );
        fprintf(
            stderr,
            "Failed to resize shared memory segment %s to %zu butes: %s\n",
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
        fprintf(
            stderr,
            "failed to map shared memory segment %s: %s\n",
            name,
            strerror( errno )
        );
        return false;
    }

    *mapped_address = ( void * ) address;
    *mapped_size = size;
#ifdef SHM_DBUG
    fprintf(
        stderr,
        "Mapped %s (%lu) to %p (size %zu)\n",
        name,
        handle,
        ( void * ) *mapped_address,
        ( size_t ) *mapped_size
    );
#endif // SHM_DEBUG
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
        fprintf(
            stderr,
            "Failed to close shared memory segment %s: %s\n",
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
            fprintf(
                stderr,
                "Failed to remove shared memory segment %s: %s\n",
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

    // Round a given size to a multiple of the system page size, including header overhead
    page_size = _get_system_page_size();
    result    = page_size * ( ( ( size + offsetof( seg_header, data ) ) / page_size ) + 1 );
    return result;
}

static size_t _get_system_page_size( void )
{
#ifdef __linux__
    return sysconf( _SC_PAGESIZE );
#endif // __linux__
#if defined( __FreeBSD__ ) || defined( __APPLE__ ) || defined( __unix__ )
    return ( size_t ) getpagesize();
#endif // __FreeBSD__ || __APPLE__ || __unix__
    return ( size_t ) DEFAULT_PAGE_SIZE;
}

static inline bool _shm_check_owner( ctrl_header * header )
{
    if( unlikely( !_shm_check_control( header ) ) )
        return false;
    if( likely( header->owner == getpid() || header->owner == getppid() ) )
        return true;

    return false;
}

static inline bool _shm_check_control( ctrl_header * header )
{
    if( unlikely( header == NULL ) )
        return false;
    if( unlikely( header->magic != CONTROL_HEADER_MAGIC ) )
        return false;
    if( unlikely( header->entry_count > header->max_entries ) )
        return false;

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
            fprintf(
                stderr,
                "Failed to allocate prune list for old segments\n"
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
            fprintf(
                stderr,
                "Failed to resize prune list for old segments\n"
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
#ifdef SHM_DEBUG
        fprintf(
            stderr,
            "Performing cleanup for %lu\n",
            current_ctrl
        );
#endif // SHM_DEBUG
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
                fprintf(
                    stderr,
                    "Mapping failed for control segment %lu\n",
                    ( uint64_t ) current_ctrl
                );
                continue;
            }

            header = ( ctrl_header * ) mapped_address;

            if( header->magic != CONTROL_HEADER_MAGIC )
            {
                // Doesn't belong to pg_ctblmgr?
                fprintf(
                    stderr,
                    "Bad magic, expected %u, got %u\n",
                    ( uint32_t ) CONTROL_HEADER_MAGIC,
                    ( uint32_t ) header->magic
                );
                continue;
            }

            owner_pid = header->owner;

            if( owner_pid <= 1 )
            {
                fprintf(
                    stderr,
                    "Invalid pid %d in stale control handle\n",
                    owner_pid
                );
                continue;
            }
#ifdef _POSIX_C_SOURCE
            // Indicates we have access to kill
            save_errno = errno;
            if( kill( owner_pid, 0 ) < 0 )
            {
#ifdef SHM_DEBUG
                fprintf(
                    stderr,
                    "Kill( 0 ) to PID %u gave %s\n",
                    owner_pid,
                    strerror( errno )
                );
#endif // SHM_DEBUG
                if( errno == ESRCH )
                {
                    // pid does not exist - safe to remove
                    can_remove = true;
                }
                else
                {
                    continue;
                }
            }

            errno = save_errno;
#endif // _POSIX_C_SOURCE
            if( can_remove )
            {
                control_handle = current_ctrl;
#ifdef SHM_DEBUG
                fprintf(
                    stderr,
                    "Pruning segments belonging to control handle %lu\n",
                    ( uint64_t ) current_ctrl
                );
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
                        fprintf(
                            stderr,
                            "Failed to destroy stale segment %lu for control handle %lu\n",
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
                    fprintf(
                        stderr,
                        "Failed to detach control segment %lu after cleanup\n",
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
                    fprintf(
                        stderr,
                        "Failed to destroy control segment %lu after cleanup\n",
                        current_ctrl
                    );

                    continue;
                }

            }
        }
        else
        {
            fprintf(
                stderr,
                "Failed to attach control segment %lu for inspection\n",
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
    uint32_t i = 0;
    if( header == NULL )
        return;
    fprintf(
        stderr,
        "Header data:\n  " \
          "MAGIC: %u\n  " \
          "OWNER: %d\n  " \
          "LOCKED: %s\n  " \
          "ENTRY_COUNT: %u\n  " \
          "MAX_ENTRIES: %u\n  " \
          "SEGMENTS[]:\n",
        header->magic,
        header->owner,
        header->locked ? "TRUE" : "FALSE",
        header->entry_count,
        header->max_entries
    );

    for( i = 0; i < header->max_entries; i++ )
    {
        if( header->segments[i] == SEGMENT_HANDLE_INVALID )
            continue;

        fprintf(
            stderr,
            "    [%u]: %lu\n",
            i,
            header->segments[i]
        );
    }

    return;
}

static void __dump_seg_header( seg_header * header )
{
    if( header == NULL )
        return;

    fprintf(
        stderr,
        "Header data:\n  " \
          "MAGIC: %u\n  " \
          "OWNER: %d\n  " \
          "LOCKED: %s\n  " \
          "ENTRY_COUNT: %u\n  " \
          "REF_COUNT: %u\n  " \
          "CONTROL: %lu\n  " \
          "DATA: %p\n",
        header->magic,
        header->owner,
        header->locked ? "TRUE" : "FALSE",
        header->entry_count,
        header->ref_count,
        header->control,
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
        fprintf(
            stderr,
"ERROR: Stack growth detected as growing upward, but \
program compiled with STACK_GROWS_DOWNWARD\n"
        );
        return false;
    }

    if( heap_a > heap_b )
    {
        fprintf(
            stderr,
"ERROR: Heap growth detected as growing downward, but \
program compiled with STACK_GROWS_DOWNWARD\n \
(This conflicts with stack growth direction by convention)"
        );
        return false;
    }
#else
    if( !is_up )
    {
        fprintf(
            stderr,
"ERROR: Stack growth detected as growing downward, but \
program wasn't compiled with STACK_GROWS_DOWNWARD\n"
        );
        return false;
    }

    if( heap_a < heap_b )
    {
        fprintf(
            stderr,
"ERROR: Heap growth detected as growing upward, but \
program wasn't compiled with STACK_GROWS_DOWNWARD\n \
(This conflicts with stack growth direction by convention)"
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
