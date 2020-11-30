#include "pg_ctblmgr.h"

void _PG_init( void )
{
    // this is a stub for now, we may need to read in GUCs
    // that determine whether minimal JSON records are output or not
    // NOTE: We'll need to set up a queue of guc_change entries in SHM
    return;
}

// Setup callbacks for logical decoding
void _PG_output_plugin_init( OutputPluginCallbacks * callback )
{
    AssertVariableIsOfType( &_PG_output_plugin_init, LogicalOutputPluginInit );

    callback->startup_cb  = pg_ctblmgr_decode_startup;
    callback->begin_cb    = pg_ctblmgr_decode_begin_tx;
    callback->change_cb   = pg_ctblmgr_decode_change;
    callback->commit_cb   = pg_ctblmgr_decode_commit_tx;
    callback->shutdown_cb = pg_ctblmgr_decode_shutdown;
#if PG_VERSION_NUM >= 90600
    callback->message_cb = pg_ctblmgr_decode_message;
#endif // PG_VERSION_NUM
#if PG_VERSION_NUM >= 110000
    callback->truncate_cb = pg_ctblmgr_decode_truncate;
#endif // PG_VERSION_NUM

    return;
}

// Initialize the decoder
static void pg_ctblmgr_decode_startup(
    LogicalDecodingContext * context,
    OutputPluginOptions *    options,
    bool                     is_init
)
{
    struct pgc_table * table      = NULL;
    decode_data *      data       = NULL;
    ListCell *         cell       = NULL;
    DefElem *          element    = NULL;
    char *             raw_string = NULL;

    data = ( decode_data * ) palloc0( sizeof( decode_data ) );

    if( data == NULL )
    {
        ereport(
            ERROR,
            (
                errcode( ERRCODE_OUT_OF_MEMORY ),
                errmsg( "Failed to initialize pg_ctblmgr_decoder" )
            )
        );
    }

    // Setup memory allocation for this context
    data->num_changes         = 0;
    data->wal_level           = PGC_WAL_FULL;
    data->include_transaction = true;
    data->wrote_tx_changes    = false;
    data->filter_tables       = NIL;
    data->context             = AllocSetContextCreate(
        TopMemoryContext,
        "pg_ctblmgr decoder context",
#if PG_VERSION_NUM >= 90600
        ALLOCSET_DEFAULT_SIZES
#else
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE
#endif // PG_VERSION_NUM
    );

    // Read in configuration
    foreach( cell, context->output_plugin_options )
    {
        element = ( DefElem * ) lfirst( cell );
        Assert( element->arg == NULL || IsA( element->arg, String ) );

        if( strcmp( element->defname, "filter-tables" ) == 0 )
        {
            if( element->arg == NULL )
            {
                data->filter_tables = NIL;
            }
            else
            {
                raw_string = pstrdup( strVal( element->arg ) );

                if(
                    !config_to_filter_table(
                        raw_string,
                        &(data->filter_tables)
                    )
                  )
                {
                    pfree( raw_string );
                    ereport(
                        ERROR,
                        (
                            errcode( ERRCODE_INVALID_NAME ),
                            errmsg(
                                "Could not parse object name \"%s\"",
                                strVal( element->arg )
                            )
                        )
                    );
                }

                pfree( raw_string );
            }
        }
        else if( strcmp( element->defname, "include-transaction" ) == 0 )
        {
            if( element->arg == NULL )
            {
                data->include_transaction = true;
            }
            else
            {
                if(
                    !parse_bool(
                        strVal( element->arg ),
                        &(data->include_transaction)
                    )
                  )
                {
                    ereport(
                        ERROR,
                        (
                            errcode( ERRCODE_INVALID_PARAMETER_VALUE ),
                            errmsg(
                                "Could not parse setting \"%s\""\
                                " for include-transaction",
                                strVal( element->arg )
                            )
                        )
                    );
                }
            }
        }
        else if( strcmp( element->defname, "wal-level" ) == 0 )
        {
            if( element->arg == NULL )
            {
                data->wal_level = PGC_WAL_FULL;
            }
            else
            {
                raw_string = pstrdup( strVal( element->arg ) );

                if( strncmp( raw_string, "F", 1 ) == 0 )
                {
                    data->wal_level = PGC_WAL_FULL;
                }
                else if( strncmp( raw_string, "R", 1 ) == 0 )
                {
                    data->wal_level = PGC_WAL_REDUCED;
                }
                else if( strncmp( raw_string, "M", 1 ) == 0 )
                {
                    data->wal_level = PGC_WAL_MINIMAL;
                }
                else
                {
                    ereport(
                        ERROR,
                        (
                            errcode( ERRCODE_INVALID_PARAMETER_VALUE ),
                            errmsg(
                                "Invalid WAL level specified '%s'",
                                strVal( element->arg )
                            )
                        )
                    );
                }

                pfree( raw_string );
            }
        }
    }

    if( data->filter_tables == NIL )
    {
        table = ( struct pgc_table * ) palloc0( sizeof( struct pgc_table ) );
        table->all_schemas = true;
        table->all_tables = true;
        data->filter_tables = lappend( NIL, table );
    }

    context->output_plugin_private = data;
    options->output_type           = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;

    return;
}

