/*--------------------------------------------------------------------------
 *
 * changeset.c
 *     Internal structure storing WAL
 *
 * This structure stores differential records related to changes made in
 * the database we're replicating from.
 *
 * Copyright (c) 2019-2022, MerchLogix, Inc.
 *
 * IDENTIFICATION
 *          service/src/changeset.c
 *--------------------------------------------------------------------------
 */

#include "changeset.h"

static changeset_ref_t _new_changeset( void );
static inline char * _json_token_to_string( char *, jsmntok_t *, jsmntype_t );
static void _jsmn_dump( jsmntok_t * );
static void _parse_data_record(
    char *,         // json string
    jsmntok_t *,    // token array
    uint32_t,       // num_tokens
    uint32_t,       // start index
    changeset_array_ref_t *,// target array
    uint16_t *, // array len
    changeset_array_ref_t * // columns array (population of this is one-shotted)
);

static context_t changeset_context        = INVALID_CONTEXT;
static context_t changeset_string_context = INVALID_CONTEXT;
static context_t changeset_array_context  = INVALID_CONTEXT;

bool initialize_changeset_context( void )
{
    context_t _changeset_context        = INVALID_CONTEXT;
    context_t _changeset_string_context = INVALID_CONTEXT;
    context_t _changeset_array_context  = INVALID_CONTEXT;

    _changeset_context        = new_slab( CHANGESET_CONTEXT_NAME, sizeof( struct changeset ) );
    _changeset_string_context = new_slab( CHANGESET_STRING_CONTEXT_NAME, sizeof( char ) );
    _changeset_array_context  = new_slab( CHANGESET_ARRAY_CONTEXT_NAME, sizeof( changeset_string_ref_t ) );

    changeset_context         = _changeset_context;
    changeset_array_context   = _changeset_array_context;
    changeset_string_context  = _changeset_string_context;

    if(
          changeset_context        == INVALID_CONTEXT
       || changeset_array_context  == INVALID_CONTEXT
       || changeset_string_context == INVALID_CONTEXT
      )
    {
        // error
        return false;
    }

    return true;
}

context_t get_changeset_array_context( void )
{
    return changeset_array_context;
}

context_t get_changeset_context( void )
{
    return changeset_context;
}

context_t get_changeset_string_context( void )
{
    return changeset_string_context;
}

