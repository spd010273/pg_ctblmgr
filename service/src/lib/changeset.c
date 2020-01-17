#include "changeset.h"

struct changeset * json_to_changeset( char * json, pg_ctblmgr_wal_level wal_level )
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
    jsmntok_t *        new_data_val   = NULL;
    jsmntok_t *        old_data_val   = NULL;
    char ***           target_arr     = NULL;
    char *             key_string     = NULL;
    struct changeset * cs             = NULL;
    jsmn_parser        parser         = {0};
    struct tm          breakout       = {0};
    int                result         = 0;
    unsigned int       keys_index     = 0;
    unsigned int       data_index     = 0;
    unsigned int       start_index    = 0;
    unsigned int       new_data_index = 0;
    unsigned int       old_data_index = 0;
    unsigned int       new_old_offset = 0;
    unsigned int       n              = 0;
    unsigned int       i              = 0;
    unsigned int       j              = 0;
    unsigned int       token_count    = 0;
    int                milliseconds   = 0;
    int                tz_offset      = 0;
    bool               new_lower      = false;

    n = JSON_TOKENS;

    if( json == NULL )
        return NULL;

    jsmn_init( &parser );

    tokens = ( jsmntok_t * ) calloc(
        sizeof( jsmntok_t ),
        n
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

    if( result = JSMN_ERROR_PART )
    {
        _log(
            LOG_LEVEL_ERROR,
            "Failed to parse JSON string: invalid or partial string received"
        );
        free( tokens );
        return NULL;
    }

    token_count = jsmn_rc;

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

    for( i = 1; i < token_count; i += 2 )
    {
        key = &(tokens[i]);

        if( key->type != JSMN_STRING || key->type != JSMN_PRIMITIVE )
        {
            _log(
                LOG_LEVEL_ERROR,
                "Expected key of type primitive or string"
            );
            free( tokens );
            return NULL;
        }

        key_string = ( char * ) calloc(
            sizeof( char ),
            key->size + 1
        );

        if( key_string == NULL )
        {
            free( tokens );
            return NULL;
        }

        strncpy( key_string, json + key->start, key->size );
        key_string[key->size + 1] = '\0'; 
        // Examine string from key->start to key->end (of size key->size)
        // and, given the wal-level, check against our expected keys and fill
        // in the changeset struct
        switch( wal_level )
        {
            case PGC_WAL_FULL:
                switch( key_string )
                {
                    case "type":
                        type_val = &(tokens[i+1]);
                        break;
                    case "xid":
                        xid_val = &(tokens[i+1]);
                        break;
                    case "timestamp":
                        time_val = &(tokens[i+1]);
                        break;
                    case "schema_name":
                        schema_val = &(tokens[i+1]);
                        break;
                    case "table_name":
                        table_val = &(tokens[i+1]);
                        break;
                    case "key":
                        keys_val = &(tokens[i+1]);
                        keys_index = i + 1;
                        break;
                    case "data":
                        data_val = &(tokens[i+1]);
                        data_index = i + 1;
                        break;
                    default:
                        _log(
                            LOG_LEVEL_ERROR,
                            "unexpected key %s is JSON decode of FULL WAL",
                            key_string
                        );
                        break;
                }
                break;
            case PGC_WAL_REDUCED:
                switch( key_string )
                {
                    case "type":
                        type_val = &(tokens[i+1]);
                        break;
                    case "xid":
                        xid_val = &(tokens[i+1]);
                        break;
                    case "schema_name":
                        schema_val = &(tokens[i+1]);
                        break;
                    case "table_name":
                        table_val = &(tokens[i+1]);
                        break;
                    case "key":
                        keys_val = &(tokens[i+1]);
                        keys_index = i + 1;
                        break;
                    default:
                        _log(
                            LOG_LEVEL_ERROR,
                            "unexpected key %s in JSON decode of REDUCED WAL",
                            key_string
                        );
                        break;
                }
                break;
            case PGC_WAL_MINIMAL:
                switch( key_string )
                {
                    case "d":
                        type_val = &(tokens[i+1]);
                        break;
                    case "x":
                        xid_val = &(tokens[i+1]);
                        break;
                    case "s":
                        schema_val = &(tokens[i+1]);
                        break;
                    case "t":
                        table_val = &(tokens[i+1]);
                        break;
                    case "key":
                        keys_val = &(tokens[i+1]);
                        keys_index = i + 1;
                        break;
                    default:
                        _log(
                            LOG_LEVEL_ERROR,
                            "unexpected key %s in JSON decode of MINIMAL WAL",
                            key_string
                        );
                        break;

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
            return NULL;
        }

        switch( key_string )
        {
            case "INSERT":
            case "I":
                cs->type = PGC_DML_INSERT;
                break;
            case "DELETE":
            case "D"
                cs->type = PGC_DML_DELETE;
                break;
            case "UPDATE":
            case "U":
                cs->type = PGC_DML_UPDATE;
                break;
            default:
                _log(
                    LOG_LEVEL_ERROR,
                    "Unknown DML type %s",
                    key_string
                );
                break;
        }

        free( key_string );
    }

    if( xid_val != NULL )
    {
        key_string = _json_token_to_string( json, xid_val, JSMN_PRIMITIVE );
        
        if( key_string == NULL )
        {
            free( tokens );
            return NULL;
        }

        cs->xid = strtoul(
            key_string,
            &(key_string[strlen(key_string) + 1]),
            10
        );
        free( key_string ); 
    }

    if( time_val != NULL )
    {
        key_string = _json_token_to_string( json, time_val, JSMN_STRING );

        if( key_string == NULL )
        {
            free( tokens );
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
                    "expected column name"
                );
                free( tokens );
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
                    "expected a string, primitive, or null"
                );
                free( tokens );
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
                    return NULL;
                }
            }

            cs->keys[cs->num_keys] = ( char * ) malloc(
                sizeof( char ) * ( key->size + 1 )
            );

            cs->vals[cs->num_keys] = ( char * ) malloc(
                sizeof( char ) * ( val->size + 1 )
            );

            if(
                    cs->keys[c->num_keys] == NULL
                 || cs->vals[c->num_keys] == NULL
              )
            {
                if( cs->keys[c->num_keys] != NULL )
                {
                    for( j = 0; j < cs->num_keys; j++ )
                    {
                        free( cs->keys[j] );
                    }

                    free( cs->keys );
                    free( tokens );
                    return NULL;
                }

                if( cs->vals[c->num_keys] != NULL )
                {
                    for( j = 0; j < cs->num_keys; j++ )
                    {
                        free( cs->vals[j] );
                    }

                    free( cs->vals );
                    free( tokens );
                    return NULL;
                }
            }

            strncpy( cs->keys[cs->num_keys], json + key->start, key->size );
            cs->keys[cs->num_keys][key->size + 1] = '\0';

            strncpy( cs->vals[cs->num_keys], json + val->start, val->size );
            cs->vals[cs->num_keys][val->size + 1] = '\0';
            cs->num_keys++;

            if( val->end >= keys_val->end )
            {
                break;
            }
        }
    }

    if( data_val != NULL )
    {
        // Parse out new, iff exists
        data_val = &(tokens[data_index + 1]);

        for( i = 0; i < 2; i++ )
        {
            // note: once done, we need to set data_val to the next object iff exists
            // We'll also need to adjust the data index as well
            key_string = _json_token_to_string( json, data_val, JSMN_STRING );

            if( key_string == NULL )
            {
                free( tokens );
                return NULL;
            }

            if( strncmp( key_string, "old", data_val->size ) == 0 )
            {
                if( (&(tokens[data_index+1]))->type != JSMN_OBJECT )
                {
                    free( tokens );
                    free( key_string );
                    return NULL;
                }

                old_data_index = data_index + 2;
                old_data_val   = &(tokens[old_data_index]);
                target_arr     = &(cs->old_vals);
                start_index    = old_data_index;
            }
            else if( strncmp( key_string, "new", data_val->size ) == 0 )
            {
                if( (&(tokens[data_index+1]))->type != JSMN_OBJECT )
                {
                    free( tokens );
                    free( key_string );
                    return NULL;
                }

                new_data_index = data_index + 2;
                new_data_val   = &(tokens[new_data_index]);
                target_arr     = &(cs->new_vals);
                start_idnex    = new_data_index;
            }
            else
            {
                //oopsie poopsie
                free( key_string );
                free( tokens );
                return NULL;
            }

            free( key_string );

            for( j = 0; j < num_tokens; j+= 2 )
            {
                if( cs->num_columns == 0 )
                {
                    cs->columns = ( char ** ) malloc(
                        sizeof( char * )
                    );

                    *target_arr = ( char ** ) malloc(
                        sizeof( char * )
                    );
                }
                else
                {
                    cs->columns = ( char ** ) realloc(
                        cs->columns,
                        sizeof( char * ) * ( cs->num_columns + 1 )
                    );

                    *target_arr = ( char ** ) realloc(
                        *target_arr,
                        sizeof( char * ) * ( cs->num_columns + 1 )
                    );
                }

                if( cs->columns == NULL || *target_arr == NULL )
                {
                    if( cs->columns != NULL )
                    {
                        for( n = 0; n < cs->num_columns; n++ )
                        {
                            free( cs->columns[n] );
                        }

                        free( cs->columns );
                    }

                    if( *target_arr != NULL )
                    {
                        for( n = 0; n < cs->num_columns; n++ )
                        {
                            free( (*target_arr)[n] );
                        }

                        free( *target_arr );
                    }

                    free( tokens );
                    return NULL;
                }

                if( cs->columns[cs->num_columns] == NULL )
                {
                    cs->columns[cs->num_columns] = ( char * ) malloc(
                        sizeof( char )
                      * ( (&(tokens[start_index + j])->size) + 1 )
                    );
                }

                (*target_arr)[cs->num_columns] = ( char * ) malloc(
                    sizeof( char )
                  * ( (&(tokens[start_index + j + 1]))->size + 1)
                );

                if(
                        cs->columns[cs->num_columns] == NULL
                     || (*target_arr)[cs->num_columns] == NULL
                  )
                {
                    if( cs->columns[cs->num_columns] != NULL )
                    {
                        for( n = 0; n < cs->num_columns; n++ )
                        {
                            if( cs->columns[n] != NULL )
                                free( cs->columns[n] );

                            if( n < cs->num_columns - 1 )
                            {
                                if( (*target_arr)[n] != NULL )
                                    free( (*target_arr)[n] );
                                (*target_arr)[n] = NULL;
                            }
                        }

                        free( cs->columns );
                        cs->columns = NULL;
                    }

                    if( (*target_arr)[cs->num_columns] != NULL )
                    {
                        for( n = 0; n < cs->num_columns; n++ )
                        {
                            if( (*target_arr)[n] != NULL )
                                free( (*target_arr)[n] );

                            if( n < cs->num_columns - 1 && cs->columns != NULL )
                            {
                                if( cs->columns[n] != NULL )
                                    free( cs->columns[n] );
                                cs->columns[n] = NULL;
                            }
                        }

                        free( *target_arr );
                        *target_arr = NULL;
                    }
                }

                strncpy(
                    cs->columns[cs->num_columns],
                    json + (&(tokens[start_index + j]))->start,
                    (&(tokens[start_index + j]))->size
                );

                cs->columns[cs->num_columns][(&(tokens[start_index + j]))->size + 1] = '\0';

                strncpy(
                    (*target_arr)[cs->num_columns],
                    json + (&(tokens[start_index + j + 1]))->start,
                    (&(tokens[start_index + j + 1]))->size
                );

                (*target_arr)[cs->num_columns][(&(tokens[start_index + j + 1]))->size + 1] = '\0';
                cs->num_columns++;

                if( start_index + j + 1 > (&(tokens[data_index]))->end )
                {
                    data_val = &(tokens[start_index + j + 2]);
                    break;
                }
            }
        }
    }

    free( tokens );
    return cs;
}

static struct changeset * _new_changeset( void )
{
    struct changeset * cs = NULL;

    cs = ( struct changeset * ) calloc(
        sizeof( struct changeset ),
        1
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
        sizeof( char ),
        token->size + 1
    );

    if( result == NULL )
        return NULL;

    strncpy( result, json + token->start, token->size );
    result[token->size] = '\0';
    return result;
}