static void pg_ctblmgr_decode_shutdown( LogicalDecodingContext * context )
{
    decode_data * data = NULL;

    data = ( decode_data * ) context->output_plugin_private;
    MemoryContextDelete( data->context );
    return;
}

#if PG_VERSION_NUM >= 90600
static void pg_ctblmgr_decode_message(
    LogicalDecodingContext * context,
    ReorderBufferTXN *       txn,
    XLogRecPtr               lsn,
    bool                     is_transactional,
    const char *             prefix,
    Size                     content_size,
    const char *             content
)
{
    // this is a stub for now
    return;
}
#endif // PG_VERSION_NUM

#if PG_VERSION_NUM >= 110000
static void pg_ctblmgr_decode_truncate(
    LogicalDecodingContext * context,
    ReorderBufferTXN *       txn,
    int                      num_relations,
    Relation                 relations[],
    ReorderBufferChange *    change
)
{
    // this is a stub for now
    return;
}

#endif // PG_VERSION_NUM
static void pg_ctblmgr_decode_begin_tx(
    LogicalDecodingContext * context,
    ReorderBufferTXN *       txn
)
{
    decode_data * data = NULL;

    data = ( decode_data * ) context->output_plugin_private;
    data->wrote_tx_changes = false;

    if( !data->include_transaction || data->wal_level == PGC_WAL_MINIMAL )
    {
        return;
    }

    OutputPluginPrepareWrite( context, true );

    switch( data->wal_level )
    {
        case PGC_WAL_FULL:
            appendStringInfo(
                context->out,
                transaction_boundary_full,
                "BEGIN",
                txn->xid,
                timestamptz_to_str(
                    txn->commit_time
                )
            );
            break;
        case PGC_WAL_REDUCED:
            appendStringInfo(
                context->out,
                transaction_boundary_reduced,
                "BEGIN",
                txn->xid
            );
            break;
        case PGC_WAL_MINIMAL:
            appendStringInfo(
                context->out,
                transaction_boundary_minimal,
                txn->xid
            );
            break;
        default:
            return;
    }

    OutputPluginWrite( context, true );
    return;
}

static void pg_ctblmgr_decode_commit_tx(
    LogicalDecodingContext * context,
    ReorderBufferTXN *       txn,
    XLogRecPtr               commit_lsn
)
{
    decode_data * data = NULL;
    data = ( decode_data * ) context->output_plugin_private;
    data->wrote_tx_changes = true;

    if( !data->include_transaction || data->wal_level == PGC_WAL_MINIMAL )
    {
        return;
    }

    OutputPluginPrepareWrite( context, true );

    switch( data->wal_level )
    {
        case PGC_WAL_FULL:
            appendStringInfo(
                context->out,
                transaction_boundary_full,
                "COMMIT",
                txn->xid,
                timestamptz_to_str( txn->commit_time )
            );
            break;
        case PGC_WAL_REDUCED:
            appendStringInfo(
                context->out,
                transaction_boundary_reduced,
                "COMMIT",
                txn->xid
            );
            break;
        case PGC_WAL_MINIMAL:
            appendStringInfo(
                context->out,
                transaction_boundary_minimal,
                txn->xid
            );
            break;
        default:
            return;
    }
    OutputPluginWrite( context, true );
    return;
}