changeset_ref_t json_to_changeset(
    char *               json,
    pg_ctblmgr_wal_level wal_level
)
{
    jsmntok_t *              type_val       = NULL;
    jsmntok_t *              xid_val        = NULL;
    jsmntok_t *              time_val       = NULL;
    jsmntok_t *              schema_val     = NULL;
    jsmntok_t *              table_val      = NULL;
    jsmntok_t *              keys_val       = NULL;
    jsmntok_t *              data_val       = NULL;
    jsmntok_t *              key            = NULL;
    jsmntok_t *              val            = NULL;
    jsmntok_t *              tokens         = NULL;
    char *                   key_string     = NULL;
    changeset_ref_t          changeset      = NULLREF;
    changeset_string_ref_t * keys           = NULL;
    changeset_string_ref_t * vals           = NULL;
    char *                   keys_elem      = NULL;
    char *                   vals_elem      = NULL;
    char *                   schema_name    = NULL;
    char *                   table_name     = NULL;
    struct changeset *       cs             = NULL;
    jsmn_parser              parser         = {0};
    struct tm                breakout       = {0};
    int32_t                  result         = 0;
    uint32_t                 keys_index     = 0;
    uint32_t                 data_index     = 0;
    uint32_t                 n              = 0;
    uint32_t                 i              = 0;
    uint32_t                 token_count    = 0;
    uint32_t                 key_len        = 0;
    uint32_t                 size           = 0;
    int32_t                  milliseconds   = 0;
    int32_t                  tz_offset      = 0;
    bool                     done           = false; // Initial parse oneshot
    uint16_t                 column_canary  = 0;
    bool                     in_subobject   = false;
    uint32_t                 subobject_end  = 0;
    jsmntok_t *              temp_val       = NULL;

    n = JSON_TOKENS;

    if( json == NULL )
        return NULLREF;

    jsmn_init( &parser );

    tokens = ( jsmntok_t * ) calloc(
        n,
        sizeof( jsmntok_t )
    );

    if( tokens == NULL )
        return NULLREF;

    result = jsmn_parse( &parser, json, strlen( json ), tokens, n );

    while( result == JSMN_ERROR_NOMEM )
    {
        n = n + JSON_TOKENS;

        tokens = realloc( tokens, sizeof( jsmntok_t ) * n );

        if( tokens == NULL )
            return NULLREF;

        result = jsmn_parse( &parser, json, strlen( json ), tokens, n );
    }

    if( result == JSMN_ERROR_INVAL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to parse JSON string: invalid or corrupted string"
        );
        free( tokens );
        return NULLREF;
    }

    if( result == JSMN_ERROR_PART )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to parse JSON string: invalid or partial string received"
        );
        free( tokens );
        return NULLREF;
    }

    token_count = result;

    // Sanity check the # of tokens returned vs allocated memory
    if( token_count > n )
        return NULLREF;

    if( tokens[0].type != JSMN_OBJECT )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Root element of JSON is not an object"
        );
        free( tokens );
        return NULLREF;
    }

    changeset = _new_changeset();
    cs = to_ptr( changeset_context, changeset );

    if( cs == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to allocate changeset"
        );
        rsfree( changeset_context, changeset );
        return NULLREF;
    }

    for( i = 1; i < token_count && !done; i += 2 )
    {
        key = &(tokens[i]);

        if( key->type != JSMN_STRING && key->type != JSMN_PRIMITIVE )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Expected key of type primitive or string at token "\
                "index %u, got:",
                i
            );
            _jsmn_dump( key );
            free( tokens );
            free_changeset( changeset );
            return NULLREF;
        }

        key_string = ( char * ) calloc(
            ( key->end - key->start ) + 1,
            sizeof( char )
        );

        if( key_string == NULL )
        {
            free( tokens );
            free_changeset( changeset );
            return NULLREF;
        }

        if( in_subobject && key->end > subobject_end )
        {
            in_subobject = false;
        }

        strncpy( key_string, json + key->start, key->end - key->start );
        key_string[key->end - key->start] = '\0';
        // Examine string from key->start to key->end (of size key->size)
        // and, given the wal-level, check against our expected keys and fill
        // in the changeset struct
        key_len = strlen( key_string );

        switch( wal_level )
        {
            case PGC_WAL_FULL:
                if( strncmp( key_string, "type", MIN( key_len, 4 ) ) == 0 )
                {
                    type_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "xid", MIN( key_len, 3 ) ) == 0 )
                {
                    xid_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "timestamp", MIN( key_len, 9 ) ) == 0 )
                {
                    time_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "schema_name", MIN( key_len, 11 ) ) == 0 )
                {
                    schema_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "table_name", MIN( key_len, 10 ) ) == 0 )
                {
                    table_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "key", MIN( key_len, 3 ) ) == 0 )
                {
                    keys_val      = &(tokens[i+1]);
                    keys_index    = i + 1;
                    in_subobject  = true;
                    subobject_end = keys_val->end;
                }
                else if( strncmp( key_string, "data", MIN( key_len, 4 ) ) == 0 )
                {
                    data_val      = &(tokens[i+1]);
                    data_index    = i + 1;
                    in_subobject  = true;
                    subobject_end = data_val->end;
                }
                else if(
                            type_val != NULL && xid_val != NULL
                         && time_val != NULL && schema_val != NULL
                         && table_val != NULL && keys_val != NULL
                         && data_val != NULL
                       )
                {
                    done = true;
                }
                break;
            case PGC_WAL_REDUCED:
                if( strncmp( key_string, "type", MIN( key_len, 4 ) ) == 0 )
                {
                    type_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "xid", MIN( key_len, 3 ) ) == 0 )
                {
                    xid_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "schema_name", MIN( key_len, 11 ) ) == 0 )
                {
                    schema_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "table_name", MIN( key_len, 10 ) ) == 0 )
                {
                    table_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "key", MIN( key_len, 3 ) ) == 0 )
                {
                    keys_val      = &(tokens[i+1]);
                    keys_index    = i + 1;
                    in_subobject  = true;
                    subobject_end = keys_val->end;
                }
                else if(
                           type_val   != NULL && xid_val   != NULL
                        && schema_val != NULL && table_val != NULL
                        && keys_val   != NULL
                       )
                {
                    done = true;
                }
                break;
            case PGC_WAL_MINIMAL:
                if( strncmp( key_string, "d", MIN( key_len, 1 ) ) == 0 )
                {
                    type_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "x", MIN( key_len, 1 ) ) == 0 )
                {
                    xid_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "s", MIN( key_len, 1 ) ) == 0 )
                {
                    schema_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "t", MIN( key_len, 1 ) ) == 0 )
                {
                    table_val = &(tokens[i+1]);
                }
                else if( strncmp( key_string, "key", MIN( key_len, 3 ) ) == 0 )
                {
                    keys_val      = &(tokens[i+1]);
                    keys_index    = i + 1;
                    in_subobject  = true;
                    subobject_end = keys_val->end;
                }
                else if(
                            type_val != NULL && xid_val != NULL
                         && schema_val != NULL && table_val != NULL
                         && keys_val != NULL
                       )
                {
                    done = true;
                }
                break;
            default:
                _log(
                    LOG_LEVEL_ERROR,
                    "invalid WAL_LEVEL specified"
                );
                break;
        }

        free( key_string );
    }

    if( type_val != NULL )
    {
        key_string = _json_token_to_string( json, type_val, JSMN_STRING );

        if( key_string == NULL )
        {
            free( tokens );
            _log(
                LOG_LEVEL_ERROR,
                "Failed to parse DML type from json token"
            );
            free_changeset( changeset );
            return NULLREF;
        }

        key_len = strlen( key_string );

        if( strncmp( key_string, "I", 1 ) == 0 )
        {
            cs->type = PGC_DML_INSERT;
        }
        else if( strncmp( key_string, "D", 1 ) == 0 )
        {
            cs->type = PGC_DML_DELETE;
        }
        else if( strncmp( key_string, "U", 1 ) == 0 )
        {
            cs->type = PGC_DML_UPDATE;
        }
        else if( strncmp( key_string, "B", 1 ) == 0 )
        {
            cs->type = PGC_DML_BEGIN;
        }
        else if( strncmp( key_string, "C", 1 ) == 0 )
        {
            cs->type = PGC_DML_COMMIT;
        }
        else if( strncmp( key_string, "R", 1 ) == 0 )
        {
            cs->type = PGC_DML_ROLLBACK;
        }
        else
        {
            _log(
                LOG_LEVEL_ERROR,
                "Unknown DML type '%s' (%p)",
                key_string,
                key_string
            );
        }

        free( key_string );
    }

    if( xid_val != NULL )
    {
        key_string = _json_token_to_string( json, xid_val, JSMN_STRING );

        if( key_string == NULL )
        {
            _jsmn_dump( xid_val );
            free( tokens );
            free_changeset( changeset );
            _log(
                LOG_LEVEL_ERROR,
                "Failed to parse XID value from json token"
            );
            return NULLREF;
        }
        errno = 0;
        cs->xid = strtoul(
            key_string,
            NULL,
            10
        );

        if( errno != 0 )
        {
            // Oopsie poopsie!
        }

        free( key_string );
    }

    if( time_val != NULL )
    {
        key_string = _json_token_to_string( json, time_val, JSMN_STRING );

        if( key_string == NULL )
        {
            free( tokens );
            free_changeset( changeset );
            _log(
                LOG_LEVEL_ERROR,
                "Failed to parse timestamp value from json token"
            );
            return NULLREF;
        }

        if(
            sscanf(
                key_string,
                "%4d-%2d-%2d %2d:%2d:%2d.%d-%2d",
                &(breakout.tm_year),
                &(breakout.tm_mon),
                &(breakout.tm_mday),
                &(breakout.tm_hour),
                &(breakout.tm_min),
                &(breakout.tm_sec),
                &milliseconds,
                &tz_offset
            ) >= 7
          )
        {
            timezone = tz_offset;
            breakout.tm_year -= 1900;
            breakout.tm_mon  -= 1;
            cs->timestamp     = mktime( &breakout );

            if( cs->timestamp == (time_t) - 1 )
            {
                _log(
                    LOG_LEVEL_ERROR,
                    "Failed to convert ISO timestamp from WAL"
                );
                free( key_string );
                free( tokens );
                free_changeset( changeset );
                return NULLREF;
            }
        }
        else
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to convert ISO timestamp from WAL"
            );
            free( key_string );
            free( tokens );
            free_changeset( changeset );
            return NULLREF;
        }

        free( key_string );
    }

    if( schema_val != NULL )
    {
        key_string = _json_token_to_string( json, schema_val, JSMN_STRING );

        if( key_string == NULL )
        {
            free( tokens );
            free_changeset( changeset );
            _log(
                LOG_LEVEL_ERROR,
                "Failed to parse schema from json token"
            );
            return NULLREF;
        }

        cs->schema_name = ( changeset_string_ref_t ) rsmalloc(
            changeset_string_context,
            strlen( key_string ) + 1
        );

        schema_name = ( char * ) to_ptr( changeset_string_context, cs->schema_name );

        if( schema_name == NULL )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to allocate string for schema_name"
            );
            free_changeset( changeset );
            return NULLREF;
        }

        strncpy(
            schema_name,
            key_string,
            strlen( key_string )
        );
        schema_name[strlen( key_string )] = '\0';
    }

    if( table_val != NULL )
    {
        key_string = _json_token_to_string( json, table_val, JSMN_STRING );

        if( key_string == NULL )
        {
            free( tokens );
            free_changeset( changeset );
            _log(
                LOG_LEVEL_ERROR,
                "Failed to parse table from json token"
            );
            return NULLREF;
        }

        cs->table_name = ( changeset_string_ref_t ) rsmalloc(
            changeset_string_context,
            strlen( key_string ) + 1
        );

        table_name = ( char * ) to_ptr( changeset_string_context, cs->table_name );

        if( table_name == NULL )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Failed to allocate string for table_name"
            );
            free_changeset( changeset );
            return NULLREF;
        }

        strncpy(
            table_name,
            key_string,
            strlen( key_string )
        );
        schema_name[strlen( key_string )] = '\0';
    }

    // parse out subobjects using the saved token and index into tokens[]
    if( keys_val != NULL )
    {
        if( keys_val->type != JSMN_OBJECT )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Expected a JSON object for keys."
            );
            free( tokens );
            free_changeset( changeset );
            return NULLREF;
        }

        for( i = keys_index + 1; i < token_count; i += 2 )
        {
            // Iterate pairwise over k-v set
            key = &(tokens[i]);
            val = &(tokens[i + 1]);

            if( key->type != JSMN_STRING )
            {
                // We're expecting a column name here.
                _log(
                    LOG_LEVEL_ERROR,
                    "Unexpected JSON subtype in keys structure for key: "
                    "expected column name, got %s, literal\n'%s'\n at index %u\nnumkeys %u",
                    key->type == JSMN_OBJECT
                        ? "OBJECT"
                        : key->type == JSMN_ARRAY
                        ? "ARRAY"
                        : key->type == JSMN_PRIMITIVE
                        ? "PRIMITIVE"
                        : key->type == JSMN_UNDEFINED
                        ? "UNDEF"
                        : "UNKNOWN",
                        json + key->start,
                        i,
                        cs->num_keys
                );

                free( tokens );
                free_changeset( changeset );
                return NULLREF;
            }

            if(
                  val->type != JSMN_STRING
               && val->type != JSMN_PRIMITIVE
               && val->type != JSMN_UNDEFINED
              )
            {
                // unexpected value
                _log(
                    LOG_LEVEL_DEBUG,
                    "Unexpected JSON subtype in keys structure for value: "
                    "expected a string, primitive, or null, got %s, literal\n'%s' at index %u\nnumkeys: %u",
                    val->type == JSMN_OBJECT
                        ? "OBJECT"
                        : val->type == JSMN_ARRAY
                        ? "ARRAY"
                        : val->type == JSMN_PRIMITIVE
                        ? "PRIMITIVE"
                        : val->type == JSMN_UNDEFINED
                        ? "UNDEF"
                        : val->type == JSMN_STRING
                        ? "STRING"
                        : "UNKNOWN",
                        json + val->start,
                        i + 1,
                        cs->num_keys
                );
                free( tokens );
                free_changeset( changeset );
                return NULLREF;
            }

            if( cs->num_keys == 0 )
            {
                cs->keys = ( changeset_array_ref_t ) rsmalloc(
                    changeset_array_context,
                    sizeof( changeset_string_ref_t )
                );
                cs->vals = ( changeset_array_ref_t ) rsmalloc(
                    changeset_array_context,
                    sizeof( changeset_string_ref_t )
                );

                if( cs->keys == NULLREF || cs->vals == NULLREF )
                {
                    free( tokens );
                    _log(
                        LOG_LEVEL_ERROR,
                        "Failed to perform initial alloc for changeset kv"
                    );
                    free_changeset( changeset );
                    return NULLREF;
                }
            }
            else
            {
                cs->keys = ( changeset_array_ref_t ) rsrealloc(
                    changeset_array_context,
                    cs->keys,
                    ( cs->num_keys + 1 ) * sizeof( changeset_string_ref_t )
                );

                cs->vals = ( changeset_array_ref_t ) rsrealloc(
                    changeset_array_context,
                    cs->vals,
                    ( cs->num_keys + 1 ) * sizeof( changeset_string_ref_t )
                );

                if( cs->keys == NULLREF || cs->vals == NULLREF )
                {
                    free_changeset( changeset );
                    free( tokens );
                    _log(
                        LOG_LEVEL_ERROR,
                        "Failed to perform incremental alloc for cs kv"
                    );
                    return NULLREF;
                }
            }

            keys = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->keys );
            vals = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->vals );

            if( keys == NULL || vals == NULL )
            {
                _log(
                    LOG_LEVEL_ERROR,
                    "Failed to dereferece keys or values array"
                );
                free_changeset( changeset );
                free( tokens );
                return NULLREF;
            }

            keys[cs->num_keys] = ( changeset_string_ref_t ) rsmalloc(
                changeset_string_context,
                sizeof( char ) * ( key->end - key->start + 1 )
            );

            vals[cs->num_keys] = ( changeset_string_ref_t ) rsmalloc(
                changeset_string_context,
                sizeof( char ) * ( val->end - val->start + 1 )
            );

            if(
                    keys[cs->num_keys] == NULLREF
                 || vals[cs->num_keys] == NULLREF
              )
            {
                free_changeset( changeset );
                free( tokens );
                return NULLREF;
            }

            size = key->end - key->start;
            keys_elem = ( char * ) to_ptr( changeset_string_context, keys[cs->num_keys] );

            if( keys_elem == NULL )
            {
                _log(
                    LOG_LEVEL_ERROR,
                    "Failed to dereference keys element"
                );
                free_changeset( changeset );
                free( tokens );
                return NULLREF;
            }

            strncpy(
                keys_elem,
                json + key->start,
                size
            );

            keys_elem[size] = '\0';

            size = val->end - val->start;
            vals_elem = ( char * ) to_ptr( changeset_string_context, vals[cs->num_keys] );

            if( vals_elem == NULL )
            {
                _log(
                    LOG_LEVEL_ERROR,
                    "Failed to dereference vals element"
                );

                free_changeset( changeset );
                free( tokens );
                return NULLREF;
            }

            strncpy(
                vals_elem,
                json + val->start,
                size
            );

            vals_elem[size] = '\0';
            cs->num_keys++;

            if( val->end >= keys_val->end - 1 )
            {
                break;
            }
        }
    }

    if( data_val != NULL )
    {
        // Data index is pointing to the OBJECT opening token, the first string
        // of the key/value pair will be +1 offset, and will be either new/old
        // Parse out new, iff exists
        data_val   = &(tokens[data_index + 1]);
        temp_val   = &(tokens[data_index + 1]);
        key_string = _json_token_to_string(
            json,
            temp_val,
            JSMN_STRING
        );

        if( key_string == NULL )
        {
            _log( LOG_LEVEL_ERROR, "Error parsing data subobject string '%s'", json + temp_val->start );
            free( tokens );
            free_changeset( changeset );
            return NULLREF;
        }

        key_len = strlen( key_string );

        if( strncmp( key_string, "new", MIN( key_len, 3 ) ) == 0 )
        {
            _parse_data_record(
                json,
                tokens,
                token_count,
                data_index + 2,
                &(cs->new_vals),
                &(cs->num_columns),
                &(cs->columns)
            );

            if( cs->new_vals == NULLREF )
            {
                _log( LOG_LEVEL_ERROR, "failed to parse new vals from data object" );
                free( tokens );
                free_changeset( changeset );
                return NULLREF;
            }
        }
        else if( strncmp( key_string, "old", MIN( key_len, 3 ) ) == 0 )
        {
            _parse_data_record(
                json,
                tokens,
                token_count,
                data_index + 2,
                &(cs->old_vals),
                &(cs->num_columns),
                &(cs->columns)
            );

            if( cs->old_vals == NULLREF )
            {
                _log( LOG_LEVEL_ERROR, "Failed to parse old vals from data object" );
                free( tokens );
                free_changeset( changeset );
                return NULLREF;
            }
        }
        else
        {
            _log( LOG_LEVEL_ERROR, "unexpected record key in data subobject %s", key_string );
            free( tokens );
            free_changeset( changeset );
            return NULLREF;
        }

        // Hijack data_val to locate the next subobject
        temp_val      = &(tokens[data_index+2]);
        subobject_end = temp_val->end;
        data_val      = &(tokens[data_index]);

        while( temp_val->start < subobject_end && data_index < token_count )
        {
            temp_val = &(tokens[data_index]);
            data_index++;
        }

        if( temp_val->end >= data_val->end - 2 )
        {
            // No other record
            free( tokens );
            return changeset;
        }

        key_string = _json_token_to_string(
            json,
            temp_val,
            JSMN_STRING
        );

        if( key_string == NULL )
        {
            _log( LOG_LEVEL_ERROR, "Error parsing data subobject string '%s'", json + temp_val->start );
            free( tokens );
            free_changeset( changeset );
            return NULLREF;
        }

        key_len = strlen( key_string );

        if( strncmp( key_string, "new", MIN( key_len, 3 ) ) == 0 && cs->new_vals == NULLREF )
        {
            _parse_data_record(
                json,
                tokens,
                token_count,
                data_index,
                &(cs->new_vals),
                &(column_canary),
                NULL
            );

            if( cs->new_vals == NULLREF )
            {
                _log( LOG_LEVEL_ERROR, "failed to parse new vals from data object" );
                free( tokens );
                free_changeset( changeset );
                return NULLREF;
            }
        }
        else if( strncmp( key_string, "old", MIN( key_len, 3 ) ) == 0 && cs->old_vals == NULLREF )
        {
            _parse_data_record(
                json,
                tokens,
                token_count,
                data_index,
                &(cs->old_vals),
                &(column_canary),
                NULL
            );

            if( cs->old_vals == NULLREF )
            {
                _log( LOG_LEVEL_ERROR, "Failed to parse old vals from data object" );
                free( tokens );
                free_changeset( changeset );
                return NULLREF;
            }
        }
        else
        {
            _log( LOG_LEVEL_ERROR, "unexpected record key in data subobject %s", key_string );
            free( tokens );
            free_changeset( changeset );
            return NULLREF;
        }

        if( column_canary > 0 && column_canary != cs->num_columns )
        {
            // These records should be of the same length
            _log( LOG_LEVEL_ERROR, "Mismatch in recordlengths between new and old" );
            free( tokens );
            free_changeset( changeset );
            return NULLREF;
        }
    }

    free( tokens );
    return changeset;
}

