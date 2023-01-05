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

static shm_handle     control_segment              = SEGMENT_HANDLE_INVALID;
#ifdef _SLAB_CONTROL_IN_OWN_SEGMENT
static __ref          control_segment_address      = {0};
#endif // _SLAB_CONTROL_IN_OWN_SEGMENT
static slab_control * control                      = NULL;
static bool           _slab_init                   = false;
static pid_t          p_pid                        = 0;
static ptr_cache      __ptr_cache[_SLAB_MAX_SLABS] = {{0}};
static bool           __no_test                    = true;

// Check and boilerplate helpers
static INLINE bool _fail_canary( void ) ALWAYS_INLINE_FLATTEN;
static INLINE bool _init_slab( slab_header *, bool );
static INLINE context_t get_ctx_by_id( const char * ) ALWAYS_INLINE_FLATTEN;
static INLINE bool check_slab_header( header_iter ) ALWAYS_INLINE_FLATTEN;
static INLINE bool _check_context( context_t ) ALWAYS_INLINE_FLATTEN;
static INLINE bool _check_canaries( slab_header * ) ALWAYS_INLINE_FLATTEN;
static INLINE __ref _get_alloc_element_by_index( slab_header *, uint64_t ) ALWAYS_INLINE_FLATTEN;
static INLINE slab_header * _get_header_by_context( context_t ) ALWAYS_INLINE_FLATTEN;
static INLINE size_t _get_allocset_element_by_index( slab_header *, uint64_t );

// Allocation helpers / primitives
static INLINE context_t _new_slab( const char *, size_t, uint64_t );
static INLINE ref_t _shmalloc( slab_header *, size_t, bool );
static INLINE bool _realloc_internal( slab_header *, uint64_t );
static INLINE ref_t _shrealloc( slab_header *, ref_t, uint64_t );
static INLINE void _shfree( slab_header *, ref_t, bool ) ALWAYS_INLINE_FLATTEN;
static INLINE bool __free_by_index( slab_header *, uint64_t, size_t, bool ) ALWAYS_INLINE_FLATTEN;

// FSM helpers
static INLINE uint64_t _get_fsm_length( slab_header * ) ALWAYS_INLINE_FLATTEN;
static INLINE void _set_fsm_element_by_index( slab_header *, uint64_t ) ALWAYS_INLINE_FLATTEN;
static INLINE void _set_fsm_elements_by_range( slab_header *, uint64_t, uint64_t ) ALWAYS_INLINE_FLATTEN;
static INLINE void _clear_fsm_element_by_index( slab_header *, uint64_t ) ALWAYS_INLINE_FLATTEN;
static INLINE void _clear_fsm_elements_by_range( slab_header *, uint64_t, uint64_t ) ALWAYS_INLINE_FLATTEN;
static INLINE bool _get_fsm_element_by_index( slab_header *, uint64_t ) ALWAYS_INLINE_FLATTEN;
static INLINE uint64_t _get_fsm_slot_by_width( slab_header *, uint64_t, bool ) ALWAYS_INLINE_FLATTEN;
static INLINE uint64_t __find_fsm_spot( slab_header *, uint64_t ) ALWAYS_INLINE_FLATTEN;
static INLINE bool _ref_get_index_and_size( slab_header *, __ref, uint64_t *, size_t * ) ALWAYS_INLINE;

// Utility
static INLINE void * _move_to_local( slab_header *, ref_t *, bool ) ALWAYS_INLINE;
static INLINE ref_t _move_to_shared( slab_header *, void **, size_t, bool ) ALWAYS_INLINE;
static INLINE void _slab_log( slab_ll, char *, ... ) PRINTF;
static INLINE uint64_t _get_random( void );
static INLINE char * _get_context_name( slab_header * ) ALWAYS_INLINE;

// Allocset tools
static INLINE _as_ind_t _get_allocset_item_by_index( slab_header *, uint64_t index );
static INLINE _as_ind_t _get_available_allocset_item( slab_header * );
static INLINE bool _set_allocset_item_by_index( slab_header *, uint64_t, __ref, size_t, _as_ind_t );
static INLINE bool _clear_allocset_item_by_index( slab_header *, uint64_t );

// ref_t functions
static INLINE void * _to_ptr( slab_header *, ref_t ) ALWAYS_INLINE_FLATTEN_HOT;
static INLINE ref_t _to_ref( slab_header *, void * ) ALWAYS_INLINE_FLATTEN;

// Caching functions
static INLINE void * _as_ptr_cache( slab_header * ) ALWAYS_INLINE_FLATTEN_HOT;
static INLINE void * _allocs_ptr_cache( slab_header * ) ALWAYS_INLINE_FLATTEN_HOT;
static INLINE void * _fsm_ptr_cache( slab_header * ) ALWAYS_INLINE_FLATTEN_HOT;

static INLINE bool _sync_slab( slab_header *, bool ) ALWAYS_INLINE_FLATTEN_HOT;
static INLINE bool _sync_control( void ) ALWAYS_INLINE_FLATTEN_HOT;

#ifdef _SLAB_DEBUG
// Debugging
static void _dump_control( slab_control * );
static void _dump_header( slab_header *, bool );
static void _dump_context( context_t ); // calls dump_header()
static void print_byte( uint8_t );
static void print_bin( uint64_t );
static void _print_fsm( slab_header * );

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
#endif // _SLAB_DEBUG

void __test_harness( void )
{
    // for testing - quiets some 'error' we intend to hit
    __no_test = false;
    return;
}

void * to_ptr( context_t ctx, ref_t ref )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( ( header == NULL ) || ( ref == NULLREF ) ) )
    {
        _slab_log( LL_SLAB_ERROR, "Header is NULL (%p) or ref is NULLREF (%lu)", header, ( uint64_t ) ref );
        return NULL;
    }

    return _to_ptr( header, ref );
}

static INLINE void * _to_ptr( slab_header * header, ref_t ref )
{
    // Dereference a ref_t by getting the allocset's index (ref is the
    // typedef'd index of the allocset). We can either take the allocset and deref the
    // issued_ref (__ref) or index into allocs[]
    register void * ptr = NULL;

    if( unlikely( _as_ptr_cache( header ) == NULL ) )
    {
        // This slab has not been mapped!
        #ifdef _SLAB_DEBUG
        _slab_log( LL_SLAB_DEBUG, "Header %p may not be mapped", header );
        _slab_log( LL_SLAB_DEBUG, "Header Magic: %x", header->magic );
        _slab_log( LL_SLAB_DEBUG, "Header segment: %lu", ( uint64_t ) header->segment );
        _slab_log( LL_SLAB_DEBUG, "Context name: %s", header->object_id );
        #endif // _SLAB_DEBUG
        map_segment( header->segment );

    }

    ptr = _PTR_ADD_OFFSET(
        ( _as_ptr_cache( header ) ),
        ( sizeof( allocset_item_t ) * ( uint64_t ) ref )
    );

    if( unlikely( !_PTR_BOUND_CHECK( ptr, _as_ptr_cache( header ), header->allocset.max_allocset * sizeof( allocset_item_t ) ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "REF %u does not appear to be allocated in context %u (%s) - BP: %p S: %zu. Got %p",
            ( uint32_t ) ref,
            ( uint32_t ) header->self,
            _get_context_name( header ),
            _as_ptr_cache( header ),
            header->allocset.max_allocset * sizeof( allocset_item_t ),
            ptr
        );
        return NULL;
    }

    ptr = get_ptr( ( ( allocset_item_t * ) ptr )->issued_ref );

    #ifdef _SLAB_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "Returning pointer %p for ref %lu",
        ptr,
        ( uint64_t ) ref
    );
    #endif // _SLAB_DEBUG

    return ptr;

}

static INLINE ref_t _to_ref( slab_header * header, void * ptr )
{
    _as_ind_t         ind  = ALLOCSET_ITEM_INVALID;
    void *            base = NULL;
    allocset_item_t * item = NULL;
    uint32_t          i    = 0; //XXX

    if( unlikely( header->allocset.head == ALLOCSET_ITEM_INVALID ) )
        return NULLREF;

    ind = header->allocset.head;
    base = _as_ptr_cache( header );

    if( unlikely( base == NULL ) )
        return NULLREF;

    item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
        base,
        ( sizeof( allocset_item_t ) * ind )
    );

    if( unlikely( item == NULL ) )
        return NULLREF;

    // Check allocset head prior to moving through linked list
    if( _PTR_BOUND_CHECK( ptr, get_ptr( item->issued_ref ), item->size ) )
    {
        //_slab_log( LL_SLAB_ERROR, "Returning ref to head, as ind %lu", ( uint64_t ) ind );
        return ( ref_t ) ind;
    }

    while( item->next != ALLOCSET_ITEM_INVALID )
    {
        i++;
        ind  = item->next;
        item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
            base,
            ( sizeof( allocset_item_t ) * ind )
        );

        if( unlikely( item == NULL ) )
            return NULLREF;

        //_slab_log( LL_SLAB_ERROR, "CMP %p to %p, size %zu", ptr, get_ptr( item->issued_ref ), item->size );
        if( _PTR_BOUND_CHECK( ptr, get_ptr( item->issued_ref ), item->size ) )
        {
            //_slab_log( LL_SLAB_ERROR, "returning ref to ith item (%u), size %zu, as_ind %lu", i, item->size, ( uint64_t ) ind );
            return ( ref_t ) ind;
        }
    }

    return NULLREF;
}

ref_t to_ref( context_t ctx, void * ptr )
{
    // This is rather expensive, but we don't expect the user to invoke this a lot
    slab_header *     header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULLREF;

    if( unlikely( ptr == NULL ) )
        return NULLREF;

    return _to_ref( header, ptr );
}

bool sync_context( context_t ctx )
{
    slab_header * header = NULL;

    header = get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Could not sync context %lu: header is NULL",
            ( uint64_t ) ctx
        );
        return false;
    }

    if( !_sync_slab( header, false) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Could not sync context %lu",
            ( uint64_t ) ctx
        );
        return false;
    }

    if( !_sync_control() )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Could not sync slab control header"
        );

        return false;
    }

    return true;
}

static INLINE bool _sync_control( void )
{
    ctrl_header * header     = NULL;
    int32_t       save_errno = 0;

    // Fetch the base address for the entire control page from shm lib
    header = ( ctrl_header * ) get_control_header();

    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Could not sync control header: header is NULL"
        );

        return false;
    }

    if( !__TNS_MUTEX( &(header->locked) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to lock shm control header for sync"
        );
        return false;
    }

    save_errno = errno;

    if(
        msync(
            ( void * ) header,
            header->mapped_size,
            __SHM_SYNC_FLAGS
        )
      )
    {
        __C_MUTEX( &(header->locked) );
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to sync shm control header: %s",
            strerror( errno )
        );
        return false;
    }

    __C_MUTEX( &(header->locked) );
    errno = save_errno;
    return true;
}

static INLINE bool _sync_slab( slab_header * header, bool nolock )
{
    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Could not sync slab header, header is NULL"
        );

        return false;
    }

    // Place lock in slab-visible lock
    if( !nolock && !__TNS_MUTEX( &(header->locked) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to lock segment header %p prior to sync",
            header
        );

        return false;
    }

    if( !__SYNC( header->segment, false ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to sync header %p's segment %lu",
            header,
            ( uint64_t ) header->segment
        );

        if( !nolock )
            __C_MUTEX( &(header->locked) );

        return false;
    }

    if( !__SYNC( ref_get_segment( header->allocset.set ), false ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to sync header %p's allocset segment %lu",
            header,
            ( uint64_t ) header->allocset.set
        );

        if( !nolock )
            __C_MUTEX( &(header->locked) );

        return false;
    }

    if( !nolock )
        __C_MUTEX( &(header->locked) );

    return true;
}


/*
 * bool slab_init()
 *
 * Called by either the parent process, pre fork, to setup the control segment
 * (via shm.c), or called by child process(es), post-fork, to attach to the
 * control segment
 */
