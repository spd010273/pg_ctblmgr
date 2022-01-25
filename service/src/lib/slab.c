/*--------------------------------------------------------------------------
 *
 * slab.c
 *     Shared Memory slab allocator
 *
 * The overall goal of this library, together with shm.c, is to present a
 * simplified malloc/calloc/realloc/free-esque interface to the user for
 * manipulating shared memory, while attempting to abstract the maintenance
 * and book-keeping functionality away from the user using minimal boilerplate
 * code.
 *
 * This library provides a mechanism for slab allocation within the segments
 * mapped in by shm.c. It is designed to handle both large and small objects
 * but strongly favors usages where the total number of allocations is either
 * known upfront or otherwise constrained. This library uses the offset-based
 * references (provided by shm.c) within its own structures and user-facing
 * subroutines.
 *
 * Copyright (c) 2021, MerchLogix Inc.
 *
 * IDENTIFICATION
 *        service/src/lib/slab.c
 *
 *--------------------------------------------------------------------------
 */
#include "slab.h"

static shm_handle        control_segment         = SEGMENT_HANDLE_INVALID;
static __ref             control_segment_address = {0};
static shalloc_control * mapped_control          = NULL;
static bool              _slab_init              = false;
static pid_t             p_pid                   = 0;

// Check and boilerplate helpers
static __inline__ bool _fail_canary( void ) __attribute__((always_inline, flatten));
static __inline__ bool _init_slab( context_t, shalloc_header *, bool );
static __inline__ context_t get_ctx_by_id( const char * ) __attribute__((always_inline, flatten));
static __inline__ bool check_shalloc_header( header_iter ) __attribute__((always_inline, flatten));
static __inline__ bool check_context( context_t ) __attribute__((always_inline, flatten));
static __inline__ bool _check_canaries( shalloc_header * ) __attribute__((always_inline, flatten));
static __inline__ __ref _get_alloc_element_by_index( shalloc_header *, uint64_t ) __attribute__((always_inline));
static __inline__ shalloc_header * _get_header_by_context( context_t );// __attribute__((always_inline));
static __inline__ uint32_t _get_allocset_element_by_index( shalloc_header *, uint32_t );
static __inline__ bool _set_allocset_element_by_index( shalloc_header *, uint32_t, uint32_t );

// Allocation helpers
static __inline__ context_t _new_slab( const char *, size_t, uint64_t );
static __inline__ __ref _shmalloc( context_t, size_t, bool );
static __inline__ bool __shrealloc_internal( shalloc_header *, uint64_t );
static __inline__ __ref _shrealloc( context_t, __ref, uint64_t );

// FSM helpers
static __inline__ uint64_t _get_fsm_length( shalloc_header * );// __attribute__((always_inline));
static __inline__ void _set_fsm_element_by_index( shalloc_header *, uint64_t );// __attribute__((always_inline));
static __inline__ void _clear_fsm_element_by_index( shalloc_header *, uint64_t );// __attribute__((always_inline));
static __inline__ bool _get_fsm_element_by_index( shalloc_header *, uint64_t );// __attribute__((always_inline));
static __inline__ uint64_t _get_fsm_slot_by_width( shalloc_header *, uint64_t );// __attribute__((always_inline));
static __inline__ uint64_t __find_fsm_spot( shalloc_header *, uint64_t );// __attribute__((always_inline));
static __inline__ bool _ref_get_index_and_size( shalloc_header *, __ref, uint64_t *, size_t * );// __attribute__((always_inline));

// Utility
static __inline__ void * _move_to_local( shalloc_header *, __ref *, bool );// __attribute__((always_inline));
static __inline__ __ref _move_to_shared( shalloc_header *, void **, size_t, bool );// __attribute__((always_inline));

#ifdef SLAB_DEBUG
// Debugging
static void _dump_header( shalloc_header * );
static void _dump_context( context_t ); // calls dump_header()
static void print_byte( uint8_t );
static void print_bin( uint64_t );
static void _print_fsm( shalloc_header * );
static const char * bits[16] = {
    [ 0] = "0000", [ 1] = "0001", [ 2] = "0010", [ 3] = "0011",
    [ 4] = "0100", [ 5] = "0101", [ 6] = "0110", [ 7] = "0111",
    [ 8] = "1000", [ 9] = "1001", [10] = "1010", [11] = "1011",
    [12] = "1100", [13] = "1101", [14] = "1110", [15] = "1111",
};
static const char * hexes[16] = {
    [ 0] = "0",   [ 1] = "1",   [ 2] = "2",   [ 3] = "3",
    [ 4] = "4",   [ 5] = "5",   [ 6] = "6",   [ 7] = "7",
    [ 8] = "8",   [ 9] = "9",   [10] = "A",   [11] = "B",
    [12] = "C",   [13] = "D",   [14] = "E",   [15] = "F",
};
#endif // SLAB_DEBUG

/*
 * bool slab_init()
 *
 * Called by either the parent process, pre fork, to setup the control segment
 * (via shm.c), or called by child process(es), post-fork, to attach to the control segment
 */
bool slab_init( void )
{
    void *      segment_address = NULL;
    header_iter i               = 0;

    if( _slab_init == false )
    {
        // Parent initialization sequence
        p_pid = getpid();
        #ifndef _SHALLOC_CONTROL_IN_OWN_SEGMENT
        shm_init_extra( sizeof( shalloc_control ) );
        segment_address = get_control_data_section();
        #else
        segment_address = new_segment( sizeof( shalloc_control ) );
        #endif // _SHALLOC_CONTROL_IN_OWN_SEGMENT

        if( segment_address == NULL )
            return false;

        srand( ( unsigned int ) _INVALID_CONTEXT );
        #ifdef _SHALLOC_CONTROL_IN_OWN_SEGMENT
        control_segment         = ref_get_segment( get_ref( segment_address ) );
        #else
        control_segment         = get_control_segment();
        #endif // _SHALLOC_CONTROL_IN_OWN_SEGMENT
        _slab_init              = true;
        mapped_control          = ( shalloc_control * ) segment_address;

        mapped_control->magic = ( uint64_t ) _SHALLOC_CONTROL_MAGIC;

        for( i = 0; i < _SHALLOC_MAX_SLABS; i++ )
        {
            mapped_control->headers[i].magic       = ( uint64_t ) _SHALLOC_HEADER_MAGIC;
            mapped_control->headers[i].segment     = SEGMENT_HANDLE_INVALID;
            mapped_control->headers[i].object_size = 0;
            mapped_control->headers[i].allocs      = get_null_ref();
            mapped_control->headers[i].n_allocs    = 0;
            mapped_control->headers[i].fsm         = get_null_ref();
            mapped_control->headers[i].self        = INVALID_CONTEXT;
            mapped_control->headers[i].locked      = false;
            mapped_control->headers[i].count_hint  = 0;

            mapped_control->headers[i].c_allocstart     = ( canary_t ) random();
            mapped_control->headers[i].c_fsmstart       = ( canary_t ) random();
            mapped_control->headers[i].c_fsmend         = ( canary_t ) random();
            mapped_control->headers[i].loc_c_allocstart = get_null_ref();
            mapped_control->headers[i].loc_c_fsmstart   = get_null_ref();
            mapped_control->headers[i].loc_c_fsmend     = get_null_ref();

            memset(
                mapped_control->headers[i].object_id,
                '\0',
                ( size_t ) _SHALLOC_MAX_IDENT
            );
        }
    }
    else if( likely( shm_is_init() || p_pid != getpid() ) )
    {
        // child initialization sequence
        shm_child_init();
        #ifndef SLAB_LAZY_LOAD
        map_all();
        #endif // SLAB_LAZY_LOAD

        // segment address will be automatically mapped in when the __ref is dereferenced
        segment_address = get_ptr( control_segment_address );

        if( segment_address == NULL || control_segment == SEGMENT_HANDLE_INVALID )
            return false;

        mapped_control = ( shalloc_control * ) segment_address;

        if( mapped_control->magic != _SHALLOC_CONTROL_MAGIC )
            return false;

        for( i = 0; i < _SHALLOC_MAX_SLABS; i++ )
        {
            if( !check_shalloc_header( i ) )
            #ifdef SLAB_DEBUG
            {
                fprintf(
                    stderr,
                    "Shalloc header %lu failed sanity checks\n",
                    ( uint64_t ) i
                );
            #endif // SLAB_DEBUG
                return false;
            #ifdef SLAB_DEBUG
            }
            #endif // SLAB_DEBUG
        }
    }

    return true;
}