static void _parse_data_record(
    char *                  json,
    jsmntok_t *             tokens,
    uint32_t                num_tokens,
    uint32_t                index,
    changeset_array_ref_t * val_array,
    uint16_t *              num_elements,
    changeset_array_ref_t * columns
)
{
    uint32_t                 obj_end   = 0;
    uint32_t                 i         = 0;
    uint32_t                 e         = 0;
    jsmntok_t *              val       = NULL;
    jsmntok_t *              key       = NULL;
    jsmntok_t *              temp      = NULL;
    changeset_string_ref_t * vals      = NULL;
    changeset_string_ref_t * cols      = NULL;
    char *                   vals_elem = NULL;
    char *                   cols_elem = NULL;

    if(
            json == NULL
         || tokens == NULL
         || num_tokens == 0
         || val_array == NULL
         || index >= num_tokens
      )
    {
        _log( LOG_LEVEL_ERROR, "Invalid input to parse_data_record" );
        return;
    }

    if( (&(tokens[index]))->type != JSMN_OBJECT )
    {
        _log( LOG_LEVEL_ERROR, "Starting point in data parse must be the object" );
        return;
    }

    obj_end = (&(tokens[index]))->end;

    for( i = index + 1; i < num_tokens; i += 2 )
    {
        key = &(tokens[i]);
        val = &(tokens[i + 1]);

        if( key == NULL || val == NULL )
        {
            return;
        }

        if( key->type != JSMN_STRING )
        {
            _log( LOG_LEVEL_ERROR, "Unexpected key in data structure at token index %u, position %u", i, key->start );
            return;
        }

        if( key->end >= obj_end - 1 )
        {
            break;
        }

        if( *num_elements == 0 )
        {
            *val_array = ( changeset_array_ref_t ) rsmalloc( changeset_array_context, sizeof( char * ) );

            if( columns != NULL )
            {
                *columns = ( changeset_array_ref_t ) rsmalloc( changeset_array_context, sizeof( char * ) );
            }
        }
        else
        {
            *val_array = ( changeset_array_ref_t ) rsrealloc(
                changeset_array_context,
                ( ref_t ) *val_array,
                sizeof( char * ) * ( (*num_elements) + 1 )
            );

            if( columns != NULL )
            {
                *columns = ( changeset_array_ref_t ) rsrealloc(
                    changeset_array_context,
                    ( ref_t ) *columns,
                    sizeof( char * ) * ( (*num_elements) + 1 )
                );
            }
        }

        if( (*val_array) == NULLREF || ( columns != NULL && (*columns) == NULLREF ) )
        {
            _log( LOG_LEVEL_ERROR, "Failed to allocate memory" );
            return;
        }

        vals = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, (*val_array) );

        if( columns != NULL )
            cols = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, (*columns) );

        if( vals == NULL || ( columns != NULL && cols == NULL ) )
        {
            _log( LOG_LEVEL_ERROR, "Failed to dereference vals_array or columns" );
            return;
        }

        vals[(*num_elements)] = ( changeset_string_ref_t ) rsmalloc(
            changeset_string_context,
            ( val->end - val->start + 1 ) * sizeof( char )
        );

        vals_elem = ( char * ) to_ptr( changeset_string_context, vals[(*num_elements)] );

        if( vals_elem == NULL )
        {
            _log( LOG_LEVEL_ERROR, "Failed to allocate memory" );
            // canary check will catch and free changeset
            return;
        }

        if( columns != NULL )
        {
            cols[(*num_elements)] = ( changeset_string_ref_t ) rsmalloc(
                changeset_string_context,
                ( key->end - key->start + 1 ) * sizeof( char )
            );

            cols_elem = ( char * ) to_ptr( changeset_string_context, cols[(*num_elements)] );
            if( cols_elem == NULL )
            {
                _log( LOG_LEVEL_ERROR, "Failed to allocate memory" );
                return;
            }
        }

        strncpy(
            vals_elem,
            json + val->start,
            val->end - val->start
        );
        vals_elem[val->end - val->start] = '\0';

        if( columns != NULL )
        {
            strncpy(
                cols_elem,
                json + key->start,
                key->end - key->start
            );
            cols_elem[key->end - key->start] = '\0';
        }

        (*num_elements)++;

        if( key->type == JSMN_OBJECT || key->type == JSMN_ARRAY )
        {
            temp = key;
            while( e < temp->end && e < num_tokens )
            {
                e++;
            }

            i += e;
        }
    }

    return;
}