/*
 * This is the bulk of the logic for this decoder. Our goal is to translate
 * the reorder buffer's changes into a JSON representation of what happened
 * to a table within the publication (aka replication set). For all statements,
 * we seek to find the surrogate or primary key that can identify the tuple,
 * transmitting that in the JSON's "key" field, as well as (optionally), new
 * and old versions of the tuple. This can be further used to identify records
 * downstream that need to be updated via a NATURAL, or FULL OUTER join on all
 * relation attributes.
 *
 * TODO: We need to figure out a way to read the session GUCs while decoding,
 * which may require us to pull it from the transaction / backend memory.
 * AFAICT, this information is not stored in the ReorderBufferTXN or
 * ReorderBufferChange structs, ~~but~~ it may be derivable from those
 * structs. We will be looking for PGC_S_SESSION gucs. This may need to be done
 * by hooing src/backend/utils/misc/guc.c:SetConfigOption. For x86_64,
 * CentOS 7.6 this function is 48 bytes long (it is hookable), and merely wraps
 * set_config_option() within the same file (to provide a consistent interface).
 *
 * We could hook this function, but we also need the XID at the time the hook is
 * performed. Then again, the decoder may be running within the backend and have
 * access to the GUC stack as well as the current XID in which it was changed in
 * the session (I actually doubt this is the case).
 *
 * More than likely - the best place to intercept this is at the tcop, because
 * it has access to xid information as well as directing the SET command to
 * guc.c routines, a good starting point is the standard_ProcessUtility in
 * backend/tcop/utility.c
 *
 * We may also need to cannibalize pg_logical_emit_message() to insert the GUC
 * state into the WAL so that the decoder can reach it
 */