// Destroys a slab
void destroy_slab( context_t ctx )
{
    shalloc_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return;
    // XXX
    return;
}

// Initializes a new slab
context_t new_slab( const char * tag, size_t object_size )
{
    return _new_slab( tag, object_size, 0 );
}

context_t new_slab_with_hint( const char * tag, size_t object_size, uint64_t count_hint )
{
    return _new_slab( tag, object_size, count_hint );
}

static __inline__ context_t _new_slab( const char * tag, size_t object_size, uint64_t count_hint )
{
    context_t ret                       = INVALID_CONTEXT;
    char      ident[_SHALLOC_MAX_IDENT] = {0};

    snprintf(
        ident,
        _SHALLOC_MAX_IDENT,
        "%s",
        tag
    );

    ret = get_ctx_by_id( ident );

    if( ret == INVALID_CONTEXT )
    {
        // Allocate a new context

        // lock and inc next_header index and return as the new
        // context
        if( !__TNS_MUTEX( &(mapped_control->locked) ) )
            return INVALID_CONTEXT;

        if( mapped_control->next_header + 1 > _SHALLOC_MAX_SLABS )
        {
            __C_MUTEX( &(mapped_control->locked) );
            return INVALID_CONTEXT;
        }

        ret = ( context_t ) mapped_control->next_header;
        mapped_control->next_header += 1;

        __C_MUTEX( &(mapped_control->locked) );

        // Setup our header to a semi-initialized state - we'll
        // handle setup of allocs[] and fsm[] later
        mapped_control->headers[ret].object_size      = object_size;
        mapped_control->headers[ret].n_allocs         = 0;
        mapped_control->headers[ret].self             = ret;
        mapped_control->headers[ret].locked           = false;
        mapped_control->headers[ret].max_allocations  = 0;
        mapped_control->headers[ret].i_rear_fsm_word  = 0;
        mapped_control->headers[ret].i_front_fsm_bit  = 0;
        mapped_control->headers[ret].count_hint       = count_hint;

        strncpy(
            mapped_control->headers[ret].object_id,
            ident,
            _SHALLOC_MAX_IDENT
        );
    }

    return ret;
}

// Main interface allocation functions

/*
 * __ref scalloc( context_t, size_t, uint64_t )
 *
 *  Shared memory equivilent of calloc()
 *  Makes an allocation of size * count and zeros out the allocation
 *  prior to returning a __ref to that location to the caller.
 *
 *  context_t ctx: Slab context the allocation is made in
 *  size_t size: Unit size of allocation, we already know this, but it's here
 *               for standardization with calloc()
 *  uint64_t count: Number of objects being requested
 */
__ref scalloc( context_t ctx, size_t size, uint64_t count )
{
    shalloc_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return get_null_ref();

    if(
        unlikely(
            ( size != header->object_size )
         || ( size % header->object_size != 0 )
        )
      )
    {
        fprintf( stderr, "scalloc: size is not a multiple of object_size\n" );
        return get_null_ref();
    }

    return _shmalloc( ctx, size, true );
}

/*
 * __ref smalloc( context_t, size_t )
 *
 * Shared memory equivilent of malloc()
 * Makes an allocation of size bytes (or size / header->object_size units)
 * and returns a __ref to that memory location to the caller.
 *
 *  context_t ctx: Slab context the allocation is made in
 *  size_t size: Size in bytes to make for this allocation
 *
 */
__ref smalloc( context_t ctx, size_t size )
{
    shalloc_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return get_null_ref();


    return _shmalloc( ctx, size, false );
}

/*
 * __ref srealloc( context_t, __ref, size_t )
 *
 * Shared memory equivilent of realloc()
 * Reallocates the passed in __ref to the requested size, returning a __ref to
 * that location in memory to the caller
 *
 * context_t ctx: Slab context the allocation is made in
 * __ref oldref: Reference in which we would like to reallocate
 * size_t size: The new size we would like for oldref
 */
__ref srealloc( context_t ctx, __ref oldref, size_t size )
{
    shalloc_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return get_null_ref();

    fprintf( stdout, "srealloc: not implemented\n" );
    return get_null_ref();
}

/*
 * __ref scalloc_object_count( context_t, uint64_t )
 *
 * Shared memory equivilent of calloc(). Instead of taking a
 * size_t (bytes) argument, this takes the number of object we
 * want an allocation for.
 *
 *  context_t ctx: Slab context the allocation is made in
 *  uint64_t count: The number of objects requested.
 */
__ref scalloc_object_count( context_t ctx, uint64_t count )
{
    shalloc_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return get_null_ref();

    return _shmalloc( ctx, count * header->object_size, true );
}

/*
 * __ref smalloc_object_count( context_t, uint64_t )
 *
 * Shared memory equivilent of malloc(). Instead of taking a
 * size_t (bytes) argument, this takes the number of objects we
 * want an allocation for.
 *
 * context_t ctx: Slab context the allocation is made in
 * uint64_t count: The number of objects requested.
 *
 */
__ref smalloc_object_count( context_t ctx, uint64_t count )
{
    shalloc_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return get_null_ref();

    return _shmalloc( ctx, count * header->object_size, false );
}

/*
 * __ref srealloc_object_count( context_t, __ref, uint64_t )
 *
 * Shared memory equivilent of realloc(). Instead of taking a
 * size_t (bytes) argument, this takes the number of objects we
 * want the reallocated __ref to contain.
 *
 * context_t ctx: Slab context the allocation is made in
 * __ref oldref: Reference to the memory area we want to reallocate
 * uint64_t count: Number of objects the reallocated __ref should hold
 *
 */
__ref srealloc_object_count( context_t ctx, __ref oldref, uint64_t count )
{
    shalloc_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return get_null_ref();

    fprintf( stderr, "srealloc_object_count: not implemented\n" );
    return get_null_ref();
}

/*
 * __shrealloc_internal( shalloc_header *, uint64_t )
 *     shalloc_header *: Reference to the header for the segment in which we want to resize
 *     uint64_t: The number of objects desired for the new segment
 *
 * Note that the segment the header lives in and the segment the data
 * (allocs, fsm, canaries), since header->segment stores the segment the data
 * lives in, we use that to perform the resize. This is passed onto the underlying
 * shm functions
 */