bool slab_init( void )
{
    void *      segment_address = NULL;
    header_iter i               = 0;
    size_t      ctrl_size       = 0;

    if( _slab_init == false )
    {
        ctrl_size = sizeof( slab_control )
                  + ( sizeof( slab_header ) * _SLAB_MAX_SLABS );
        // Parent initialization sequence
        p_pid = getpid();
        #ifndef _SLAB_CONTROL_IN_OWN_SEGMENT
        shm_init_extra( ctrl_size );
        segment_address = get_control_data_section();
        #else
        segment_address = new_segment( ctrl_size );
        #endif // _SLAB_CONTROL_IN_OWN_SEGMENT

        if( segment_address == NULL )
            return false;

        srand( ( unsigned int ) _INVALID_CONTEXT );
        #ifdef _SLAB_CONTROL_IN_OWN_SEGMENT
        control_segment = ref_get_segment( get_ref( segment_address ) );
        #else
        control_segment = get_control_segment();
        #endif // _SLAB_CONTROL_IN_OWN_SEGMENT
        _slab_init      = true;
        control  = ( slab_control * ) segment_address;

        control->magic = ( uint64_t ) _SLAB_CONTROL_MAGIC;

        for( i = 0; i < _SLAB_MAX_SLABS; i++ )
        {
            control->headers[i].magic       = ( uint64_t ) _SLAB_HEADER_MAGIC;
            control->headers[i].segment     = SEGMENT_HANDLE_INVALID;
            control->headers[i].object_size = 0;
            control->headers[i].allocs      = get_null_ref();
            control->headers[i].n_allocs    = 0;
            control->headers[i].fsm         = get_null_ref();
            control->headers[i].self        = INVALID_CONTEXT;
            control->headers[i].locked      = false;
            control->headers[i].count_hint  = 0;

            control->headers[i].c_allocstart     = ( canary_t ) _get_random();
            control->headers[i].c_fsmstart       = ( canary_t ) _get_random();
            control->headers[i].c_fsmend         = ( canary_t ) _get_random();
            control->headers[i].loc_c_allocstart = get_null_ref();
            control->headers[i].loc_c_fsmstart   = get_null_ref();
            control->headers[i].loc_c_fsmend     = get_null_ref();

            control->headers[i].allocset.locked       = false;
            control->headers[i].allocset.max_allocset = 0;
            control->headers[i].allocset.used         = 0;
            control->headers[i].allocset.head         = ALLOCSET_ITEM_INVALID;
            control->headers[i].allocset.tail         = ALLOCSET_ITEM_INVALID;
            memset(
                control->headers[i].object_id,
                '\0',
                ( size_t ) _SLAB_MAX_IDENT
            );
        }

        __FENCE();

        if( unlikely( !__SYNC_CONTROL_HEADER() ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "Failed to sync control headers after initialization"
            );
        }
    }
    else if( likely( shm_is_init() || p_pid != getpid() ) )
    {
        // child initialization sequence

        #ifdef _SLAB_DEBUG
        _slab_log( LL_SLAB_DEBUG, "Initializing slab in child context" );
        #endif // _SLAB_DEBUG
        shm_child_init();
        #ifndef SLAB_LAZY_LOAD
        map_all();
        #endif // SLAB_LAZY_LOAD
        #ifndef _SLAB_CONTROL_IN_OWN_SEGMENT
        segment_address = get_control_data_section();
        #else
        segment_address = get_ptr( control_segment_address );
        #endif // _SLAB_CONTROL_IN_OWN_SEGMENT
        // segment address will be automatically mapped in when the __ref
        // is dereferenced
        #ifdef _SLAB_DEBUG
        _slab_log(
            LL_SLAB_DEBUG,
            "Child has control segment %lu",
            ( uint64_t ) control_segment_address
        );
        #endif // _SLAB_DEBUG
        if(
                segment_address == NULL
             || control_segment == SEGMENT_HANDLE_INVALID
          )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "No control segment - has parent initialized the slab?"
            );
            return false;
        }

        #ifdef _SLAB_DEBUG
        _slab_log(
            LL_SLAB_DEBUG,
            "Child got %p for control segment",
            segment_address
        );
        #endif // _SLAB_DEBUG

        control = ( slab_control * ) segment_address;

        if( control->magic != _SLAB_CONTROL_MAGIC )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "Mapped control segment does not have correct magic %x,"
                " expected %x",
                control->magic,
                _SLAB_CONTROL_MAGIC
            );

            return false;
        }

        for( i = 0; i < _SLAB_MAX_SLABS; i++ )
        {
            if( unlikely( !check_slab_header( i ) ) )
            {
                _slab_log(
                    LL_SLAB_ERROR,
                    "Header %lu failed sanity checks",
                    ( uint64_t ) i
                );
                return false;
            }
            // XXX can setup pointer cache as we validate
        }
    }

    return true;
}

// Destroys a slab
void destroy_slab( context_t ctx )
{
    slab_header * header   = NULL;
    void *        allocs   = NULL;
    void *        allocset = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return;

    if( !__TNS_MUTEX( &(header->locked) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "destroy_slab: Failed to lock header"
        );
        return;
    }

    if( !__TNS_MUTEX( &(control->locked) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "destroy_slab: Failed to lock control header"
        );
        return;
    }

    allocs   = _allocs_ptr_cache( header );
    allocset = _as_ptr_cache( header );

    if( unlikely( (allocs == NULL) || (allocset == NULL) ) )
    {
        __C_MUTEX( &(control->locked) );
        __C_MUTEX( &(header->locked) );
        return;
    }

    // Will not wipe but decrement global ref count
    unmap_segment( allocs );
    unmap_segment( allocset );

    // Reset back to uninitialized state
    header->n_allocs         = 0;
    header->max_allocations  = 0;
    header->segment          = SEGMENT_HANDLE_INVALID;
    header->allocs           = get_null_ref();
    header->loc_c_allocstart = get_null_ref();
    header->loc_c_fsmstart   = get_null_ref();
    header->loc_c_fsmend     = get_null_ref();
    header->c_allocstart     = ( canary_t ) 0;
    header->c_fsmstart       = ( canary_t ) 0;
    header->c_fsmend         = ( canary_t ) 0;
    header->magic            = ( uint32_t ) 0;
    header->object_size      = 0;
    header->self             = INVALID_CONTEXT;
    header->count_hint       = 0;
    header->i_front_fsm_bit  = ( uint64_t ) 0;
    memset(
        header->object_id,
        0,
        _SLAB_MAX_IDENT
    );

    header->allocset.max_allocset   = 0;
    header->allocset.set            = get_null_ref();

    #ifdef _SLAB_EXTRA_SANE
    if( unlikely( !_sync_control() ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to sync control segment after destroying slab %lu",
            ( uint64_t ) ctx
        );
    }
    #endif // _SLAB_EXTRA_SANE

    __C_MUTEX( &(control->locked) );
    __C_MUTEX( &(header->locked) );
    __ptr_cache[ctx].as_base     = NULL;
    __ptr_cache[ctx].as_size     = 0;
    __ptr_cache[ctx].allocs_base = NULL;
    __ptr_cache[ctx].allocs_size = 0;
    __ptr_cache[ctx].fsm_base    = NULL;
    __ptr_cache[ctx].fsm_size    = 0;
    return;
}

// Initializes a new slab
context_t new_slab( const char * tag, size_t object_size )
{
    return _new_slab( tag, object_size, 0 );
}

context_t new_slab_with_hint(
    const char * tag,
    size_t       object_size,
    uint64_t     count_hint
)
{
    return _new_slab( tag, object_size, count_hint );
}

static INLINE context_t _new_slab(
    const char * tag,
    size_t       object_size,
    uint64_t     count_hint
)
{
    context_t ret                    = INVALID_CONTEXT;
    char      ident[_SLAB_MAX_IDENT] = {0};

    if( unlikely( !_slab_init ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Slab is not initialized - call slab_init() first"
        );
        return INVALID_CONTEXT;
    }

    snprintf(
        ident,
        _SLAB_MAX_IDENT,
        "%s",
        tag
    );

    // TODO Child -> parent alloc seems to be failing here
    ret = get_ctx_by_id( ident );

    if( ret == INVALID_CONTEXT )
    {
        // Allocate a new context
        // lock and inc next_header index and return as the new
        // context
        if( !__TNS_MUTEX( &(control->locked) ) )
            return INVALID_CONTEXT;

        if( control->next_header + 1 > _SLAB_MAX_SLABS )
        {
            __C_MUTEX( &(control->locked) );
            return INVALID_CONTEXT;
        }

        ret = ( context_t ) control->next_header;
        control->next_header += 1;

        __C_MUTEX( &(control->locked) );

        #ifdef _SLAB_DEBUG
        _slab_log( LL_SLAB_DEBUG, "INITIALIZING SLAB %lu\n", ( uint64_t ) ret );
        #endif // _SLAB_DEBUG
        // Setup our header to a semi-initialized state - we'll
        // handle setup of allocs[] and fsm[] later
        control->headers[ret].object_size      = object_size;
        control->headers[ret].n_allocs         = 0;
        control->headers[ret].self             = ret;
        control->headers[ret].locked           = false;
        control->headers[ret].max_allocations  = 0;
        control->headers[ret].i_front_fsm_bit  = 0;
        control->headers[ret].count_hint       = count_hint;

        strncpy(
            control->headers[ret].object_id,
            ident,
            _SLAB_MAX_IDENT
        );
    }
    else
    {
        // For child processes, verify that we have our headers setup and correct XXX
        // So i've got a hunch the mapping is copied in but is not shared
        slab_header * header = NULL;

        header = _get_header_by_context( ret );
        #ifdef _SLAB_DEBUG
        _slab_log( LL_SLAB_DEBUG, "Child entry into _new_slab() %s, got header %p (%u)", ident, header, header->magic );
        #endif // _SLAB_DEBUG
        // TODO: So far, the parent enters here and sees that the segment is allocated (by child) but the pointer information is incorrect / NULL
        if( __ptr_cache[ret].as_base != get_ptr( header->allocset.set ) )
        {
            __ptr_cache[ret].as_base = get_ptr( header->allocset.set );
            __ptr_cache[ret].as_size = header->allocset.max_allocset;
        }

        if( __ptr_cache[ret].allocs_base != get_ptr( header->allocs ) )
        {
            __ptr_cache[ret].allocs_base = get_ptr( header->allocs );
            __ptr_cache[ret].allocs_size = header->max_allocations * header->object_size;
        }

        if( __ptr_cache[ret].fsm_base != get_ptr( header->fsm ) )
        {
            __ptr_cache[ret].fsm_base = get_ptr( header->fsm );
            __ptr_cache[ret].fsm_size = ( sizeof( fsm_t ) * _get_fsm_length( header ) );
        }

        #ifdef _SLAB_DEBUG
        _slab_log(
            LL_SLAB_ERROR,
            "PTR_CACHE::\nAS_BASE:       %p\nAS_SIZE:      %zu\nALLOCS_BASE:   %p\nALLOCS_SIZE:    %zu\nFSM_BASE:      %p\nFSM_SIZE:      %zu",
            __ptr_cache[ret].as_base,
            __ptr_cache[ret].as_size,
            __ptr_cache[ret].allocs_base,
            __ptr_cache[ret].allocs_size,
            __ptr_cache[ret].fsm_base,
            __ptr_cache[ret].fsm_size
        );
        #endif // _SLAB_DEBUG
    }

    return ret;
}

// Main interface allocation functions

/*
 * ref_t rscalloc( context_t, size_t, uint64_t )
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
ref_t rscalloc( context_t ctx, size_t size, uint64_t count )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULLREF;

    if(
        unlikely(
            ( size != header->object_size )
         || ( size % header->object_size != 0 )
        )
      )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "rscalloc: size %zu is not a multiple of object_size %zu",
            size,
            header->object_size
        );
        return NULLREF;
    }

    return _shmalloc( header, size, true );
}

/*
 * ref_t rsmalloc( context_t, size_t )
 *
 * Shared memory equivilent of malloc()
 * Makes an allocation of size bytes (or size / header->object_size units)
 * and returns a ref_t to that memory location to the caller.
 *
 *  context_t ctx: Slab context the allocation is made in
 *  size_t size: Size in bytes to make for this allocation
 *
 */
ref_t rsmalloc( context_t ctx, size_t size )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULLREF;

    return _shmalloc( header, size, false );
}

/*
 * ref_t rsrealloc( context_t, ref_t, size_t )
 *
 * Shared memory equivilent of realloc()
 * Reallocates the passed in ref_t to the requested size, returning a ref_t to
 * that location in memory to the caller
 *
 * context_t ctx: Slab context the allocation is made in
 * ref_t oldref: Reference in which we would like to reallocate
 * size_t size: The new size we would like for oldref
 */
ref_t rsrealloc( context_t ctx, ref_t oldref, size_t size )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULLREF;

    if( size % header->object_size != 0 )
        return NULLREF;

    return _shrealloc( header, oldref, size / header->object_size );
}

/*
 * ref_t rscalloc_object_count( context_t, uint64_t )
 *
 * Shared memory equivilent of calloc(). Instead of taking a
 * size_t (bytes) argument, this takes the number of object we
 * want an allocation for.
 *
 *  context_t ctx: Slab context the allocation is made in
 *  uint64_t count: The number of objects requested.
 */
ref_t rscalloc_object_count( context_t ctx, uint64_t count )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULLREF;

    return _shmalloc( header, count * header->object_size, true );
}

/*
 * ref_t rsmalloc_object_count( context_t, uint64_t )
 *
 * Shared memory equivilent of malloc(). Instead of taking a
 * size_t (bytes) argument, this takes the number of objects we
 * want an allocation for.
 *
 * context_t ctx: Slab context the allocation is made in
 * uint64_t count: The number of objects requested.
 *
 */
ref_t rsmalloc_object_count( context_t ctx, uint64_t count )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULLREF;

    return _shmalloc( header, count * header->object_size, false );
}

/*
 * ref_t rsrealloc_object_count( context_t, ref_t, uint64_t )
 *
 * Shared memory equivilent of realloc(). Instead of taking a
 * size_t (bytes) argument, this takes the number of objects we
 * want the reallocated ref_t to contain.
 *
 * context_t ctx: Slab context the allocation is made in
 * ref_t oldref: Reference to the memory area we want to reallocate
 * uint64_t count: Number of objects the reallocated ref_t should hold
 *
 */
ref_t rsrealloc_object_count( context_t ctx, ref_t oldref, uint64_t count )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULLREF;

    return _shrealloc( header, oldref, count );
}

void * scalloc_object_count( context_t ctx, uint64_t count )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULL;

    return _to_ptr(
        header,
        _shmalloc( header, count * header->object_size, true )
    );
}

void * smalloc_object_count( context_t ctx, uint64_t count )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULL;

    return _to_ptr(
        header,
        _shmalloc( header, count * header->object_size, false )
    );
}

void * srealloc_object_count( context_t ctx, void * oldptr, uint64_t count )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULL;

    return _to_ptr(
        header,
        _shrealloc( header, _to_ref( header, oldptr ), count )
    );
}

void * scalloc( context_t ctx, size_t size, uint64_t count )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULL;

    return _to_ptr(
        header,
        _shmalloc( header, size, true )
    );
}

void * smalloc( context_t ctx, size_t size )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULL;

    return _to_ptr(
        header,
        _shmalloc( header, size, false )
    );
}

void * srealloc( context_t ctx, void * oldptr, size_t size )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return NULL;

    if( size % header->object_size != 0 )
        return NULL;

    return _to_ptr(
        header,
        _shrealloc( header, _to_ref( header, oldptr ), size / header->object_size )
    );
}

/*
 * __realloc_internal( slab_header *, uint64_t )
 *     slab_header *: Reference to the header for the segment in which we want
 *                    to resize
 *     uint64_t:      The number of objects desired for the new segment
 *
 * Note that the segment the header lives in and the segment the data
 * (allocs, fsm, canaries), since header->segment stores the segment the data
 * lives in, we use that to perform the resize. This is passed onto the
 * underlying shm functions
 */
