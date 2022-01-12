/*------------------------------------------------------------------------
 *
 * slab.c
 *     Shared Memory slab allocator
 *
 * Copyright (c) 2021, MerchLogix Inc.
 *
 * IDENTIFICATION
 *        service/src/lib/slab.c
 *
 *------------------------------------------------------------------------
 */
#include "slab.h"

static shm_handle        control_segment         = SEGMENT_HANDLE_INVALID;
static __ref             control_segment_address = {0};
static shalloc_control * mapped_control          = NULL;
static bool              _slab_init              = false;
static pid_t             p_pid                   = 0;

static __inline__ bool _fail_canary( void ) __attribute__((always_inline));
static __inline__ bool _init_slab( context_t, shalloc_header *, bool );
static __inline__ context_t get_ctx_by_id( const char * ) __attribute__((always_inline));
static __inline__ bool check_shalloc_header( header_iter ) __attribute__((always_inline));
static __inline__ bool check_context( context_t ) __attribute__((always_inline));
static __inline__ bool _check_canaries( shalloc_header * ) __attribute__((always_inline));
static __inline__ __ref _get_alloc_element_by_index( shalloc_header *, uint64_t ) __attribute__((always_inline));

static __inline__ __ref _shmalloc( context_t, size_t, bool );
static __inline__ uint64_t _get_fsm_length( shalloc_header * );// __attribute__((always_inline));
static __inline__ void _set_fsm_element_by_index( shalloc_header *, uint64_t );// __attribute__((always_inline));
static __inline__ void _clear_fsm_element_by_index( shalloc_header *, uint64_t );// __attribute__((always_inline));
static __inline__ bool _get_fsm_element_by_index( shalloc_header *, uint64_t );// __attribute__((always_inline));
static __inline__ uint64_t _get_fsm_slot_by_width( shalloc_header *, uint64_t );// __attribute__((always_inline));
static __inline__ uint64_t __find_fsm_spot( shalloc_header *, uint64_t );// __attribute__((always_inline));
static __inline__ shalloc_header * _get_header_by_context( context_t );// __attribute__((always_inline));

// Called by either the parent process, pre fork to setup the allocation
// or by the child process(es) post-fork to attach to said control segment
bool slab_init( void )
{
    void *      segment_address = NULL;
    header_iter i               = 0;

    if( _slab_init == false )
    {
        p_pid = getpid();
        // Parent initialization sequence
        shm_init();
        segment_address = new_segment( sizeof( shalloc_control ) );

        if( segment_address == NULL )
            return false;

        srand( ( unsigned int ) _INVALID_CONTEXT );
        control_segment_address = get_ref( segment_address );
        control_segment         = ref_get_segment( control_segment_address );
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

// Initializes a new slab
context_t new_slab( const char * tag, size_t object_size )
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

        strncpy(
            mapped_control->headers[ret].object_id,
            ident,
            _SHALLOC_MAX_IDENT
        );
    }

    return ret;
}

__ref scalloc( context_t ctx, size_t size, uint64_t count )
{
    shalloc_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return get_null_ref();

    return _shmalloc( ctx, size, true );
}

__ref smalloc( context_t ctx, size_t size )
{
    shalloc_header * header = NULL;

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return get_null_ref();


    return _shmalloc( ctx, size, false );
}

__ref srealloc( context_t ctx, __ref oldref, size_t size )
{
    shalloc_header * header = NULL;
    __ref            retref = {0};

    header = _get_header_by_context( ctx );

    if( unlikely( header == NULL ) )
        return get_null_ref();

    retref = get_null_ref();
    return retref;
}

