#include "changeset.h"

static struct changeset * _new_changeset( void );
static inline char * _json_token_to_string( char *, jsmntok_t *, jsmntype_t );
static void _jsmn_dump( jsmntok_t * );
static void _parse_data_record(
    char *,         // json string
    jsmntok_t *,    // token array
    unsigned int,   // num_tokens
    unsigned int,   // start index
    char ***,       // target array
    unsigned int *, // array len
    char ***        // columns array (population of this is one-shotted)
);

struct changeset * json_to_changeset(
    char *               json,
    pg_ctblmgr_wal_level wal_level
)
{
    jsmntok_t *        type_val       = NULL;
    jsmntok_t *        xid_val        = NULL;
    jsmntok_t *        time_val       = NULL;
    jsmntok_t *        schema_val     = NULL;
    jsmntok_t *        table_val      = NULL;
    jsmntok_t *        keys_val       = NULL;
    jsmntok_t *        data_val       = NULL;
    jsmntok_t *        key            = NULL;
    jsmntok_t *        val            = NULL;
    jsmntok_t *        tokens         = NULL;
    char *             key_string     = NULL;
    struct changeset * cs             = NULL;
    jsmn_parser        parser         = {0};
    struct tm          breakout       = {0};
    int                result         = 0;
    unsigned int       keys_index     = 0;
    unsigned int       data_index     = 0;
    unsigned int       n              = 0;
    unsigned int       i              = 0;
    unsigned int       j              = 0;
    unsigned int       token_count    = 0;
    unsigned int       key_len        = 0;
    unsigned int       size           = 0;
    int                milliseconds   = 0;
    int                tz_offset      = 0;
    bool               done           = false; // Initial parse oneshot
    unsigned int       column_canary  = 0;
    bool               in_subobject   = false;
    unsigned int       subobject_end  = 0;
    jsmntok_t *        temp_val       = NULL;

    n = JSON_TOKENS;

    if( json == NULL )
        return NULL;

    jsmn_init( &parser );

    tokens = ( jsmntok_t * ) calloc(
        n,
        sizeof( jsmntok_t )
    );

    if( tokens == NULL )
        return NULL;

    result = jsmn_parse( &parser, json, strlen( json ), tokens, n );

    while( result == JSMN_ERROR_NOMEM )
    {
        n = n + JSON_TOKENS;

        tokens = realloc( tokens, sizeof( jsmntok_t ) * n );

        if( tokens == NULL )
            return NULL;

        result = jsmn_parse( &parser, json, strlen( json ), tokens, n );
    }

    if( result == JSMN_ERROR_INVAL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to parse JSON string: invalid or corrupted string"
        );
        free( tokens );
        return NULL;
    }

    if( result == JSMN_ERROR_PART )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to parse JSON string: invalid or partial string received"
        );
        free( tokens );
        return NULL;
    }

    token_count = result;

    // Sanity check the # of tokens returned vs allocated memory
    if( token_count > n )
        return NULL;

    if( tokens[0].type != JSMN_OBJECT )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Root element of JSON is not an object"
        );
        free( tokens );
        return NULL;
    }

    cs = _new_changeset();

    if( cs == NULL )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to allocate changeset"
        );
        return NULL;
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
            free( cs );
            return NULL;
        }

        key_string = ( char * ) calloc(
            ( key->end - key->start ) + 1,
            sizeof( char )
        );

        if( key_string == NULL )
        {
            free( tokens );
            free( cs );
            return NULL;
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
            free( cs );
            return NULL;
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
            free( cs );
            _log(
                LOG_LEVEL_ERROR,
                "Failed to parse XID value from json token"
            );
            return NULL;
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
            free( cs );
            _log(
                LOG_LEVEL_ERROR,
                "Failed to parse timestamp value from json token"
            );
            return NULL;
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
                free( cs );
                return NULL;
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
            free( cs );
            return NULL;
        }

        free( key_string );
    }

    if( schema_val != NULL )
    {
        key_string = _json_token_to_string( json, schema_val, JSMN_STRING );

        if( key_string == NULL )
        {
            free( tokens );
            free( cs );
            _log(
                LOG_LEVEL_ERROR,
                "Failed to parse schema from json token"
            );
            return NULL;
        }

        cs->schema_name = key_string;
    }

    if( table_val != NULL )
    {
        key_string = _json_token_to_string( json, table_val, JSMN_STRING );

        if( key_string == NULL )
        {
            free( tokens );
            free( cs );
            _log(
                LOG_LEVEL_ERROR,
                "Failed to parse table from json token"
            );
            return NULL;
        }

        cs->table_name = key_string;
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
            free( cs );
            return NULL;
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
                free( cs );
                return NULL;
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
                free( cs );
                return NULL;
            }

            if( cs->num_keys == 0 )
            {
                cs->keys = ( char ** ) malloc(
                    sizeof( char * )
                );

                cs->vals = ( char ** ) malloc(
                    sizeof( char * )
                );

                if( cs->keys == NULL || cs->vals == NULL )
                {
                    if( cs->keys != NULL )
                    {
                        free( cs->keys );
                    }

                    if( cs->vals != NULL )
                    {
                        free( cs->vals );
                    }

                    free( tokens );
                    _log(
                        LOG_LEVEL_ERROR,
                        "Failed to perform initial alloc for changeset kv"
                    );
                    free( cs );
                    return NULL;
                }
            }
            else
            {
                cs->keys = ( char ** ) realloc(
                    cs->keys,
                    ( cs->num_keys + 1 ) * sizeof( char * )
                );

                cs->vals = ( char ** ) realloc(
                    cs->vals,
                    ( cs->num_keys + 1 ) * sizeof( char * )
                );

                if( cs->keys == NULL || cs->vals == NULL )
                {
                    if( cs->keys != NULL )
                    {
                        for( j = 0; j < cs->num_keys - 1; j++ )
                        {
                            free( cs->keys[j] );
                        }

                        free( cs->keys );
                    }

                    if( cs->vals != NULL )
                    {
                        for( j = 0; j < cs->num_keys - 1; j++ )
                        {
                            free( cs->vals[j] );
                        }

                        free( cs->vals );
                    }

                    free( tokens );
                    free( cs );
                    _log(
                        LOG_LEVEL_ERROR,
                        "Failed to perform incremental alloc for cs kv"
                    );
                    return NULL;
                }
            }

            cs->keys[cs->num_keys] = ( char * ) malloc(
                sizeof( char ) * ( key->end - key->start + 1 )
            );

            cs->vals[cs->num_keys] = ( char * ) malloc(
                sizeof( char ) * ( val->end - val->start + 1 )
            );

            if(
                    cs->keys[cs->num_keys] == NULL
                 || cs->vals[cs->num_keys] == NULL
              )
            {
                if( cs->keys[cs->num_keys] != NULL )
                {
                    for( j = 0; j < cs->num_keys; j++ )
                    {
                        free( cs->keys[j] );
                    }

                    free( cs->keys );
                    free( tokens );
                    free( cs );
                    _log(
                        LOG_LEVEL_ERROR,
                        "Failed to allocate key array member"
                    );
                    return NULL;
                }

                if( cs->vals[cs->num_keys] != NULL )
                {
                    for( j = 0; j < cs->num_keys; j++ )
                    {
                        free( cs->vals[j] );
                    }

                    free( cs->vals );
                    free( tokens );
                    free( cs );
                    _log(
                        LOG_LEVEL_ERROR,
                        "Failed to allocate value array member"
                    );
                    return NULL;
                }
            }

            size = key->end - key->start;
            strncpy(
                cs->keys[cs->num_keys],
                json + key->start,
                size
            );

            cs->keys[cs->num_keys][size] = '\0';

            size = val->end - val->start;
            strncpy(
                cs->vals[cs->num_keys],
                json + val->start,
                size
            );

            cs->vals[cs->num_keys][size] = '\0';
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
            free( cs );
            return NULL;
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

            if( cs->new_vals == NULL )
            {
                _log( LOG_LEVEL_ERROR, "failed to parse new vals from data object" );
                free( tokens );
                free( cs );
                return NULL;
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

            if( cs->old_vals == NULL )
            {
                _log( LOG_LEVEL_ERROR, "Failed to parse old vals from data object" );
                free( tokens );
                free( cs );
                return NULL;
            }
        }
        else
        {
            _log( LOG_LEVEL_ERROR, "unexpected record key in data subobject %s", key_string );
            free( tokens );
            free( cs );
            return NULL;
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
            return cs;
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
            free( cs );
            return NULL;
        }

        key_len = strlen( key_string );

        if( strncmp( key_string, "new", MIN( key_len, 3 ) ) == 0 && cs->new_vals == NULL )
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

            if( cs->new_vals == NULL )
            {
                _log( LOG_LEVEL_ERROR, "failed to parse new vals from data object" );
                free( tokens );
                free( cs );
                return NULL;
            }
        }
        else if( strncmp( key_string, "old", MIN( key_len, 3 ) ) == 0 && cs->old_vals == NULL )
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

            if( cs->old_vals == NULL )
            {
                _log( LOG_LEVEL_ERROR, "Failed to parse old vals from data object" );
                free( tokens );
                free( cs );
                return NULL;
            }
        }
        else
        {
            _log( LOG_LEVEL_ERROR, "unexpected record key in data subobject %s", key_string );
            free( tokens );
            free( cs );
            return NULL;
        }

        if( column_canary > 0 && column_canary != cs->num_columns )
        {
            // These records should be of the same length
            _log( LOG_LEVEL_ERROR, "Mismatch in recordlengths between new and old" );
            free( tokens );
            free( cs );
            return NULL;
        }
    }

    free( tokens );
    return cs;
}