static INLINE bool _realloc_internal(
    slab_header * header,
    uint64_t      new_size
)
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
    {
        _slab_log(
            LL_SLAB_ERROR,
            "__realloc_internal: Invalid header"
        );
        return false;
    }

    if( unlikely( !(header->locked) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_realloc_internal: Expected locked header as input"
        );
        return false;
    }

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
    //  We'll be doing the latter, for now. The collateral benefit is we make
    //  more room for singleton allocations but this is a reach.
    alloc_offset = get_segment_size( header->segment );
    size_needed  = ( size_needed * unit_size ) + alloc_offset;
    old_fsm      = get_ptr_fast( header->loc_c_fsmstart );

    if( unlikely( !shm_resize_segment( header->segment, size_needed ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "realloc_internal: Failed to resize segment"
        );
        return false;
    }

    if( unlikely( !check_slab_header( ( header_iter ) header->segment ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "realloc_internal: Failed to validate segment header post-resize"
        );
        return false;
    }

    // If we've got to this point we need to punt the FSM and associated
    // canaries to the new end of the segment, recalculate max_allocations and
    // return to the user. __refs are preserved as they still point to the
    // same memory segment, but the offset and segment # will be the same.
    // Still need to efficient-ize the call.

    // Unit size - stores sizeof( fsm_t ) * CHAR_BIT allocations
    old_fsm_length = _get_fsm_length( header );
    // Available space - accounting for headers and canaries
    available = ( get_segment_size( header->segment )
              - ( 3 * sizeof( canary_t ) ) );

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

    // allocs base stays static
    __ptr_cache[header->self].allocs_base = get_ptr( header->allocs );
    __ptr_cache[header->self].allocs_size = ( header->object_size * header->max_allocations );
    __ptr_cache[header->self].fsm_base    = fsm;
    __ptr_cache[header->self].fsm_size    = ( sizeof( fsm_t ) * _get_fsm_length( header ) );
    header->loc_c_fsmend   = get_ref( c_fsmend );
    header->loc_c_fsmstart = get_ref( c_fsmstart );
    header->fsm            = get_ref( fsm );

    *( ( canary_t * ) c_fsmend ) = header->c_fsmend;

    // Blank out the old FSM and canaries to avoid divulging allocation
    // information / canaries to the caller
    memset(
        old_fsm,
        0,
        old_fsm_length + 1
    );

    if( header->allocset.used >= header->allocset.max_allocset )
    {
        // Extend allocset
        size_needed = header->allocset.max_allocset
                    + ( ALLOCSET_DEFAULT_ALLOC_BLOCK * sizeof( allocset_item_t ) );

        if( !shm_resize_segment( ref_get_segment( header->allocset.set ), size_needed ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "realloc_internal: Failed to resize allocset segment"
            );
            return false;
        }

        header->allocset.max_allocset = get_segment_size(
                                   ref_get_segment( header->allocset.set )
                               ) / sizeof( allocset_item_t );
        __ptr_cache[header->self].as_base = get_ptr( header->allocset.set );
        __ptr_cache[header->self].as_size = header->allocset.max_allocset * sizeof( allocset_item_t );

        #ifdef _SLAB_DEBUG
        _slab_log(
            LL_SLAB_DEBUG,
            "Resized allocset segment to hold %lu individual allocations",
            ( uint64_t ) header->allocset.max_allocset
        );
        #endif // _SLAB_DEBUG
    }

    #ifdef _SLAB_FSM_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "FSM post-resize:"
    );
    _print_fsm( header );
    #endif // _SLAB_FSM_DEBUG
    #ifdef _SLAB_EXTRA_SANE
    return _check_canaries( header );
    #else
    return true;
    #endif // _SLAB_EXTRA_SANE
}

// Returns the allocation index and size of a given __ref,
// assuming this ref points to the start of the allocation
static INLINE bool _ref_get_index_and_size(
    slab_header * header,
    __ref         ref,
    uint64_t *    index,
    size_t *      size
)
{
    offset_t offset = 0;

    if(
        unlikely(
            ( index == NULL ) ||
            ( size == NULL ) ||
            ( header == NULL )
        )
      )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "ref_get_index_and_size: NULL parameters provided I %p S %p H %p",
            index,
            size,
            header
        );
        return false;
    }

    if( unlikely( ref_is_null( ref ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "ref_get_index_and_size: __ref is NULL"
        );
        return false;
    }

    offset = ref_get_offset( ref );

    #ifdef _SLAB_EXTRA_SANE
    if(
        unlikely(
        !_PTR_BOUND_CHECK(
            get_ptr( ref ),
            get_ptr( header->allocs ),
            header->max_allocations * header->object_size
        ))
      )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_ref_get_index_and_size:: Ref fails cursory bounds check and "
            "doesn't lie in the allocatable space of the shared memory segment"
        );
        return false;
    }
    #endif // _SLAB_EXTRA_SANE

    if( unlikely( offset % header->object_size != 0 ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_ref_get_index_and_size: __ref offset is not aligned to"
            " object_size in header"
        );
        return false;
    }

    *index = ( offset / header->object_size ) - 1;
    *size  = ( size_t ) _get_allocset_element_by_index(
        header,
        ( uint64_t ) *index
    );

    if( *size == ULONG_MAX )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_ref_get_index_and_size: Failed to find allocation size for "
            "allocset %lu",
            ( uint64_t ) *index
        );
        return false;
    }

    return true;
}

void rsfree( context_t ctx, ref_t ref )
{
    slab_header * header = NULL;

    header = get_header_by_context( ctx );

    if( header == NULL )
        return;

    return _shfree( header, ref, false );
}

void sfree( context_t ctx, void * ptr )
{
    slab_header * header = NULL;
    ref_t         ref = NULLREF;

    header = get_header_by_context( ctx );

    if( header == NULL )
        return;

    ref = _to_ref( header, ptr );

    return _shfree( header, ref, false );
}

static INLINE void _shfree( slab_header * header, ref_t pointer, bool nolock )
{
    uint64_t          index   = 0;
    uint64_t          size    = 0;
    allocset_item_t * item    = NULL;

    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_shfree: Context check failed."
        );
        return;
    }

    if( unlikely( pointer == NULLREF ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_shfree: Cannot free. NULL __ref given."
        );
        return;
    }

    if( unlikely( pointer >= header->allocset.max_allocset ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_shfree: pointer out of bounds"
        );
        return;
    }

    item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
        ( _as_ptr_cache( header ) ),
        ( sizeof( allocset_item_t ) * pointer )
    );

    if( unlikely( item == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_shfree: Failed to locate allocset item"
        );
        return;
    }

    index = item->index;
    size  = item->size;

    if( unlikely( !__free_by_index( header, index, size, nolock ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to free_by_index %lu, size %zu",
            ( uint64_t ) index,
            ( size_t ) size
        );
        return;
    }


    if( !_clear_allocset_item_by_index( header, index ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to clear allocset element %lu",
            ( uint64_t ) index
        );
        return;
    }

    header->allocset.used--;

    return;
}

static INLINE bool __free_by_index( slab_header * header, uint64_t index, size_t size, bool nolock )
{
    if( !_get_fsm_element_by_index( header, index ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "__free_by_index: Provided reference is not allocated (got index %lu)",
            ( uint64_t) index
        );
        return false;
    }

    if( unlikely( size == 0 ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "__free_by_index: Cannot free 0-sized element"
        );
        return false;
    }

    if( unlikely( !nolock ) )
    {
        if( unlikely( !__TNS_MUTEX( &(header->locked) ) ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "__free_by_index: Unable to obtain lock on header"
            );
            return false;
        }
    }

    if( size > 1 )
        _clear_fsm_elements_by_range( header, index, index + size );
    else
        _clear_fsm_element_by_index( header, index );

    header->n_allocs -= size;
    if( unlikely( !nolock ) )
        __C_MUTEX( &(header->locked) );

    #ifdef _SLAB_FSM_DEBUG
    _slab_log( LL_SLAB_DEBUG, "FSM after free:" );
    _print_fsm( header );
    #endif // _SLAB_FSM_DEBUG
    return true;
}

static INLINE ref_t _shrealloc(
    slab_header * header,
    ref_t         olduserref,
    uint64_t      count
)
{
    uint64_t          bit_position = 0;
    uint64_t          old_size     = 0;
    uint64_t          index        = 0;
    void *            new          = NULL;
    void *            old          = NULL;
    void *            base         = NULL;
    allocset_item_t * item         = NULL;
    allocset_item_t * temp         = NULL;

    if( unlikely( header == NULL ) )
        return NULLREF;

    #ifdef _SLAB_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "_shrealloc( %p, %lu ) entry",
        header,
        ( uint64_t ) count
    );
    #endif // _SLAB_DEBUG
    if( unlikely( !__TNS_MUTEX( &(header->locked) ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_shrealloc: Failed to obtain header lock."
        );
        return NULLREF;
    }

    if( unlikely( count > UINT_MAX ) )
    {
        // Note: need to typedef this and handle insanely large allocations,
        // esp. if we intend to support HUGETLB in shm.c
        _slab_log(
            LL_SLAB_ERROR,
            "_shrealloc: need resize of type for allocset[]!"
        );
        __C_MUTEX( &(header->locked) );
        return NULLREF;
    }


    if( unlikely( olduserref == NULLREF ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_shrealloc: Passed ref is NULL"
        );
        __C_MUTEX( &(header->locked) );
        return NULLREF;
    }

    if( unlikely( olduserref >= header->allocset.max_allocset ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_shrealloc: Passed ref is out of bounds"
        );
        __C_MUTEX( &(header->locked) );
        return NULLREF;
    }

    base = _as_ptr_cache( header );
    item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
        base,
        ( sizeof( allocset_item_t ) * ( uint64_t ) olduserref )
    );

    if( unlikely( item == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_shrealloc: Failed to get allocset item"
        );
        __C_MUTEX( &(header->locked) );
        return NULLREF;
    }

    index    = item->index;
    old_size = item->size;
    old      = get_ptr( item->issued_ref );

    #ifdef _SLAB_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "_shrealloc: resizing ref from %lu to %lu objects",
        ( uint64_t ) old_size,
        ( uint64_t ) count
    );
    #endif // _SLAB_DEBUG

    if( unlikely( count == old_size ) )
    {
        __C_MUTEX( &(header->locked) );
        return olduserref;
    }

    if( unlikely( count < old_size ) )
    {
        // Set allocset size to new size, shrink the FSM mask and return

        if( unlikely( !_clear_allocset_item_by_index( header, index ) ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_shrealloc: Failed to clear old allocset item for index %lu",
                ( uint64_t ) index
            );
            __C_MUTEX( &(header->locked) );
            return NULLREF;
        }

        item->size = count;
        _clear_fsm_elements_by_range( header, index, index + old_size );
        _set_fsm_elements_by_range( header, index, index + count );
        header->n_allocs = header->n_allocs - old_size + count;
    }
    else
    {
        // Here we do the whole thing that justifies the existence of ref_t types. We're going to
        // relocate the memory to a different area, with a new index (not _as_ind_t, but rather new FSM/allocs[] index)
        // and return the same ref_t to the user. This involves an Indiana Jones'-esque swap behind the scenes
        // where we need to:
        // - Update the information in our ref_t (_as_ind_t)'s position
        // - reorder the linked list - likely moving our node down a few spots (but not guaranteed).
        // - perform sanity checks.
        // This is the ~~only~~ place we need to do this type of work, so the logic that was previously in _set_allocset_item_by_index
        // will be placed here as some specialized work needs to be done
        errno = 0;
        bit_position = _get_fsm_slot_by_width( header, count, false );

        if( bit_position == ULONG_MAX || errno == ENOSPC )
        {
            if( unlikely( !_realloc_internal( header, count ) ) )
            {
                _slab_log(
                    LL_SLAB_ERROR,
                    "_shrealloc: Failed to extend segment"
                );
                errno = ENOSPC;
                __C_MUTEX( &(header->locked) );
                return NULLREF;
            }

            // Item /may/ have moved here! - get new ptr
            base = _as_ptr_cache( header );
            item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
                base,
                ( sizeof( allocset_item_t ) * olduserref )
            );

            bit_position = _get_fsm_slot_by_width( header, count, false );

            if( unlikely( bit_position == ULONG_MAX ) )
            {
                _slab_log(
                    LL_SLAB_ERROR,
                    "_shrealloc: Failed to find FSM opening after resize"
                );

                errno = ENOSPC;
                __C_MUTEX( &(header->locked) );
                return NULLREF;
            }
        }
        // We're at a position now where the underlying index into the alloc/fsm
        // has changed from index->bit_posititon. We'll need to fixup the allocset
        // linkedlist
        item->index      = bit_position;
        item->issued_ref = _get_alloc_element_by_index( header, bit_position );
        new              = get_ptr( item->issued_ref );
        item->size       = count;
        header->n_allocs += count; // _shfree will decrement the old allocation

        if( unlikely( new == NULL ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "Newly reallocated reference is NULL for index %lu",
                ( uint64_t ) bit_position
            );
            return NULLREF;
        }

        if( item->last != ALLOCSET_ITEM_INVALID )
        {
            temp = ( allocset_item_t * ) _PTR_ADD_OFFSET(
                base,
                ( sizeof( allocset_item_t ) * ( uint64_t ) item->last )
            );

            temp->next = item->next;
        }
        else
        {
            // We're removing the head
            if( item->next != ALLOCSET_ITEM_INVALID )
            {
                header->allocset.head = item->next;
            }
            else
            {
                header->allocset.head = ALLOCSET_ITEM_INVALID;
                header->allocset.tail = ALLOCSET_ITEM_INVALID;
            }
        }

        if( item->next != ALLOCSET_ITEM_INVALID )
        {
            temp = ( allocset_item_t * ) _PTR_ADD_OFFSET(
                base,
                ( sizeof( allocset_item_t ) * ( uint64_t ) item->next )
            );

            temp->last = item->last;
        }
        else
        {
            // We're removing the tail
            if( item->last != ALLOCSET_ITEM_INVALID )
            {
                header->allocset.tail = item->last;
            }
            else
            {
                header->allocset.head = ALLOCSET_ITEM_INVALID;
                header->allocset.tail = ALLOCSET_ITEM_INVALID;
            }
        }

        // We've detached item from the linked list, now we need to find its new home
        if(
               ( header->allocset.head == ALLOCSET_ITEM_INVALID )
            && ( header->allocset.tail == ALLOCSET_ITEM_INVALID )
          )
        {
            header->allocset.head = olduserref;
            header->allocset.tail = olduserref;
        }
        else
        {
            if(
                !_set_allocset_item_by_index(
                    header,
                    bit_position,
                    item->issued_ref,
                    count,
                    olduserref
                )
              )
            {
                _slab_log(
                    LL_SLAB_ERROR,
                    "_shrealloc: Failed to reorder linked list"
                );
                __C_MUTEX( &(header->locked) );
                return NULLREF;
            }
        }

        memcpy(
            new,
            old,
            old_size * header->object_size
        );

        if( unlikely( !__free_by_index( header, index, old_size, true ) ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_shrealloc: Free failed"
            );
            return NULLREF;
        }

        __FENCE();
    }

    if( !_sync_slab( header, true ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to sync slab %p",
            header
        );
    }

    __C_MUTEX( &(header->locked) );
    return olduserref;
}

