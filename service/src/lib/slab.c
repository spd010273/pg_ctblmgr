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

static __inline__ __ref _shmalloc( context_t ctx, size_t size ) __attribute__((always_inline));
static __inline__ bool _init_slab( context_t, shalloc_header *, bool );
static __inline__ context_t get_ctx_by_id( const char * );
static bool check_shalloc_header( header_iter );
static __inline__ bool check_context( context_t ) __attribute__((always_inline));
static __inline__ bool _check_canaries( shalloc_header * ) __attribute__((always_inline));

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
            mapped_control->headers[i].magic           = ( uint64_t ) _SHALLOC_HEADER_MAGIC;
            mapped_control->headers[i].segment         = SEGMENT_HANDLE_INVALID;
            mapped_control->headers[i].object_size     = 0;
            mapped_control->headers[i].allocs          = get_null_ref();
            mapped_control->headers[i].n_allocs        = 0;
            mapped_control->headers[i].freelist        = get_null_ref();
            mapped_control->headers[i].n_freelist      = 0;
            mapped_control->headers[i].self            = INVALID_CONTEXT;
            mapped_control->headers[i].locked          = false;

            mapped_control->headers[i].c_allocstart        = ( canary_t ) random();
            mapped_control->headers[i].c_allocend          = ( canary_t ) random();
            mapped_control->headers[i].c_freeliststart     = ( canary_t ) random();
            mapped_control->headers[i].c_freelistend       = ( canary_t ) random();
            mapped_control->headers[i].loc_c_allocstart    = get_null_ref();
            mapped_control->headers[i].loc_c_allocend      = get_null_ref();
            mapped_control->headers[i].loc_c_freeliststart = get_null_ref();
            mapped_control->headers[i].loc_c_freelistend   = get_null_ref();

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
        // handle setup of allocs[] and freelist[] later
        mapped_control->headers[ret].object_size     = object_size;
        mapped_control->headers[ret].n_allocs        = 0;
        mapped_control->headers[ret].n_freelist      = 0;
        mapped_control->headers[ret].self            = ret;
        mapped_control->headers[ret].locked          = false;
        mapped_control->headers[ret].max_allocations = 0;
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
    __ref            retref = {0};

    if( !check_context( ctx ) )
        return get_null_ref();

    header = &(mapped_control->headers[ctx]);

    return retref;
}

__ref smalloc( context_t ctx, size_t size )
{
    shalloc_header * header = NULL;
    __ref            retref = {0};

    retref = get_null_ref();

    if( !check_context( ctx ) )
        return get_null_ref();

    header = &(mapped_control->headers[ctx]);

    return retref;
}

__ref srealloc( context_t ctx, __ref oldref, size_t size )
{
    shalloc_header * header = NULL;
    __ref            retref = {0};

    if( !check_context( ctx ) )
        return get_null_ref();

    header = &(mapped_control->headers[ctx]);

    return retref;
}

void sfree( context_t ctx, __ref pointer )
{
    shalloc_header * header = NULL;

    if( !check_context( ctx ) )
        return;

    header = &(mapped_control->headers[ctx]);

    return;
}

static __inline__ __ref _shmalloc( context_t ctx, size_t size )
{
    shalloc_header * header = NULL;
    __ref            retref = {0};

    header = &(mapped_control->headers[ctx]);

    // Check to see if this context has ever been allocated.
    if(
            header->segment == SEGMENT_HANDLE_INVALID
         && header->max_allocations == 0
      )
    {
        if( !_init_slab( ctx, header, false ) )
            return get_null_ref();
    }

    return retref;
}