static changeset_ref_t _new_changeset( void )
{
    changeset_ref_t changeset = NULLREF;
    struct changeset * cs = NULL;
    changeset = ( changeset_ref_t ) rsmalloc(
        changeset_context,
        sizeof( struct changeset )
    );

    cs = ( struct changeset * ) to_ptr( changeset_context, changeset );

    if( cs == NULL )
        return NULLREF;

    cs->keys        = ( changeset_array_ref_t ) NULLREF;
    cs->vals        = ( changeset_array_ref_t ) NULLREF;
    cs->new_vals    = ( changeset_array_ref_t ) NULLREF;
    cs->old_vals    = ( changeset_array_ref_t ) NULLREF;
    cs->num_keys    = 0;
    cs->schema_name = ( changeset_string_ref_t ) NULLREF;
    cs->table_name  = ( changeset_string_ref_t ) NULLREF;
    cs->columns     = ( changeset_array_ref_t ) NULLREF;
    cs->num_columns = 0;
    cs->timestamp   = 0;
    cs->type        = PGC_DML_UNINITIALIZED;
    cs->xid         = 0;
    return changeset;
}

static inline char * _json_token_to_string(
    char *      json,
    jsmntok_t * token,
    jsmntype_t  type
)
{
    char * result = NULL;

    if( token == NULL )
        return NULL;

    if( token->type != type )
        return NULL;

    result = ( char * ) calloc(
        ( token->end - token->start ) + 1,
        sizeof( char )
    );

    if( result == NULL )
        return NULL;

    strncpy( result, json + token->start, token->end - token->start );
    result[token->end - token->start] = '\0';
    return result;
}