static INLINE ref_t _shmalloc(
    slab_header * header,
    size_t        size,
    bool          zero_fill
)
{
    __ref    retref      = {0};
    ref_t    userref     = NULLREF;
    uint64_t num_objects = 0;
    uint64_t index       = 0;
    void *   ptr         = NULL;

    if( header == NULL )
        return NULLREF;

    num_objects = size / header->object_size;

    #ifdef _SLAB_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "Handling _shmalloc( %p, %zu, %s )",
        header,
        size,
        zero_fill ? "T" : "F"
    );
    #endif // _SLAB_DEBUG

    if( size % header->object_size != 0 )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "requested allocation size %lu is not a multiple of slab's object"
            " size %lu",
            ( uint64_t ) size,
            ( uint64_t ) header->object_size
        );
        return NULLREF;
    }

    // Check to see if this context has ever been allocated.
    if(
        unlikely(
              header->segment == SEGMENT_HANDLE_INVALID
           && header->max_allocations == 0
        )
      )
    {
        #ifdef _SLAB_DEBUG
        _slab_log(
            LL_SLAB_DEBUG,
            "Initializing slab"
        );
        #endif // _SLAB_DEBUG
        // Need to generate a context
        if( !_init_slab( header, false ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "Attempt to allocate to uninitialized segment"
            );
            return NULLREF;
        }
    }

    if( unlikely( !__TNS_MUTEX( &(header->locked) ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to acquire segment lock"
        );
        return NULLREF;
    }

    // Layout & usage of free list:
    // [ front - single allocs ..... contiguous allocs - rear ]
    if( num_objects >= ( header->max_allocations - header->n_allocs ) )
    {
        if( unlikely( !_realloc_internal( header, num_objects ) ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "Failed to reallocate segment"
            );
            return NULLREF;
        }
    }

    if( num_objects > 1 )
    {
        errno = 0;
        index = _get_fsm_slot_by_width( header, num_objects, true );

        if( unlikely( errno == ENOSPC ) ) // Need to reallocate
        {
            errno = 0;

            if( unlikely( !_realloc_internal( header, num_objects ) ) )
                return NULLREF;

            index = _get_fsm_slot_by_width( header, num_objects, true );

            if( errno == ENOSPC )
            {
                _slab_log(
                    LL_SLAB_ERROR,
                    "Failed to allocate to segment after extension"
                );
                return NULLREF;
            }
        }

        retref  = _get_alloc_element_by_index( header, index );
        userref = _get_allocset_item_by_index( header, index );

        #ifdef _SLAB_DEBUG
        _slab_log(
            LL_SLAB_DEBUG,
            "Returning ref to alloc[%lu] of %lu ( len %lu )",
            ( uint64_t ) index,
            ( uint64_t ) header->max_allocations,
            ( uint64_t ) header->max_allocations - index
        );
        #endif // _SLAB_DEBUG
        header->n_allocs += num_objects;
    }
    else
    {
        index = header->i_front_fsm_bit;

        if( likely( !_get_fsm_element_by_index( header, index ) ) )
        { // Indexed search
            _set_fsm_element_by_index( header, index );
            retref = _get_alloc_element_by_index( header, index );
            header->i_front_fsm_bit = index + 1;
            header->n_allocs += 1;
        }
        else
        { // Exhaustive search
            #ifdef _SLAB_DEBUG
            _slab_log(
                LL_SLAB_DEBUG,
                "_shmalloc: Performing exhaustive allocation search"
            );
            #endif // _SLAB_DEBUG
            for(
                    index = 0;
                    index < ( FSM_WIDTH * _get_fsm_length( header ) );
                    index++
               )
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
        if( !_set_allocset_item_by_index( header, index, retref, 1, ALLOCSET_ITEM_INVALID ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "Failed to set allocset element %lu",
                ( uint64_t ) index
            );
            return get_null_ref();
        }

        userref = _get_allocset_item_by_index( header, index );
    }

    if( zero_fill == true )
    {
        ptr = get_ptr_fast( retref );
        if( ptr == NULL )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_shmalloc: Dereference of recently created __ref is NULL"
            );
            return get_null_ref();
        }

        memset(
            ptr,
            ( unsigned char ) _ZERO_FILL_BYTE,
            ( header->object_size * num_objects )
        );
    }

    if( !_sync_slab( header, true ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to sync slab %p",
            header
        );
    }

    if( !_sync_control() )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Failed to sync control header"
        );
    }

    __C_MUTEX( &(header->locked) );

    return userref;
}

static INLINE __ref _get_alloc_element_by_index(
    slab_header * header,
    uint64_t      ind
)
{
    if( unlikely( header == NULL ) )
        return get_null_ref();

    if( unlikely( ind >= header->max_allocations ) )
        return get_null_ref();

    return get_ref(
        _PTR_ADD_OFFSET(
            ( _allocs_ptr_cache( header ) ),
            ( ( header->object_size ) * ind )
        )
    );
}

// This function adequately (and quickly) handles setting FSM entries en masse
// but could use a cleanup either by simplification of iterand boundaries or
// consolidation of check logic. There are a few fencepost issues that this
// solves due to the design decision in the layout of the FSM. The important
// note here is that start is the inclusive index for setting end is the
// exclusive index for setting. IE if fsm_size == 512 and you wanted to set
// the whole block, this would get called with (0,512) when you obviously need
// to set bits 0..511
static INLINE void _set_fsm_elements_by_range(
    slab_header * header,
    uint64_t         start,
    uint64_t         end
)
{
    fsm_t *  fsm_word     = NULL;
    uint32_t fsm_start    = 0;
    uint32_t fsm_end      = 0;
    uint8_t  start_offset = 0;
    uint8_t  end_offset   = 0;
    uint32_t fsm_i        = 0;
    void *   fsm          = NULL;

    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_set_fsm_elements_by_range: NULL header."
        );
        return;
    }

    if( unlikely( start > end ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_set_fsm_elements_by_range: start index exceeds end index."
        );
        return;
    }

    if( unlikely( end > _get_fsm_length( header ) * FSM_WIDTH ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_set_fsm_elements_by_index: end index is out of bounds"
        );
        return;
    }

    // We set bits from start to end-1
    // This function has a fenceposting issue
    fsm = _fsm_ptr_cache( header );
    fsm_start    = ( start / FSM_WIDTH );
    start_offset = start - ( ( start / FSM_WIDTH ) * FSM_WIDTH );
    fsm_end      = ( ( end - 1 ) / FSM_WIDTH );
    end_offset   = ( end - 1 ) - ( ( ( end - 1 ) / FSM_WIDTH ) * FSM_WIDTH );

    #ifdef _SLAB_FSM_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "_set_fsm_elements_by_range:\n"
        "  start: %lu\n"
        "  fsm_start: %lu\n"
        "  start_offset: %lu\n"
        "  end: %lu\n"
        "  fsm_end: %lu\n"
        "  end_offset: %lu\n",
        ( uint64_t ) start,
        ( uint64_t ) fsm_start,
        ( uint64_t ) start_offset,
        ( uint64_t ) end,
        ( uint64_t ) fsm_end,
        ( uint64_t ) end_offset
    );
    #endif // _SLAB_FSM_DEBUG

    // shiftless, bulk setting optimization
    if( fsm_start != fsm_end )
    {
        // This is the case where the request is fully aligned (on both sides)
        // to the fsm word
        if( end_offset == FSM_WIDTH - 1 && start_offset == 0 )
        {
            for( fsm_i = fsm_start; fsm_i <= fsm_end; fsm_i++ )
            {
                fsm_word = ( fsm_t * ) _PTR_ADD_OFFSET(
                    fsm,
                    ( fsm_i * sizeof( fsm_t ) )
                );
                *fsm_word = ( fsm_t ) ULONG_MAX;
            }

            return;
        }

        // This is the case where either the start or end are not fully aligned
        // Bulk set the intermediate words (whole words between start and end
        // offsets)
        if( start_offset == 0 || fsm_start < fsm_end - 1 )
        {
            for( fsm_i = fsm_start + 1; fsm_i < fsm_end; fsm_i++ )
            {
                #ifdef _SLAB_FSM_DEBUG
                _slab_log(
                    LL_SLAB_DEBUG,
                    "_set_fsm_elements_by_range: Setting fsm word %lu en-masse",
                    ( uint64_t ) fsm_i
                );
                #endif // _SLAB_FSM_DEBUG
                fsm_word = ( fsm_t * ) _PTR_ADD_OFFSET(
                    fsm,
                    ( fsm_i * sizeof( fsm_t ) )
                );
                *fsm_word = ( fsm_t ) ULONG_MAX;
            }
        }
        // Set the words at fsm_start and fsm_end
        fsm_word = ( fsm_t * ) _PTR_ADD_OFFSET(
            fsm,
            ( fsm_start * sizeof( fsm_t ) )
        );

        #ifdef _SLAB_FSM_DEBUG
        _slab_log( LL_SLAB_DEBUG, "start:" );
        print_bin( *fsm_word );
        #endif // _SLAB_FSM_DEBUG
        *fsm_word |= ( ( fsm_t ) ULONG_MAX ) << start_offset;
        #ifdef _SLAB_FSM_DEBUG
        print_bin( *fsm_word );
        #endif // _SLAB_FSM_DEBUG

        fsm_word = ( fsm_t * ) _PTR_ADD_OFFSET(
            fsm,
            ( fsm_end * sizeof( fsm_t ) )
        );

        if( end_offset == FSM_WIDTH - 1 )
        {
            *fsm_word = ( fsm_t ) ULONG_MAX;
        }
        else
        {
            *fsm_word |= ~( ( ( fsm_t ) ULONG_MAX ) << ( end_offset + 1 ) );
        }
        #ifdef _SLAB_FSM_DEBUG
        _slab_log( LL_SLAB_DEBUG, "start:" );
        print_bin( *fsm_word );
        #endif // _SLAB_FSM_DEBUG
        #ifdef _SLAB_FSM_DEBUG
        print_bin( *fsm_word );
        #endif // _SLAB_FSM_DEBUG
    }
    else
    {
        // Handle case where FSM word is the same word and we're just setting
        // a range within that word
        fsm_word = ( fsm_t * ) _PTR_ADD_OFFSET(
            fsm,
            ( fsm_start * sizeof( fsm_t ) )
        );

        // Special case - whole word fully aligned.
        if( end_offset == FSM_WIDTH - 1 && start_offset == 0 )
        {
            // For implementation defined reasons,
            // ( ULONG_MAX ) << 64 == ULONG_MAX, so we handle that here. in
            // amd64 world this is because the  "shl rax, cl" is often masked
            // with cl & 0x3F, meaning rax may not be the expected 0 afterwards
            // ( Thanks, Dave! )
            *fsm_word = ( fsm_t ) ULONG_MAX;
        }
        else
        {
            *fsm_word |= ~(
                            ( ( fsm_t ) ULONG_MAX )
                         << ( end_offset - start_offset + 1 )
                          ) << start_offset;
        }
    }

    return;
}

static INLINE void _set_fsm_element_by_index(
    slab_header * header,
    uint64_t      ind
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
        ( _fsm_ptr_cache( header ) ),
        fsm_index * sizeof( fsm_t )
    );

    if( fsm_word == NULL )
        return;

    *fsm_word |= ( fsm_t ) 1 << fsm_offset;

    return;
}

static INLINE void _clear_fsm_elements_by_range(
    slab_header * header,
    uint64_t         start,
    uint64_t         end
)
{
    fsm_t *  fsm_word     = NULL;
    uint32_t fsm_start    = 0;
    uint32_t fsm_end      = 0;
    uint8_t  start_offset = 0;
    uint8_t  end_offset   = 0;
    uint32_t fsm_i        = 0;
    void *   fsm          = NULL;

    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "__clear_elements_by_range: NULL header."
        );
        return;
    }

    if( unlikely( start > end ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_clear_fsm_elements_by_range: start index exceeds end index."
        );
        return;
    }

    if( unlikely( end > _get_fsm_length( header ) * FSM_WIDTH ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_clear_fsm_elements_by_index: end index is out of bounds"
        );
        return;
    }

    // We set bits from start to end-1
    // This function has a fenceposting issue
    fsm          = _fsm_ptr_cache( header );
    fsm_start    = ( start / FSM_WIDTH );
    start_offset = start - ( ( start / FSM_WIDTH ) * FSM_WIDTH );
    fsm_end      = ( ( end - 1 ) / FSM_WIDTH );
    end_offset   = ( end - 1 ) - ( ( ( end - 1 ) / FSM_WIDTH ) * FSM_WIDTH );

    #ifdef _SLAB_FSM_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "_clear_fsm_elements_by_range:\n"
        "  start: %lu\n"
        "  fsm_start: %lu\n"
        "  start_offset: %lu\n"
        "  end: %lu\n"
        "  fsm_end: %lu\n"
        "  end_offset: %lu\n",
        ( uint64_t ) start,
        ( uint64_t ) fsm_start,
        ( uint64_t ) start_offset,
        ( uint64_t ) end,
        ( uint64_t ) fsm_end,
        ( uint64_t ) end_offset
    );
    #endif // _SLAB_FSM_DEBUG

    // shiftless, bulk setting optimization
    if( fsm_start != fsm_end )
    {
        // This is the case where the request is fully aligned (on both sides)
        // to the fsm word
        if( end_offset == FSM_WIDTH - 1 && start_offset == 0 )
        {
            for( fsm_i = fsm_start; fsm_i <= fsm_end; fsm_i++ )
            {
                fsm_word = ( fsm_t * ) _PTR_ADD_OFFSET(
                    fsm,
                    ( fsm_i * sizeof( fsm_t ) )
                );
                *fsm_word = ( fsm_t ) 0;
            }

            return;
        }

        // This is the case where either the start or end are not fully aligned
        // Bulk set the intermediate words (whole words between start and end
        // offsets)
        if( start_offset == 0 || fsm_start < fsm_end - 1 )
        {
            for( fsm_i = fsm_start + 1; fsm_i < fsm_end; fsm_i++ )
            {
                fsm_word = ( fsm_t * ) _PTR_ADD_OFFSET(
                    fsm,
                    ( fsm_i * sizeof( fsm_t ) )
                );
                *fsm_word = ( fsm_t ) 0;
            }
        }
        // Set the words at fsm_start and fsm_end
        fsm_word = ( fsm_t * ) _PTR_ADD_OFFSET(
            fsm,
            ( fsm_start * sizeof( fsm_t ) )
        );

        *fsm_word &= ~( ( fsm_t ) ULONG_MAX << start_offset );

        fsm_word = ( fsm_t * ) _PTR_ADD_OFFSET(
            fsm,
            ( fsm_end * sizeof( fsm_t ) )
        );

        if( end_offset == FSM_WIDTH - 1 )
        {
            *fsm_word = ( fsm_t ) 0;
        }
        else
        {
            *fsm_word &= ( ( ( fsm_t ) ULONG_MAX ) << ( end_offset + 1 ) );
        }
    }
    else
    {
        // Handle case where FSM word is the same word and we're just setting
        // a range within that word
        fsm_word = ( fsm_t * ) _PTR_ADD_OFFSET(
            fsm,
            ( fsm_start * sizeof( fsm_t ) )
        );

        // Special case - whole word fully aligned.
        if( end_offset == FSM_WIDTH - 1 && start_offset == 0 )
        {
            *fsm_word = ( fsm_t ) 0;
        }
        else
        {
            *fsm_word &= ~(
                            ~(
                                ( ( fsm_t ) ULONG_MAX )
                             << ( end_offset - start_offset + 1 )
                             ) << start_offset
                          );
        }
    }

    return;
}