static void pg_ctblmgr_decode_change(
    LogicalDecodingContext * context,
    ReorderBufferTXN *       txn,
    Relation                 relation,
    ReorderBufferChange *    change
)
{
    decode_data *         data             = NULL;
    ListCell *            cell             = NULL;
    struct pgc_table *    table            = NULL;
    Relation              index            = {0};
    Oid                   index_oid        = InvalidOid;
    Oid                   oid              = InvalidOid;
    Form_pg_class         class_form       = {0};
    FormData_pg_attribute attribute_form   = {0};
    TupleDesc             tuple_descriptor = {0};
    HeapTuple             old_tuple        = {0};
    HeapTuple             new_tuple        = {0};
    HeapTuple             tuple            = {0};
    MemoryContext         old_context      = {0};
    char *                table_name       = NULL;
    char *                schema_name      = NULL;
    char *                dml_type         = NULL;
    unsigned int          i                = 0;
    unsigned int          j                = 0;
    bool                  found            = false;

    data = ( decode_data * ) context->output_plugin_private;
    data->wrote_tx_changes = true;

    class_form       = RelationGetForm( relation );
    tuple_descriptor = RelationGetDescr( relation );

    table_name  = NameStr( class_form->relname );
    schema_name = get_namespace_name(
        get_rel_namespace(
            RelationGetRelid(
                relation
            )
        )
    );

    if( strncmp( table_name, "pg_temp_", 8 ) == 0 )
    {
        return;
    }

    old_context = MemoryContextSwitchTo( data->context );
    RelationGetIndexList( relation );

    switch( change->action )
    {
        case REORDER_BUFFER_CHANGE_INSERT:
            if( change->data.tp.newtuple == NULL )
            {
                elog(
                    WARNING,
                    "No tupledata for new tuple in INSERT for %s.%s",
                    schema_name,
                    table_name
                );
                return;
            }

            dml_type  = "INSERT";
            new_tuple = &(change->data.tp.newtuple->tuple);
            tuple     = new_tuple;
            break;
        case REORDER_BUFFER_CHANGE_UPDATE:
            if( change->data.tp.newtuple == NULL )
            {
                elog(
                    WARNING,
                    "No tupledata for new tuple in UPDATE for %s.%s",
                    schema_name,
                    table_name
                );
                return;
            }

            dml_type  = "UPDATE";
            old_tuple = change->data.tp.oldtuple != NULL ?
                        &(change->data.tp.oldtuple->tuple) :
                        NULL;
            new_tuple = &(change->data.tp.newtuple->tuple);
            tuple     = new_tuple;
            break;
        case REORDER_BUFFER_CHANGE_DELETE:
            dml_type  = "DELETE";
            old_tuple = change->data.tp.oldtuple != NULL ?
                        &(change->data.tp.oldtuple->tuple) :
                        NULL;
            tuple     = old_tuple;
            break;
        default:
            dml_type = "UNKNOWN";
    }

    // Check if our WAL'd table is in the list of tables we care about
    if( list_length( data->filter_tables ) > 0 )
    {
        foreach( cell, data->filter_tables )
        {
            table = ( struct pgc_table * ) lfirst( cell );

            if(
                  (
                      table->all_schemas
                   || strcmp( table->schema_name, schema_name ) == 0
                  )
               && (
                      table->all_tables
                   || strcmp( table->table_name, table_name   ) == 0
                  )
              )
            {
                found = true;
            }
        }
    }
    else
    {
        elog( DEBUG1, "Filter tables list empty" );
    }

    if( found == false )
    {
        // Table is not in our filter list
        MemoryContextSwitchTo( old_context );
        MemoryContextReset( data->context );
        return;
    }

    OutputPluginPrepareWrite( context, true );

    switch( data->wal_level )
    {
        case PGC_WAL_FULL:
            appendStringInfo(
                context->out,
                dml_preamble_full,
                dml_type,
                txn->xid,
                timestamptz_to_str( txn->commit_time ),
                schema_name,
                table_name
            );
            break;
        case PGC_WAL_REDUCED:
            appendStringInfo(
                context->out,
                dml_preamble_reduced,
                dml_type,
                txn->xid,
                schema_name,
                table_name
            );
            break;
        case PGC_WAL_MINIMAL:
            appendStringInfo(
                context->out,
                dml_preamble_minimal,
                dml_type[0],
                txn->xid,
                schema_name,
                table_name
            );
            break;
        default:
            MemoryContextSwitchTo( old_context );
            MemoryContextReset( data->context );
            elog( WARNING, "Invalid WAL level" );
            // May need to tear down the memory context
            return;
    }

    // Append key information
    if( tuple != NULL )
    {
        appendStringInfoString( context->out, ",\"key\":{" );

        switch( relation->rd_rel->relreplident )
        {
            case REPLICA_IDENTITY_DEFAULT:
                if( OidIsValid( relation->rd_pkindex ) )
                    index_oid = relation->rd_pkindex;
                break;
            case REPLICA_IDENTITY_INDEX:
                if( OidIsValid( relation->rd_replidindex ) )
                    index_oid = relation->rd_replidindex;
                break;
            case REPLICA_IDENTITY_FULL:
            case REPLICA_IDENTITY_NOTHING:
            default:
                if( OidIsValid( relation->rd_replidindex ) )
                    index_oid = relation->rd_replidindex;
        }

        if( !OidIsValid( index_oid ) )
        {
            // Last-ditch attempt to find an suitable unique index
            foreach( cell, relation->rd_indexlist )
            {
                oid = ( Oid ) lfirst_oid( cell );

                if( OidIsValid( oid ) )
                {
                    index = index_open( oid, AccessShareLock );

                    if(
                            index->rd_index != NULL
                         && index->rd_index->indisunique
                         && index->rd_index->indimmediate
                         && RelationGetIndexPredicate( index ) == NIL
#if (PG_VERSION_NUM < 12000 )
                         && IndexIsValid( index->rd_index )
#endif
                         && index->rd_rel->relam == BTREE_AM_OID
                         && index->rd_index->indnatts > 0
                      )
                    {
                        index_oid = oid;
                    }

                    index_close( index, NoLock );
                }
            }
        }

        if( !OidIsValid( index_oid ) )
        {
            appendStringInfoString( context->out, "\"ERROR\":\"ERROR\"" );
        }
        else
        {
            // we may need to cache the index entries - though this should be
            // cached already on most databases
            index = index_open( index_oid, ShareLock );

            for( i = 0; i < index->rd_index->indnatts; i++ )
            {
                j = index->rd_index->indkey.values[i];
#if (PG_VERSION_NUM >= 90600 && PG_VERSION_NUM < 90605) \
 || (PG_VERSION_NUM >= 90500 && PG_VERSION_NUM < 90509) \
 || (PG_VERSION_NUM >= 90400 && PG_VERSION_NUM < 90414)
                attribute_form = tuple_descriptor->attrs[j - 1];
#else
                attribute_form = *(TupleDescAttr( tuple_descriptor, j - 1 ));
#endif
                if( i > 0 )
                {
                    appendStringInfoChar( context->out, ',' );
                }

                appendStringInfo(
                    context->out,
                    "\"%s\":",
                    NameStr( attribute_form.attname )
                );

                append_tuple_value(
                    context->out,
                    tuple_descriptor,
                    tuple,
                    j
                );
            }

            index_close( index, NoLock );
        }

        appendStringInfoChar( context->out, '}' );
    }

    if( data->enable_data_write || data->wal_level == PGC_WAL_FULL )
    {
        appendStringInfoString( context->out, ",\"data\":{" );

        if(
               change->action == REORDER_BUFFER_CHANGE_INSERT
            || change->action == REORDER_BUFFER_CHANGE_UPDATE
          )
        {
            appendStringInfoString( context->out, "\"new\":{" );
            append_tuple( context->out, tuple_descriptor, new_tuple );
            appendStringInfoChar( context->out, '}' );
        }

        if(
               (
                   change->action == REORDER_BUFFER_CHANGE_UPDATE
                || change->action == REORDER_BUFFER_CHANGE_DELETE
               )
            && old_tuple != NULL
          )
        {
            if( change->action == REORDER_BUFFER_CHANGE_UPDATE )
            {
                appendStringInfoChar( context->out, ',' );
            }

            appendStringInfoString( context->out, "\"old\":{" );
            append_tuple( context->out, tuple_descriptor, old_tuple );
            appendStringInfoChar( context->out, '}' );
        }

        appendStringInfoChar( context->out, '}' );
    }

    appendStringInfoChar( context->out, '}' );
    data->num_changes++;

    MemoryContextSwitchTo( old_context );
    MemoryContextReset( data->context );

    OutputPluginWrite( context, true );
    return;
}