static void _parse_data_record(
    char *         json,
    jsmntok_t *    tokens,
    unsigned int   num_tokens,
    unsigned int   index,
    char ***       val_array,
    unsigned int * num_elements,
    char ***       columns
)
{
    unsigned int obj_end       = 0;
    unsigned int i             = 0;
    unsigned int e             = 0;
    jsmntok_t *  val           = NULL;
    jsmntok_t *  key           = NULL;
    jsmntok_t *  temp          = NULL;

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
            *val_array = ( char ** ) calloc( 1, sizeof( char * ) );

            if( columns != NULL )
            {
                *columns = ( char ** ) calloc( 1, sizeof( char * ) );
            }
        }
        else
        {
            *val_array = ( char ** ) realloc( *val_array, sizeof( char * ) * ( (*num_elements) + 1 ) );

            if( columns != NULL )
            {
                *columns = ( char ** ) realloc( *columns, sizeof( char * ) * ( (*num_elements) + 1 ) );
            }
        }

        if( (*val_array) == NULL || ( columns != NULL && (*columns) == NULL ) )
        {
            _log( LOG_LEVEL_ERROR, "Failed to allocate memory" );
            return;
        }

        (*val_array)[(*num_elements)] = ( char * ) calloc(
            val->end - val->start + 1,
            sizeof( char )
        );

        if( (*val_array)[(*num_elements)] == NULL )
        {
            _log( LOG_LEVEL_ERROR, "Failed to allocate memory" );
            free( *val_array );
            return;
        }

        if( columns != NULL )
        {
            (*columns)[(*num_elements)] = ( char * ) calloc(
                key->end - key->start + 1,
                sizeof( char )
            );

            if( (*columns)[(*num_elements)] == NULL )
            {
                _log( LOG_LEVEL_ERROR, "Failed to allocate memory" );
                free( *columns );
                return;
            }
        }

        strncpy(
            (*val_array)[(*num_elements)],
            json + val->start,
            val->end - val->start
        );
        (*val_array)[(*num_elements)][val->end - val->start] = '\0';

        if( columns != NULL )
        {
            strncpy(
                (*columns)[(*num_elements)],
                json + key->start,
                key->end - key->start
            );
            (*columns)[(*num_elements)][key->end - key->start] = '\0';
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

static struct changeset * _new_changeset( void )
{
    struct changeset * cs = NULL;

    cs = ( struct changeset * ) calloc(
        1,
        sizeof( struct changeset )
    );

    if( cs == NULL )
        return NULL;

    cs->keys        = NULL;
    cs->vals        = NULL;
    cs->new_vals    = NULL;
    cs->old_vals    = NULL;
    cs->num_keys    = 0;
    cs->schema_name = NULL;
    cs->table_name  = NULL;
    cs->columns     = NULL;
    cs->num_columns = 0;
    cs->timestamp   = 0;
    cs->type        = PGC_DML_UNINITIALIZED;
    cs->xid         = 0;
    return cs;
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