static INLINE void _clear_fsm_element_by_index(
    slab_header * header,
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
        ( _fsm_ptr_cache( header ) ),
        fsm_index * sizeof( fsm_t )
    );

    if( fsm_word == NULL )
        return;

    *fsm_word &= ~( ( fsm_t ) 1 << fsm_offset );

    return;
}

static INLINE bool _get_fsm_element_by_index(
    slab_header * header,
    uint64_t      ind
)
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
        ( _fsm_ptr_cache( header ) ),
        fsm_index * sizeof( fsm_t )
    );

    if( ( ( *fsm_word >> fsm_offset ) & ( fsm_t ) 1 ) > 0 )
        return true;

    return false;
}

static INLINE uint64_t _get_fsm_slot_by_width(
    slab_header * header,
    uint64_t      width,
    bool          set_allocset
)
{
    uint64_t bit_position = 0;

    bit_position = __find_fsm_spot( header, width );

    if( unlikely( bit_position == ULONG_MAX ) )
    {
        errno = ENOSPC;
        return 0;
    }

    #ifdef _SLAB_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "ISSUING ALLOCATION FOR INDEX %lu",
        ( uint64_t ) bit_position
    );
    #endif // _SLAB_DEBUG

    _set_fsm_elements_by_range( header, bit_position, bit_position + width );
    // We need to track how large the allocation is for
    // purposes of freeing later
    if( set_allocset )
    {
        if(
            !_set_allocset_item_by_index(
                header,
                bit_position,
                _get_alloc_element_by_index( header, bit_position ),
                width,
                ALLOCSET_ITEM_INVALID
            )
          )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_get_fsm_slot_by_width: failed to set allocset"
            );
            errno = EINVAL;
            return 0;
        }
    }
    #ifdef _SLAB_FSM_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "_get_fsm_slot_by_width( %p, %lu ) POST RUN FSM SNAPSHOT",
        header,
        width
    );
    _print_fsm( header );
    #endif // _SLAB_FSM_DEBUG

    return bit_position;
}

/*
 * __find_fsm_spot( slab_header *, uint64_t )
 *   - slab_header * header - the headers whose FSM we are searching
 *   - uint64_t requested_length - the width of the allocation needed in
 *                                 <objects>, not bytes
 *
 * Performs a fast masked search of a bitfield searching for a free area
 * In order to handle referential integrity iff the page gets resized, the
 * search begins at the 'rear' (nth index) of the FSM, and moves towards the
 * 0th index for large allocations. Small (single) allocations are done by a
 * separate subroutine, which searches from the 'front' (0th index) of the FSM.
 *
 * This algorithm makes the tradeoff between search speed and packing
 * efficiency.
 * FSM_SHIFT_WIDTH controls this, and is expected to be 8, 16, 32, or 64
 * Another caveat is the end of an allocation is anchored to a multiple of the
 * FSM_SHIFT_WIDTH, meaning that in the following example, it would not be able
 * to find an optimal solution:
 *
 * Given: FSM_SHIFT_WIDTH = 16
 * __find_fsm_spot( header, 67 )
 * FSM\word:   0                1               2                3
 * [1]: 1111111000000000 0000000000000000 0000000000000000 0000000000000000
 * [0]: 0000000000000000 0000000000000000 0000000000000000 0000000000000111
 *
 * would result in an allocation being placed at
 * [1]: 1111111000000000 1111111111111111 1111111111111111 1111111111111111
 * [0]: 1111111111111111 1111111000000000 0000000000000000 0000000000000111
 *
 * For the same case setup, but with __find_fsm_spot( header, 97 ), we would not
 * be able to find a solution with SHIFT_WIDTH = 16, but could with
 * SHIFT_WIDTH = 8.
 *
 * My rationalization is that this leaves room for single allocations after bulk
 * allocations have been made, though this can lead to terrible fragmentation
 */
static INLINE uint64_t __find_fsm_spot(
    slab_header * header,
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
    uint8_t            pos_offset       = 0;

    fsm_length = _get_fsm_length( header );
    mask_last  = ~( mask_last << ( requested_length % FSM_SHIFT_WIDTH ) );

    // Note that the position / iter expressed in these statements is inverted
    // (directionally) prior to return to caller, instead of the 0th element
    // being the LSB of the 0th word, it's the MSB of the nth word.
    #ifdef _SLAB_FSM_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "__find_fsm_spot( %p, %lu ) startup",
        header,
        requested_length
    );
    #endif // _SLAB_FSM_DEBUG

    if( requested_length <= FSM_SHIFT_WIDTH )
    {
        last_word      = true;
        compare_active = true;
        mask           = mask_last;
        pos_offset     = FSM_SHIFT_WIDTH - requested_length;
        // Pos offset prevents overlap of allocations
    }

    for( fsm_i = 0; fsm_i < fsm_length; fsm_i++ )
    {
        // Iterate over words of sizeof( fsm_t ) bytes
        fsm_word = *( ( fsm_t * ) _PTR_ADD_OFFSET(
            ( _fsm_ptr_cache( header ) ),
            ( ( fsm_length - 1 - fsm_i ) * sizeof( fsm_t ) )
        )); // Deref in outer loop

        if( ( fsm_word & skip_mask ) == skip_mask && !last_word )
        {
            // Mask out the fsm word, if it's filled we can jump ahead by the
            // full width
            iter          += FSM_WIDTH;
            position       = iter;
            // Reset counter in case we were mid compare
            bits_comp      = requested_length;
            compare_active = false;
            #ifdef _SLAB_FSM_DEBUG
            _slab_log(
                LL_SLAB_DEBUG,
                "Fast skipped to iter %lu",
                ( uint64_t ) iter
            );
            #endif // _SLAB_FSM_DEBUG
            continue;
        }

        //for( fsm_word_i = 0; fsm_word_i < FSM_RATIO; fsm_word_i++ )
        for(
              fsm_word_i = FSM_RATIO - 1;
              fsm_word_i != ( uint8_t ) UCHAR_MAX;
              --fsm_word_i
           )
        {
            // Iterate over words within the given fsm_t word, size
            // FSM_SHIFT_WIDTH bits
            temp = ( fsm_cmp_t ) (
                fsm_word >> ( ( fsm_word_i ) * FSM_SHIFT_WIDTH )
            );

            #ifdef _SLAB_FSM_DEBUG
            _slab_log(
                LL_SLAB_DEBUG,
                " fsm_i: %lu,"
                " fsm_word_i: %lu,"
                " position %lu,"
                " iter %lu,"
                " bits_comp: %lu,"
                " last_word %s,"
                " compare_active %s,"
                " pos_offset: %lu",
                ( uint64_t ) fsm_i,
                ( uint64_t ) fsm_word_i,
                ( uint64_t ) position,
                ( uint64_t ) iter,
                ( uint64_t ) bits_comp,
                last_word ? "T" : "F",
                compare_active ? "T" : "F",
                ( uint64_t ) pos_offset
            );
            _slab_log( LL_SLAB_DEBUG, "Current FSM Word:" );
            print_bin( ( uint64_t ) fsm_word );
            _slab_log( LL_SLAB_DEBUG, "Temp:" );
            print_bin( ( uint64_t ) temp );
            _slab_log( LL_SLAB_DEBUG, "Mask:" );
            print_bin( ( uint64_t ) mask );
            #endif // _SLAB_FSM_DEBUG

            if( ( ~(temp) & mask ) == mask )
            {
                if( last_word )
                {
                    // Prep for return & attempt to compactify past word
                    // boundaries
                    #ifdef _SLAB_FSM_DEBUG
                    _slab_log(
                        LL_SLAB_DEBUG,
                        "Early exit triggered for position %lu",
                        ( uint64_t ) position
                    );
                    #endif // _SLAB_FSM_DEBUG
                    if( ( last_word_val & FSM_LAST_WORD_MASK ) > 0 )
                        return (
                            header->max_allocations
                          - ( position + requested_length ) - pos_offset
                        );

                    if( requested_length > FSM_SHIFT_WIDTH )
                        temp = last_word_val;

                    mask = ( fsm_cmp_t ) FSM_LAST_WORD_MASK;

                    while( ( ~temp & mask ) != 0 )
                    {
                        if( temp == 0 )
                            break;
                        temp = temp << 1;
                        position++;
                    }

                    #ifdef _SLAB_FSM_DEBUG
                    _slab_log(
                        LL_SLAB_DEBUG,
                        "Returning compactified position %lu (%lu)",
                        position,
                        header->max_allocations - ( position + requested_length ) - pos_offset
                    );
                    #endif // _SLAB_FSM_DEBUG
                    return (
                        header->max_allocations
                      - ( position + requested_length )
                      - pos_offset
                    );
                }

                last_word  = ( bits_comp <= FSM_SHIFT_WIDTH );
                bits_comp -= FSM_SHIFT_WIDTH;

                if( !compare_active )
                    compare_active = true;

                if( last_word )
                    mask = mask_last;
            }
            else
            {   // No match
                #ifdef _SLAB_FSM_DEBUG
                _slab_log( LL_SLAB_DEBUG, "No match - state reset." );
                #endif // _SLAB_FSM_DEBUG
                //if( compare_active )
                //{ // reset counters and markers
                    bits_comp = requested_length;
                    position  = iter;
                //}

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

    #ifdef _SLAB_FSM_DEBUG
    _slab_log( LL_SLAB_DEBUG, "FSM Search exhausted" );
    #endif // _SLAB_FSM_DEBUG
    return ULONG_MAX;
}

// Gets the number of fsm_t's we'll need to store the bitmap of allocations
static INLINE uint64_t _get_fsm_length( slab_header * header )
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
    slab_header * header = NULL;

    if( !_check_context( ctx ) )
        return;

    header = &(control->headers[ctx]);
    header->count_hint = count_hint;
    return;
}

static INLINE bool _init_slab( slab_header * header, bool zero_fill )
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

    if( unlikely( header == NULL || header->self == INVALID_CONTEXT ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_init_slab: header is uninitialized, call slab_init() first"
        );
        return false;
    }

    // Don't want to trash an initialized segment and be idempotent
    if( header->segment != SEGMENT_HANDLE_INVALID )
    {
        // Seems to be already allocated
        if( header->n_allocs > 0 || header->max_allocations > 0 )
            return true;
    }

    // Unit size - stores sizeof( fsm_t ) * CHAR_BIT allocations
    unit_size = ( sizeof( fsm_t ) * CHAR_BIT * header->object_size )
              + sizeof( fsm_t );

    count_hint = header->count_hint;

    #ifdef _SLAB_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "_init_slab: Header count hint is %lu",
        ( uint64_t ) count_hint
    );
    #endif // _SLAB_DEBUG

    if( count_hint == 0 )
        count_hint = SLAB_DEFAULT_ALLOCATION;

    initial_size = ( count_hint / ( sizeof( fsm_t ) * CHAR_BIT ) );

    if( initial_size == 0 )
        initial_size = 1;

    initial_size   = initial_size * unit_size + sizeof( canary_t ) * 3;
    mapped_address = new_segment( initial_size );

    if( mapped_address == NULL )
        return false;

    handle = get_handle_from_ptr( mapped_address );
    header->segment = handle;

    // Available space - accounting for headers and canaries
    available = ( get_segment_size( handle ) - ( 3 * sizeof( canary_t ) ) );
    header->max_allocations = ( available / unit_size )
                            * CHAR_BIT * sizeof( fsm_t );

    allocset = new_segment( ALLOCSET_DEFAULT_ALLOC_BLOCK * sizeof( allocset_item_t ) );
    header->allocset.set          = get_ref( allocset );
    header->allocset.max_allocset = get_segment_size( ref_get_segment( header->allocset.set ) )
                                  / sizeof( allocset_item_t );
    header->allocset.used = ( _as_ind_t ) 0;
    header->allocset.head = ( _as_ind_t ) ALLOCSET_ITEM_INVALID;
    header->allocset.tail = ( _as_ind_t ) ALLOCSET_ITEM_INVALID;
    header->n_allocs      = 0;
    header->c_allocstart  = ( canary_t ) _get_random();
    header->c_fsmstart    = ( canary_t ) _get_random();
    header->c_fsmend      = ( canary_t ) _get_random();

    #ifdef _SLAB_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "Initialized allocset at segment %lu, max_allocset %lu",
        ( uint64_t ) ref_get_segment( header->allocset.set ),
        ( uint64_t ) header->allocset.max_allocset
    );
    #endif // _SLAB_DEBUG
    // Layout setup - we'll calculate locally for readability,
    // then convert to __ref
    c_allocstart = mapped_address;
    alloc = ( void * ) _PTR_ADD_OFFSET(
        c_allocstart,
        sizeof( canary_t )
    );
    c_fsmstart = ( void * ) _PTR_ADD_OFFSET(
        alloc,
        ( header->object_size * header->max_allocations )
    );
    fsm = ( void * ) _PTR_ADD_OFFSET(
        c_fsmstart,
        sizeof( canary_t )
    );
    c_fsmend = ( void * ) _PTR_ADD_OFFSET(
        fsm,
        ( sizeof( fsm_t ) * _get_fsm_length( header ) )
    );

    __ptr_cache[header->self].as_base     = allocset;
    __ptr_cache[header->self].as_size     = ( ALLOCSET_DEFAULT_ALLOC_BLOCK * sizeof( allocset_item_t ) );
    __ptr_cache[header->self].allocs_base = alloc;
    __ptr_cache[header->self].allocs_size = ( header->object_size * header->max_allocations );
    __ptr_cache[header->self].fsm_base    = fsm;
    __ptr_cache[header->self].fsm_size    = ( sizeof( fsm_t ) * _get_fsm_length( header ) );

    #ifdef _SLAB_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "Layout:\n"
        "  Allocstart canary: %p\n"
        "  Alloc:             %p\n"
        "  FSMstart Canary:   %p\n"
        "  FSM:               %p\n"
        "  FSMend Canary      %p",
        c_allocstart,
        alloc,
        c_fsmstart,
        fsm,
        c_fsmend
    );
    #endif // _SLAB_DEBUG

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
            _allocs_ptr_cache( header ),
            ( unsigned char ) _ZERO_FILL_BYTE,
            ( header->object_size * header->max_allocations )
        );
    }

    return _check_canaries( header );
}