static void _jsmn_dump( jsmntok_t * token )
{
    if( token == NULL )
        return;

    _log(
        LOG_LEVEL_DEBUG,
        "Token %p\n type: %s\n start: %d\n end: %d\n size: %d\n",
        token,
        token->type == JSMN_UNDEFINED ? "UNDEFINED" :
        token->type == JSMN_OBJECT ? "OBJECT" :
        token->type == JSMN_ARRAY ? "ARRAY" :
        token->type == JSMN_STRING ? "STRING" :
        token->type == JSMN_PRIMITIVE ? "PRIMITIVE" : "N/A",
        token->start,
        token->end,
        token->size
    );

    return;
}

void free_changeset( changeset_ref_t changeset )
{
    struct changeset *       cs   = NULL;
    changeset_string_ref_t * keys = NULL;
    changeset_string_ref_t * vals = NULL;
    changeset_string_ref_t * cols = NULL;
    changeset_string_ref_t * olds = NULL;
    changeset_string_ref_t * news = NULL;
    uint16_t                 i    = 0;

    if( changeset == NULLREF )
        return;

    cs = ( struct changeset * ) to_ptr( changeset_context, changeset );

    if( cs == NULL )
        return;

    if( cs->num_keys > 0 )
    {
        keys = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->keys );
        vals = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->vals );

        if( keys != NULL || vals != NULL )
        {
            for( i = 0; i < cs->num_keys; i++ )
            {
                if( keys != NULL && keys[i] != NULLREF )
                    rsfree( changeset_string_context, keys[i] );
                if( vals != NULL && vals[i] != NULLREF )
                    rsfree( changeset_string_context, vals[i] );
            }
        }
    }

    if( cs->keys != NULLREF )
        rsfree( changeset_array_context, cs->keys );
    if( cs->vals != NULLREF )
        rsfree( changeset_array_context, cs->vals );

    cs->keys = NULLREF;
    cs->vals = NULLREF;
    cs->num_keys = 0;

    if( cs->num_columns > 0 )
    {
        cols = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->columns );
        news = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->new_vals );
        olds = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->old_vals );

        if( cols != NULL && ( news != NULL || olds != NULL ) )
        {
            for( i = 0; i < cs->num_columns; i++ )
            {
                if( cols != NULL && cols[i] != NULLREF )
                    rsfree( changeset_string_context, cols[i] );

                if( olds != NULL && olds[i] != NULLREF )
                    rsfree( changeset_string_context, olds[i] );

                if( news != NULL && news[i] != NULLREF )
                    rsfree( changeset_string_context, news[i] );
            }
        }
    }

    if( cs->columns != NULLREF )
        rsfree( changeset_array_context, cs->columns );

    if( cs->old_vals != NULLREF )
        rsfree( changeset_array_context, cs->old_vals );

    if( cs->new_vals != NULLREF )
        rsfree( changeset_array_context, cs->new_vals );

    cs->columns     = NULLREF;
    cs->old_vals    = NULLREF;
    cs->new_vals    = NULLREF;
    cs->num_columns = 0;

    if( cs->schema_name != NULLREF )
        rsfree( changeset_string_context, cs->schema_name );

    if( cs->table_name != NULLREF )
        rsfree( changeset_string_context, cs->table_name );

    cs->schema_name = NULLREF;
    cs->table_name  = NULLREF;

    rsfree( changeset_context, changeset );
    return;
}