static __inline__ bool _init_slab( context_t ctx, shalloc_header * header, bool zero_fill )
{
    shm_handle handle          = SEGMENT_HANDLE_INVALID;
    void *     mapped_address  = NULL; 
    void *     alloc           = NULL;
    void *     freelist        = NULL;
    void *     c_allocstart    = NULL;
    void *     c_allocend      = NULL;
    void *     c_freeliststart = NULL;
    void *     c_freelistend   = NULL;
    void *     temp            = NULL;
    __ref      tempref         = {0};
    uint64_t   i               = 0;
    bool       canary_check    = false;

    if( ctx == INVALID_CONTEXT || header == NULL )
        return false;

    // Don't want to trash an initialized segment
    if( header->segment != SEGMENT_HANDLE_INVALID )
        return false;

    mapped_address = new_segment(
        SLAB_DEFAULT_ALLOCATION * header->object_size
    );

    if( mapped_address == NULL )
        return false;

    handle = get_handle_from_ptr( mapped_address );

    header->segment         = handle;
    header->max_allocations = ( get_segment_size( handle ) - ( 4 * sizeof( canary_t ) ) )
                            / ( header->object_size + sizeof( __ref ) );
    header->n_freelist      = header->max_allocations;
    header->n_allocs        = 0;
    header->c_allocstart    = ( canary_t ) random();
    header->c_allocend      = ( canary_t ) random();
    header->c_freeliststart = ( canary_t ) random();
    header->c_freelistend   = ( canary_t ) random();

    // Layout setup - we'll calculate locally for readability, then convert to __ref
    c_allocstart    = mapped_address;
    alloc           = ( void * ) _PTR_ADD_OFFSET( c_allocstart, sizeof( canary_t ) );
    c_allocend      = ( void * ) _PTR_ADD_OFFSET( alloc, ( header->object_size * header->max_allocations ) );
    c_freeliststart = ( void * ) _PTR_ADD_OFFSET( c_allocend, sizeof( canary_t ) );
    freelist        = ( void * ) _PTR_ADD_OFFSET( c_freeliststart, sizeof( canary_t ) );
    c_freelistend   = ( void * ) _PTR_ADD_OFFSET( freelist, ( sizeof( __ref ) * header->max_allocations ) );

    // Write out canaries
    *( ( canary_t * ) c_allocstart )    = header->c_allocstart;
    *( ( canary_t * ) c_allocend )      = header->c_allocend;
    *( ( canary_t * ) c_freeliststart ) = header->c_freeliststart;
    *( ( canary_t * ) c_freelistend )   = header->c_freelistend;

    // Initialize freelist to point to every alloc element
    for( i = 0; i < header->n_freelist; i++ )
    {
        // Get location of alloc element
        temp = ( void * ) ( ( uint8_t * ) alloc + ( i * header->object_size ) );
        tempref = get_ref( temp );

        // Get location of freelist element to write to
        temp = ( void * ) ( ( uint8_t * ) freelist + ( i * sizeof( __ref ) ) );
        *( ( __ref * ) temp ) = tempref;
    }

    header->allocs              = get_ref( alloc );
    header->freelist            = get_ref( freelist );
    header->loc_c_allocstart    = get_ref( c_allocstart );
    header->loc_c_allocend      = get_ref( c_allocend );
    header->loc_c_freeliststart = get_ref( c_freeliststart );
    header->loc_c_freelistend   = get_ref( c_freelistend );

    if(
           ref_is_null( header->allocs )
        || ref_is_null( header->freelist )
      )
    {
        return false;
    }

    if( zero_fill == true )
    {
        memset(
            get_ptr( header->allocs ),
            ( unsigned char ) _ZERO_FILL_BYTE,
            ( header->object_size * header->n_freelist )
        );
    }

    canary_check = _check_canaries( header );

#ifdef _FORCE_SIGSEGV_ON_CANARY_FAILURE
    if( !canary_check )
        *( ( int * ) 0 ) = 42;
#endif // _FORCE_SIGSEGV_ON_CANARY_FAILURE

    return canary_check;
}

static __inline__ bool _check_canaries( shalloc_header * header )
{
    canary_t * c_ptr = NULL;

    if( header == NULL )
        return false;
    // Check allocstart canary
    c_ptr = get_ptr( header->c_allocstart );

    if( c_ptr == NULL )
        return false;
    if( header->c_allocstart != *c_ptr )
        return false;

    // Check allocend canary
    c_ptr = get_ptr( header->c_allocend );

    if( c_ptr == NULL )
        return false;
    if( header->c_allocend != *c_ptr )
        return false;

    // Check freeliststart canary
    c_ptr = get_ptr( header->c_freeliststart );

    if( c_ptr == NULL )
        return false;
    if( header->c_freeliststart != *c_ptr )
        return false;

    c_ptr = get_ptr( header->c_freelistend );

    if( c_ptr == NULL )
        return false;
    if( header->c_freelistend != *c_ptr )
        return false;
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
            "Bad magic: %p\n",
            ( void * ) mapped_control->magic
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
            "Bad header magic %p at index %lu\n",
            ( void * ) header->magic,
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
        return false;
    if( unlikely( mapped_control == NULL ) ) // check that we're mapped
        return false;
    if( unlikely( ctx >= ( context_t ) _SHALLOC_MAX_SLABS ) ) // Ensure no overrun
        return false;
    if( unlikely( mapped_control->headers[ctx].magic != _SHALLOC_HEADER_MAGIC ) ) // See if it's reachable
        return false;
    if( unlikely( mapped_control->headers[ctx].self != ctx ) )
        return false;

    return true;
}