bool force_canary_check( context_t ctx )
{
    slab_header * header = NULL;

    if( !_check_context( ctx ) )
        return false;

    header = &(control->headers[ctx]);

    return _check_canaries( header );
}

slab_header * get_header_by_context( context_t ctx )
{
    return _get_header_by_context( ctx );
}

static INLINE slab_header * _get_header_by_context( context_t ctx )
{
    if( unlikely( !_check_context( ctx ) ) )
        return NULL;

    return &(control->headers[ctx]);
}

void dump_headers( void )
{
    uint64_t i = 0;
    slab_header * header = NULL;

    if( control == NULL )
        return;

    fprintf( stdout, "CONTROL HEADER: %p, segment %u\n", control, ( uint32_t ) control_segment );
    for( i = 0; i < _SLAB_MAX_SLABS; i++ )
    {
        header = &(control->headers[i]);
        fprintf( stdout, "  HEADER %p, (%u)\n", header, (uint32_t) i );
        fprintf(
            stdout,
            "    allocs: %s\n    fsm: %s\n    allocset.set: %s\n    CONTEXT: %u\n    obj_size: %zu\n    object_id: %s\n",
            ref_is_null( header->allocs ) ? "NULL" : "set",
            ref_is_null( header->fsm ) ? "NULL" : "set",
            ref_is_null( header->allocset.set ) ? "NULL" : "set",
            ( uint32_t ) header->self,
            header->object_size,
            header->object_id == NULL ? "NULL" : header->object_id
        );
    }

    return;
}
static INLINE bool _clear_allocset_item_by_index( slab_header * header, uint64_t index )
{
    _as_ind_t         ind  = 0;
    allocset_item_t * item = NULL;
    allocset_item_t * temp = NULL;
    register void *   base = NULL;

    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_clear_allocset_item_by_index: header NULL"
        );
        return false;
    }

    ind = _get_allocset_item_by_index( header, index );

    if( unlikely( ind == ALLOCSET_ITEM_INVALID ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_clear_allocset_item_by_index: could not locate "
            "item with index %lu",
            ( uint64_t ) ind
        );
        return false;
    }

    if( unlikely( !__TNS_MUTEX( &(header->allocset.locked) ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_clear_allocset_item_by_index: Unable to obtain lock"
        );
        return false;
    }

    base = _as_ptr_cache( header );

    if( unlikely( base == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_clear_allocset_item_by_index: Base address is NULL"
        );
        __C_MUTEX( &(header->allocset.locked) );
        return false;
    }

    item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
        base,
        ( sizeof( allocset_item_t ) * ind )
    );

    if( unlikely( item == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_clear_allocset_item_by_index: Dereferenced item is NULL at %lu",
            ( uint64_t ) ind
        );
        __C_MUTEX( &(header->allocset.locked) );
        return false;
    }

    // Fixup offset-based pointers
    if( ind == header->allocset.head )
    {
        header->allocset.head = item->next;
        temp = ( allocset_item_t * ) _PTR_ADD_OFFSET(
            base,
            ( ind * sizeof( allocset_item_t ) )
        );

        if( temp == NULL )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_clear_allocset_item_by_index: Dereferenced head pointer NULL"
            );
            __C_MUTEX( &(header->allocset.locked) );
            return false;
        }

        temp->last = ALLOCSET_ITEM_INVALID;
    }

    if( ind == header->allocset.tail )
    {
        header->allocset.tail = item->last;
        temp = ( allocset_item_t * ) _PTR_ADD_OFFSET(
            base,
            ( ind * sizeof( allocset_item_t ) )
        );

        if( temp == NULL )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_clear_allocset_item_by_index: Dereferenced tail pointer NULL"
            );
            __C_MUTEX( &(header->allocset.locked) );
            return false;
        }

        temp->next = ALLOCSET_ITEM_INVALID;
    }

    if( item->next != ALLOCSET_ITEM_INVALID )
    {
        temp = ( allocset_item_t * ) _PTR_ADD_OFFSET(
            base,
            ( item->next * sizeof( allocset_item_t ) )
        );

        if( temp == NULL )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_clear_allocset_item_by_index: Dereferenced next pointer NULL"
            );
            __C_MUTEX( &(header->allocset.locked) );
            return false;
        }

        temp->last = item->last;
    }

    if( item->last != ALLOCSET_ITEM_INVALID )
    {
        temp = ( allocset_item_t * ) _PTR_ADD_OFFSET(
            base,
            ( item->last * sizeof( allocset_item_t ) )
        );

        if( temp == NULL )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_clear_allocset_item_by_index: Dereferenced last pointer NULL"
            );
            __C_MUTEX( &(header->allocset.locked) );
            return false;
        }

        temp->next = item->next;
    }

    item->size       = 0;
    item->issued_ref = get_null_ref();
    item->index      = 0;
    item->last       = ALLOCSET_ITEM_INVALID;
    item->next       = ALLOCSET_ITEM_INVALID;
    __C_MUTEX( &(header->allocset.locked) );
    return true;
}

static INLINE bool _set_allocset_item_by_index(
    slab_header * header,
    uint64_t      index,
    __ref         issued_ref,
    size_t        size,
    _as_ind_t     useind
)
{
    _as_ind_t         nextind = 0;
    _as_ind_t         prevind = 0;
    _as_ind_t         newind  = 0;
    register void *   base    = NULL;
    allocset_item_t * next    = NULL;
    allocset_item_t * new     = NULL;
    allocset_item_t * prev    = NULL;

    #ifdef _SLAB_DEBUG
    _slab_log(
        LL_SLAB_DEBUG,
        "_set_allocset_item_by_index( %p, %lu, __ref, %zu, %lu ) entry",
        header,
        ( uint64_t ) index,
        ( size_t ) size,
        ( uint64_t ) useind
    );
    #endif // _SLAB_DEBUG

    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_set_allocset_item_by_index: header NULL"
        );
        return false;
    }

    base = _as_ptr_cache( header );

    if( unlikely( base == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_set_allocset_item_by_index: Allocset has bad base address"
        );
        return false;
    }

    if( useind == ALLOCSET_ITEM_INVALID )
    {
        #ifdef _SLAB_DEBUG
        _slab_log( LL_SLAB_DEBUG, "Handling insertion case of index %lu, size %zu", ( uint64_t ) index, ( size_t ) size );
        #endif // _SLAB_DEBUG
        // Insertion case
        newind = _get_available_allocset_item( header );

        if( unlikely( !__TNS_MUTEX( &(header->allocset.locked) ) ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_set_allocset_item_by_index: Failed to get header lock"
            );
            return false;
        }

        if( newind == ALLOCSET_ITEM_INVALID )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_set_allocset_item_by_index: No available slots in allocset"
            );
            __C_MUTEX( &(header->allocset.locked) );
            return false;
        }

        new = ( allocset_item_t * ) _PTR_ADD_OFFSET(
            base,
            ( sizeof( allocset_item_t ) * ( uint64_t ) newind )
        );

        new->size       = size;
        new->index      = index;
        new->issued_ref = issued_ref;
        new->last       = ALLOCSET_ITEM_INVALID;
        new->next       = ALLOCSET_ITEM_INVALID;
    }
    else
    {
        if( unlikely( useind >= header->allocset.max_allocset ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_set_allocset_item_by_index: provided reuse-index %lu is out of bounds",
                ( uint64_t ) useind
            );
            __C_MUTEX( &(header->allocset.locked) );
            return false;
        }

        newind = useind;
        new = ( allocset_item_t * ) _PTR_ADD_OFFSET(
            base,
            ( sizeof( allocset_item_t ) * ( uint64_t ) newind )
        );

        if( unlikely( new == NULL ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_set_allocset_item_by_index: Recycled allocset node %lu dereferenced to NULL",
                ( uint64_t ) newind
            );
            __C_MUTEX( &(header->allocset.locked) );
            return false;
        }

        // Later part of the insertion routine relies on these being /NULL/
        new->last = ALLOCSET_ITEM_INVALID;
        new->next = ALLOCSET_ITEM_INVALID;
    }

    if( unlikely( header->allocset.head == ALLOCSET_ITEM_INVALID ) )
    {
        // Initialization case
        #ifdef _SLAB_DEBUG
        _slab_log(
            LL_SLAB_DEBUG,
            "Inserted new index %lu as head of list (initialized)",
            ( uint64_t ) index
        );
        #endif // _SLAB_DEBUG
        header->allocset.head = newind;
        header->allocset.tail = newind;
        __C_MUTEX( &(header->allocset.locked) );
        return true;
    }

    next = ( allocset_item_t * ) _PTR_ADD_OFFSET(
        base,
        ( sizeof( allocset_item_t ) * header->allocset.head )
    );

    if( unlikely( next == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_set_allocset_item_by_index: List head dereferenced to NULL"
        );
        __C_MUTEX( &(header->allocset.locked) );
        return false;
    }

    // Trivial head replacement insertion case
    if( unlikely( next->index > index ) )
    {
        next->last = newind;
        new->next  = header->allocset.head;
        __FENCE();
        header->allocset.head = newind;
        __C_MUTEX( &(header->allocset.locked) );
        return true;
    }

    if( unlikely( next->index == index ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_set_allocset_item_by_index: INDEX COLLISION at %lu at head of allocset, head size is %zu",
            ( uint64_t ) index,
            ( size_t ) next->size
        );
        __C_MUTEX( &(header->allocset.locked) );
        return false;
    }

    while( next->next != ALLOCSET_ITEM_INVALID )
    {
        prev    = next;
        prevind = nextind;
        nextind = next->next;
        next    = ( allocset_item_t * ) _PTR_ADD_OFFSET(
            base,
            ( sizeof( allocset_item_t ) * nextind )
        );

        if( unlikely( next == NULL ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_set_allocset_item_by_index: Dereferenced next list node (%lu) is NULL",
                ( uint64_t ) nextind
            );
            __C_MUTEX( &(header->allocset.locked) );
            return false;
        }

        if( next->index > index )
        {
            next->last = newind;
            new->next  = nextind;
            prev->next = newind;
            new->last  = prevind;
            __C_MUTEX( &(header->allocset.locked) );
            return true;
        }

        if( unlikely( next->index == index ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_set_allocset_item_by_index: INDEX COLLISION at %lu at AS IND %lu, collided size is %zu",
                ( uint64_t ) index,
                ( uint64_t ) nextind,
                ( size_t ) next->size
            );
            __C_MUTEX( &(header->allocset.locked) );
            return false;
        }
    }

    // Insert at tail, it can be inferred that tail->index <= index
    if( header->allocset.tail == ALLOCSET_ITEM_INVALID )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_set_allocset_item_by_index: Tail insert failed - tail is invalid"
        );
        __C_MUTEX( &(header->allocset.locked) );
        return false;
    }

    prevind = header->allocset.tail;
    prev = ( allocset_item_t * ) _PTR_ADD_OFFSET(
        base,
        ( sizeof( allocset_item_t ) * prevind )
    );

    if( unlikely( prev == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_set_allocset_item_by_index: Tail index (%lu) dereferenced as NULL",
            ( uint64_t ) prevind
        );
        __C_MUTEX( &(header->allocset.locked) );
        return false;
    }

    if( unlikely( prev->index == index ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_set_allocset_item_by_index: INDEX COLLISION at %lu at TAIL",
            ( uint64_t ) index
        );
        __C_MUTEX( &(header->allocset.locked) );
        return false;
    }

    new->last             = prevind;
    prev->next            = newind;
    header->allocset.tail = newind;
    __C_MUTEX( &(header->allocset.locked) );
    return true;
}