static __inline__ bool __shrealloc_internal( shalloc_header * header, uint64_t new_size )
{
    size_t   unit_size      = 0;
    size_t   available      = 0;
    void *   c_fsmstart     = NULL;
    void *   c_fsmend       = NULL;
    void *   fsm            = NULL;
    size_t   old_fsm_length = 0;
    size_t   size_needed    = 0;
    size_t   alloc_offset   = 0;
    void *   old_fsm        = NULL;

    if( unlikely( header == NULL ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "___shrealloc_internal: Invalid header\n" );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( !header->locked ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "__shrealloc_internal: Expected locked header as input\n" );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    /*
     * For now we'll be lazy - there can be some smarts here
     */
    // Size to store FSM_WIDTH physical objects and the FSM word for them
    unit_size = ( FSM_WIDTH * header->object_size ) + sizeof( fsm_t );

    // Intermediate - number of units need to hold the request
    size_needed = ( new_size / FSM_WIDTH );
    if( new_size % FSM_WIDTH != 0 )
        size_needed++;

    // Here's the part where we're being lazy - we could:
    //  - Figure out how much free space is available at the front of the FSM,
    //    and deduct the available space at the front from the request size
    //  or
    //  - Just allocate (requested_size) + existing size
    //
    //  We'll be doing the latter.
    alloc_offset = get_segment_size( header->segment );
    size_needed  = ( size_needed * unit_size ) + alloc_offset;
    old_fsm      = get_ptr_fast( header->loc_c_fsmstart );

    if( !shm_resize_segment( header->segment, size_needed ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "_shrealloc_internal: Failed to resize segment\n" );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( !check_shalloc_header( ( header_iter ) header->segment ) ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "_shrealloc_internal: Failed to validate segment header post-resize\n" );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    // If we've got to this point we need to punt the FSM and associated canaries
    // to the new end of the segment, recalculate max_allocations and return to the user.
    // __refs are preserved as they still point to the same memory segment, but the offset and segment #
    // will be the same. Still need to efficient-ize the call.

    // Unit size - stores sizeof( fsm_t ) * CHAR_BIT allocations
    old_fsm_length = _get_fsm_length( header );
    // Available space - accounting for headers and canaries
    available = ( get_segment_size( header->segment ) - ( 3 * sizeof( canary_t ) ) );

    header->max_allocations = ( available / unit_size ) * FSM_WIDTH;
    c_fsmstart = ( void * ) _PTR_ADD_OFFSET(
        get_ptr_fast( header->allocs ),
        ( header->object_size * header->max_allocations )
    );
    fsm      = ( void * ) _PTR_ADD_OFFSET( c_fsmstart, sizeof( canary_t ) );
    c_fsmend = ( void * ) _PTR_ADD_OFFSET(
        fsm,
        ( sizeof( fsm_t ) * _get_fsm_length( header ) )
    );

    memcpy(
        ( void * ) c_fsmstart,
        ( void * ) get_ptr_fast( header->loc_c_fsmstart ),
        old_fsm_length * sizeof( fsm_t ) + sizeof( canary_t )
    );

    header->loc_c_fsmend   = get_ref( c_fsmend );
    header->loc_c_fsmstart = get_ref( c_fsmstart );
    header->fsm            = get_ref( fsm );

    *( ( canary_t * ) c_fsmend ) = header->c_fsmend;

    // Blank out the old FSM and canaries to avoid divulging allocation
    // information to the caller
    memset(
        old_fsm,
        0,
        old_fsm_length + 1
    );
    
    if( header->max_allocset <= header->max_allocations )
    {
        // Extend allocset
        size_needed = header->max_allocations * sizeof( uint32_t );
        if( !shm_resize_segment( header->allocset_handle, size_needed ) )
        #ifdef SLAB_DEBUG
        {
            fprintf( stderr, "_shrealloc_internal: Failed to resize allocset segment\n" );
        #endif // SLAB_DEBUG
            return false;
        #ifdef SLAB_DEBUG
        }
        #endif // SLAB_DEBUG
    
        header->max_allocset = get_segment_size( header->allocset_handle ) / sizeof( uint32_t );

        #ifdef SLAB_DEBUG
        fprintf( stdout, "Resized allocset segment to hold %lu individual allocations\n", ( uint64_t ) header->max_allocset );
        #endif // SLAB_DEBUG
    }

    return _check_canaries( header );
}

// Returns the allocation index and size of a given __ref,
// assuming this ref points to the start of the allocation
static __inline__ bool _ref_get_index_and_size(
    shalloc_header * header,
    __ref            ref,
    uint64_t *       index,
    size_t *         size
)
{
    offset_t offset = 0;
    if( unlikely( ( index == NULL ) || ( size == NULL ) || ( header == NULL ) ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "ref_get_index_and_size: NULL parameters provided I %p S %p H %p\n",
            index,
            size,
            header
        );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( ref_is_null( ref ) ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "ref_get_index_and_size: __ref is NULL\n"
        );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    offset = ref_get_offset( ref );

    #ifdef _SHALLOC_EXTRA_SANE
    if(
        unlikely(
        !_PTR_BOUND_CHECK(
            get_ptr( ref ),
            get_ptr( header->allocs ),
            header->max_allocations * header->object_size
        ))
      )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "sfree: Ref fails cursory bounds check and doesn't lie in the"
            " allocatable space of the shared memory segment\n"
        );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG
    #endif // _SHALLOC_EXTRA_SANE

    if( unlikely( offset % header->object_size != 0 ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "ref_get_index_and_size: __ref offset is not aligned to object_size in header\n"
        );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    *index = ( offset / header->object_size ) - 1;
    *size  = ( size_t ) _get_allocset_element_by_index( header, ( uint32_t ) *index );

    if( *size == UINT_MAX )
        return false;

    return true;
}

void sfree( context_t ctx, __ref pointer )
{
    shalloc_header * header = NULL;
    uint64_t         index  = 0;
    uint64_t         size   = 0;
    uint64_t         i      = 0;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "sfree: Context check fauled\n" );
    #endif // SLAB_DEBUG
        return;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( ref_is_null( pointer ) ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "sfree: Cannot free. NULL __ref given.\n" );
    #endif // SLAB_DEBUG
        return;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( !_ref_get_index_and_size( header, pointer, &index, &size ) ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "sfree: Failed to find allocation info for __ref\n" );
    #endif // SLAB_DEBUG
        return;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( !_get_fsm_element_by_index( header, index ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "sfree: Provided reference is not allocated\n"
        );
    #endif // SLAB_DEBUG
        return;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    size = _get_allocset_element_by_index( header, index );
    
    if( unlikely( size == UINT_MAX ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stdout, "allocset[%lu] invalid\n", ( uint64_t ) index );
    #endif // SLAB_DEBUG
        return;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    #ifdef SLAB_DEBUG
    fprintf( stdout, "Freeing element of size %lu\n", size );
    #endif // SLAB_DEBUG

    if( unlikely( size == 0 ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "sfree: Cannot free 0-sized element\n" );
    #endif // SLAB_DEBUG
        return;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( !__TNS_MUTEX( &(header->locked) ) ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "sfree: Unable to obtain lock on header\n" );
    #endif // SLAB_DEBUG
        return;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    for( i = index; i < index + size; i++ )
    {
        _clear_fsm_element_by_index( header, i );
    }

    if( !_set_allocset_element_by_index( header, index, 0 ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "Failed to clear allocset element %lu\n", ( uint64_t ) index );
    #endif // SLAB_DEBUG
        return;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG
    
    header->n_allocs       -= size;
    __C_MUTEX( &(header->locked) );
    return;
}

static __inline__ __ref _shrealloc( context_t ctx, __ref oldref, uint64_t count )
{
    shalloc_header * header = NULL;

    header = get_header_by_context( ctx );

    if( header == NULL )
        return get_null_ref();

    return get_null_ref();
}

static __inline__ __ref _shmalloc( context_t ctx, size_t size, bool zero_fill )
{
    shalloc_header * header      = NULL;
    __ref            retref      = {0};
    uint64_t         num_objects = 0;
    uint64_t         index       = 0;
    void *           ptr         = NULL;

    // this is unsafe, but the callers check our context prior
    // to this dereference happening
    header      = &(mapped_control->headers[ctx]);
    num_objects = size / header->object_size;

    fprintf(
        stdout,
        "Handling _shmalloc( %lu, %zu, %s )\n",
        ( uint64_t ) ctx,
        size,
        zero_fill ? "T" : "F"
    );

    if( size % header->object_size != 0 )
    {
        fprintf(
            stderr,
            "requested allocation size %zu is not a multiple of slab's object"
            " size %zu\n",
            size,
            header->object_size
        );
        return get_null_ref();
    }

    // Check to see if this context has ever been allocated.
    if(
            header->segment == SEGMENT_HANDLE_INVALID
         && header->max_allocations == 0
      )
    {
        fprintf( stderr, "Initializing slab\n" );
        if( !_init_slab( ctx, header, false ) )
        {
            fprintf(
                stderr,
                "Attempt to allocate to uninitialized segment\n"
            );
            return get_null_ref();
        }
    }

    if( unlikely( !__TNS_MUTEX( &(header->locked) ) ) )
    {
        fprintf( stderr, "Failed to acquire segment lock\n" );
        return get_null_ref();
    }

    // Layout & usage of free list:
    // [ front - single allocs ..... contiguous allocs - rear ]
    if( num_objects >= ( header->max_allocations - header->n_allocs ) )
    {
        // TODO: Reallocate entire segment
        // or come up with a clever way to extend (shm.c) the segment
        // then shake out the extension to the page
        // This / should / be easy - we only need to relocate the free list
        // to the end of the new, extended page. This will preserve existing
        // __refs
        fprintf( stderr, "NEED REALLOC\n" );
        if( unlikely( !__shrealloc_internal( header, num_objects ) ) )
        #ifdef SLAB_DEBUG
        {
            fprintf( stderr, "Failed to reallocated\n" );
        #endif // SLAB_DEBUG
            return get_null_ref();
        #ifdef SLAB_DEBUG
        }
        #endif // SLAB_DEBUG
    }

    if( num_objects > 1 )
    {
        errno = 0;
        index = _get_fsm_slot_by_width( header, num_objects );

        if( unlikely( errno == ENOSPC ) ) // Need to reallocate
        {
            errno = 0;
            fprintf( stderr, "NEED REALLOC 2\n" );
            if( unlikely( !__shrealloc_internal( header, num_objects ) ) )
                return get_null_ref();

            index = _get_fsm_slot_by_width( header, num_objects );

            if( errno == ENOSPC )
                return get_null_ref();
        }

        retref = _get_alloc_element_by_index( header, index );
        fprintf(
            stdout,
            "Returning ref to alloc[%lu] of %lu ( len %lu )\n",
            ( uint64_t ) index,
            ( uint64_t ) header->max_allocations,
            ( uint64_t ) header->max_allocations - index
        );
        header->n_allocs += num_objects;
    }
    else
    {
        index = header->i_front_fsm_bit;
        if( likely( !_get_fsm_element_by_index( header, index ) ) )
        { // Indexed search
            #ifdef SLAB_DEBUG
            fprintf( stdout, "DEBUG: Making indexed allocation\n" );
            #endif // SLAB_DEBUG
            _set_fsm_element_by_index( header, index );
            retref = _get_alloc_element_by_index( header, index );
            header->i_front_fsm_bit = index + 1;
            header->n_allocs += 1;
        }
        else
        { // Exhaustive search
            #ifdef SLAB_DEBUG
            fprintf( stdout, "DEBUG: Performing exhaustive allocation search\n" );
            #endif // SLAB_DEBUG
            for( index = 0; index < ( FSM_WIDTH * _get_fsm_length( header ) ); index++ )
            {
                if( !_get_fsm_element_by_index( header, index ) )
                {
                    _set_fsm_element_by_index( header, index );
                    retref = _get_alloc_element_by_index( header, index );
                    header->i_front_fsm_bit = index + 1;
                    header->n_allocs += 1;
                    break;
                }
            }
        }

        // Mark allocation size
        if( !_set_allocset_element_by_index( header, index, 1 ) )
        #ifdef SLAB_DEBUG
        {
            fprintf( stderr, "Failed to set allocset element %lu\n", ( uint64_t ) index );
        #endif // SLAB_DEBUG
            return get_null_ref();
        #ifdef SLAB_DEBUG
        }
        #endif // SLAB_DEBUG
    }

    if( zero_fill == true )
    {
        ptr = get_ptr_fast( retref );
        if( ptr == NULL )
        #ifdef SLAB_DEBUG
        {
            fprintf(
                stderr,
                "_shmalloc: Dereference of recently created __ref is NULL\n"
            );
        #endif // SLAB_DEBUG
            return get_null_ref();
        #ifdef SLAB_DEBUG
        }
        #endif // SLAB_DEBUG
        memset(
            ptr,
            ( unsigned char ) _ZERO_FILL_BYTE,
            ( header->object_size * num_objects )
        );
    }

    __C_MUTEX( &(header->locked) );

    return retref;
}

static __inline__ __ref _get_alloc_element_by_index( shalloc_header * header, uint64_t ind )
{
    if( unlikely( header == NULL ) )
        return get_null_ref();

    if( unlikely( ind >= header->max_allocations ) )
        return get_null_ref();

    return get_ref(
        _PTR_ADD_OFFSET(
            get_ptr_fast( header->allocs ),
            ( header->object_size ) * ind
        )
    );
}

static __inline__ void _set_fsm_element_by_index(
    shalloc_header * header,
    uint64_t         ind
)
{
    uint32_t fsm_index  = 0;
    uint8_t  fsm_offset = 0;
    fsm_t *  fsm_word   = NULL;

    if( unlikely( header == NULL ) )
        return;

    /*
     * FSM is laid out like:
     * fsm word             ... bits ...
     * [n]  n*64 - 1         ...                       n*64 - 64
     * ...
     * [1]  ...                           68   67   66   65   64
     * [0]  ...                            4    3    2    1    0
     */
    fsm_index  = ( ind / FSM_WIDTH );
    fsm_offset = ind - ( ( ind / FSM_WIDTH ) * FSM_WIDTH );
    fsm_word   = ( fsm_t * ) _PTR_ADD_OFFSET(
        get_ptr_fast( header->fsm ),
        fsm_index * sizeof( fsm_t )
    );

    if( fsm_word == NULL )
        return;

    *fsm_word |= ( fsm_t ) 1 << fsm_offset;

    return;
}

static __inline__ void _clear_fsm_element_by_index(
    shalloc_header * header,
    uint64_t         ind
)
{
    uint32_t fsm_index  = 0;
    uint8_t  fsm_offset = 0;
    fsm_t *  fsm_word   = NULL;

    if( unlikely( header == NULL ) )
        return;

    /*
     * FSM is laid out like:
     * fsm word             ... bits ...
     * [n]  n*64 - 1         ...                       n*64 - 64
     * ...
     * [1]  ...                           68   67   66   65   64
     * [0]  ...                            4    3    2    1    0
     */
    fsm_index  = ( ind / FSM_WIDTH );
    fsm_offset = ind - ( ( ind / FSM_WIDTH ) * FSM_WIDTH );
    fsm_word   = ( fsm_t * ) _PTR_ADD_OFFSET(
        get_ptr_fast( header->fsm ),
        fsm_index * sizeof( fsm_t )
    );

    if( fsm_word == NULL )
        return;

    *fsm_word &= ~( ( fsm_t ) 1 << fsm_offset );

    return;
}

static __inline__ bool _get_fsm_element_by_index( shalloc_header * header, uint64_t ind )
{
    uint32_t fsm_index  = 0;
    uint8_t  fsm_offset = 0;
    fsm_t *  fsm_word   = NULL;

    if( unlikely( header == NULL ) )
    {
        errno = EINVAL;
        return false;
    }

    /*
     * FSM is laid out like:
     * fsm word             ... bits ...
     * [n]  n*64 - 1         ...                       n*64 - 64
     * ...
     * [1]  ...                           68   67   66   65   64
     * [0]  ...                            4    3    2    1    0
     */
    fsm_index  = ( ind / FSM_WIDTH );
    fsm_offset = ind - ( ( ind / FSM_WIDTH ) * FSM_WIDTH );
    fsm_word   = ( fsm_t * ) _PTR_ADD_OFFSET(
        get_ptr_fast( header->fsm ),
        fsm_index * sizeof( fsm_t )
    );

    if( ( ( *fsm_word >> fsm_offset ) & ( fsm_t ) 1 ) > 0 )
        return true;

    return false;
}

// XXX: __find_fsm_spot() seems to be having issues locating the next opening after an allocation has been made.
static __inline__ uint64_t _get_fsm_slot_by_width( shalloc_header * header, uint64_t width )
{
    uint64_t bit_position = 0;
    uint64_t fsm_index    = 0;

    bit_position = __find_fsm_spot( header, width );

    fprintf( stdout, "Got bit position %lu for initial FSM search (req: %lu)\n", bit_position, width );
    //if( unlikely( bit_position == ULONG_MAX ) )
    //    bit_position = __find_fsm_spot( header, width, true );

    if( unlikely( bit_position == ULONG_MAX ) )
    {
        errno = ENOSPC;
        return 0;
    }

    // Mark field as used TODO this can be done in bulk i'm just lazy
    fprintf( stdout, "ISSUING ALLOCATION FOR INDEX %lu\n", bit_position );
    fprintf( stdout, "FSM prior:\n" );

    // This need to start at the highest fsm_word_i and start marking from there
    for( fsm_index = bit_position; fsm_index < bit_position + width; fsm_index++ )
    {
        // this can be bulkified so we dont have to call this routine for every single bit
        _set_fsm_element_by_index( header, fsm_index );
    }
    // XXX: We need to track how large the allocation is for purposes of freeing later

    if( !_set_allocset_element_by_index( header, bit_position, width ) )
    {
        fprintf( stderr, "_get_fsm_slot_by_width: failed to set allocset\n" );
        errno = EINVAL;
        return 0;
    }
    
    #ifdef SLAB_DEBUG
    fprintf( stdout, "_get_fsm_slot_by_width( %p, %lu ) FSM SNAPSHOT\n", header, width );
    _print_fsm( header );
    #endif // SLAB_DEBUG

    return bit_position;
}

/*
 * __find_fsm_spot( shalloc_header *, uint64_t )
 *   - shalloc_header * header - the headers whose FSM we are searching
 *   - uint64_t requested_length - the width of the allocation needed in <objects>, not bytes
 * Performs a fast masked search of a bitfield searching for a free area
 * In order to handle referential integrity iff the page gets resized, the
 * search begins at the 'rear' (nth index) of the FSM, and moves towards the
 * 0th index for large allocations. Small (single) allocations are done by a
 * separate subroutine, which searches from the 'front' (0th index) of the FSM.
 *
 */
static __inline__ uint64_t __find_fsm_spot(
    shalloc_header * header,
    uint64_t         requested_length
)
{
    uint32_t           fsm_i            = 0;
    uint32_t           fsm_length       = 0;
    register uint64_t  bits_comp        = requested_length;
    uint64_t           position         = 0;
    register uint64_t  iter             = 0;
    fsm_cmp_t          last_word_val    = 0;
    register uint8_t   fsm_word_i       = 0;
    fsm_t              fsm_word         = 0;
    register fsm_cmp_t temp             = 0;
    register fsm_cmp_t mask             = ( fsm_cmp_t ) ULONG_MAX;
    fsm_cmp_t          mask_last        = ( fsm_cmp_t ) ULONG_MAX;
    fsm_t              skip_mask        = ( fsm_t ) ULONG_MAX;
    register bool      compare_active   = false;
    register bool      last_word        = false;

    fsm_length = _get_fsm_length( header );
    mask_last  = ~( mask_last << ( requested_length % FSM_SHIFT_WIDTH ) );

    // Note that the position / iter expressed in these statements is inverted (directionally) prior
    // to return to caller, instead of the 0th element being the LSB of the 0th word, it's the MSB of the nth word.
//    #ifdef SLAB_DEBUG
//    fprintf( stdout, "__find_fsm_spot( %p, %lu ) startup\n", header, requested_length );
//    #endif // SLAB_DEBUG
    if( requested_length <= FSM_SHIFT_WIDTH )
    {
        last_word      = true;
        compare_active = true;
        mask           = mask_last;
    }

    for( fsm_i = 0; fsm_i < fsm_length; fsm_i++ )
    { // Iterate over words of sizeof( fsm_t ) bytes
        fsm_word = *( ( fsm_t * ) _PTR_ADD_OFFSET(
            get_ptr_fast( header->fsm ),
            ( ( fsm_length - 1 - fsm_i ) * sizeof( fsm_t ) )
        )); // Deref in outer loop

        if( ( fsm_word & skip_mask ) == skip_mask )
        { // Mask out the fsm word, if it's filled we can jump ahead by the full width
            iter     += FSM_WIDTH;
            position += FSM_WIDTH;
//            #ifdef SLAB_DEBUG
//            fprintf( stdout, "Fast skipped to iter %lu\n", ( uint64_t ) iter );
//            #endif // SLAB_DEBUG
            continue;
        }

        for( fsm_word_i = 0; fsm_word_i < FSM_RATIO; fsm_word_i++ )
        { // Iterate over words within the given fsm_t word, size FSM_SHIFT_WIDTH bits
            temp = ( fsm_cmp_t ) ( fsm_word >> ( ( fsm_word_i ) * FSM_SHIFT_WIDTH ) );
//            #ifdef SLAB_DEBUG
//            fprintf(
//                stdout,
//                "fsm_i: %lu, fsm_word_i: %lu, position %lu, iter %lu, bits_comp: %lu, last_word %s, compare_active %s\n",
//                ( uint64_t ) fsm_i,
//                ( uint64_t ) fsm_word_i,
//                ( uint64_t ) position,
//                ( uint64_t ) iter,
//                ( uint64_t ) bits_comp,
//                last_word ? "T" : "F",
//                compare_active ? "T" : "F"
//            );
//            fprintf( stdout, "Current FSM Word:\n" );
//            print_bin( ( uint64_t ) fsm_word );
//            fprintf( stdout, "Temp:\n" );
//            print_bin( ( uint64_t ) temp );
//            fprintf( stdout, "Mask:\n" );
//            print_bin( ( uint64_t ) mask );
//            #endif // SLAB_DEBUG
            if( ( ~(temp) & mask ) == mask )
            {
                if( last_word )
                { // Prep for return & attempt to compactify past word boundaries
                    // Early exit when shifting wont help
//                    #ifdef SLAB_DEBUG
//                    fprintf( stdout, "Early exit triggered for position %lu\n", ( uint64_t ) position );
//                    #endif // SLAB_DEBUG
                    if( ( last_word_val & FSM_LAST_WORD_MASK ) > 0 )
                        return header->max_allocations - ( position + requested_length );

                    mask = ( fsm_cmp_t ) FSM_LAST_WORD_MASK;
                    temp = last_word_val;

                    while( ( ~temp & mask ) != 0 )
                    {
                        if( temp == 0 )
                            break;
                        temp = temp << 1;
                        position--;
                    }

                    return header->max_allocations - ( position + requested_length );
                }

                bits_comp -= FSM_SHIFT_WIDTH;
                last_word  = ( bits_comp <= FSM_SHIFT_WIDTH );

                if( !compare_active )
                    compare_active = true;

                if( last_word )
                    mask = mask_last;
            }
            else
            {   // No match
//                #ifdef SLAB_DEBUG
//                fprintf( stdout, "No match - state reset.\n" );
//                #endif // SLAB_DEBUG
                if( compare_active )
                { // reset counters and markers
                    bits_comp = requested_length;
                    position  = iter;
                }

                compare_active = false;

                if( last_word )
                {
                    last_word = false;
                    if( requested_length > FSM_SHIFT_WIDTH )
                        mask = ( uint16_t ) ULONG_MAX;
                }
            }

            iter += FSM_SHIFT_WIDTH;

            if( !compare_active )
            {
                position     += FSM_SHIFT_WIDTH;
                last_word_val = temp;
            }
        }
    }

    return ULONG_MAX;
}

// Gets the number of fsm_t's we'll need to store the bitmap of allocations
static __inline__ uint64_t _get_fsm_length( shalloc_header * header )
{
    uint64_t fsm_length = 0;
    if( unlikely( header == NULL ) )
        return 0;

    fsm_length = header->max_allocations / FSM_WIDTH;

    if( header->max_allocations % FSM_WIDTH != 0 )
        fsm_length++;

    return fsm_length;
}

void slab_set_count_hint( context_t ctx, size_t count_hint )
{
    shalloc_header * header = NULL;

    #ifdef SLAB_DEBUG
    fprintf( stdout, "Validating context in slab count hint\n" );
    #endif // SLAB_DEBUG
    if( !check_context( ctx ) )
        return;

    header = &(mapped_control->headers[ctx]);
    header->count_hint = count_hint;
    return;
}

static __inline__ bool _init_slab( context_t ctx, shalloc_header * header, bool zero_fill )
{
    shm_handle handle         = SEGMENT_HANDLE_INVALID;
    void *     mapped_address = NULL;
    void *     alloc          = NULL;
    void *     fsm            = NULL;
    void *     c_allocstart   = NULL;
    void *     c_fsmstart     = NULL;
    void *     c_fsmend       = NULL;
    void *     temp           = NULL;
    uint64_t   i              = 0;
    uint64_t   available      = 0;
    uint64_t   unit_size      = 0;
    uint64_t   count_hint     = 0;
    uint64_t   initial_size   = 0;
    void *     allocset       = NULL;

    if( ctx == INVALID_CONTEXT || header == NULL )
        return false;

    // Don't want to trash an initialized segment and be idempotent
    if( header->segment != SEGMENT_HANDLE_INVALID )
    {
        // Seems to be already allocated
        if( header->n_allocs > 0 || header->max_allocations > 0 )
            return true;
    }

    // Unit size - stores sizeof( fsm_t ) * CHAR_BIT allocations
    unit_size = ( sizeof( fsm_t ) * CHAR_BIT * header->object_size ) + sizeof( fsm_t );

    count_hint = header->count_hint;
    fprintf( stdout, "Count hint: %lu\n", ( uint64_t ) header->count_hint );
    if( count_hint == 0 )
        count_hint = SLAB_DEFAULT_ALLOCATION;

    initial_size = ( count_hint / ( sizeof( fsm_t ) * CHAR_BIT ) );

    if( initial_size == 0 )
    {
        fprintf( stderr, "Bad count hint, defaulting to single allocation unit\n" );
        initial_size = 1;
    }

    initial_size = initial_size * unit_size + sizeof( canary_t ) * 3;

    mapped_address = new_segment( initial_size );

    if( mapped_address == NULL )
        return false;

    handle = get_handle_from_ptr( mapped_address );
    header->segment         = handle;

    // Available space - accounting for headers and canaries
    available = ( get_segment_size( handle ) - ( 3 * sizeof( canary_t ) ) );
    header->max_allocations = ( available / unit_size ) * CHAR_BIT * sizeof( fsm_t );
    
    allocset = new_segment( header->max_allocations * sizeof( uint32_t ) );
    header->allocset_handle = get_handle_from_ptr( allocset );
    header->allocset = get_ref( allocset );
    header->max_allocset = get_segment_size( header->allocset_handle ) / sizeof( uint32_t );
    fprintf( stdout, "Allocset given handle %lu\n, max_allocset %lu\n", ( uint64_t ) header->allocset_handle, ( uint64_t ) header->max_allocset );
    fprintf(
        stdout,
        "Max allocations is %lu objects of size %lu\n",
        ( uint64_t ) header->max_allocations,
        ( uint64_t ) header->object_size
    );
    header->n_allocs        = 0;
    header->c_allocstart    = ( canary_t ) random();
    header->c_fsmstart      = ( canary_t ) random();
    header->c_fsmend        = ( canary_t ) random();

    // Layout setup - we'll calculate locally for readability, then convert to __ref
    c_allocstart = mapped_address;
    alloc        = ( void * ) _PTR_ADD_OFFSET( c_allocstart, sizeof( canary_t ) );
    c_fsmstart   = ( void * ) _PTR_ADD_OFFSET( alloc, ( header->object_size * header->max_allocations ) );
    fsm          = ( void * ) _PTR_ADD_OFFSET( c_fsmstart, sizeof( canary_t ) );
    c_fsmend     = ( void * ) _PTR_ADD_OFFSET( fsm, ( sizeof( fsm_t ) * _get_fsm_length( header ) ) );

    fprintf(
        stdout,
        "Layout:\n  Allocstart canary: %p\n  Alloc: %p\n  FSMstart Canary: %p\n  FSM: %p\n  FSMend Canary %p\n",
        c_allocstart,
        alloc,
        c_fsmstart,
        fsm,
        c_fsmend
    );
    // Write out canaries
    *( ( canary_t * ) c_allocstart ) = header->c_allocstart;
    *( ( canary_t * ) c_fsmstart )   = header->c_fsmstart;
    *( ( canary_t * ) c_fsmend )     = header->c_fsmend;

    // Initialize FSM to point to every alloc element
    for( i = 0; i < _get_fsm_length( header ); i++ )
    {
        temp = ( void * ) _PTR_ADD_OFFSET( fsm, ( sizeof( fsm_t ) * i ) );
        *( ( fsm_t * ) temp ) = ( fsm_t ) 0;
    }

    header->allocs           = get_ref( alloc );
    header->fsm              = get_ref( fsm );
    header->loc_c_allocstart = get_ref( c_allocstart );
    header->loc_c_fsmstart   = get_ref( c_fsmstart );
    header->loc_c_fsmend     = get_ref( c_fsmend );
    // Indexes to the word and bit positions in the FSM. We ignore endian-ness
    // and treat it as an array with 0'th position being leftmost and nth being
    // rightmost
    header->i_rear_fsm_word  = _get_fsm_length( header ) - 1;
    header->i_front_fsm_bit  = 0;

    if(
           ref_is_null( header->allocs )
        || ref_is_null( header->fsm )
      )
    {
        return false;
    }

    if( zero_fill == true )
    {
        memset(
            get_ptr_fast( header->allocs ),
            ( unsigned char ) _ZERO_FILL_BYTE,
            ( header->object_size * header->max_allocations )
        );
    }

    return _check_canaries( header );
}

bool force_canary_check( context_t ctx )
{
    shalloc_header * header = NULL;
    #ifdef SLAB_DEBUG
    fprintf( stdout, "Validating context in canary check\n" );
    #endif // SLAB_DEBUG
    if( !check_context( ctx ) )
        return false;

    header = &(mapped_control->headers[ctx]);

    return _check_canaries( header );
}

shalloc_header * get_header_by_context( context_t ctx )
{
    return _get_header_by_context( ctx );
}

static __inline__ shalloc_header * _get_header_by_context( context_t ctx )
{
    #ifdef SLAB_DEBUG
    fprintf( stdout, "Validating context in header retreival by context\n" );
    #endif // SLAB_DEBUG
    if( unlikely( !check_context( ctx ) ) )
        return NULL;

    return &(mapped_control->headers[ctx]);
}

static __inline__ uint32_t _get_allocset_element_by_index(
    shalloc_header * header,
    uint32_t         index
)
{
    uint32_t * ptr = NULL;

    if( unlikely( header == NULL ) )
        return UINT_MAX;
    
    if( unlikely( index > header->max_allocset ) )
        return UINT_MAX;

    ptr = ( uint32_t * ) _PTR_ADD_OFFSET(
        get_ptr_fast( header->allocset ),
        sizeof( uint32_t ) * index
    );

    if( unlikely( ptr == NULL ) )
        return UINT_MAX;
    
    return *ptr;
}

static __inline__ bool _set_allocset_element_by_index(
    shalloc_header * header,
    uint32_t         index,
    uint32_t         value
)
{
    uint32_t * ptr = NULL;

    if( unlikely( header == NULL ) )
        return false;

    if( unlikely( index > header->max_allocset ) )
        return false;

    ptr = ( uint32_t * ) _PTR_ADD_OFFSET(
        get_ptr_fast( header->allocset ),
        sizeof( uint32_t ) * index
    );

    if( unlikely( ptr == NULL ) )
        return false;

    *ptr = value;

    return true;
}

static __inline__ bool _fail_canary( void )
{
#ifdef _FORCE_SIGSEGV_ON_CANARY_FAILURE
    *( ( uint8_t * ) 0 ) = 42;
#endif // _FORCE_SIGSEGV_ON_CANARY_FAILURE
    return false;
}

static __inline__ bool _check_canaries( shalloc_header * header )
{
    register canary_t * c_ptr = NULL;

    if( unlikely( header == NULL ) )
        return false;

    // Check start of slab
    #ifdef SLAB_DEBUG
    fprintf( stdout, "Checking allocstart canary\n" );
    #endif // SLAB_DEBUG
    c_ptr = get_ptr_fast( header->loc_c_allocstart );

    if( unlikely( c_ptr == NULL ) )
        return false;
    if( unlikely( header->c_allocstart != *c_ptr ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "Failed allocstart canary check: Got %x, expected %x\n",
            ( uint32_t ) *c_ptr,
            ( uint32_t ) header->c_allocstart
        );
    #endif // SLAB_DEBUG
        return _fail_canary();
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    // Check start of FSM
    #ifdef SLAB_DEBUG
    fprintf( stdout, "Checking fsmstart canary\n" );
    #endif // SLAB_DEBUG
    c_ptr = get_ptr_fast( header->loc_c_fsmstart );

    if( unlikely( c_ptr == NULL ) )
        return false;
    if( unlikely( header->c_fsmstart != *c_ptr ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "Failed fsmstart canary check: Got %x, expected %x at %p\n",
            ( uint32_t ) *c_ptr,
            ( uint32_t ) header->c_fsmstart,
            c_ptr
        );
    #endif // SLAB_DEBUG
        return _fail_canary();
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    // Check end of FSM
    #ifdef SLAB_DEBUG
    fprintf( stdout, "Checking fsmend canary\n" );
    #endif // SLAB_DEBUG
    c_ptr = get_ptr_fast( header->loc_c_fsmend );

    if( unlikely( c_ptr == NULL ) )
        return false;
    if( unlikely( header->c_fsmend != *c_ptr ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "Failed fsmend canary check: Got %x, expected %x\n",
            ( uint32_t ) *c_ptr,
            ( uint32_t ) header->c_fsmend
        );
    #endif // SLAB_DEBUG
        return _fail_canary();
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    return true;
}

static __inline__ context_t get_ctx_by_id( const char * ident )
{
    header_iter i        = ( header_iter ) 0;
    char *      i_ident = NULL;
    bool        found    = false;

    for( i = 0; i < ( header_iter ) _SHALLOC_MAX_SLABS; i++ )
    {
        i_ident = mapped_control->headers[i].object_id;

        if( strncmp( i_ident, ident, _SHALLOC_MAX_IDENT ) == 0 )
        {
            found = true;
            break;
        }
    }

    if( found == true )
        return ( context_t ) i;

    return INVALID_CONTEXT;
}

static bool check_slab_state( void )
{
    if( unlikely( _slab_init == false ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "Slab is not initialized\n"
        );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( mapped_control == NULL || control_segment == SEGMENT_HANDLE_INVALID )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "Control segment mapping is NULL or handle is invalid\n"
        );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( mapped_control->magic != _SHALLOC_CONTROL_MAGIC )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "Bad magic: %x\n",
            mapped_control->magic
        );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG
    return true;
}

static bool check_shalloc_header( header_iter index )
{
    shalloc_header * header = NULL;

    if( unlikely( !check_slab_state() ) )
        return false;

    if( unlikely( index > _SHALLOC_MAX_SLABS ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "index (%lu) is out of bounds for _SHALLOC_MAX_SLABS (%lu)\n",
            ( uint64_t ) index,
            ( uint64_t ) _SHALLOC_MAX_SLABS
        );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    header = &(mapped_control->headers[index]);

    if( header->magic != _SHALLOC_HEADER_MAGIC )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "Bad header magic %x at index %lu\n",
            header->magic,
            ( uint64_t ) index
        );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    return true;
}

static __inline__ bool check_context( context_t ctx )
{
    if( unlikely( ctx == INVALID_CONTEXT ) ) // Sanity check the context
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "Context check failed: INVALID_CONTEXT\n" );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( mapped_control == NULL ) ) // check that we're mapped
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "Context check failed: control segment not mapped\n" );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( ctx >= ( context_t ) _SHALLOC_MAX_SLABS ) ) // Ensure no overrun
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "Context check failed: context out of bounds\n" );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( mapped_control->headers[ctx].magic != _SHALLOC_HEADER_MAGIC ) ) // See if it's reachable
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "Context check failed: context's magic is bad\n" );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( mapped_control->headers[ctx].self != ctx ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "Context check failed: context not initialized\n" );
    #endif // SLAB_DEBUG
        return false;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    return true;
}

__ref move_to_shared( context_t ctx, void ** pointer, size_t size )
{
    shalloc_header * header = NULL;

    header = _get_header_by_context( ctx );

    return _move_to_shared( header, pointer, size, true );
}

static __inline__ __ref _move_to_shared(
    shalloc_header * header,
    void **          pointer,
    size_t           size,
    bool             do_free
)
{
    void *           target = NULL;
    __ref            retref = {0};

    retref = get_null_ref();
    // We're trusting the user to have set a correct object size - we can only do cursory checks
    if( unlikely( pointer == NULL || *pointer == NULL ) )
        return retref;

    if( unlikely( header == NULL ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "move_to_shared: Invalid context.\n" );
    #endif // SLAB_DEBUG
        return retref;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( ( size % header->object_size ) != 0 ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "move_to_shared: requested size %zu is not a multiple of object size %lu\n",
            size,
            ( uint64_t ) header->object_size
        );
    #endif // SLAB_DEBUG
        return retref;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( !__TNS_MUTEX( &(header->locked) ) ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "move_to_shared: Failed to obtain header lock\n" );
    #endif // SLAB_DEBUG
        return retref;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    retref = _shmalloc( header->self, size, false );
    target = get_ptr_fast( retref );

    if( unlikely( ref_is_null( retref ) || target == NULL ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "move_to_shared: Failed to allocate shared memory\n"
        );
    #endif // SLAB_DEBUG
        return retref;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    memcpy(
        target,
        *pointer,
        size
    );

    if( do_free )
    {
        free( *pointer );
        *pointer = NULL;
    }

    return retref;
}

void * move_to_local( context_t ctx, __ref * ref )
{
    shalloc_header * header = NULL;

    header = _get_header_by_context( ctx );
    return _move_to_local( header, ref, true );
}

static __inline__ void * _move_to_local( shalloc_header * header, __ref * ref, bool do_free )
{
    void *           target = NULL;
    void *           source = NULL;
    size_t           size   = 0;
    uint64_t         index  = 0;

    if( unlikely( header == NULL ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "move_to_local: Invalid context.\n" );
    #endif // SLAB_DEBUG
        return NULL;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( !__TNS_MUTEX( &(header->locked) ) ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "move_to_local: Failed to lock header\n" );
    #endif // SLAB_DEBUG
        return NULL;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    if( unlikely( !_ref_get_index_and_size( header, *ref, &index, &size ) ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "move_to_local: Failed to get allocation info for __ref\n"
        );
    #endif // SLAB_DEBUG
        return NULL;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    source = ( void * ) get_ptr_fast( *ref );

    if( unlikely( source == NULL ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "move_to_local: NULL reference\n" );
    #endif // SLAB_DEBUG
        return NULL;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    target = malloc( header->object_size * size );

    if( unlikely( target == NULL ) )
    #ifdef SLAB_DEBUG
    {
        fprintf( stderr, "move_to_local: Insufficient memory available\n" );
    #endif // SLAB_DEBUG
        return NULL;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    memcpy(
        target,
        source,
        header->object_size * size
    );

    __C_MUTEX( &(header->locked) );
    if( do_free )
    {
        sfree( header->self, *ref );
        *ref = get_null_ref();
    }

    return target;
}

void print_fsm( shalloc_header * header )
{
    #ifdef SLAB_DEBUG
    _print_fsm( header );
    #endif // SLAB_DEBUG
    return;
}

#ifdef SLAB_DEBUG
static void _print_fsm( shalloc_header * header )
{
    uint64_t i = 0;
    fsm_t * fsm_word = NULL;
    uint64_t len = 0;

    if( header == NULL )
        return;

    len = _get_fsm_length( header );
    fprintf( stdout, "----- FSM %p -----\n", get_ptr( header->fsm ) );

    for( i = 0; i < len; i++ )
    {
        fsm_word = (fsm_t *) _PTR_ADD_OFFSET(
            get_ptr( header->fsm ),
            ( ( len - 1 - i ) * sizeof( fsm_t ) )
        );

        fprintf( stdout, "FSM[%lu] (%p): ", ( len - 1 - i ), fsm_word );
        print_bin( ( uint64_t ) *fsm_word );
    }

}

static void print_byte( uint8_t data )
{
    fprintf( stdout, "%s%s", bits[data >> 4], bits[ data & 0x0F ] );
    return;
}

static void print_bin( uint64_t data )
{
    print_byte( ( uint8_t ) ( ( data >> 56 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 48 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 40 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 32 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 24 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 16 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 8  ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data       ) & 0xFF ) );
    fprintf( stdout, "\n" );
    return;
}

static void _dump_header( shalloc_header * header )
{
    void *   ptr        = NULL;
    uint64_t i          = 0;
    uint64_t j          = 0;
    uint64_t k          = 0;
    uint32_t alloc_size = 0;

    if( header == NULL )
        return;

    fprintf( stdout, "==== SHALLOC HEADER %p\n", header );
    fprintf(
        stdout,
        "  magic: %x\n"
        "  segment: %lu\n"
        "  object_size: %zu\n"
        "  count_hint: %zu\n"
        "  allocs: %p\n"
        "  n_allocs: %lu\n"
        "  fsm: %p\n"
        "  max_allocations: %lu\n"
        "  object_id: %s\n"
        "  self: %lu\n"
        "  locked: %s\n"
        "  i_front_fsm_bit: %lu\n"
        "  i_rear_fsm_word: %lu\n"
        "  loc_c_allocstart: %p\n"
        "  c_allocstart: %x %x\n"
        "  loc_c_fsmstart: %p\n"
        "  c_fsmstart: %x %x\n"
        "  loc_c_fsmend: %p\n"
        "  c_fsmend: %x %x\n"
        "  allocset: %p\n"
        "  max_allocset %u\n"
        "  allocset_handle %lu\n",
        ( uint32_t ) header->magic,
        ( uint64_t ) header->segment,
        ( size_t ) header->object_size,
        ( size_t ) header->count_hint,
        ( void * ) get_ptr( header->allocs ),
        ( uint64_t ) header->n_allocs,
        ( void * ) get_ptr( header->fsm ),
        ( uint64_t ) header->max_allocations,
        ( char * ) header->object_id,
        ( uint64_t ) header->self,
        header->locked ? "T" : "F",
        ( uint64_t ) header->i_front_fsm_bit,
        ( uint64_t ) header->i_rear_fsm_word,
        ( void * ) get_ptr( header->loc_c_allocstart ),
        ( uint32_t ) ( ( ( uint64_t ) header->c_allocstart ) >> 32 ),
        ( uint32_t ) header->c_allocstart,
        ( void * ) get_ptr( header->loc_c_fsmstart ),
        ( uint32_t ) ( ( ( uint64_t ) header->c_fsmstart ) >> 32 ),
        ( uint32_t ) header->c_fsmstart,
        ( void * ) get_ptr( header->loc_c_fsmend ),
        ( uint32_t ) ( ( ( uint64_t ) header->c_fsmend ) >> 32 ),
        ( uint32_t ) header->c_fsmend,
        ( void * ) get_ptr( header->allocset ),
        ( uint32_t ) header->max_allocset,
        ( uint64_t ) header->allocset_handle

    );
    fprintf( stdout, "---- HEADER DATA DETAIL:\n-- FSM:\n" );
    _print_fsm( header );
    fprintf( stdout, "-- ALLOCS[]:\n" );
    for( i = 0; i < header->max_allocations; i++ )
    {
        ptr = _PTR_ADD_OFFSET(
            get_ptr( header->allocs ),
            ( i * header->object_size )
        );

        alloc_size = _get_allocset_element_by_index( header, i );

        if( alloc_size == UINT_MAX )
        {
            fprintf( stderr, "allocset index %lu invalid\n", i );
            continue;
        }
        
        if( alloc_size == 0 )
            continue;

        fprintf( stdout, "ALLOC[%lu] (%p):\n", ( uint64_t ) i, ptr );

        for( j = 0; j < alloc_size; j++ )
        {
            ptr = _PTR_ADD_OFFSET( ptr, header->object_size );
            for( k = 0; k < header->object_size; k++ )
            {
                fprintf(
                    stdout,
                    "%s%s",
                    hexes[*( ( uint8_t * ) _PTR_ADD_OFFSET( ptr, k )) >> 4],
                    hexes[*( ( uint8_t * ) _PTR_ADD_OFFSET( ptr, k )) & 0x0F]
                );
            }

            fprintf( stdout, "\n" );
        }
    }

    fprintf( stdout, "-- ALLOCSET[]\n" );
    for( i = 0; i < header->max_allocset; i++ )
    {
        ptr = _PTR_ADD_OFFSET( get_ptr( header->allocset ), i * sizeof( uint32_t ) );
        if( *((uint32_t * ) ptr) == 0 )
            continue;
        fprintf( stdout, "ALLOCSET[%lu]: %lu\n", ( uint64_t ) i, ( uint64_t ) *(( uint32_t * ) ptr) );
    }
    fprintf( stdout, "==============================\n" );
    return;
}

static void _dump_context( context_t ctx )
{
    shalloc_header * header = NULL;
    header = _get_header_by_context( ctx );

    return _dump_header( header );
}
void dump_context( context_t ctx )
{
    return _dump_context( ctx );
}
#endif // SLAB_DEBUG
