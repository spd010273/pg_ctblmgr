#include "xlog.h"

uint64_t lsn_to_offset( char * lsn )
{
    uint64_t offset = 0;
    int      len1   = 0;
    int      len2   = 0;
    uint32_t id     = 0;
    uint32_t off    = 0;

    if( lsn == NULL )
        return 0;

    // Validate format, LSN should be two HEX numbers seperated by a slash
    len1 = strspn( lsn, "0123456789abcdefABCDEF" );

    if( len1 < 1 || len1 > LSN_SEGMENT_LENGTH || lsn[len1] != '/' )
        return 0;

    len2 = strspn( lsn + len1 + 1, "0123456789abcdefABCDEF" );

    if( len2 < 1 || len2 > LSN_SEGMENT_LENGTH || lsn[len1 + 1 + len2] != '\0' )
        return 0;

    id  = ( uint32_t ) strtoul( lsn, NULL, 16 );
    off = ( uint32_t ) strtoul( lsn + len1 + 1, NULL, 16 );
    offset = (( uint64_t ) id << 32 ) | off;

    return offset;
}

char * offset_to_lsn( uint64_t offset )
{
    char *   result = NULL;
    uint32_t id     = 0;
    uint32_t off    = 0;

    id  = ( uint32_t ) ( offset >> 32 );
    off = ( uint32_t ) offset;

    // Max LSN size is XXXXXXXX/XXXXXXXX\0 (8 hex, slash, 8 hex, null term)
    result = ( char * ) calloc(
        LSN_TOTAL_LENGTH,
        sizeof( char )
    );

    if( result == NULL )
        return NULL;

    snprintf( result, LSN_TOTAL_LENGTH - 1, "%X/%X", id, off );
    return result;
}

uint32_t xid_in( char * xid )
{
    uint32_t out  = 0;
    int      len1 = 0;

    if( xid == NULL )
        return 0;

    len1 = strspn( xid, "0123456789" );

    if( len1 < 1 || len1 > 10 )
        return 0;

    out = strtoul( xid, NULL, 10 );

    return out;
}

char * xid_out( uint32_t xid )
{
    char * out  = NULL;
    int    size = 0;

    size = floor( log10( xid ) ) + 1;

    out = ( char * ) calloc(
        size,
        sizeof( char )
    );

    if( out == NULL )
        return NULL;

    snprintf( out, size, "%u", xid );

    out[size] = '\0';

    return out;
}