static INLINE _as_ind_t _get_available_allocset_item( slab_header * header )
{
    register _as_ind_t i    = 0;
    register void *    base = NULL;
    allocset_item_t *  item = NULL;

    /* Perform exhaustive search of allocset for available slot */
    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_get_available_allocset_item: header NULL"
        );
        return ALLOCSET_ITEM_INVALID;
    }

    base = _as_ptr_cache( header );

    if( unlikely( base == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_get_available_allocset_item: base address is NULL"
        );
        return ALLOCSET_ITEM_INVALID;
    }

    if( unlikely( !__TNS_MUTEX( &(header->allocset.locked) ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_get_available_allocset_item: Could not obtain lock"
        );
        return ALLOCSET_ITEM_INVALID;
    }

    for( i = 0; i < header->allocset.max_allocset; i++ )
    {
        item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
            base,
            ( sizeof( allocset_item_t ) * i )
        );

        if( unlikely( item == NULL ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_get_available_allocset_item: in-bounds list "
                "item is NULL at %lu",
                ( uint64_t ) i
            );
            __C_MUTEX( &(header->allocset.locked) );
            return ALLOCSET_ITEM_INVALID;
        }

        if( item->size == 0 )
            break;
    }

    if( i > header->allocset.used )
        header->allocset.used++;

    __C_MUTEX( &(header->allocset.locked) );
    return i;
}

// Note we can remove extra uses of this such as the update portion of _set_allocset_item_by_index, etc
static INLINE _as_ind_t _get_allocset_item_by_index(
    slab_header * header,
    uint64_t      index
)
{
    allocset_item_t * item         = NULL;
    _as_ind_t         curr_index   = 0;
    void *            base         = NULL;

    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_get_allocset_item_by_index: header NULL"
        );
        return ALLOCSET_ITEM_INVALID;
    }

    if( !__TNS_MUTEX( &(header->allocset.locked) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_get_allocset_item_by_index: Failed to obtain lock"
        );
        return ALLOCSET_ITEM_INVALID;
    }

    base = _as_ptr_cache( header );

    if( header->allocset.head == ALLOCSET_ITEM_INVALID )
    { // Uninitialized
        __C_MUTEX( &(header->allocset.locked) );
        return ALLOCSET_ITEM_INVALID;
    }

    item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
        base,
        ( sizeof( allocset_item_t ) * ( uint64_t ) header->allocset.head )
    );

    if( unlikely( item == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_get_allocset_item_by_index: Base address or dereferenced "
            "head (%lu) is NULL",
            ( uint64_t ) header->allocset.head
        );
        __C_MUTEX( &(header->allocset.locked) );
        return ALLOCSET_ITEM_INVALID;
    }

    curr_index = header->allocset.head;

    while( item->next != ALLOCSET_ITEM_INVALID )
    {
        if( item->index == index )
            break;

        curr_index = item->next;

        if( unlikely( curr_index == ALLOCSET_ITEM_INVALID ) )
        {
            __C_MUTEX( &(header->allocset.locked) );
            return ALLOCSET_ITEM_INVALID;
        }

        item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
            base,
            ( sizeof( allocset_item_t ) * ( uint64_t ) curr_index )
        );

        if( unlikely( item == NULL ) )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "_get_allocset_item_by_index: Next item is NULL at index %lu",
                ( uint64_t ) curr_index
            );
            __C_MUTEX( &(header->allocset.locked) );
            return ALLOCSET_ITEM_INVALID;
        }
    }

    // Final sanity check
    item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
        base,
        ( sizeof( allocset_item_t ) * curr_index )
    );

    if( unlikely( item == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_get_allocset_item_by_index: Located item is NULL at %lu",
            ( uint64_t ) curr_index
        );
        __C_MUTEX( &(header->allocset.locked) );
        return ALLOCSET_ITEM_INVALID;
    }

    if( unlikely( item->index != index ) )
    {
        // Not found
        __C_MUTEX( &(header->allocset.locked) );
        return ALLOCSET_ITEM_INVALID;
    }

    __C_MUTEX( &(header->allocset.locked) );
    return curr_index;
}

static INLINE size_t _get_allocset_element_by_index(
    slab_header * header,
    uint64_t      index
)
{
    _as_ind_t         allocset_index = 0;
    allocset_item_t * item           = NULL;

    allocset_index = _get_allocset_item_by_index( header, index );

    if( unlikely( allocset_index == ALLOCSET_ITEM_INVALID ) )
        return ULONG_MAX;

    item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
        ( _as_ptr_cache( header ) ),
        ( sizeof( allocset_item_t ) * allocset_index )
    );

    if( unlikely( item == NULL ) )
        return ULONG_MAX;

    return item->size;
}

static INLINE bool _fail_canary( void )
{
    #ifdef _FORCE_SIGSEGV_ON_CANARY_FAILURE
    *( ( uint8_t * ) 0 ) = 42;
    #endif // _FORCE_SIGSEGV_ON_CANARY_FAILURE
    return false;
}

static INLINE bool _check_canaries( slab_header * header )
{
    register canary_t * c_ptr = NULL;

    if( unlikely( header == NULL ) )
        return false;

    // Check start of slab
    c_ptr = get_ptr_fast( header->loc_c_allocstart );

    if( unlikely( c_ptr == NULL ) )
        return _fail_canary();
    if( unlikely( header->c_allocstart != *c_ptr ) )
    {
        if( __no_test )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "Failed allocstart canary check: Got %x, expected %x as %p",
                ( uint32_t ) *c_ptr,
                ( uint32_t ) header->c_allocstart,
                c_ptr
            );
        }
        return _fail_canary();
    }

    // Check start of FSM
    c_ptr = get_ptr_fast( header->loc_c_fsmstart );

    if( unlikely( c_ptr == NULL ) )
        return _fail_canary();
    if( unlikely( header->c_fsmstart != *c_ptr ) )
    {
        if( __no_test )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "Failed fsmstart canary check: Got %x, expected %x at %p",
                ( uint32_t ) *c_ptr,
                ( uint32_t ) header->c_fsmstart,
                c_ptr
            );
        }
        return _fail_canary();
    }

    // Check end of FSM
    c_ptr = get_ptr_fast( header->loc_c_fsmend );

    if( unlikely( c_ptr == NULL ) )
        return _fail_canary();
    if( unlikely( header->c_fsmend != *c_ptr ) )
    {
        if( __no_test )
        {
            _slab_log(
                LL_SLAB_ERROR,
                "Failed fsmend canary check: Got %x, expected %x at %p",
                ( uint32_t ) *c_ptr,
                ( uint32_t ) header->c_fsmend,
                c_ptr
            );
        }
        return _fail_canary();
    }

    return true;
}

static INLINE context_t get_ctx_by_id( const char * ident )
{
    header_iter i       = ( header_iter ) 0;
    char *      i_ident = NULL;
    bool        found   = false;

    for( i = 0; i < ( header_iter ) _SLAB_MAX_SLABS; i++ )
    {
        i_ident = control->headers[i].object_id;

        if( strncmp( i_ident, ident, _SLAB_MAX_IDENT ) == 0 )
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
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Slab is not initialized"
        );
        return false;
    }

    if( control == NULL || control_segment == SEGMENT_HANDLE_INVALID )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Control segment mapping is NULL or handle is invalid"
        );
        return false;
    }

    if( control->magic != _SLAB_CONTROL_MAGIC )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Bad magic: %x expected %x",
            control->magic,
            _SLAB_CONTROL_MAGIC
        );
        return false;
    }

    return true;
}

static bool check_slab_header( header_iter index )
{
    slab_header * header = NULL;

    if( unlikely( !check_slab_state() ) )
        return false;

    if( unlikely( index > _SLAB_MAX_SLABS ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "index (%lu) is out of bounds for _SLAB_MAX_SLABS (%lu)",
            ( uint64_t ) index,
            ( uint64_t ) _SLAB_MAX_SLABS
        );
        return false;
    }

    header = ( slab_header * ) &(control->headers[index]);

    if( header->magic != _SLAB_HEADER_MAGIC )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Bad header magic %x at index %lu, expected %x",
            header->magic,
            ( uint64_t ) index,
            _SLAB_HEADER_MAGIC
        );
        return false;
    }

    return true;
}

bool check_context( context_t ctx )
{
    return _check_context( ctx );
}

static INLINE bool _check_context( context_t ctx )
{
    if( unlikely( ctx == INVALID_CONTEXT ) ) // Sanity check the context
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Context check failed: INVALID_CONTEXT"
        );
        return false;
    }

    if( unlikely( control == NULL ) ) // check that we're mapped
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Context check failed: control segment not mapped"
        );
        return false;
    }

    if( unlikely( ctx >= ( context_t ) _SLAB_MAX_SLABS ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Context check failed: context out of bounds"
        );
        return false;
    }

    if( unlikely( control->headers[ctx].magic != _SLAB_HEADER_MAGIC ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "Context check failed: context's magic is bad"
        );
        return false;
    }

    if( unlikely( control->headers[ctx].self != ctx ) )
    {
        #ifdef _SLAB_DEBUG
        _slab_log(
            LL_SLAB_DEBUG,
            "Context check failed: context not initialized"
        );
        #endif // _SLAB_DEBUG
        return false;
    }

    return true;
}

/*
 *  Helper functions for moving memory between local (malloc/calloc/realloc/etc) context
 *  and shared memory contexts en-masse
 */
ref_t move_to_shared( context_t ctx, void ** pointer, size_t size )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );

    return _move_to_shared( header, pointer, size, true );
}

static INLINE ref_t _move_to_shared(
    slab_header * header,
    void **       pointer,
    size_t        size,
    bool          do_free
)
{
    void * target  = NULL;
    __ref  retref  = {0};
    ref_t  userref = NULLREF;
    retref = get_null_ref();
    // We're trusting the user to have set a correct object size
    // - we can only do cursory checks
    if( unlikely( pointer == NULL || *pointer == NULL ) )
        return NULLREF;

    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "move_to_shared: Invalid context."
        );
        return NULLREF;
    }

    if( unlikely( ( size % header->object_size ) != 0 ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "move_to_shared: requested size %zu is not a multiple of "
            "object size %lu",
            size,
            ( uint64_t ) header->object_size
        );
        return NULLREF;
    }

    if( unlikely( !__TNS_MUTEX( &(header->locked) ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "move_to_shared: Failed to obtain header lock"
        );
        return NULLREF;
    }

    userref = _shmalloc( header, size, false );
    target = _to_ptr( header, userref );

    if( unlikely( userref == NULLREF || target == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "move_to_shared: Failed to allocate shared memory"
        );
        return NULLREF;
    }

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

void * move_to_local( context_t ctx, ref_t * ref )
{
    slab_header * header = NULL;

    header = _get_header_by_context( ctx );
    return _move_to_local( header, ref, true );
}

static INLINE void * _move_to_local(
    slab_header * header,
    ref_t *       ref,
    bool          do_free
)
{
    void *            target  = NULL;
    void *            source  = NULL;
    size_t            size    = 0;
    allocset_item_t * item    = NULL;

    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "move_to_local: Invalid context."
        );
        return NULL;
    }

    if( unlikely( ref == NULL ) )
        return NULL;

    if( unlikely( *ref == NULLREF ) )
        return NULL;

    if( unlikely( !__TNS_MUTEX( &(header->locked) ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "move_to_local: Failed to lock header"
        );
        return NULL;
    }

    if( unlikely( !( *ref < header->allocset.used ) ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "move_to_local: ref out of range"
        );
        __C_MUTEX( &(header->locked) );
        return NULL;
    }

    item = ( allocset_item_t * ) _PTR_ADD_OFFSET(
        ( _as_ptr_cache( header ) ),
        ( sizeof( allocset_item_t ) * ( uint64_t ) *ref )
    );

    if( unlikely( item == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "move_to_local: Failed to get allocset item for move"
        );
        __C_MUTEX( &(header->locked) );
        return NULL;
    }

    //index = item->index;
    size  = item->size;

    source = ( void * ) _to_ptr( header, *ref );

    if( unlikely( source == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "move_to_local: NULL reference"
        );
        return NULL;
    }

    target = malloc( header->object_size * size );

    if( unlikely( target == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "move_to_local: Insufficient memory available"
        );
        return NULL;
    }

    memcpy(
        target,
        source,
        header->object_size * size
    );

    __C_MUTEX( &(header->locked) );

    if( do_free )
    {
        _shfree( header, *ref, false );
        *ref = NULLREF;
    }

    return target;
}

#ifdef _SLAB_DEBUG
void print_fsm( slab_header * header )
{
    #ifdef _SLAB_DEBUG
    _print_fsm( header );
    #endif // _SLAB_DEBUG
    return;
}

static void _print_fsm( slab_header * header )
{
    uint64_t i = 0;
    fsm_t * fsm_word = NULL;
    uint64_t len = 0;

    if( header == NULL )
        return;

    len = _get_fsm_length( header );
    _slab_log( LL_SLAB_DEBUG, "----- FSM %p -----", get_ptr( header->fsm ) );

    for( i = 0; i < len; i++ )
    {
        fsm_word = (fsm_t *) _PTR_ADD_OFFSET(
            ( _fsm_ptr_cache( header ) ),
            ( ( len - 1 - i ) * sizeof( fsm_t ) )
        );

        fprintf( stdout, "FSM[%03lu] (%p): ", ( len - 1 - i ), fsm_word );
        print_bin( ( uint64_t ) *fsm_word );
    }

    return;
}

// Pretty printing for binary numbers > 32 bits
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

/*
 * _dump_header( header *, bool verbose )
 * prints out a compactified form of the shared memory state and
 * contents to the console
 */