// XXX
void dump_changeset( changeset_ref_t changeset )
{
    changeset_array_ref_t * keys = NULL;
    changeset_array_ref_t * vals = NULL;
    changeset_array_ref_t * cols = NULL;
    changeset_array_ref_t * news = NULL;
    changeset_array_ref_t * olds = NULL;
    struct changeset *      cs   = NULL;
    uint32_t                i    = 0;
    char *                  a    = NULL;
    char *                  b    = NULL;
    char *                  c    = NULL;
    char *                  d    = NULL;
    char *                  e    = NULL;
    char *                  f    = NULL;
    uint32_t                a_sz = 0;
    uint32_t                b_sz = 0;
    uint32_t                c_sz = 0;
    char *                  lsn  = NULL;

    if( changeset == NULLREF )
        return;

    cs = ( struct changeset * ) to_ptr( changeset_context, changeset );
    _log( LOG_LEVEL_DEBUG, "Changeset %p", cs );

    if( cs == NULL )
        return;

    lsn = offset_to_lsn( cs->lsn );
    _log( LOG_LEVEL_DEBUG, "LSN: %lu (%s)", cs->lsn, lsn );
    free( lsn );
    _log( LOG_LEVEL_DEBUG, "num_keys: %u", cs->num_keys );
    if( cs->num_keys > 0 )
    {
        keys = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->keys );
        vals = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->vals );

        if( keys == NULL || vals == NULL )
        {
            _log( LOG_LEVEL_ERROR, "NULL deref on keys | vals" );
            return;
        }

        for( i = 0; i < cs->num_keys; i++ )
        {
            a = ( char * ) to_ptr( changeset_string_context, keys[i] );
            b = ( char * ) to_ptr( changeset_string_context, vals[i] );
            if( a != NULL )
                a_sz += strlen( a ) + 1;
            if( b != NULL )
                b_sz += strlen( b ) + 1;
        }

        a = ( char * ) calloc( a_sz, sizeof( char ) );
        b = ( char * ) calloc( b_sz, sizeof( char ) );
        c = ( char * ) to_ptr( changeset_array_context, keys[0] );
        d = ( char * ) to_ptr( changeset_array_context, vals[0] );

        if( c != NULL )
            strncpy( a, c, strlen( c ) );
        if( d != NULL )
            strncpy( b, c, strlen( c ) );

        for( i = 1; i < cs->num_keys; i++ )
        {
            c = ( char * ) to_ptr( changeset_array_context, keys[i] );
            d = ( char * ) to_ptr( changeset_array_context, vals[i] );
            if( c != NULL )
            {
                strncat( a, ",", 1 );
                strncat( a, c, strlen( c ) );
            }

            if( d != NULL )
            {
                strncat( b, ",", 1 );
                strncat( b, d, strlen( d ) );
            }
        }

        _log( LOG_LEVEL_DEBUG, "keys: '%s'", a );
        _log( LOG_LEVEL_DEBUG, "vals: '%s'", b );
        free( a );
        free( b );
        a = NULL;
        b = NULL;
        c = NULL;
        d = NULL;
        a_sz = 0;
        b_sz = 0;
    }
    else
    {
        _log( LOG_LEVEL_DEBUG, "keys: %p", to_ptr( changeset_array_context, cs->keys ) );
        _log( LOG_LEVEL_DEBUG, "vals: %p", to_ptr( changeset_array_context, cs->vals ) );
    }

    _log( LOG_LEVEL_DEBUG, "num_columns: %u", cs->num_columns );
    if( cs->num_columns > 0 )
    {
        cols = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->columns );
        news = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->new_vals );
        olds = ( changeset_string_ref_t * ) to_ptr( changeset_array_context, cs->old_vals );

        for( i = 0; i < cs->num_columns; i++ )
        {
            if( cols != NULL && cols[i] != NULLREF )
                d = ( char * ) to_ptr( changeset_string_context, cols[i] );

            if( news != NULL && news[i] != NULLREF )
                e = ( char * ) to_ptr( changeset_string_context, news[i] );

            if( olds != NULL && olds[i] != NULLREF )
                f = ( char * ) to_ptr( changeset_string_context, olds[i] );

            if( d != NULL )
                a_sz += strlen( d ) + 1;
            if( e )
                b_sz += strlen( e ) + 1;
            if( f )
                c_sz += strlen( f ) + 1;
        }

        if( a_sz > 0 )
            a = ( char * ) calloc( a_sz, sizeof( char ) );

        if( b_sz > 0 )
            b = ( char * ) calloc( b_sz, sizeof( char ) );

        if( c_sz > 0 )
            c = ( char * ) calloc( c_sz, sizeof( char ) );

        if( cols != NULL && cols[0] != NULLREF )
            d = ( char * ) to_ptr( changeset_string_context, cols[0] );

        if( news != NULL && news[0] != NULLREF )
            e = ( char * ) to_ptr( changeset_string_context, news[0] );

        if( olds != NULL && olds[0] != NULLREF )
            f = ( char * ) to_ptr( changeset_string_context, olds[0] );

        if( d != NULL )
            strncpy( a, d, strlen( d ) );

        if( e != NULL )
            strncpy( b, e, strlen( e ) );

        if( f != NULL )
            strncpy( c, f, strlen( f ) );

        for( i = 1; i < cs->num_columns; i++ )
        {
            if( cols != NULL && cols[i] != NULLREF )
                d = ( char * ) to_ptr( changeset_string_context, cols[i] );

            if( news != NULL && news[i] != NULLREF )
                e = ( char * ) to_ptr( changeset_string_context, news[i] );

            if( olds != NULL && olds[i] != NULLREF )
                f = ( char * ) to_ptr( changeset_string_context, olds[i] );

            if( d != NULL )
            {
                strncat( a, ",", 1 );
                strncat( a, d, strlen( d ) );
            }

            if( e != NULL )
            {
                strncat( b, ",", 1 );
                strncat( b, e, strlen( e ) );
            }

            if( f != NULL )
            {
                strncat( c, ",", 1 );
                strncat( c, f, strlen( f ) );
            }
        }

        _log( LOG_LEVEL_DEBUG, "columns: '%s'", a );
        free( a );
        if( news != NULL )
        {
            _log( LOG_LEVEL_DEBUG, "new_vals: '%s'", b );
            free( b );
        }
        else
        {
            _log( LOG_LEVEL_DEBUG, "new_vals: %p", news );
        }

        if( olds != NULL )
        {
            _log( LOG_LEVEL_DEBUG, "old_vals: '%s'", c );
            free( c );
        }
        else
        {
            _log( LOG_LEVEL_DEBUG, "old_vals: %p", olds );
        }
    }
    else
    {
        _log( LOG_LEVEL_DEBUG, "new_vals: %p", news );
        _log( LOG_LEVEL_DEBUG, "old_vals: %p", olds );
    }

    a = ( char * ) to_ptr( changeset_string_context, cs->schema_name );
    if( a != NULL )
        _log( LOG_LEVEL_DEBUG, "schema_name: %s", a );
    else
        _log( LOG_LEVEL_DEBUG, "schema_name: NULL" );

    a = ( char * ) to_ptr( changeset_string_context, cs->table_name );

    if( a != NULL )
        _log( LOG_LEVEL_DEBUG, "table_name: %s", a );
    else
        _log( LOG_LEVEL_DEBUG, "table_name: NULL" );

    _log( LOG_LEVEL_DEBUG, "XID: %lu", cs->xid );
    _log(
        LOG_LEVEL_DEBUG,
        "type: %s",
        cs->type == PGC_DML_UNINITIALIZED ? "UNINITIALIZED" :
        cs->type == PGC_DML_INSERT ? "INSERT" :
        cs->type == PGC_DML_UPDATE ? "UPDATE" :
        cs->type == PGC_DML_DELETE ? "DELETE" :
        cs->type == PGC_DML_TRUNCATE ? "TRUNCATE" :
        cs->type == PGC_DML_COMMIT ? "COMMIT" :
        cs->type == PGC_DML_BEGIN ? "BEGIN" :
        cs->type == PGC_DML_ROLLBACK ? "ROLLBACK" :
        "UNKNOWN"
    );

    _log( LOG_LEVEL_DEBUG, "timestamp: %s", ctime( &(cs->timestamp) ) );

    return;
}