static void append_tuple_value(
    StringInfo   string,
    TupleDesc    tuple_descriptor,
    HeapTuple    tuple,
    unsigned int index
)
{
    bool                  type_is_variable_length = false;
    bool                  is_null                 = false;
    Oid                   type_output             = {0};
    FormData_pg_attribute attribute_form          = {0};
    Datum                 original_value          = {0};
    Oid                   type_id                 = {0};
    Datum                 value                   = {0};

#if (PG_VERSION_NUM >= 90600 && PG_VERSION_NUM < 90605) \
 || (PG_VERSION_NUM >= 90500 && PG_VERSION_NUM < 90509) \
 || (PG_VERSION_NUM >= 90400 && PG_VERSION_NUM < 90414)
    attribute_form = tuple_descriptor->attrs[index - 1];
#else
    attribute_form = *(TupleDescAttr( tuple_descriptor, index - 1 ));
#endif
    original_value = fastgetattr(
        tuple,
        index,
        tuple_descriptor,
        &is_null
    );

    type_id = attribute_form.atttypid;

    getTypeOutputInfo(
        type_id,
        &type_output,
        &type_is_variable_length
    );

    if( is_null )
    {
        appendStringInfoString( string, "null" );
    }
    else if(
                type_is_variable_length
             && VARATT_IS_EXTERNAL_ONDISK( original_value )
           )
    {
        // May need to de-toast?
    }
    else if( !type_is_variable_length )
    {
        append_literal_value(
            string,
            type_id,
            OidOutputFunctionCall(
                type_output,
                original_value
            )
        );
    }
    else
    {
        value = PointerGetDatum( PG_DETOAST_DATUM( original_value ) );
        append_literal_value(
            string,
            type_id,
            OidOutputFunctionCall(
                type_output,
                value
            )
        );
    }

    return;
}

