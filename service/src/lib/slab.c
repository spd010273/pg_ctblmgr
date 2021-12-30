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

static __inline__ context_t get_ctx_by_id( const char * );
static bool check_shalloc_header( header_iter );
static __inline__ bool check_context( context_t ) __attribute__((always_inline));

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
            mapped_control->headers[i].allocs      = NULL;
            mapped_control->headers[i].n_allocs    = 0;
            mapped_control->headers[i].freelist    = NULL;
            mapped_control->headers[i].n_freelist  = 0;
            mapped_control->headers[i].self        = INVALID_CONTEXT;
            mapped_control->headers[i].locked      = false;
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
        mapped_control->headers[ret].object_size = object_size;
        mapped_control->headers[ret].n_allocs    = 0;
        mapped_control->headers[ret].n_freelist  = 0;
        mapped_control->headers[ret].self        = ret;
        mapped_control->headers[ret].locked      = false;
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

void sfree( context_t ctx, __ref pointer )
{
    shalloc_header * header = NULL;

    if( !check_context( ctx ) )
        return;

    header = &(mapped_control->headers[ctx]);

    return;
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