static void _dump_header( slab_header * header, bool simple )
{
    void *   ptr        = NULL;
    uint64_t i          = 0;
    uint64_t j          = 0;
    uint64_t k          = 0;
    uint32_t alloc_size = 0;
    _as_ind_t ind       = 0;
    if( header == NULL )
        return;

    fprintf( stdout, "==== SLAB HEADER %p\n", header );
    fprintf(
        stdout,
        "  magic:            0x%08x\n"
        "  segment:          %lu\n"
        "  object_size:      %zu\n"
        "  count_hint:       %zu\n"
        "  allocs:           %p\n"
        "  (allocs segment): %lu\n"
        "  n_allocs:         %lu\n"
        "  fsm:              %p\n"
        "  (fsm segment):    %lu\n"
        "  max_allocations:  %lu\n"
        "  object_id:        %s\n"
        "  self:             %lu\n"
        "  locked:           %s\n"
        "  i_front_fsm_bit:  %lu\n"
        "  loc_c_allocstart: %p (%08x%08x)\n"
        "  c_allocstart:     0x%08x%08x\n"
        "  loc_c_fsmstart:   %p (%08x%08x)\n"
        "  c_fsmstart:       0x%08x%08x\n"
        "  loc_c_fsmend:     %p (%08x%08x)\n"
        "  c_fsmend:         0x%08x%08x\n"
        "    allocset.set:          %p\n"
        "     segment:              %lu\n"
        "    allocset.locked:       %s\n"
        "    allocset.max_allocset: %lu\n"
        "    allocset.used:         %lu\n"
        "    allocset.head:         %lu\n"
        "    allocset.tail:         %lu\n",
        ( uint32_t ) header->magic,
        ( uint64_t ) header->segment,
        ( size_t ) header->object_size,
        ( size_t ) header->count_hint,
        ( void * ) get_ptr( header->allocs ),
        ( uint64_t ) ref_get_segment( header->allocs ),
        ( uint64_t ) header->n_allocs,
        ( void * ) get_ptr( header->fsm ),
        ( uint64_t ) ref_get_segment( header->fsm ),
        ( uint64_t ) header->max_allocations,
        ( char * ) header->object_id,
        ( uint64_t ) header->self,
        header->locked ? "T" : "F",
        ( uint64_t ) header->i_front_fsm_bit,
        ( void * ) get_ptr( header->loc_c_allocstart ),
        ( uint32_t ) ( ( *( ( uint64_t * ) get_ptr( header->loc_c_allocstart ) ) ) >> 32 ),
        ( uint32_t ) ( *( ( uint64_t * ) get_ptr( header->loc_c_allocstart ) ) ),
        ( uint32_t ) ( ( ( uint64_t ) header->c_allocstart ) >> 32 ),
        ( uint32_t ) header->c_allocstart,
        ( void * ) get_ptr( header->loc_c_fsmstart ),
        ( uint32_t ) ( ( *( ( uint64_t * ) get_ptr( header->loc_c_fsmstart ) ) ) >> 32 ),
        ( uint32_t ) ( *( ( uint64_t * ) get_ptr( header->loc_c_fsmstart ) ) ),
        ( uint32_t ) ( ( ( uint64_t ) header->c_fsmstart ) >> 32 ),
        ( uint32_t ) header->c_fsmstart,
        ( void * ) get_ptr( header->loc_c_fsmend ),
        ( uint32_t ) ( ( *( ( uint64_t * ) get_ptr( header->loc_c_fsmend ) ) ) >> 32 ),
        ( uint32_t ) ( *( ( uint64_t * ) get_ptr( header->loc_c_fsmend ) ) ),
        ( uint32_t ) ( ( ( uint64_t ) header->c_fsmend ) >> 32 ),
        ( uint32_t ) header->c_fsmend,
        ( void * ) _as_ptr_cache( header ),
        ( uint64_t ) ref_get_segment( header->allocset.set ),
        ( uint64_t ) header->allocset.locked ? "T" : "F",
        ( uint64_t ) header->allocset.max_allocset,
        ( uint64_t ) header->allocset.used,
        ( uint64_t ) header->allocset.head,
        ( uint64_t ) header->allocset.tail
    );
    fflush( stdout );
    if( simple )
        return;
    fprintf( stdout, "---- HEADER DATA DETAIL:\n-- FSM:\n" );
    _print_fsm( header );
    fprintf( stdout, "-- ALLOCSET[]\n" );
    if( header->allocset.head != ALLOCSET_ITEM_INVALID )
    {
        ind = header->allocset.head;
        ptr = _PTR_ADD_OFFSET(
            ( _as_ptr_cache( header ) ),
            ( sizeof( allocset_item_t ) * ind )
        );

        if( ptr != NULL )
        {
            while( ind != ALLOCSET_ITEM_INVALID )
            {
                fprintf(
                    stdout,
                    "    ALLOCSET[%lu]\n"
                    "      size:  %zu\n"
                    "      index: %lu\n"
                    "      __ref: %p\n"
                    "      next:  %lu\n"
                    "      last:  %lu\n",
                    ( uint64_t ) ind,
                    ( ( allocset_item_t * ) ptr )->size,
                    ( uint64_t ) ( ( allocset_item_t * ) ptr )->index,
                    get_ptr( ( ( allocset_item_t * ) ptr )->issued_ref ),
                    ( uint64_t ) ( ( allocset_item_t * ) ptr )->next,
                    ( uint64_t ) ( ( allocset_item_t * ) ptr )->last
                );
                ind = ( ( allocset_item_t * ) ptr )->next;
                ptr = _PTR_ADD_OFFSET(
                    ( _as_ptr_cache( header ) ),
                    ( ind * sizeof( allocset_item_t ) )
                );

                if( ptr == NULL || ind == ALLOCSET_ITEM_INVALID )
                    break;

                if( unlikely( ind == ( ( allocset_item_t * ) ptr )->next ) )
                {
                    fprintf(
                        stderr,
                        "LOOP DETECTED. Next item is current index (%lu)\n",
                        ( uint64_t ) ind
                    );
                    break;
                }
            }
        }
        else
        {
            _slab_log(
                LL_SLAB_ERROR,
                "Null pointer from head"
            );
        }
    }
    else
    {
        fprintf( stdout, "    none\n" );
    }

    fprintf( stdout, "-- ALLOCS[]:\n" );
    for( i = 0; i < header->max_allocations; i++ )
    {
        ptr = _PTR_ADD_OFFSET(
            ( _allocs_ptr_cache( header ) ),
            ( i * header->object_size )
        );

        alloc_size = _get_allocset_element_by_index( header, i );

        if( alloc_size == UINT_MAX )
            continue;

        if( alloc_size == 0 )
            continue;

        fprintf( stdout, "ALLOC[%lu] (%p):\n", ( uint64_t ) i, ptr );

        for( j = 0; j < alloc_size; j++ )
        {
            fprintf( stdout, "0x" );

            for( k = 0; k < header->object_size; k++ )
            {
                fprintf(
                    stdout,
                    "%s%s",
                    hexes[*( ( uint8_t * ) _PTR_ADD_OFFSET( ptr, k )) >> 4],
                    hexes[*( ( uint8_t * ) _PTR_ADD_OFFSET( ptr, k )) & 0x0F]
                );
            }

            ptr = _PTR_ADD_OFFSET( ptr, header->object_size );
            if( ( j + 1 ) % 4 == 0 )
                fprintf( stdout, "\n" );
            else
                fprintf( stdout, " " );
        }

        fprintf( stdout, "\n" );
    }

    fprintf( stdout, "==============================\n" );
    return;
}

static void _dump_context( context_t ctx )
{
    slab_header * header = NULL;
    header = _get_header_by_context( ctx );

    return _dump_header( header, false );
}

static void _dump_control( slab_control * ctrl )
{
    header_iter i = 0;

    if( ctrl == NULL )
        return;

    fprintf(
        stdout,
        "Control header %p:\n"
        "  Magic: %x\n"
        "  locked: %s\n"
        "  headers[%lu]\n",
        ctrl,
        ( uint32_t ) ctrl->magic,
        ctrl->locked ? "T" : "F",
        ( uint64_t ) _SLAB_MAX_SLABS
    );

    for( i = 0; i < _SLAB_MAX_SLABS; i++ )
    {
        fprintf( stdout, " Header[%lu]:\n", ( uint64_t ) i );
        _dump_header( &(ctrl->headers[i]), true );
    }
}
#endif // _SLAB_DEBUG

/*
 * Debugging functions - dump either the control headers and associated short-form headers (dump_control)
 * or the full context (same as _dump_header(header, false))
 */
void dump_control( void )
{
    if( control == NULL )
    {
        _slab_log( LL_SLAB_ERROR, "No mapped control header" );
        return;
    }

    #ifdef _SLAB_DEBUG
    _dump_control( control );
    #else
    _slab_log(
        LL_SLAB_ERROR,
        "Cannot dump control header - SLAB_DEBUG not enabled"
    );
    #endif // _SLAB_DEBUG
    return;
}

void dump_context( context_t ctx )
{
    #ifdef _SLAB_DEBUG
    return _dump_context( ctx );
    #endif // _SLAB_DEBUG
    _slab_log(
        LL_SLAB_ERROR,
        "Cannot dump context - SLAB_DEBUG not enabled"
    );
    return;
}

/*
 *  printf-esque logging function for this CU
 */
static void _slab_log( slab_ll log_level, char * message, ... )
{
    va_list        args          = {{0}};
    FILE *         output_handle = NULL;
    struct timeval tv            = {0};
    char           buff_time[28] = {0};


    if( unlikely( message == NULL ) )
        return;

    #if !defined( _SLAB_DEBUG ) && !defined( _SLAB_FSM_DEBUG )
    if( log_level == LL_SLAB_DEBUG )
        return;
    #endif // !_SLAB_DEBUG && !_SLAB_FSM_DEBUG

     gettimeofday( &tv, NULL );

    strftime(
        buff_time,
        sizeof( buff_time ) / sizeof( *buff_time ),
        "%Y-%m-%d %H:%M:%S",
        gmtime( &tv.tv_sec )
    );

    output_handle = stdout;
    if( log_level == LL_SLAB_ERROR )
        output_handle = stderr;

    va_start( args, message );

    fprintf(
        output_handle,
        "%s.%05d [%d] (%s) %s: ",
        buff_time,
        ( int ) ( tv.tv_usec / 1000 ),
        getpid(),
        "SLAB.C",
        log_level == LL_SLAB_DEBUG ?
            "DEBUG" : log_level == LL_SLAB_ERROR ?
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

/*
 * _get_random()
 * returns a random uint64_t number
 * This is by no means cryptographically secure (we don't check entropy sources
 * or verify that the result has sufficient entropy
 */
static INLINE uint64_t _get_random( void )
{
    uint64_t random_val = 0;
    #ifdef _SLAB_HAS_RANDOM
    // attempt to use /dev/urandom, then /dev/random, then rand() this is
    // relatively safe - and will eventually return but provides maximal
    // entropy in the cases where the system is has sufficient bytes in the
    // urandom device. For the worst case, we fall back to rand() when the
    // system has urandom/random as the same device and insufficient entropy
    // is present.
    if(
    #ifndef _SLAB_RAND_USE_SYSCALL
        getrandom(
            &random_val,
            sizeof( random_val ),
            GRND_NOBLOCK
        ) < sizeof( random_val )
    #else
        syscall(
            SYS_getrandom,
            &random_val,
            sizeof( random_val ),
            GRND_NOBLOCK
        ) > 0
    #endif // !_SLAB_RAND_USE_SYSCALL
      )
    {
        random_val = 0;
        if(
        #ifndef _SLAB_RAND_USE_SYSCALL
            getrandom(
                &random_val,
                sizeof( random_val ),
                GRND_NOBLOCK | GRND_RANDOM
            ) < sizeof( random_val )
        #else
            syscall(
                SYS_getrandom,
                &random_val,
                sizeof( random_val ),
                GRND_NOBLOCK | GRND_RANDOM
            ) > 0
        #endif // !_SLAB_RAND_USE_SYSCALL
          )
        {
    #endif // _SLAB_HAS_RANDOM
            random_val = rand();
    #ifdef _SLAB_HAS_RANDOM
        }
    }
    #endif // _SLAB_HAS_RANDOM

    return random_val;
}

static INLINE char * _get_context_name( slab_header * header )
{
    if( header == NULL )
        return NULL;

    return header->object_id;
}

/* Cache access functions
 *
 *  These check the state of the cache (validating size on the fly)
 *  and will update-and-return the appropriate base pointer.
 *
 *  These should be used in lieu of get_ptr_fast or get_ptr for __refs
 *  that point to the FSM, allocset base, or allocs[]
 *  so that common memory-related routines can be kept to a minimal
 *  latency
 */
static INLINE void * _as_ptr_cache( slab_header * header )
{
    #ifdef _SLAB_EXTRA_SANE
    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_as_ptr_cache: Header is NULL"
        );
        return NULL;
    }

    if( unlikely( header->self == INVALID_CONTEXT ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_as_ptr_cache: Invalid header context"
        );
        return NULL;
    }

    #endif // _SLAB_EXTRA_SANE
    if(
        unlikely(
            ( __ptr_cache[header->self].as_size != ( header->allocset.max_allocset * sizeof( allocset_item_t ) ) )
         || ( __ptr_cache[header->self].as_base == NULL ) // may not be needed
        )
      )
    {
        __ptr_cache[header->self].as_size = ( header->allocset.max_allocset * sizeof( allocset_item_t ) );
        __ptr_cache[header->self].as_base = get_ptr( header->allocset.set );
        __FENCE();
        return __ptr_cache[header->self].as_base;
    }

    return __ptr_cache[header->self].as_base;
}

static INLINE void * _allocs_ptr_cache( slab_header * header )
{
    #ifdef _SLAB_EXTRA_SANE
    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_as_ptr_cache: Header is NULL"
        );
        return NULL;
    }

    if( unlikely( header->self == INVALID_CONTEXT ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_as_ptr_cache: Invalid header context"
        );
        return NULL;
    }
    #endif // _SLAB_EXTRA_SANE
    if( unlikely( __ptr_cache[header->self].allocs_size != ( header->max_allocations * header->object_size ) ) )
    {
        __ptr_cache[header->self].allocs_size = ( header->max_allocations * header->object_size );
        __ptr_cache[header->self].allocs_base = get_ptr( header->allocs );
        __FENCE();
    }

    return __ptr_cache[header->self].allocs_base;
}

static INLINE void * _fsm_ptr_cache( slab_header * header )
{
    #ifdef _SLAB_EXTRA_SANE
    if( unlikely( header == NULL ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_as_ptr_cache: Header is NULL"
        );
        return NULL;
    }

    if( unlikely( header->self == INVALID_CONTEXT ) )
    {
        _slab_log(
            LL_SLAB_ERROR,
            "_as_ptr_cache: Invalid header context"
        );
        return NULL;
    }
    #endif // _SLAB_EXTRA_SANE
    if( unlikely( __ptr_cache[header->self].fsm_size != ( _get_fsm_length( header ) * sizeof( fsm_t ) ) ) )
    {
        __ptr_cache[header->self].fsm_size = ( _get_fsm_length( header ) * sizeof( fsm_t ) );
        __ptr_cache[header->self].fsm_base = get_ptr( header->fsm );
        __FENCE();
    }

    return __ptr_cache[header->self].fsm_base;
}