static void append_literal_value(
    StringInfo string,
    Oid        type_id,
    char *     output
)
{
    const char * value     = NULL;
    char         character = '\0';

    switch( type_id )
    {
        case INT2OID:
        case INT4OID:
        case INT8OID:
        case OIDOID:
        case FLOAT4OID:
        case FLOAT8OID:
        case NUMERICOID:
            appendStringInfoString( string, output );
            break;
        case BITOID:
        case VARBITOID:
            appendStringInfo( string, "\"B'%s'\"", output );
            break;
        case BOOLOID:
            if( strcmp( output, "t" ) == 0 )
            {
                appendStringInfoString( string, "true" );
            }
            else
            {
                appendStringInfoString( string, "false" );
            }

            break;
        default:
            appendStringInfoChar( string, '"' );

            for( value = output; *value; value++ )
            {
                //escape characters
                character = *value;
                if( character == '\n' )
                {
                    appendStringInfoString( string, "\\n" );
                }
                else if( character == '\r' )
                {
                    appendStringInfoString( string, "\\r" );
                }
                else if( character == '\t' )
                {
                    appendStringInfoString( string, "\\t" );
                }
                else if( character == '"' )
                {
                    appendStringInfoString( string, "\\\"" );
                }
                else if( character == '\\' )
                {
                    appendStringInfoString( string, "\\\\" );
                }
                else
                {
                    appendStringInfoChar( string, character );
                }
            }

            appendStringInfoChar( string, '"' );
            break;
    }

    return;
}

static void append_tuple(
    StringInfo string,
    TupleDesc  tuple_descriptor,
    HeapTuple  tuple
)
{
    unsigned int          i              = 0;
    FormData_pg_attribute attribute_form = {0};

    for( i = 0; i < tuple_descriptor->natts; i++ )
    {
#if (PG_VERSION_NUM >= 90600 && PG_VERSION_NUM < 90605) \
 || (PG_VERSION_NUM >= 90500 && PG_VERSION_NUM < 90509) \
 || (PG_VERSION_NUM >= 90400 && PG_VERSION_NUM < 90414)
        attribute_form = tuple_descriptor->attrs[i];
#else
        attribute_form = *(TupleDescAttr( tuple_descriptor, i ));
#endif
        if(
              attribute_form.attisdropped
           || attribute_form.attnum < 0
          )
        {
            continue;
        }

        appendStringInfo(
            string,
            "\"%s\":",
            NameStr(
                attribute_form.attname
            )
        );

        append_tuple_value( string, tuple_descriptor, tuple, i + 1 );

        if( i < ( tuple_descriptor->natts - 1 ) )
        {
            appendStringInfoChar( string, ',' );
        }
    }

    return;
}

static bool config_to_filter_table( char * raw_string, List ** table_list )
{
    char * str_pointer  = NULL;
    char * current_name = NULL;
    char * end_pointer  = NULL;
    char * qual_name    = NULL;
    bool   done         = false;
    List * output       = NIL;

    str_pointer = raw_string;

    while( isspace( *str_pointer ) )
    {
        str_pointer++; // Leading whitespace
    }

    if( *str_pointer == '\0' )
    {
        return false;
    }

    /*
     * Read along a comma-delimited list, replacing the separator (',') with a
     * NULL and strduping the index pointer to a output char *. We also remove
     * any leading or trailing whitespace as we go
     */
    do
    {
        current_name = str_pointer;

        while( *str_pointer && *str_pointer != ',' && !isspace(*str_pointer) )
        {
            if( *str_pointer == '\\' )
            {
                str_pointer++;
            }

            str_pointer++;
        }

        end_pointer = str_pointer;

        if( current_name == str_pointer )
        {
            return false;
        }

        while( isspace( *str_pointer ) )
        {
            str_pointer++; // Trailing whitespace
        }

        if( *str_pointer == ',' )
        {
            str_pointer++;

            while( isspace( *str_pointer ) )
            {
                str_pointer++;
            }
        }
        else if( *str_pointer == '\0' )
        {
            done = true;
        }
        else
        {
            return false;
        }

        *end_pointer = '\0';

        qual_name = pstrdup( current_name );
        output = lappend( output, qual_name );
    } while( !done );

    if( !parse_table_identifier( output, table_list ) )
    {
        return false;
    }

    list_free_deep( output );

    return true;
}