void sfree( context_t ctx, __ref pointer )
{
    shalloc_header * header = NULL;
    offset_t         offset = 0;
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

    offset = ref_get_offset( pointer );

    if( unlikely( ( offset % header->object_size != 0 ) ) )
    #ifdef SLAB_DEBUG
    {
        fprintf(
            stderr,
            "Unaligned free of offset %lu with object size %lu\n",
            ( uint64_t ) offset,
            ( uint64_t ) header->object_size
        );
    #endif // SLAB_DEBUG
        return;
    #ifdef SLAB_DEBUG
    }
    #endif // SLAB_DEBUG

    index = ( offset / header->object_size ) - 1;
    fprintf( stdout, "Got index %lu\n", index );

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

    size = header->allocset[index];

    #ifdef SLAB_DEBUG
    fprintf( stdout, "Freeing element of size %lu\n", size );
    #endif // SLAB_DEBUG

    if( unlikely( header->allocset[index] == 0 ) )
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
        _set_fsm_element_by_index( header, i );
    }

    header->allocset[index] = 0;
    header->n_allocs       -= size;
    __C_MUTEX( &(header->locked) );
    return;
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

    if( num_objects >= ( header->max_allocations - header->n_allocs ) )
    {
        // TODO: Reallocate entire segment
        // or come up with a clever way to extend (shm.c) the segment
        // then shake out the extension to the page
        // This / should / be easy - we only need to relocate the free list
        // to the end of the new, extended page. This will preserve existing
        // __refs
        fprintf( stderr, "NEED REALLOC\n" );
    }

    // Layout & usage of free list:
    // [ front - single allocs ..... contiguous allocs - rear ]
    if( unlikely( !__TNS_MUTEX( &(header->locked) ) ) )
    {
        fprintf( stderr, "Failed to acquire segment lock\n" );
        return get_null_ref();
    }

    if( num_objects > 1 )
    {
        errno = 0;
        index = _get_fsm_slot_by_width( header, num_objects );

        if( unlikely( errno == ENOSPC ) ) // Need to reallocate
        {
            fprintf( stderr, "NEED REALLOC 2\n" );
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
    }

    if( zero_fill == true )
    {
        ptr = get_ptr( retref );
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
            get_ptr( header->allocs ),
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

    fsm_index  = ind / FSM_WIDTH;
    fsm_offset = ind % FSM_WIDTH;
    fsm_word   = ( fsm_t * ) _PTR_ADD_OFFSET(
        get_ptr( header->fsm ),
        fsm_index
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
    uint64_t fsm_index  = 0;
    uint8_t  fsm_offset = 0;
    fsm_t *  fsm_word   = NULL;

    if( unlikely( header == NULL ) )
        return;

    fsm_index  = ind / FSM_WIDTH;
    fsm_offset = ind % FSM_WIDTH;

    fsm_word = ( fsm_t * ) _PTR_ADD_OFFSET(
        get_ptr( header->fsm ),
        fsm_index
    );

    if( fsm_word == NULL )
        return;

    *fsm_word &= ~( ( fsm_t ) 1 << fsm_offset );

    return;
}

static __inline__ bool _get_fsm_element_by_index( shalloc_header * header, uint64_t ind )
{
    uint64_t fsm_index  = 0;
    uint8_t  fsm_offset = 0;
    fsm_t *  fsm_word   = NULL;

    if( unlikely( header == NULL ) )
    {
        errno = EINVAL;
        return false;
    }

    fsm_index  = ind / FSM_WIDTH;
    fsm_offset = ind % FSM_WIDTH;
    fsm_word   = ( fsm_t * ) _PTR_ADD_OFFSET(
        get_ptr( header->fsm ),
        fsm_index
    );

    if( fsm_word == NULL )
    {
        errno = EINVAL;
        return true;
    }

    if( ( ( *fsm_word >> fsm_offset ) & ( fsm_t ) 1 ) > 0 )
        return true;

    return false;
}

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
    for( fsm_index = bit_position; fsm_index < bit_position + width; fsm_index++ )
    {
        _set_fsm_element_by_index( header, fsm_index );
    }
    // XXX: We need to track how large the allocation is for purposes of freeing later

    header->allocset[bit_position] = width;

    return bit_position;
}

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
    register bool      compare_active   = false;
    register bool      last_word        = false;

    fsm_length = _get_fsm_length( header );
    mask_last  = ~( mask_last << ( requested_length % CHAR_BIT ) );

    if( requested_length <= FSM_SHIFT_WIDTH )
    {
        last_word      = true;
        compare_active = true;
        mask           = mask_last;
    }

    for( fsm_i = 0; fsm_i < fsm_length; fsm_i++ )
    { // Iterate over words of sizeof( fsm_t ) bytes
        fsm_word = *( ( fsm_t * ) _PTR_ADD_OFFSET(
            get_ptr( header->fsm ),
            ( ( fsm_length - 1 - fsm_i ) * sizeof( fsm_t ) )
        )); // Deref in outer loop

        for( fsm_word_i = 0; fsm_word_i < FSM_RATIO; fsm_word_i++ )
        { // Iterate over words within the given fsm_t word, size FSM_SHIFT_WIDTH bits
            temp = ( fsm_cmp_t ) ( fsm_word >> ( ( fsm_word_i ) * FSM_SHIFT_WIDTH ) );

            if( ( ~(temp) & mask ) == mask )
            {
                if( last_word )
                { // Prep for return & attempt to compactify past word boundaries
                    // Early exit when shifting wont help
                    if( ( last_word_val & FSM_LAST_WORD_MASK ) > 0 )
                        return header->max_allocations - requested_length + position;

                    mask = ( fsm_cmp_t ) FSM_LAST_WORD_MASK;
                    temp = last_word_val;

                    while( ( ~temp & mask ) != 0 )
                    {
                        if( temp == 0 )
                            break;
                        temp = temp << 1;
                        position--;
                    }

                    return header->max_allocations - requested_length + position;
                }

                bits_comp -= FSM_SHIFT_WIDTH;
                last_word  = ( bits_comp < FSM_SHIFT_WIDTH );

                if( !compare_active )
                    compare_active = true;

                if( last_word )
                    mask = mask_last;
            }
            else
            {   // No match
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

    initial_size = initial_size * unit_size + sizeof( canary_t ) * 4;

    mapped_address = new_segment( initial_size );

    if( mapped_address == NULL )
        return false;

    handle = get_handle_from_ptr( mapped_address );
    header->segment         = handle;

    // Available space - accounting for headers and canaries
    available = ( get_segment_size( handle ) - ( 3 * sizeof( canary_t ) ) );
    header->max_allocations = ( available / unit_size ) * CHAR_BIT * sizeof( fsm_t );

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
            get_ptr( header->allocs ),
            ( unsigned char ) _ZERO_FILL_BYTE,
            ( header->object_size * header->max_allocations )
        );
    }

    return _check_canaries( header );
}

bool force_canary_check( context_t ctx )
{
    shalloc_header * header = NULL;

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
    if( unlikely( !check_context( ctx ) ) )
        return NULL;

    return &(mapped_control->headers[ctx]);
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
    c_ptr = get_ptr( header->loc_c_allocstart );

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
    c_ptr = get_ptr( header->loc_c_fsmstart );

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
    c_ptr = get_ptr( header->loc_c_fsmend );

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

__ref move_to_shared( void * pointer, size_t size )
{
    // Stub
    return get_null_ref();
}

void * move_to_local( __ref ref )
{
    // Stub
    return NULL;
}