static bool parse_table_identifier( List * qual_tables, List ** table_list )
{
    struct pgc_table * table  = NULL;
    ListCell *         cell   = NULL;
    char *             string = NULL;
    char *             start  = NULL;
    char *             next   = NULL;
    int                length = 0;

    foreach( cell, qual_tables )
    {
        string = ( char * ) lfirst( cell );
        table  = ( struct pgc_table * ) palloc0( sizeof( struct pgc_table ) );

        if( string[0] == '*' && string[1] == '.' )
        {
            table->all_schemas = true;
        }
        else
        {
            table->all_schemas = false;
        }

        start = string;
        next  = string;

        while( *next && *next != '.' )
        {
            if( *next == '\\' )
            {
                memmove( next, next + 1, strlen( next ) );
            }

            next++;
        }

        length = next - start;

        if( *next == '\0' )
        {
            pfree( table );
            return false;
        }
        else
        {
            table->schema_name = ( char * ) palloc0(
                ( length + 1 )
              * sizeof( char )
            );

            strncpy(
                table->schema_name,
                start,
                length
            );

            next++;
            start = next;

            if( start[0] == '*' && start[1] == '\0' )
            {
                table->all_tables = true;
            }
            else
            {
                table->all_tables = false;
            }

            while( *next )
            {
                if( *next == '\\' )
                {
                    memmove( next, next + 1, strlen( next ) );
                }

                next++;
            }

            length = next - start;

            table->table_name = ( char * ) palloc0(
                ( length + 1 )
              * sizeof( char )
            );

            strncpy(
                table->table_name,
                start,
                length
            );
        }

        *table_list = lappend( *table_list, table );
    }

    return true;
}
/*
Datum _hook_set_config_by_name( PG_FUNCTION_ARGS )
{
    char *        name      = NULL;
    char *        value     = NULL;
    char *        new_value = NULL;
    bool          is_local  = false;
    txid          val       = 0;
    // struct: { TransactionId last_xid; uint32 epoch; }
    TxidEpoch     state     = {0};
    TransactionId xid       = {0};

    if( PG_ARGISNULL(0) )
    {
        ereport(
            ERROR,
            (
                errcode( ERRCODE_NULL_VALUE_NOT_ALLOWED ),
                errmsg( "SET requires parameter name" )
            )
        );
    }

    name = TextDatumGetCString( PG_GETARG_DATUM(0) );

    if( PG_ARGISNULL(1) )
    {
        value = NULL;
    }
    else
    {
        value = TextDatumGetCString( PG_GETARG_DATUM(1) );
    }

    if( PG_ARGISNULL(2) )
    {
        is_local = false;
    }
    else
    {
        is_local = PG_GETARG_BOOL(2);
    }

    ( void ) set_config_option(
        name,
        value,
        ( superuser() ? PGC_SUSET : PGC_USERSET ),
        PGC_S_SESSION,
        is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET,
        true,
        0,
        false
    );

    // Hook for pg_ctlbmgr to record session GUCs we're interested in replicating downstream
    if( should_forward_guc_to_wal( name ) )
      // need to check if the guc name is what we're interested in
    { // copied from txid_current()
        PreventCommandDuringRecovery( "txid_current()" );
        GetNextXidAndEpoch( &state->last_xid, &state->epoch );
        xid = GetTopTransactionId();
        // Jacked from txid.c:convert_xid()
        if( !TransactionIdIsNormal( xid ) )
        {
            val = (txid) xid;
        }
        else
        {
            epoch = (uint64) state->epoch;

            if(
                   xid > state->last_xid
                && TransactionIdPreceds( xid, state->last_xid )
              )
            {
                epoch--;
            }
            else if(
                       xid < state->last_xid
                    && TransactionIdFollows( xid, state->last_xid )
                   )
            {
                epoch++;
            }

            val = ( epoch << 32 ) | xid;
        }
        // val will be a uint64
    }

    new_value = GetConfigOptionByName( name, NULL, false );

    PG_RETURN_TEXT_P( cstring_to_text( new_value ) );
}

bool should_foward_guc_to_wal( const char * name )
{

}
*/
