#include "pg_ctblmgr.h"

void _PG_init( void )
{
    // this is a stub for now, we may need to read in GUCs
    // that determine whether minimal JSON records are output or not
    // NOTE: We'll need to set up a queue of guc_change entries in SHM
    // if we are to add any type of EM behavior
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

PG_FUNCTION_INFO_V1( get_parse_tree );

Datum get_parse_tree( PG_FUNCTION_ARGS )
{
    text * sql_in   = PG_GETARG_TEXT_P(0);
    char * sql      = NULL;
    char * out      = NULL;
    text * tree_out = NULL;
    List * tree     = NULL;

    sql = text_to_cstring( sql_in );
    tree = raw_parser( sql
    #if PG_VERSION_NUM >= 14000
    , RAW_PARSE_DEFAULT
    #endif // PG_VERSION_NUM
    );
    out = node_to_json_string( tree );
    tree_out = cstring_to_text( out );

    PG_RETURN_TEXT_P( tree_out );
}

static char * node_to_json_string( void * node )
{
    StringInfoData json;
    initStringInfo( &json );
    Node_out( &json, node );
    return json.data;
}

// Enum parsers
static char * enum_JoinType( JoinType jt )
{
    switch( jt )
    {
        case JOIN_INNER:
            return "INNER";
        case JOIN_LEFT:
            return "LEFT";
        case JOIN_FULL:
            return "FULL";
        case JOIN_RIGHT:
            return "RIGHT";
        case JOIN_SEMI:
            return "SEMI";
        case JOIN_ANTI:
            return "ANTI";
        case JOIN_UNIQUE_OUTER:
            return "UNIQUE_OUTER";
        case JOIN_UNIQUE_INNER:
            return "UNIQUE_INNER";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_CoercionForm( CoercionForm cf )
{
    switch( cf )
    {
        case COERCE_EXPLICIT_CALL:
            return "EXPLICIT_CALL";
        case COERCE_EXPLICIT_CAST:
            return "EXPLICIT_CAST";
        case COERCE_IMPLICIT_CAST:
            return "IMPLICIT_CAST";
        case COERCE_SQL_SYNTAX:
            return "SQL_SYNTAX";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_SortByNulls( SortByNulls sbn )
{
    switch( sbn )
    {
        case SORTBY_NULLS_DEFAULT:
            return "DEFAULT";
        case SORTBY_NULLS_FIRST:
            return "FIRST";
        case SORTBY_NULLS_LAST:
            return "LAST";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_SortByDir( SortByDir sbd )
{
    switch( sbd )
    {
        case SORTBY_DEFAULT:
            return "DEFAULT";
        case SORTBY_ASC:
            return "ASC";
        case SORTBY_DESC:
            return "DESC";
        case SORTBY_USING:
            return "USING";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_SubLinkType( SubLinkType slt )
{
    switch( slt )
    {
        case EXISTS_SUBLINK:
            return "EXISTS";
        case ALL_SUBLINK:
            return "ALL";
        case ANY_SUBLINK:
            return "ANY";
        case ROWCOMPARE_SUBLINK:
            return "ROWCOMPARE";
        case EXPR_SUBLINK:
            return "EXPR";
        case MULTIEXPR_SUBLINK:
            return "MUILTIEXPR";
        case ARRAY_SUBLINK:
            return "ARRAY";
        case CTE_SUBLINK:
            return "CTE";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_OnCommitAction( OnCommitAction oca )
{
    switch( oca )
    {
        case ONCOMMIT_NOOP:
            return "NOOP";
        case ONCOMMIT_PRESERVE_ROWS:
            return "PRESERVE_ROWS";
        case ONCOMMIT_DELETE_ROWS:
            return "DELETE_ROWS";
        case ONCOMMIT_DROP:
            return "DROP";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_DefElemAction( DefElemAction dea )
{
    switch( dea )
    {
        case DEFELEM_UNSPEC:
            return "UNSPEC"; // Unspecified
        case DEFELEM_SET:
            return "SET";
        case DEFELEM_ADD:
            return "ADD";
        case DEFELEM_DROP:
            return "DROP";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_CmdType( CmdType ct )
{
    switch( ct )
    {
        case CMD_UNKNOWN:
            return "UNKNOWN";
        case CMD_SELECT:
            return "SELECT";
        case CMD_UPDATE:
            return "UPDATE";
        case CMD_INSERT:
            return "INSERT";
        case CMD_DELETE:
            return "DELETE";
        case CMD_UTILITY: // create, destroy, copy, vacuum, etc
            return "UTILITY";
        case CMD_NOTHING:
            return "NOTHING";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_MinMaxOp( MinMaxOp mmo )
{
    switch( mmo )
    {
        case IS_GREATEST:
            return "GREATEST";
        case IS_LEAST:
            return "LEAST";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_RTEKind( RTEKind rk )
{
    switch( rk )
    {
        case RTE_RELATION:
            return "RELATION";
        case RTE_SUBQUERY:
            return "SUBQUERY";
        case RTE_JOIN:
            return "JOIN";
        case RTE_FUNCTION:
            return "FUNCTION";
        case RTE_TABLEFUNC:
            return "TABLEFUNC";
        case RTE_VALUES:
            return "VALUES";
        case RTE_CTE:
            return "CTE";
        case RTE_NAMEDTUPLESTORE:
            return "NAMEDTUPLESTORE";
        case RTE_RESULT:
            return "RESULT";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_ScanDirection( ScanDirection sd )
{
    switch( sd )
    {
        case BackwardScanDirection:
            return "BACKWARD";
        case NoMovementScanDirection:
            return "NOMOVEMENT";
        case ForwardScanDirection:
            return "FORWARD";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_NullTestType( NullTestType ntt )
{
    switch( ntt )
    {
        case IS_NULL:
            return "IS_NULL";
        case IS_NOT_NULL:
            return "IS_NOT_NULL";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_AggStrategy( AggStrategy as )
{
    switch( as )
    {
        case AGG_PLAIN:
            return "PLAIN";
        case AGG_SORTED:
            return "SORTED";
        case AGG_HASHED:
            return "HASHED";
        case AGG_MIXED:
            return "MIXED";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_BoolTestType( BoolTestType btt )
{
    switch( btt )
    {
        case IS_TRUE:
            return "IS_TRUE";
        case IS_NOT_TRUE:
            return "IS_NOT_TRUE";
        case IS_FALSE:
            return "IS_FALSE";
        case IS_NOT_FALSE:
            return "IS_NOT_FALSE";
        case IS_UNKNOWN:
            return "IS_UNKNOWN";
        case IS_NOT_UNKNOWN:
            return "IS_NOT_UNKNOWN";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_ParamKind( ParamKind pk )
{
    switch( pk )
    {
        case PARAM_EXTERN:
            return "EXTERN";
        case PARAM_EXEC:
            return "EXEC";
        case PARAM_SUBLINK:
            return "SUBLINK";
        case PARAM_MULTIEXPR:
            return "MULTIEXPR";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

static char * enum_NodeTag( NodeTag nt )
{

    switch( nt )
    {
        case T_Invalid:
            return "INVALID";
        // Tags for executor nodes
        case T_IndexInfo:
            return "IndexInfo";
        case T_ExprContext:
            return "ExprContext";
        case T_ProjectionInfo:
            return "ProjectionInfo";
        case T_JunkFilter:
            return "JunkFilter";
        case T_OnConflictSetState:
            return "OnConflictSetState";
        case T_ResultRelInfo:
            return "ResultRelInfo";
        case T_EState:
            return "EState";
        case T_TupleTableSlot:
            return "TupleTableSlot";
        // Tags for plan nodes
        case T_Plan:
            return "Plan";
        case T_Result:
            return "Result";
        case T_ProjectSet:
            return "ProjectSet";
        case T_ModifyTable:
            return "ModifyTable";
        case T_Append:
            return "Append";
        case T_MergeAppend:
            return "MergeAppend";
        case T_RecursiveUnion:
            return "RecursiveUnion";
        case T_BitmapAnd:
            return "BitmapAnd";
        case T_BitmapOr:
            return "BitmapOr";
        case T_Scan:
            return "Scan";
        case T_SeqScan:
            return "SeqScan";
        case T_SampleScan:
            return "SampleScan";
        case T_IndexScan:
            return "IndexScan";
        case T_IndexOnlyScan:
            return "IndexOnlyScan";
        case T_BitmapIndexScan:
            return "BitmapIndexScan";
        case T_BitmapHeapScan:
            return "BitmapHeapScan";
        case T_TidScan:
            return "TidScan";
        case T_TidRangeScan:
            return "TidRangeScan";
        case T_SubqueryScan:
            return "SubqueryScan";
        case T_FunctionScan:
            return "FunctionScan";
        case T_ValuesScan:
            return "ValuesScan";
        case T_TableFuncScan:
            return "TableFuncScan";
        case T_CteScan:
            return "CteScan";
        case T_NamedTuplestoreScan:
            return "NamedTuplestoreScan";
        case T_WorkTableScan:
            return "WorkTableScan";
        case T_ForeignScan:
            return "ForeignScan";
        case T_CustomScan:
            return "CustomScan";
        case T_Join:
            return "Join";
        case T_NestLoop:
            return "NestLoop";
        case T_MergeJoin:
            return "MergeJoin";
        case T_HashJoin:
            return "HashJoin";
        case T_Material:
            return "Material";
        case T_Memoize:
            return "Memoize";
        case T_Sort:
            return "Sort";
        case T_IncrementalSort:
            return "IncrementalSort";
        case T_Group:
            return "Group";
        case T_Agg:
            return "Agg";
        case T_WindowAgg:
            return "WindowAgg";
        case T_Unique:
            return "Unique";
        case T_Gather:
            return "Gather";
        case T_GatherMerge:
            return "GatherMerge";
        case T_Hash:
            return "Hash";
        case T_SetOp:
            return "SetOp";
        case T_LockRows:
            return "LockRows";
        case T_Limit:
            return "Limit";
        case T_NestLoopParam:
            return "NestLoopParam";
        case T_PlanRowMark:
            return "PlanRowMark";
        case T_PartitionPruneInfo:
            return "PartitionPruneInfo";
        case T_PartitionedRelPruneInfo:
            return "PartitionRelPruneInfo";
        case T_PartitionPruneStepOp:
            return "PartitionPruneStepOp";
        case T_PartitionPruneStepCombine:
            return "PartitionPruneStepCombine";
        case T_PlanInvalItem:
            return "PlanInvalItem";
        // Skip exec nodes
        // Primitive nodes
        case T_Alias:
            return "Alias";
        case T_RangeVar:
            return "RangeVar";
        case T_TableFunc:
            return "TableFunc";
        case T_Expr:
            return "Expr";
        case T_Var:
            return "Var";
        case T_Const:
            return "Const";
        case T_Param:
            return "Param";
        case T_Aggref:
            return "Aggref";
        case T_GroupingFunc:
            return "GroupingFunc";
        case T_WindowFunc:
            return "WindowFunc";
        case T_SubscriptingRef:
            return "SubscriptingRef";
        case T_FuncExpr:
            return "FuncExpr";
        case T_NamedArgExpr:
            return "NamedArgExpr";
        case T_OpExpr:
            return "OpExpr";
        case T_DistinctExpr:
            return "DistinctExpr";
        case T_NullIfExpr:
            return "NullIfExpr";
        case T_ScalarArrayOpExpr:
            return "ScalarArrayOpExpr";
        case T_BoolExpr:
            return "BoolExpr";
        case T_SubLink:
            return "SubLink";
        case T_SubPlan:
            return "SubPlan";
        case T_AlternativeSubPlan:
            return "AlternativeSubPlan";
        case T_FieldSelect:
            return "FieldSelect";
        case T_FieldStore:
            return "FieldStore";
        case T_RelabelType:
            return "RelabelType";
        case T_CoerceViaIO:
            return "CoerceViaIO";
        case T_ArrayCoerceExpr:
            return "ArrayCoerceExpr";
        case T_ConvertRowtypeExpr:
            return "ConvertRowtypeExpr";
        case T_CollateExpr:
            return "CollateExpr";
        case T_CaseExpr:
            return "CaseExpr";
        case T_CaseWhen:
            return "CaseWhen";
        case T_CaseTestExpr:
            return "CaseTestExpr";
        case T_ArrayExpr:
            return "ArrayExpr";
        case T_RowExpr:
            return "RowExpr";
        case T_RowCompareExpr:
            return "RowCompareExpr";
        case T_CoalesceExpr:
            return "CoalesceExpr";
        case T_MinMaxExpr:
            return "MinMaxExpr";
        case T_SQLValueFunction:
            return "SQLValueFunction";
        case T_XmlExpr:
            return "XmlExpr";
        case T_NullTest:
            return "NullTest";
        case T_BooleanTest:
            return "BooleanTest";
        case T_CoerceToDomain:
            return "CoerceToDomain";
        case T_CoerceToDomainValue:
            return "CoerceToDomainValue";
        case T_SetToDefault:
            return "SetToDefault";
        case T_CurrentOfExpr:
            return "CurrentOfExpr";
        case T_NextValueExpr:
            return "NextValueExpr";
        case T_InferenceElem:
            return "InferenceElem";
        case T_TargetEntry:
            return "TargetEntry";
        case T_RangeTblRef:
            return "RangeTblRef";
        case T_JoinExpr:
            return "JoinExpr";
        case T_FromExpr:
            return "FromExpr";
        case T_OnConflictExpr:
            return "OnConflictExpr";
        case T_IntoClause:
            return "IntoClause";
        // Expression State Nodes
        case T_ExprState:
            return "ExprState";
        case T_WindowFuncExprState:
            return "WindowFuncExprState";
        case T_SetExprState:
            return "SetExprState";
        case T_SubPlanState:
            return "SubPlanState";
        case T_DomainConstraintState:
            return "DomainConstraintState";
        // Planner nodes
        case T_PlannerInfo:
            return "PlannerInfo";
        case T_PlannerGlobal:
            return "PlannerGlobal";
        case T_RelOptInfo:
            return "RelOptInfo";
        case T_IndexOptInfo:
            return "IndexOptInfo";
        case T_ForeignKeyOptInfo:
            return "ForeignKeyOptInfo";
        case T_ParamPathInfo:
            return "ParamPathInfo";
        case T_Path:
            return "Path";
        case T_IndexPath:
            return "IndexPath";
        case T_BitmapHeapPath:
            return "BitmapHeapPath";
        case T_BitmapAndPath:
            return "BitmapAndPath";
        case T_BitmapOrPath:
            return "BitmapOrPath";
        case T_TidPath:
            return "TidPath";
        case T_TidRangePath:
            return "TidRangePath";
        case T_SubqueryScanPath:
            return "SubqueryScanPath";
        case T_ForeignPath:
            return "ForeignPath";
        case T_CustomPath:
            return "CustomPath";
        case T_NestPath:
            return "NestPath";
        case T_MergePath:
            return "MergePath";
        case T_HashPath:
            return "HashPath";
        case T_AppendPath:
            return "AppendPath";
        case T_MergeAppendPath:
            return "MergeAppendPath";
        case T_GroupResultPath:
            return "GroupResultPath";
        case T_MaterialPath:
            return "MaterialPath";
        case T_MemoizePath:
            return "MemoizePath";
        case T_UniquePath:
            return "UniquePath";
        case T_GatherPath:
            return "GatherPath";
        case T_GatherMergePath:
            return "GatherMergePath";
        case T_ProjectionPath:
            return "ProjectionPath";
        case T_ProjectSetPath:
            return "ProjectSetPath";
        case T_SortPath:
            return "SortPath";
        case T_IncrementalSortPath:
            return "IncrementalSortPath";
        case T_GroupPath:
            return "GroupPath";
        case T_UpperUniquePath:
            return "UpperUniquePath";
        case T_AggPath:
            return "AggPath";
        case T_GroupingSetsPath:
            return "GroupingSetsPath";
        case T_MinMaxAggPath:
            return "MinMaxAggPath";
        case T_WindowAggPath:
            return "WindowAggPath";
        case T_SetOpPath:
            return "SetOpPath";
        case T_RecursiveUnionPath:
            return "RecursiveUnionPath";
        case T_LockRowsPath:
            return "LockRowsPath";
        case T_ModifyTablePath:
            return "ModifyTablePath";
        case T_LimitPath:
            return "LimitPath";
        case T_EquivalenceClass:
            return "EquivalenceClass";
        case T_EquivalenceMember:
            return "EquivalenceMember";
        case T_PathKey:
            return "PathKey";
        case T_RestrictInfo:
            return "RestrictInfo";
        case T_IndexClause:
            return "IndexClause";
        case T_PlaceHolderVar:
            return "PlaceHolderVar";
        case T_SpecialJoinInfo:
            return "SpecialJoinInfo";
        case T_AppendRelInfo:
            return "AppendRelInfo";
        case T_RowIdentityVarInfo:
            return "RowIdentityVarInfo";
        case T_PlaceHolderInfo:
            return "PlaceHolderInfo";
        case T_MinMaxAggInfo:
            return "MinMaxAggInfo";
        case T_PlannerParamItem:
            return "PlannerParamItem";
        case T_RollupData:
            return "RollupData";
        case T_GroupingSetData:
            return "GroupingSetData";
        case T_StatisticExtInfo:
            return "StatisticExtInfo";
        // Memory node tags
        case T_MemoryContext:
            return "MemoryContext";
        case T_AllocSetContext:
            return "AllocSetContext";
        case T_SlabContext:
            return "SlabContext";
        case T_GenerationContext:
            return "GenerationContext";
        // Value nodes
        case T_Value:
            return "Value";
        case T_Integer:
            return "Integer";
        case T_Float:
            return "Float";
        case T_String:
            return "String";
        case T_BitString:
            return "BitString";
        case T_Null:
            return "Null";
        // List nodes
        case T_List:
            return "List";
        case T_IntList:
            return "IntList";
        case T_OidList:
            return "OidList";
        // Extensible Nodes
        case T_ExtensibleNode:
            return "ExtensibleNode";
        // Statement Nodes
        case T_RawStmt:
            return "RawStmt";
        case T_Query:
            return "Query";
        case T_PlannedStmt:
            return "PlannedStmt";
        case T_InsertStmt:
            return "InsertStmt";
        case T_DeleteStmt:
            return "DeleteStmt";
        case T_UpdateStmt:
            return "UpdateStmt";
        case T_SelectStmt:
            return "SelectStmt";
        case T_ReturnStmt:
            return "ReturnStmt";
        case T_PLAssignStmt:
            return "PLAssignStmt";
        case T_AlterTableStmt:
            return "AlterTableStmt";
        case T_AlterTableCmd:
            return "AlterTableCmd";
        case T_AlterDomainStmt:
            return "AlterDomainStmt";
        case T_SetOperationStmt:
            return "SetOperationStmt";
        case T_GrantStmt:
            return "GrantStmt";
        case T_GrantRoleStmt:
            return "GrantRoleStmt";
        case T_AlterDefaultPrivilegesStmt:
            return "AlterDefaultPrivilegesStmt";
        case T_ClosePortalStmt:
            return "ClosePortalStmt";
        case T_ClusterStmt:
            return "ClusterStmt";
        case T_CopyStmt:
            return "CopyStmt";
        case T_CreateStmt:
            return "CreateStmt";
        case T_DefineStmt:
            return "DefineStmt";
        case T_DropStmt:
            return "DropStmt";
        case T_TruncateStmt:
            return "TruncateStmt";
        case T_CommentStmt:
            return "CommentStmt";
        case T_FetchStmt:
            return "FetchStmt";
        case T_IndexStmt:
            return "IndexStmt";
        case T_CreateFunctionStmt:
            return "CreateFunctionStmt";
        case T_AlterFunctionStmt:
            return "AlterFunctionStmt";
        case T_DoStmt:
            return "DoStmt";
        case T_RenameStmt:
            return "RenameStmt";
        case T_RuleStmt:
            return "RuleStmt";
        case T_NotifyStmt:
            return "NotifyStmt";
        case T_ListenStmt:
            return "ListenStmt";
        case T_UnlistenStmt:
            return "UnlistenStmt";
        case T_TransactionStmt:
            return "TransactionStmt";
        case T_ViewStmt:
            return "ViewStmt";
        case T_LoadStmt:
            return "LoadStmt";
        case T_CreateDomainStmt:
            return "CreateDomainStmt";
        case T_CreatedbStmt:
            return "CreatedbStmt";
        case T_DropdbStmt:
            return "DropdbStmt";
        case T_VacuumStmt:
            return "VacuumStmt";
        case T_ExplainStmt:
            return "ExplainStmt";
        case T_CreateTableAsStmt:
            return "CreateTableAsStmt";
        case T_CreateSeqStmt:
            return "CreateSeqStmt";
        case T_AlterSeqStmt:
            return "AlterSeqStatement";
        case T_VariableSetStmt:
            return "VariableSetStmt";
        case T_VariableShowStmt:
            return "VariableShowStmt";
        case T_DiscardStmt:
            return "DiscardStmt";
        case T_CreateTrigStmt:
            return "CreateTrigStmt";
        case T_CreatePLangStmt:
            return "CreatePLangStmt";
        case T_CreateRoleStmt:
            return "CreateRoleStmt";
        case T_AlterRoleStmt:
            return "AlterRoleStmt";
        case T_DropRoleStmt:
            return "DropRoleStmt";
        case T_LockStmt:
            return "LockStmt";
        case T_ConstraintsSetStmt:
            return "ConstraintsSetStmt";
        case T_ReindexStmt:
            return "ReindexStmt";
        case T_CheckPointStmt:
            return "CheckPointStmt";
        case T_CreateSchemaStmt:
            return "CreateSchemaStmt";
        case T_AlterDatabaseStmt:
            return "AlterDatabaseStmt";
        case T_AlterDatabaseSetStmt:
            return "AlterDatabaseSetStmt";
        case T_AlterRoleSetStmt:
            return "AlterRoleSetStmt";
        case T_CreateConversionStmt:
            return "CreateConversionStmt";
        case T_CreateCastStmt:
            return "CreateCastStmt";
        case T_CreateOpClassStmt:
            return "CreateOpClassStmt";
        case T_CreateOpFamilyStmt:
            return "CreateOpFamilyStmt";
        case T_AlterOpFamilyStmt:
            return "AlterOpFamilyStmt";
        case T_PrepareStmt:
            return "PrepareStmt";
        case T_ExecuteStmt:
            return "ExecuteStmt";
        case T_DeallocateStmt:
            return "DeallocateStmt";
        case T_DeclareCursorStmt:
            return "DeclareCursorStmt";
        case T_CreateTableSpaceStmt:
            return "CreateTableSpaceStmt";
        case T_DropTableSpaceStmt:
            return "DropTableSpaceStmt";
        case T_AlterObjectDependsStmt:
            return "AlterObjectDependsStmt";
        case T_AlterObjectSchemaStmt:
            return "AlterObjectSchemaStmt";
        case T_AlterOwnerStmt:
            return "AlterOwnerStmt";
        case T_AlterOperatorStmt:
            return "AlterOperatorStmt";
        case T_AlterTypeStmt:
            return "AlterTypeStmt";
        case T_DropOwnedStmt:
            return "DropOwnedStmt";
        case T_ReassignOwnedStmt:
            return "ReassignOwnedStmt";
        case T_CompositeTypeStmt:
            return "CompositeTypeStmt";
        case T_CreateEnumStmt:
            return "CreateEnumStmt";
        case T_CreateRangeStmt:
            return "CreateRangeStmt";
        case T_AlterEnumStmt:
            return "AlterEnumStmt";
        case T_AlterTSDictionaryStmt:
            return "AlterTSDictionaryStmt";
        case T_AlterTSConfigurationStmt:
            return "AlterTSConfigurationStmt";
        case T_CreateFdwStmt:
            return "CreateFdwStmt";
        case T_AlterFdwStmt:
            return "AlterFdwStmt";
        case T_CreateForeignServerStmt:
            return "CreateForeignServerStmt";
        case T_AlterForeignServerStmt:
            return "AlterForeignServerStmt";
        case T_CreateUserMappingStmt:
            return "CreateUserMappingStmt";
        case T_AlterUserMappingStmt:
            return "AlterUserMappingStmt";
        case T_DropUserMappingStmt:
            return "DropUserMappingStmt";
        case T_AlterTableSpaceOptionsStmt:
            return "AlterTableSpaceOptionsStmt";
        case T_AlterTableMoveAllStmt:
            return "AlterTableMoveAllStmt";
        case T_SecLabelStmt:
            return "SetLabelStmt";
        case T_CreateForeignTableStmt:
            return "CreateForeignTableStmt";
        case T_ImportForeignSchemaStmt:
            return "ImportForeignSchemaStmt";
        case T_CreateExtensionStmt:
            return "CreateExtensionStmt";
        case T_AlterExtensionStmt:
            return "AlterExtensionStmt";
        case T_AlterExtensionContentsStmt:
            return "AlterExtensionContentsStmt";
        case T_CreateEventTrigStmt:
            return "CreateEventTrigStmt";
        case T_AlterEventTrigStmt:
            return "AlterEventTrigStmt";
        case T_RefreshMatViewStmt:
            return "RefreshMatViewStmt";
        case T_ReplicaIdentityStmt:
            return "ReplicaIdentityStmt";
        case T_AlterSystemStmt:
            return "T_AlterSystemStmt";
        case T_CreateTransformStmt:
            return "CreateTransformStmt";
        case T_CreateAmStmt:
            return "CreateAmStmt";
        case T_CreatePublicationStmt:
            return "CreatePublicationStmt";
        case T_AlterPublicationStmt:
            return "AlterPublicationStmt";
        case T_CreateSubscriptionStmt:
            return "CreateSubscriptionStmt";
        case T_AlterSubscriptionStmt:
            return "AlterSubscriptionStmt";
        case T_DropSubscriptionStmt:
            return "DropSubscriptionStmt";
        case T_CreateStatsStmt:
            return "CreateStatsStmt";
        case T_AlterCollationStmt:
            return "AlterCollationStmt";
        case T_CallStmt:
            return "CallStmt";
        case T_AlterStatsStmt:
            return "AlterStatsStmt";
        // Parse Tree Nodes
        case T_A_Expr:
            return "T_A_Expr";
        case T_ColumnRef:
            return "ColumnRef";
        case T_ParamRef:
            return "ParamRef";
        case T_A_Const:
            return "A_Const";
        case T_FuncCall:
            return "FuncCall";
        case T_A_Star:
            return "A_Star";
        case T_A_Indices:
            return "A_Indices";
        case T_A_Indirection:
            return "A_Indirection";
        case T_A_ArrayExpr:
            return "A_ArrayExpr";
        case T_ResTarget:
            return "ResTarget";
        case T_MultiAssignRef:
            return "MultiAssignRef";
        case T_TypeCast:
            return "TypeCast";
        case T_CollateClause:
            return "CollateClause";
        case T_SortBy:
            return "SortBy";
        case T_WindowDef:
            return "WindowDef";
        case T_RangeSubselect:
            return "RangeSubselect";
        case T_RangeFunction:
            return "RangeFunction";
        case T_RangeTableSample:
            return "RangeTableSample";
        case T_RangeTableFunc:
            return "RangeTableFunc";
        case T_RangeTableFuncCol:
            return "RangeTableFuncCol";
        case T_TypeName:
            return "TypeName";
        case T_ColumnDef:
            return "ColumnDef";
        case T_IndexElem:
            return "IndexElem";
        case T_StatsElem:
            return "StatsElem";
        case T_Constraint:
            return "Constraint";
        case T_DefElem:
            return "DefElem";
        case T_RangeTblEntry:
            return "RangeTblEntry";
        case T_RangeTblFunction:
            return "RangeTblFunction";
        case T_TableSampleClause:
            return "TableSampleClause";
        case T_WithCheckOption:
            return "WithCheckOption";
        case T_SortGroupClause:
            return "SortGroupClause";
        case T_GroupingSet:
            return "GroupingSet";
        case T_WindowClause:
            return "WindowClause";
        case T_ObjectWithArgs:
            return "ObjectWithArgs";
        case T_AccessPriv:
            return "AccessPriv";
        case T_CreateOpClassItem:
            return "CreateOpClassItem";
        case T_TableLikeClause:
            return "TableLikeClause";
        case T_FunctionParameter:
            return "FunctionParameter";
        case T_LockingClause:
            return "LockingClause";
        case T_RowMarkClause:
            return "RowMarkClause";
        case T_XmlSerialize:
            return "XmlSerialize";
        case T_WithClause:
            return "WithClause";
        case T_InferClause:
            return "InferClause";
        case T_OnConflictClause:
            return "OnConflictClause";
        case T_CTESearchClause:
            return "CTESearchClause";
        case T_CTECycleClause:
            return "CTECycleClause";
        case T_CommonTableExpr:
            return "CommonTableExpr";
        case T_RoleSpec:
            return "RoleSpec";
        case T_TriggerTransition:
            return "TriggerTransition";
        case T_PartitionElem:
            return "PartitionElem";
        case T_PartitionSpec:
            return "PartitionSpec";
        case T_PartitionBoundSpec:
            return "PartitionBoundSpec";
        case T_PartitionRangeDatum:
            return "PartitionRangeDatum";
        case T_PartitionCmd:
            return "PartitionCmd";
        case T_VacuumRelation:
            return "VacuumRelation";
        default:
            return "UNKNOWN";
    }

    return NULL;
}

// Node -> JSON fragment helpers
static void Aggref_out( StringInfo str, Aggref * node )
{
    appendStringInfoString( str, "\"name\":\"AGGREF\"" );
    appendStringInfo( str, ",\"aggfnoid\":%u", node->aggfnoid );
    appendStringInfo( str, ",\"aggtype\":%u", node->aggtype );
    appendStringInfo( str, ",\"aggcollid\":%u", node->aggcollid );
    appendStringInfo( str, ",\"inputcollid\":%u", node->inputcollid );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"aggorder\":" );
    Node_out( str, node->aggorder );
    appendStringInfo( str, ",\"aggdistinct\":" );
    Node_out( str, node->aggdistinct );
    appendStringInfo( str, ",\"aggstar\":%s", node->aggstar ? "true" : "false" );
    appendStringInfo( str, ",\"agglevelsup\":%u", node->agglevelsup );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void Agg_out( StringInfo str, Agg * node )
{
    int i     = 0;
    int first = 1;

    appendStringInfoString( str, "\"name\":\"AGG\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"aggstrategy\":\"%s\"", enum_AggStrategy( node->aggstrategy ) );
    appendStringInfo( str, ",\"numCols\":%d", node->numCols );
    appendStringInfo( str, ",\"grpColIdx\":[");

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%d", node->grpColIdx[i] );
    }

    appendStringInfo(str, "],\"grpOperators\":" );
    first = 1;

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->grpOperators[i] );
    }

    appendStringInfo( str, "],\"numGroups\":%ld", node->numGroups );
    return;
}

static void Alias_out( StringInfo str, Alias * node )
{
    appendStringInfoString( str, "\"name\":\"ALIAS\"" );
    appendStringInfo( str, ",\"aliasname\":" );
    Token_out( str, node->aliasname );

    if( node->colnames )
    {
        appendStringInfo( str, ",\"colnames\":" );
        Node_out( str, node->colnames );
    }
    return;
}

static void AlternativeSubPlan_out( StringInfo str, AlternativeSubPlan * node )
{
    appendStringInfoString( str, "\"name\":\"ALTERNATIVESUBPLAN\"" );
    appendStringInfo( str, ",\"subplans\":" );
    Node_out( str, node->subplans );
    return;
}

static void AppendPath_out( StringInfo str, AppendPath * node )
{
    appendStringInfoString( str, "\"name\":\"APPENDPATH\"" );
    PathInfo_out( str, ( Path * ) node );
    appendStringInfo( str, ",\"subpaths\":" );
    Node_out( str, node->subpaths );
    return;
}

static void AppendRelInfo_out( StringInfo str, AppendRelInfo * node )
{
    appendStringInfoString( str, "\"name\":\"APPENDRELINFO\"" );
    appendStringInfo( str, ",\"parent_relid\":%u", node->parent_relid );
    appendStringInfo( str, ",\"child_relid\":%u", node->child_relid );
    appendStringInfo( str, ",\"parent_reltype\":%u", node->parent_reltype );
    appendStringInfo( str, ",\"child_reltype\":%u", node->child_reltype );
    appendStringInfo( str, ",\"translated_vars\":" );
    Node_out( str, node->translated_vars );
    appendStringInfo( str, ",\"parent_reloid\":%u", node->parent_reloid );
    return;
}

static void Append_out( StringInfo str, Append * node )
{
    appendStringInfoString( str, "\"name\":\"APPEND\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"appendplans\":" );
    Node_out( str, node->appendplans );
    return;
}

static void ArrayCoerceExpr_out( StringInfo str, ArrayCoerceExpr * node )
{
    appendStringInfoString( str, "\"name\":\"ARRAYCOERCEEXPR\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"elemexpr\":" );
    Node_out( str, node->elemexpr );
    //appendStringInfo( str, ",\"elemfuncid\":%u", node->elemfuncid );
    appendStringInfo( str, ",\"resulttype\":%u", node->resulttype );
    appendStringInfo( str, ",\"resulttypmod\":%d", node->resulttypmod );
    appendStringInfo( str, ",\"resultcollid\":%u", node->resultcollid );
    appendStringInfo( str, ",\"coerceformat\":\"%s\"", enum_CoercionForm( node->coerceformat ) );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void ArrayExpr_out( StringInfo str, ArrayExpr * node )
{
    appendStringInfoString( str, "\"name\":\"ARRAY\"" );
    appendStringInfo( str, ",\"array_typeid\":%u", node->array_typeid );
    appendStringInfo( str, ",\"array_collid\":%u", node->array_collid );
    appendStringInfo( str, ",\"element_typeid\":%u", node->element_typeid );
    appendStringInfo( str, ",\"elements\":" );
    Node_out( str, node->elements );
    appendStringInfo( str, ",\"multidims\":%s", node->multidims ? "true" : "false" );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void A_ArrayExpr_out( StringInfo str, A_ArrayExpr * node )
{
    appendStringInfoString( str, "\"name\":\"A_ARRAYEXPR\"" );
    appendStringInfo( str, ",\"elements\":" );
    Node_out( str, node->elements );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void A_Const_out( StringInfo str, A_Const * node )
{
    appendStringInfoString( str, "\"name\":\"A_CONST\"" );
    appendStringInfo(str, ",\"val\":");
    Value_out( str, &(node->val) );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void A_Expr_out( StringInfo str, A_Expr * node )
{
    appendStringInfoString( str, "\"name\":\"AEXPR" );

    switch( node->kind )
    {
        case AEXPR_OP:
            appendStringInfo(str, "_OP\"" );
            appendStringInfo( str, ",\"name\":" );
            Node_out( str, node->name );
            break;
        case AEXPR_OP_ANY:
            appendStringInfo(str, "_OP_ANY\"" );
            appendStringInfo( str, ",\"name\":" );
            Node_out( str, node->name );
            break;
        case AEXPR_OP_ALL:
            appendStringInfo(str, "_OP_ALL\"" );
            appendStringInfo( str, ",\"name\":" );
            Node_out( str, node->name );
            break;
        case AEXPR_DISTINCT:
            appendStringInfo(str, "_DISTINCT\"" );
            appendStringInfo( str, ",\"name\":" );
            Node_out( str, node->name );
            break;
        case AEXPR_NOT_DISTINCT:
            appendStringInfo(str, "_NOT_DISTINCT\"" );
            break;
        case AEXPR_NULLIF:
            appendStringInfo(str, "_NULLIF\"" );
            break;
        case AEXPR_IN:
            appendStringInfo( str, "_IN\"" );
            appendStringInfo( str, ",\"name\":" );
            Node_out( str, node->name );
            break;
        case AEXPR_LIKE:
            appendStringInfo(str, "_LIKE\"" );
            break;
        case AEXPR_ILIKE:
            appendStringInfo(str, "_ILIKE\"" );
            break;
        case AEXPR_SIMILAR:
            appendStringInfo(str, "_SIMILAR\"" );
            break;
        case AEXPR_BETWEEN:
            appendStringInfo(str, "_BETWEEN\"" );
            break;
        case AEXPR_NOT_BETWEEN:
            appendStringInfo( str, "_NOT_BETWEEN\"" );
            break;
        case AEXPR_BETWEEN_SYM:
            appendStringInfo( str, "_BETWEEN_SYM\"" );
            break;
        case AEXPR_NOT_BETWEEN_SYM:
            appendStringInfo( str, "_NOT_BETWEEN_SYM\"" );
            break;
        default:
            appendStringInfo(str, "_UNKNOWN\"" );
            break;
    }

    appendStringInfo( str, ",\"lexpr\":" );
    Node_out( str, node->lexpr );
    appendStringInfo( str, ",\"rexpr\":" );
    Node_out( str, node->rexpr );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void A_Indices_out( StringInfo str, A_Indices * node )
{
    appendStringInfoString( str, "\"name\":\"A_INDICES\"" );
    appendStringInfo( str, ",\"lidx\":" );
    Node_out( str, node->lidx );
    appendStringInfo( str, ",\"uidx\":" );
    Node_out( str, node->uidx );
    return;
}

static void A_Indirection_out( StringInfo str, A_Indirection * node )
{
    appendStringInfoString( str, "\"name\":\"A_INDIRECTION\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"indirection\":" );
    Node_out( str, node->indirection );
    return;
}

static void A_Star_out( StringInfo str, A_Star * node )
{
    appendStringInfoString( str, "\"name\":\"A_STAR\"" );
    return;
}

static void BitmapAndPath_out( StringInfo str, BitmapAndPath * node )
{
    appendStringInfoString( str, "\"name\":\"BITMAPANDPATH\"" );
    PathInfo_out( str, ( Path * ) node );
    appendStringInfo( str, ",\"bitmapquals\":" );
    Node_out( str, node->bitmapquals );
    appendStringInfo( str, ",\"bitmapselectivity\":%.4f", node->bitmapselectivity );
    return;
}

static void BitmapAnd_out( StringInfo str, BitmapAnd * node )
{
    appendStringInfoString( str, "\"name\":\"BITMAPAND\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"bitmapplans\":" );
    Node_out( str, node->bitmapplans );
    return;
}

static void BitmapHeapPath_out( StringInfo str, BitmapHeapPath * node )
{
    appendStringInfoString( str, "\"name\":\"BITMAPHEAPPATH\"" );
    PathInfo_out( str, ( Path * ) node );
    appendStringInfo( str, ",\"bitmapqual\":" );
    Node_out( str, node->bitmapqual );
    return;
}

static void BitmapHeapScan_out( StringInfo str, BitmapHeapScan * node )
{
    appendStringInfoString( str, "\"name\":\"BITMAPHEAPSCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    appendStringInfo( str, ",\"bitmapqualorig\":" );
    Node_out( str, node->bitmapqualorig );
    return;
}

static void BitmapIndexScan_out( StringInfo str, BitmapIndexScan * node )
{
    appendStringInfoString( str, "\"name\":\"BITMAPINDEXSCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    appendStringInfo( str, ",\"indexid\":%u", node->indexid );
    appendStringInfo( str, ",\"indexqual\":" );
    Node_out( str, node->indexqual );
    appendStringInfo( str, ",\"indexqualorig\":" );
    Node_out( str, node->indexqualorig );
    return;
}

static void BitmapOrPath_out( StringInfo str, BitmapOrPath * node )
{
    appendStringInfoString( str, "\"name\":\"BITMAPORPATH\"" );
    PathInfo_out( str, ( Path * ) node );
    appendStringInfo( str, ",\"bitmapquals\":" );
    Node_out( str, node->bitmapquals );
    appendStringInfo( str, ",\"bitmapselectivity\":%.4f", node->bitmapselectivity );
    return;
}

static void BitmapOr_out( StringInfo str, BitmapOr * node )
{
    appendStringInfoString( str, "\"name\":\"BITMAPOR\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"bitmapplans\":" );
    Node_out( str, node->bitmapplans );
    return;
}

static void Bitmapset_out( StringInfo str, Bitmapset * bms )
{
    Bitmapset * tmpset = NULL;
    int         x      = 0;
    int         first  = 1;

    appendStringInfoString( str, "{\"type\":\"Bitmapset\",\"value\":[" );
    tmpset = bms_copy( bms );

    while( ( x = bms_first_member( tmpset ) ) >= 0 )
    {
        if( first )
        {
            appendStringInfo( str, "%d", x );
            first = 0;
        }
        else
        {
            appendStringInfo( str, ",%d", x );
        }
    }

    bms_free( tmpset );
    appendStringInfoString( str, "]}" );
    return;
}

static void BooleanTest_out( StringInfo str, BooleanTest * node )
{
    appendStringInfoString( str, "\"name\":\"BOOLEANTEST\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"booltesttype\":\"%s\"", enum_BoolTestType( node->booltesttype ) );
    return;
}

static void BoolExpr_out( StringInfo str, BoolExpr * node )
{
    char * op_str = NULL;

    // Handle expressing the EXPR enums
    switch( node->boolop )
    {
        case AND_EXPR:
            op_str = "AND";
            break;
        case OR_EXPR:
            op_str = "OR";
            break;
        case NOT_EXPR:
            op_str = "NOT";
            break;
    }

    appendStringInfoString( str, "\"name\":\"BOOLEXPR\"" );
    appendStringInfo( str, ",\"boolop\":" );
    Token_out( str, op_str );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void CaseExpr_out( StringInfo str, CaseExpr * node )
{
    appendStringInfoString( str, "\"name\":\"CASE\"" );
    appendStringInfo( str, ",\"casetype\":%u", node->casetype );
    appendStringInfo( str, ",\"casecollid\":%u", node->casecollid );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"defresult\":" );
    Node_out( str, node->defresult );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void CaseTestExpr_out( StringInfo str, CaseTestExpr * node )
{
    appendStringInfoString( str, "\"name\":\"CASETESTEXPR\"" );
    appendStringInfo( str, ",\"typeId\":%u", node->typeId );
    appendStringInfo( str, ",\"typeMod\":%d", node->typeMod );
    appendStringInfo( str, ",\"collation\":%u", node->collation );
    return;
}

static void CaseWhen_out( StringInfo str, CaseWhen * node )
{
    appendStringInfoString( str, "\"name\":\"WHEN\"" );
    appendStringInfo( str, ",\"expr\":" );
    Node_out( str, node->expr );
    appendStringInfo( str, ",\"result\":" );
    Node_out( str, node->result );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void CoalesceExpr_out( StringInfo str, CoalesceExpr * node )
{
    appendStringInfoString( str, "\"name\":\"COALESCE\"" );
    appendStringInfo( str, ",\"coalescetype\":%u", node->coalescetype );
    appendStringInfo( str, ",\"coalescecollid\":%u", node->coalescecollid );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void CoerceToDomainValue_out( StringInfo str, CoerceToDomainValue * node )
{
    appendStringInfoString( str, "\"name\":\"COERCETODOMAINVALUE\"" );
    appendStringInfo( str, ",\"typeId\":%u", node->typeId );
    appendStringInfo( str, ",\"typeMod\":%d", node->typeMod );
    appendStringInfo( str, ",\"collation\":%u", node->collation );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void CoerceToDomain_out( StringInfo str, CoerceToDomain * node )
{
    appendStringInfoString( str, "\"name\":\"COERCETODOMAIN\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"resulttype\":%u", node->resulttype );
    appendStringInfo( str, ",\"resulttypmod\":%d", node->resulttypmod );
    appendStringInfo( str, ",\"resultcollid\":%u", node->resultcollid );
    appendStringInfo( str, ",\"coercionformat\":\"%s\"", enum_CoercionForm( node->coercionformat ) );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void CoerceViaIO_out( StringInfo str, CoerceViaIO * node )
{
    appendStringInfoString( str, "\"name\":\"COERCEVIAIO\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"resulttype\":%u", node->resulttype );
    appendStringInfo( str, ",\"resultcollid\":%u", node->resultcollid );
    appendStringInfo( str, ",\"coerceformat\":\"%s\"", enum_CoercionForm( node->coerceformat ) );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void CollateClause_out( StringInfo str, CollateClause * node )
{
    appendStringInfoString( str, "\"name\":\"COLLATECLAUSE\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"collname\":" );
    Node_out( str, node->collname );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void CollateExpr_out( StringInfo str, CollateExpr * node )
{
    appendStringInfoString( str, "\"name\":\"COLLATE\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"collOid\":%u", node->collOid );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void ColumnDef_out( StringInfo str, ColumnDef * node )
{
    appendStringInfoString( str, "\"name\":\"COLUMNDEF\"" );
    appendStringInfo( str, ",\"colname\":" );
    Token_out( str, node->colname );
    appendStringInfo( str, ",\"typeName\":" );
    Node_out( str, node->typeName );
    appendStringInfo( str, ",\"inhcount\":%d", node->inhcount );
    appendStringInfo( str, ",\"is_local\":%s", node->is_local ? "true" : "false" );
    appendStringInfo( str, ",\"is_not_null\":%s", node->is_not_null ? "true" : "false" );
    appendStringInfo( str, ",\"is_from_type\":%s", node->is_from_type ? "true" : "false" );
    appendStringInfo( str, ",\"storage\":%d", node->storage );
    appendStringInfo( str, ",\"raw_default\":" );
    Node_out( str, node->raw_default );
    appendStringInfo( str, ",\"cooked_default\":" );
    Node_out( str, node->cooked_default );
    appendStringInfo( str, ",\"collClause\":" );
    Node_out( str, node->collClause );
    appendStringInfo( str, ",\"collOid\":%u", node->collOid );
    appendStringInfo( str, ",\"constraints\":" );
    Node_out( str, node->constraints );
    return;
}

static void ColumnRef_out( StringInfo str, ColumnRef * node )
{
    appendStringInfoString( str, "\"name\":\"COLUMNREF\"" );
    appendStringInfo( str, ",\"fields\":" );
    Node_out( str, node->fields );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void CommonTableExpr_out( StringInfo str, CommonTableExpr * node )
{
    appendStringInfoString( str, "\"name\":\"COMMONTABLEEXPR\"" );
    appendStringInfo( str, ",\"ctename\":" );
    Token_out( str, node->ctename );
    appendStringInfo( str, ",\"aliascolnames\":" );
    Node_out( str, node->aliascolnames );
    appendStringInfo( str, ",\"ctequery\":" );
    Node_out( str, node->ctequery );
    appendStringInfo( str, ",\"location\":%d", node->location );
    appendStringInfo( str, ",\"cterecursive\":%s", node->cterecursive ? "true" : "false" );
    appendStringInfo( str, ",\"cterefcount\":%d", node->cterefcount );

    if( node->ctecolnames )
    {
        appendStringInfo( str, ",\"ctecolnames\":" );
        Node_out( str, node->ctecolnames );
    }

    if( node->ctecoltypes )
    {
        appendStringInfo( str, ",\"ctecoltypes\":" );
        Node_out( str, node->ctecoltypes );
    }

    if( node->ctecoltypmods )
    {
        appendStringInfo( str, ",\"ctecoltypmods\":" );
        Node_out( str, node->ctecoltypmods );
    }

    if( node->ctecolcollations )
    {
        appendStringInfo( str, ",\"ctecolcollations\":" );
        Node_out( str, node->ctecolcollations );
    }
    return;
}

static void Const_out( StringInfo str, Const * node )
{
    appendStringInfoString( str, "\"name\":\"CONST\"" );
    appendStringInfo( str, ",\"consttype\":%u", node->consttype );
    appendStringInfo( str, ",\"consttypmod\":%d", node->consttypmod );
    appendStringInfo( str, ",\"constcollid\":%u", node->constcollid );
    appendStringInfo( str, ",\"constlen\":%d", node->constlen );
    appendStringInfo( str, ",\"constbyval\":%s", node->constbyval ? "true" : "false" );
    appendStringInfo( str, ",\"constisnull\":%s", node->constisnull ? "true" : "false" );
    appendStringInfo( str, ",\"location\":%d", node->location );
    appendStringInfo(str, ",\"constvalue\":");

    if( node->constisnull )
        appendStringInfo( str, "null" );
    else
        Datum_out( str, node->constvalue, node->constlen, node->constbyval );

    return;
}

static void ConvertRowtypeExpr_out( StringInfo str, ConvertRowtypeExpr * node )
{
    appendStringInfoString( str, "\"name\":\"CONVERTROWTYPEEXPR\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"resulttype\":%u", node->resulttype );
    appendStringInfo( str, ",\"convertformat\":\"%s\"", enum_CoercionForm( node->convertformat ) );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void Constraint_out( StringInfo str, Constraint * node )
{
    appendStringInfoString( str, "\"name\":\"CONSTRAINT\"" );
    appendStringInfo( str, ",\"conname\":" );
    Token_out( str, node->conname );
    appendStringInfo( str, ",\"deferrable\":%s", node->deferrable ? "true" : "false" );
    appendStringInfo( str, ",\"initdeferred\":%s", node->initdeferred ? "true" : "false" );
    appendStringInfo( str, ",\"location\":%d", node->location );

    // TODO: Figure out formatting here
    appendStringInfo( str, ",\"contype\":" );
    switch( node->contype )
    {
        case CONSTR_NULL:
            appendStringInfo( str, "\"NULL\"");
            break;
        case CONSTR_NOTNULL:
            appendStringInfo( str, "\"NOT_NULL\"" );
            break;
        case CONSTR_DEFAULT:
            appendStringInfo( str, "\"DEFAULT\"" );
            appendStringInfo( str, ",\"raw_expr\":" );
            Node_out( str, node->raw_expr );
            appendStringInfo( str, ",\"cooked_expr\":" );
            Token_out( str, node->cooked_expr );
            break;
        case CONSTR_CHECK:
            appendStringInfo( str, "\"CHECK\"" );
            appendStringInfo( str, ",\"raw_expr\":" );
            Node_out( str, node->raw_expr );
            appendStringInfo( str, ",\"cooked_expr\":" );
            Token_out( str, node->cooked_expr );
            break;
        case CONSTR_PRIMARY:
            // Need to output where_clause and access_method attribs
            appendStringInfo( str, "\"PRIMARY_KEY\"" );
            appendStringInfo( str, ",\"keys\":" );
            Node_out( str, node->keys );
            appendStringInfo( str, ",\"options\":" );
            Node_out( str, node->options );
            appendStringInfo( str, ",\"indexname\":" );
            Token_out( str, node->indexname );
            appendStringInfo( str, ",\"indexspace\":" );
            Token_out( str, node->indexspace );
            break;
        case CONSTR_UNIQUE:
            // Need to output where_clause and access_method attribs
            appendStringInfo( str, "\"UNIQUE\"" );
            appendStringInfo( str, ",\"keys\":" );
            Node_out( str, node->keys );
            appendStringInfo( str, ",\"options\":" );
            Node_out( str, node->options );
            appendStringInfo( str, ",\"indexname\":" );
            Token_out( str, node->indexname );
            appendStringInfo( str, ",\"indexspace\":" );
            Token_out( str, node->indexspace );
            break;
        case CONSTR_EXCLUSION:
            appendStringInfo( str, "\"EXCLUSION\"" );
            appendStringInfo( str, ",\"exclusions\":" );
            Node_out( str, node->exclusions );
            appendStringInfo( str, ",\"options\":" );
            Node_out( str, node->options );
            appendStringInfo( str, ",\"indexname\":" );
            Token_out( str, node->indexname );
            appendStringInfo( str, ",\"indexspace\":" );
            Token_out( str, node->indexspace );
            appendStringInfo( str, ",\"access_method\":" );
            Token_out( str, node->access_method );
            appendStringInfo( str, ",\"where_clause\":" );
            Node_out( str, node->where_clause );
            break;
        case CONSTR_FOREIGN:
            appendStringInfo( str, "\"FOREIGN_KEY\"" );
            appendStringInfo( str, ",\"pktable\":" );
            Node_out( str, node->pktable );
            appendStringInfo( str, ",\"fk_attrs\":" );
            Node_out( str, node->fk_attrs );
            appendStringInfo( str, ",\"pk_attrs\":" );
            Node_out( str, node->pk_attrs );
            appendStringInfo( str, ",\"fk_matchtype\":\"%c\"", node->fk_matchtype );
            appendStringInfo( str, ",\"fk_upd_action\":\"%c\"", node->fk_upd_action );
            //appendStringInfo( str, ",\"fn_del_action\":\"%c\"", node->fn_del_action );
            appendStringInfo( str, ",\"skip_validation\":%s", node->skip_validation ? "true" : "false" );
            appendStringInfo( str, ",\"initially_valid\":%s", node->initially_valid ? "true" : "false" );
            break;
        case CONSTR_ATTR_DEFERRABLE:
            appendStringInfo( str, "\"ATTR_DEFERRABLE\"" );
            break;
        case CONSTR_ATTR_NOT_DEFERRABLE:
            appendStringInfo( str, "\"ATTR_NOT_DEFERRABLE\"" );
            break;
        case CONSTR_ATTR_DEFERRED:
            appendStringInfo( str, "\"ATTR_DEFERRED\"" );
            break;
        case CONSTR_ATTR_IMMEDIATE:
            appendStringInfo( str, "\"ATTR_IMMEDIATE\"" );
            break;
        default:
            appendStringInfo(
                str,
                "\"UNKNOWN (%d)\"",
                ( int ) node->contype
            );
            break;
    }

    return;
}

static void CreateForeignTableStmt_out( StringInfo str, CreateForeignTableStmt * node )
{
    appendStringInfoString( str, "\"name\":\"CREATEFOREIGNTABLESTMT\"" );
    CreateStmt_out( str, ( CreateStmt * ) &node->base );
    appendStringInfo( str, ",\"servername\":" );
    Token_out( str, node->servername );
    appendStringInfo( str, ",\"options\":" );
    Node_out( str, node->options );
    return;
}

static void CreateStmt_out( StringInfo str, CreateStmt * node )
{
    appendStringInfoString( str, "\"name\":\"CREATESTMT\"" );
    appendStringInfo( str, ",\"relation\":" );
    Node_out( str, node->relation );
    appendStringInfo( str, ",\"tableElts\":" );
    Node_out( str, node->tableElts );
    appendStringInfo( str, ",\"inhRelations\":" );
    Node_out( str, node->inhRelations );
    appendStringInfo( str, ",\"ofTypename\":" );
    Node_out( str, node->ofTypename );
    appendStringInfo( str, ",\"constraints\":" );
    Node_out( str, node->constraints );
    appendStringInfo( str, ",\"options\":" );
    Node_out( str, node->options );
    appendStringInfo( str, ",\"oncommit\":\"%s\"", enum_OnCommitAction( node->oncommit ) );
    appendStringInfo( str, ",\"tablespacename\":" );
    Token_out( str, node->tablespacename );
    appendStringInfo( str, ",\"if_not_exists\":%s", node->if_not_exists ? "true" : "false" );
    return;
}

static void CteScan_out( StringInfo str, CteScan * node )
{
    appendStringInfoString( str, "\"name\":\"CTESCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    appendStringInfo( str, ",\"ctePlanId\":%d", node->ctePlanId );
    appendStringInfo( str, ",\"cteParam\":%d", node->cteParam );
    return;
}

static void CurrentOfExpr_out( StringInfo str, CurrentOfExpr * node )
{
    appendStringInfoString( str, "\"name\":\"CURRENTOFEXPR\"" );
    appendStringInfo( str, ",\"cvarno\":%u", node->cvarno );
    appendStringInfo( str, ",\"cursor_name\":" );
    Token_out( str, node->cursor_name );
    appendStringInfo( str, ",\"cursor_param\":%d", node->cursor_param );
    return;
}

// Output the generic Datum type for JSON - specifically handle byval vs byref types in postgresql
static void Datum_out( StringInfo str, Datum value, int typlen, bool typbyval )
{
    char * s      = NULL;
    int    first  = 1;
    Size   length = 0;
    Size   i      = 0;

    length = datumGetSize( value, typbyval, typlen );

    // Todo - handle various types here. We're currently just outputing as int but byval/byref values represent all SQL types
    if( typbyval )
    {
        s = ( char * ) ( &value );
        appendStringInfo( str, "{\"size\":%u,\"value\":[", ( unsigned int ) length );

        for( i = 0; i < ( Size ) sizeof( Datum ); i++ )
        {
            if( first )
            {
                first = 0;
            }
            else
            {
                appendStringInfo( str, "," );
            }

            appendStringInfo( str, "%d", ( int ) ( s[i] ) );
        }

        appendStringInfo( str, "]}" );
    }
    else
    {
        s = ( char * ) DatumGetPointer( value );

        if( !PointerIsValid( s ) )
        {
            appendStringInfo( str, "{\"size\":0,\"value\":[]}" );
        }
        else
        {
            appendStringInfo( str, "{\"size\":%u,\"value\":[", ( unsigned int ) length );
            for( i = 0; i < length; i++ )
            {
                if( first )
                {
                    first = 0;
                }
                else
                {
                    appendStringInfo( str, "," );
                }

                appendStringInfo( str, "%d", ( int ) ( s[i] ) );
            }

            appendStringInfo( str, "]}" );
        }
    }

    return;
}

static void DeclareCursorStmt_out( StringInfo str, DeclareCursorStmt * node )
{
    appendStringInfoString( str, "\"name\":\"DECLARECURSORSTMT\"" );
    appendStringInfo( str, ",\"portalname\":" );
    Token_out( str, node->portalname );
    appendStringInfo( str, ",\"options\":%d", node->options );
    appendStringInfo( str, ",\"query\":" );
    Node_out( str, node->query );
    return;
}

static void DefElem_out( StringInfo str, DefElem * node )
{
    appendStringInfoString( str, "\"name\":\"DEFELEM\"" );
    appendStringInfo( str, ",\"defnamespace\":" );
    Token_out( str, node->defnamespace );
    appendStringInfo( str, ",\"defname\":" );
    Token_out( str, node->defname );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"defaction\":\"%s\"", enum_DefElemAction( node->defaction ) );
    return;
}

static void DistinctExpr_out( StringInfo str, DistinctExpr * node )
{
    appendStringInfoString( str, "\"name\":\"DISTINCTEXPR\"" );
    appendStringInfo( str, ",\"opno\":%u", node->opno );
    appendStringInfo( str, ",\"opfuncid\":%u", node->opfuncid );
    appendStringInfo( str, ",\"opresulttype\":%u", node->opresulttype );
    appendStringInfo( str, ",\"opretset\":%s", node->opretset ? "true" : "false" );
    appendStringInfo( str, ",\"opcollid\":%u", node->opcollid );
    appendStringInfo( str, ",\"inputcollid\":%u", node->inputcollid );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void EquivalenceClass_out( StringInfo str, EquivalenceClass * node )
{
    EquivalenceClass * topmost = NULL;
    topmost = node;
    // Climb the tree to the topmost EquivalenceClass
    while( topmost->ec_merged )
    {
        topmost = topmost->ec_merged;
    }

    appendStringInfoString( str, "\"name\":\"EQUIVALENCECLASS\"" );
    appendStringInfo( str, ",\"ec_opfamilies\":" );
    Node_out( str, topmost->ec_opfamilies );
    appendStringInfo( str, ",\"ec_collation\":%u", topmost->ec_collation );
    appendStringInfo( str, ",\"ec_members\":" );
    Node_out( str, topmost->ec_members );
    appendStringInfo( str, ",\"ec_sources\":" );
    Node_out( str, topmost->ec_sources );
    appendStringInfo( str, ",\"ec_derives\":" );
    Node_out( str, topmost->ec_derives );
    appendStringInfo( str, ",\"ec_relids\":" );
    Bitmapset_out( str, node->ec_relids );
    appendStringInfo( str, ",\"ec_has_const\":%s", topmost->ec_has_const ? "true" : "false" );
    appendStringInfo( str, ",\"ec_has_volatile\":%s", topmost->ec_has_volatile ? "true" : "false" );
    appendStringInfo( str, ",\"ec_below_outer_join\":%s", topmost->ec_below_outer_join ? "true" : "false" );
    appendStringInfo( str, ",\"ec_broken\":%s", topmost->ec_broken ? "true" : "false" );
    appendStringInfo( str, ",\"ec_sortref\":%u", topmost->ec_sortref );
    return;
}

static void EquivalenceMember_out( StringInfo str, EquivalenceMember * node )
{
    appendStringInfoString( str, "\"name\":\"EQUIVALENCEMEMBER\"" );
    appendStringInfo( str, ",\"em_expr\":" );
    Node_out( str, node->em_expr );
    appendStringInfo( str, ",\"em_relids\":" );
    Bitmapset_out( str, node->em_relids );
    appendStringInfo( str, ",\"em_is_const\":%s", node->em_is_const ? "true" : "false" );
    appendStringInfo( str, ",\"em_is_child\":%s", node->em_is_child ? "true" : "false" );
    appendStringInfo( str, ",\"em_datatype\":%u", node->em_datatype );
    return;
}

static void FieldSelect_out( StringInfo str, FieldSelect * node )
{
    appendStringInfoString( str, "\"name\":\"FIELDSELECT\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"fieldnum\":%d", node->fieldnum );
    appendStringInfo( str, ",\"resulttype\":%u", node->resulttype );
    appendStringInfo( str, ",\"resulttypmod\":%d", node->resulttypmod );
    appendStringInfo( str, ",\"resultcollid\":%u", node->resultcollid );
    return;
}

static void FieldStore_out( StringInfo str, FieldStore * node )
{
    appendStringInfoString( str, "\"name\":\"FIELDSTORE\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"newvals\":" );
    Node_out( str, node->newvals );
    appendStringInfo( str, ",\"fieldnums\":" );
    Node_out( str, node->fieldnums );
    appendStringInfo( str, ",\"resulttype\":%u", node->resulttype );
    return;
}

static void ForeignPath_out( StringInfo str, ForeignPath * node )
{
    appendStringInfoString( str, "\"name\":\"FOREIGNPATH\"" );
    PathInfo_out( str, ( Path * ) node );
    appendStringInfoString( str, ",\"fdw_outerpath\":" );
    Path_out( str, node->fdw_outerpath );
    return;
}

static void ForeignScan_out( StringInfo str, ForeignScan * node )
{
    appendStringInfoString( str, "\"name\":\"FOREIGNSCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    // Todo - add from plannodes.h
    appendStringInfo( str, ",\"fsSystemCol\":%s", node->fsSystemCol ? "true" : "false" );
    return;
}

static void FromExpr_out( StringInfo str, FromExpr * node )
{
    appendStringInfoString( str, "\"name\":\"FROMEXPR\"" );
    appendStringInfo( str, ",\"fromlist\":" );
    Node_out( str, node->fromlist );
    appendStringInfo( str, ",\"quals\":" );
    Node_out( str, node->quals );
    return;
}

static void FuncCall_out( StringInfo str, FuncCall * node )
{
    appendStringInfoString( str, "\"name\":\"FUNCCALL\"" );
    appendStringInfo( str, ",\"funcname\":" );
    Node_out( str, node->funcname );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"agg_order\":" );
    Node_out( str, node->agg_order );
    appendStringInfo( str, ",\"agg_star\":%s", node->agg_star ? "true" : "false" );
    appendStringInfo( str, ",\"agg_distinct\":%s", node->agg_distinct ? "true" : "false" );
    appendStringInfo( str, ",\"func_variadic\":%s", node->func_variadic ? "true" : "false" );
    appendStringInfo( str, ",\"over\":" );
    Node_out( str, node->over );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void FuncExpr_out( StringInfo str, FuncExpr * node )
{
    appendStringInfoString( str, "\"name\":\"FUNCEXPR\"" );
    appendStringInfo( str, ",\"funcid\":%u", node->funcid );
    appendStringInfo( str, ",\"funcresulttype\":%u", node->funcresulttype );
    appendStringInfo( str, ",\"funcretset\":%s", node->funcretset ? "true" : "false" );
    appendStringInfo( str, ",\"funcformat\":\"%s\"", enum_CoercionForm( node->funcformat ) );
    appendStringInfo( str, ",\"funccollid\":%u", node->funccollid );
    appendStringInfo( str, ",\"inputcollid\":%u", node->inputcollid );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void FunctionScan_out( StringInfo str, FunctionScan * node )
{
    appendStringInfoString( str, "\"name\":\"FUNCTIONSCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    appendStringInfo( str, ",\"functions\":" );
    Node_out( str, node->functions );
    appendStringInfo( str, ",\"funcordinality\":" );
    appendStringInfo( str, "%s", node->funcordinality ? "true" : "false" );
    return;
}

static void Group_out(StringInfo str, Group *node)
{
    int i     = 0;
    int first = 1;

    appendStringInfoString( str, "\"name\":\"GROUP\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"numCols\":%d", node->numCols );
    appendStringInfo( str, ",\"grpColIdx\":[" );

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%d", node->grpColIdx[i] );
    }

    appendStringInfo( str, "],\"grpOperators\":[" );
    first = 1;

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->grpOperators[i] );
    }

    appendStringInfoChar( str, ']' );
    return;
}

static void HashJoin_out( StringInfo str, HashJoin * node )
{
    appendStringInfoString( str, "\"name\":\"HASHJOIN\"" );
    JoinInfo_out( str, ( Join * ) node );
    appendStringInfo( str, ",\"hashclauses\":" );
    Node_out( str, node->hashclauses );
    return;
}

static void HashPath_out( StringInfo str, HashPath * node )
{
    appendStringInfoString( str, "\"name\":\"HASHPATH\"" );
    JoinPathInfo_out( str, ( JoinPath * ) node );
    appendStringInfo( str, ",\"path_hashclauses\":" );
    Node_out( str, node->path_hashclauses );
    appendStringInfo( str, ",\"num_batches\":%d", node->num_batches );
    return;
}

static void Hash_out( StringInfo str, Hash * node )
{
    appendStringInfoString( str, "\"name\":\"HASH\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"skewTable\":%u", node->skewTable );
    appendStringInfo( str, ",\"skewColumn\":%d", node->skewColumn );
    appendStringInfo( str, ",\"skewInherit\":%s", node->skewInherit ? "true" : "false" );
    appendStringInfo( str, ",\"rows_total\":%.0f", node->rows_total );
    return;
}

static void IndexElem_out( StringInfo str, IndexElem * node )
{
    appendStringInfoString( str, "\"name\":\"INDEXELEM\"" );
    appendStringInfo( str, ",\"name\":" );
    Token_out( str, node->name );
    appendStringInfo( str, ",\"expr\":" );
    Node_out( str, node->expr );
    appendStringInfo( str, ",\"indexcolname\":" );
    Token_out( str, node->indexcolname );
    appendStringInfo( str, ",\"collation\":" );
    Node_out( str, node->collation );
    appendStringInfo( str, ",\"opclass\":" );
    Node_out( str, node->opclass );
    appendStringInfo( str, ",\"ordering\":\"%s\"", enum_SortByDir( node->ordering ) );
    appendStringInfo( str, ",\"nulls_ordering\":\"%s\"", enum_SortByNulls( node->nulls_ordering ) );
    return;
}

// This is a partial set of node attributes
static void IndexOptInfo_out( StringInfo str, IndexOptInfo * node )
{
    appendStringInfoString( str, "\"name\":\"INDEXOPTINFO\"" );
    appendStringInfo( str, ",\"indexoid\":%u", node->indexoid );
    appendStringInfo( str, ",\"pages\":%u", node->pages );
    appendStringInfo( str, ",\"tuples\":%.0f", node->tuples );
    appendStringInfo( str, ",\"ncolumns\":%d", node->ncolumns );
    appendStringInfo( str, ",\"relam\":%u", node->relam );
    appendStringInfo( str, ",\"indexprs\":" );
    Node_out( str, node->indexprs );
    appendStringInfo( str, ",\"indpred\":" );
    Node_out( str, node->indpred );
    appendStringInfo( str, ",\"predOK\":%s", node->predOK ? "true" : "false" );
    appendStringInfo( str, ",\"unique\":%s", node->unique ? "true" : "false" );
    appendStringInfo( str, ",\"hypothetical\":%s", node->hypothetical ? "true" : "false" );
    return;
}

static void IndexPath_out( StringInfo str, IndexPath * node )
{
    appendStringInfoString( str, "\"name\":\"INDEXPATH\"" );
    PathInfo_out( str, ( Path * ) node );
    appendStringInfo( str, ",\"indexinfo\":" );
    Node_out( str, node->indexinfo );
    appendStringInfo( str, ",\"indexclauses\":" );
    Node_out( str, node->indexclauses );
    appendStringInfo( str, ",\"indexorderbys\":" );
    Node_out( str, node->indexorderbys );
    appendStringInfo( str, ",\"indexscandir\":\"%s\"", enum_ScanDirection( node->indexscandir ) );
    appendStringInfo( str, ",\"indextotalcost\":%.2f", node->indextotalcost );
    appendStringInfo( str, ",\"indexselectivity\":%.4f", node->indexselectivity );
    return;
}

static void IndexScan_out( StringInfo str, IndexScan * node )
{
    appendStringInfoString( str, "\"name\":\"INDEXSCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    appendStringInfo( str, ",\"indexid\":%u", node->indexid );
    appendStringInfo( str, ",\"indexqual\":" );
    Node_out( str, node->indexqual );
    appendStringInfo( str, ",\"indexqualorig\":" );
    Node_out( str, node->indexqualorig );
    appendStringInfo( str, ",\"indexorderby\":" );
    Node_out( str, node->indexorderby );
    appendStringInfo( str, ",\"indexorderbyorig\":" );
    Node_out( str, node->indexorderbyorig );
    appendStringInfo( str, ",\"indexorderdir\":\"%s\"", enum_ScanDirection( node->indexorderdir ) );
    return;
}

static void IndexStmt_out( StringInfo str, IndexStmt * node )
{
    appendStringInfoString( str, "\"name\":\"INDEXSTMT\"" );
    appendStringInfo( str, ",\"idxname\":" );
    Token_out( str, node->idxname );
    appendStringInfo( str, ",\"relation\":" );
    Node_out( str, node->relation );
    appendStringInfo( str, ",\"accessMethod\":" );
    Token_out( str, node->accessMethod );
    appendStringInfo( str, ",\"tableSpace\":" );
    Token_out( str, node->tableSpace );
    appendStringInfo( str, ",\"indexParams\":" );
    Node_out( str, node->indexParams );
    appendStringInfo( str, ",\"options\":" );
    Node_out( str, node->options );
    appendStringInfo( str, ",\"whereClause\":" );
    Node_out( str, node->whereClause );
    appendStringInfo( str, ",\"excludeOpNames\":" );
    Node_out( str, node->excludeOpNames );
    appendStringInfo( str, ",\"indexOid\":%u", node->indexOid );
    appendStringInfo( str, ",\"unique\":%s", node->unique ? "true" : "false" );
    appendStringInfo( str, ",\"primary\":%s", node->primary ? "true" : "false" );
    appendStringInfo( str, ",\"isconstraint\":%s", node->isconstraint ? "true" : "false" );
    appendStringInfo( str, ",\"deferrable\":%s", node->deferrable ? "true" : "false" );
    appendStringInfo( str, ",\"initdeferred\":%s", node->initdeferred ? "true" : "false" );
    appendStringInfo( str, ",\"concurrent\":%s", node->concurrent ? "true" : "false" );
    return;
}

static void IntoClause_out( StringInfo str, IntoClause * node )
{
    appendStringInfoString( str, "\"name\":\"INTOCLAUSE\"" );
    appendStringInfo( str, ",\"rel\":" );
    Node_out( str, node->rel );
    if( node->colNames != NIL )
    {
        appendStringInfo( str, ",\"colNames\":" );
        Node_out( str, node->colNames );
    }

    if( node->options != NIL )
    {
        appendStringInfo( str, ",\"options\":" );
        Node_out( str, node->options );
    }

    appendStringInfo( str, ",\"onCommit\":\"%s\"", enum_OnCommitAction( node->onCommit ) );
    if( node->tableSpaceName )
    {
        appendStringInfo( str, ",\"tableSpaceName\":" );
        Token_out( str, node->tableSpaceName );
    }
    return;
}

static void JoinExpr_out( StringInfo str, JoinExpr * node )
{
    appendStringInfoString( str, "\"name\":\"JOINEXPR\"" );
    appendStringInfo( str, ",\"jointype\":\"%s\"", enum_JoinType( node->jointype ) );
    appendStringInfo( str, ",\"isNatural\":%s", node->isNatural ? "true" : "false" );

    if( node->larg )
    {
        appendStringInfo( str, ",\"larg\":" );
        Node_out( str, node->larg );
    }

    if( node->rarg )
    {
        appendStringInfo( str, ",\"rarg\":" );
        Node_out( str, node->rarg );
    }

    if( node->usingClause )
    {
        appendStringInfo( str, ",\"usingClause\":" );
        Node_out( str, node->usingClause );
    }

    if( node->quals )
    {
        appendStringInfo( str, ",\"quals\":" );
        Node_out( str, node->quals );
    }

    if( node->alias )
    {
        appendStringInfo( str, ",\"alias\":" );
        Node_out( str, node->alias );
    }

    appendStringInfo( str, ",\"rtindex\":%d", node->rtindex );
    return;
}

static void JoinInfo_out( StringInfo str, Join * node )
{
    // Write out common join node stuff before returning
    // and callee writing out specific information
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"jointype\":\"%s\"", enum_JoinType( node->jointype ) );
    appendStringInfo( str, ",\"joinqual\":" );
    Node_out( str, node->joinqual );
    return;
}

static void JoinPathInfo_out( StringInfo str, JoinPath * node )
{
    // Write out common joinpath node stuff before returning
    // and callee writing out specific information
    PathInfo_out( str, ( Path * ) node );
    appendStringInfo( str, ",\"jointype\":\"%s\"", enum_JoinType( node->jointype ) );
    appendStringInfo( str, ",\"outerjoinpath\":" );
    Node_out( str, node->outerjoinpath );
    appendStringInfo( str, ",\"innerjoinpath\":" );
    Node_out( str, node->innerjoinpath );
    appendStringInfo( str, ",\"joinrestrictinfo\":" );
    Node_out( str, node->joinrestrictinfo );
    return;
}

static void Join_out( StringInfo str, Join * node )
{
    appendStringInfoString( str, "\"name\":\"JOIN\"" );
    JoinInfo_out( str, ( Join * ) node );
    return;
}

static void Limit_out( StringInfo str, Limit * node )
{
    appendStringInfoString( str, "\"name\":\"LIMIT\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"limitOffset\":" );
    Node_out( str, node->limitOffset );
    appendStringInfo( str, ",\"limitCount\":" );
    Node_out( str, node->limitCount );
    return;
}

static void List_out( StringInfo str, List * node )
{
    int        first     = 1;
    int        first_two = 1;
    ListCell * lc        = NULL;
    ListCell * k         = NULL;
    Node *     cell      = NULL;


    if( IsA( node, IntList ) )
    {
        appendStringInfoString( str, "{\"type\":\"IntList\",\"value\":[" );
    }
    else if( IsA( node, OidList ) )
    {
        appendStringInfoString( str, "{\"type\":\"OidList\",\"value\":[" );
    }
    else
    {
        // We're a list of objects, lets skip the type: value: stuff and just have the array members
        appendStringInfoChar( str, '[' );
    }

    elog(
        DEBUG1,
        "Parsing list type %d (%s)",
        ( int ) node->type,
        enum_NodeTag( node->type )
    );

    foreach( lc, node )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        // Note - we need to properly iterate over list members
        // New code
        cell = ( Node * ) lfirst( lc );

        if( IsA( cell, List ) )
        {
            first_two = 1;
            appendStringInfoChar( str, '[' );
            foreach( k, ( List * ) cell )
            {
                if( first_two )
                    first_two = 0;
                else
                    appendStringInfoChar( str, ',' );
                Node_out( str, ( Node * ) lfirst( k ) );
            }
            appendStringInfoChar( str, ']' );
        }
        else if( IsA( node, IntList ) )
        {
            appendStringInfo( str, "%d", lfirst_int( lc ) );
        }
        else if( IsA( node, OidList ) )
        {
            appendStringInfo( str, "%u", lfirst_oid( lc ) );
        }
        else if( IsA( node, List ) )
        {
            Node_out( str, cell );
        }
        else
        {
            elog(
                ERROR,
                "Unknown list node type %d (%s)",
                ( int ) node->type,
                enum_NodeTag( node->type )
            );
        }
    }

    appendStringInfoChar( str, ']' );

    if( IsA( node, OidList ) || IsA( node, IntList ) )
    {
        appendStringInfoChar( str, '}' );
    }

    return;
}

static void LockingClause_out( StringInfo str, LockingClause * node )
{
    appendStringInfoString( str, "\"name\":\"LOCKINGCLAUSE\"" );
    if( node->lockedRels )
    {
        appendStringInfo( str, ",\"lockedRels\":" );
        Node_out( str, node->lockedRels );
    }
    // Need to output strength and waitPolicy
    return;
}

static void LockRows_out( StringInfo str, LockRows * node )
{
    appendStringInfoString( str, "\"name\":\"LOCKROWS\"" );
    PlanInfo_out( str, ( Plan * ) node );
    if( node->rowMarks )
    {
        appendStringInfo( str, ",\"rowMarks\":" );
        Node_out( str, node->rowMarks );
    }
    appendStringInfo( str, ",\"epqParam\":%d", node->epqParam );
    return;
}

static void MaterialPath_out( StringInfo str, MaterialPath * node )
{
    appendStringInfoString( str, "\"name\":\"MATERIALPATH\"" );
    PathInfo_out( str, ( Path * ) node );
    if( node->subpath )
    {
        appendStringInfo( str, ",\"subpath\":" );
        Node_out( str, node->subpath );
    }
    return;
}

static void Material_out( StringInfo str, Material * node )
{
    appendStringInfoString( str, "\"name\":\"MATERIAL\"" );
    PlanInfo_out( str, (Plan *) node );
    return;
}

static void MergeAppendPath_out( StringInfo str, MergeAppendPath * node )
{
    appendStringInfoString( str, "\"name\":\"MERGEAPPENDPATH\"" );
    PathInfo_out( str, ( Path * ) node );
    if( node->subpaths )
    {
        appendStringInfo( str, ",\"subpaths\":" );
        Node_out( str, node->subpaths );
    }
    appendStringInfo( str, ",\"limit_tuples\":%.0f", node->limit_tuples );
    return;
}

static void MergeAppend_out( StringInfo str, MergeAppend * node )
{
    int i     = 0;
    int first = 1;

    appendStringInfoString( str, "\"name\":\"MERGEAPPEND\"" );
    PlanInfo_out( str, ( Plan * ) node );
    if( node->mergeplans )
    {
        appendStringInfo( str, ",\"mergeplans\":" );
        Node_out( str, node->mergeplans );
    }
    appendStringInfo( str, ",\"numCols\":%d", node->numCols );
    appendStringInfo( str, ",\"sortColIdx\":[" );

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%d", node->sortColIdx[i] );
    }

    appendStringInfo(str, "],\"sortOperators\":[");
    first = 1;

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->sortOperators[i] );
    }

    appendStringInfo(str, "],\"collations\":[" );
    first = 1;

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->collations[i] );
    }

    appendStringInfo( str, "],\"nullsFirst\":[" );
    first = 1;

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%s", node->nullsFirst[i] ? "true" : "false" );
    }

    appendStringInfoChar( str, ']' );
    return;
}

static void MergeJoin_out( StringInfo str, MergeJoin * node )
{
    int num_cols = 0;
    int i        = 0;
    int first    = 1;

    appendStringInfoString( str, "\"name\":\"MERGEJOIN\"" );
    JoinInfo_out( str, ( Join * ) node );
    if( node->mergeclauses )
    {
        appendStringInfo( str, ",\"mergeclauses\":" );
        Node_out( str, node->mergeclauses );
    }
    num_cols = list_length( node->mergeclauses );
    appendStringInfo( str, ",\"mergeFamilies\":[" );

    for( i = 0; i < num_cols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->mergeFamilies[i] );
    }

    appendStringInfo( str, "],\"mergeCollations\":[" );
    first = 1;

    for( i = 0; i < num_cols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->mergeCollations[i] );
    }

    appendStringInfo( str, "],\"mergeStrategies\":[" );
    first = 1;

    for( i = 0; i < num_cols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%d", node->mergeStrategies[i] );
    }

    appendStringInfo( str, "],\"mergeNullsFirst\":[" );
    first = 1;

    for( i = 0; i < num_cols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%d", (int) node->mergeNullsFirst[i] );
    }

    appendStringInfoChar( str, ']' );
    return;
}

static void MergePath_out( StringInfo str, MergePath * node )
{
    appendStringInfoString( str, "\"name\":\"MERGEPATH\"" );
    JoinPathInfo_out( str, ( JoinPath * ) node );
    appendStringInfo( str, ",\"path_mergeclauses\":" );
    Node_out( str, node->path_mergeclauses );
    appendStringInfo( str, ",\"outersortkeys\":" );
    Node_out( str, node->outersortkeys );
    appendStringInfo( str, ",\"innersortkeys\":" );
    Node_out( str, node->innersortkeys );
    appendStringInfo( str, ",\"materialize_inner\":%s", node->materialize_inner ? "true" : "false" );
    return;
}

static void MinMaxAggInfo_out( StringInfo str, MinMaxAggInfo * node )
{
    appendStringInfoString( str, "\"name\":\"MINMAXAGGINFO\"" );
    appendStringInfo( str, ",\"aggfnoid\":%u", node->aggfnoid );
    appendStringInfo( str, ",\"aggsortop\":%u", node->aggsortop );
    appendStringInfo( str, ",\"target\":" );
    Node_out( str, node->target );
    // ignoring subroot
    appendStringInfo( str, ",\"path\":" );
    Node_out( str, node->path );
    appendStringInfo( str, ",\"pathcost\":%.2f", node->pathcost );
    appendStringInfo( str, ",\"param\":" );
    Node_out( str, node->param );
    return;
}

static void MinMaxExpr_out( StringInfo str, MinMaxExpr * node )
{
    appendStringInfoString( str, "\"name\":\"MINMAX\"" );
    appendStringInfo( str, ",\"minmaxtype\":%u", node->minmaxtype );
    appendStringInfo( str, ",\"minmaxcollid\":%u", node->minmaxcollid );
    appendStringInfo( str, ",\"inputcollid\":%u", node->inputcollid );
    appendStringInfo( str, ",\"op\":\"%s\"", enum_MinMaxOp( node->op ) );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void ModifyTable_out( StringInfo str, ModifyTable * node )
{
    appendStringInfoString( str, "\"name\":\"MODIFYTABLE\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"operation\":\"%s\"", enum_CmdType( node->operation ) );
    appendStringInfo( str, ",\"canSetTag\":%s", node->canSetTag ? "true" : "false" );
    appendStringInfo( str, ",\"partColsUpdated\":%s", node->partColsUpdated ? "true" : "false" );
    appendStringInfo( str, ",\"resultRelations\":" );
    Node_out( str, node->resultRelations );
    appendStringInfo( str, "\"updateColnosLists\":" );
    Node_out( str, node->updateColnosLists );
    appendStringInfo( str, "\"withCheckOptionLists\":" );
    Node_out( str, node->withCheckOptionLists );
    appendStringInfo( str, ",\"returningLists\":" );
    Node_out( str, node->returningLists );
    appendStringInfo( str, ",\"rowMarks\":" );
    Node_out( str, node->rowMarks );
    appendStringInfo( str, ",\"epqParam\":%d", node->epqParam );
    appendStringInfo( str, ",\"arbiterIndexes\":" );
    Node_out( str, node->arbiterIndexes );
    appendStringInfo( str, ",\"onConflictSet\":" );
    Node_out( str, node->onConflictSet );
    appendStringInfo( str, ",\"onConflictCols\":" );
    Node_out( str, node->onConflictCols );
    appendStringInfo( str, ",\"onConflictWhere\":" );
    Node_out( str, node->onConflictWhere );
    appendStringInfo( str, ",\"exclRelTlist\":" );
    Node_out( str, node->exclRelTlist );
    return;
}

static void NamedArgExpr_out( StringInfo str, NamedArgExpr * node )
{
    appendStringInfoString( str, "\"name\":\"NAMEDARGEXPR\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"name\":" );
    Token_out( str, node->name );
    appendStringInfo( str, ",\"argnumber\":%d", node->argnumber );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void NestLoopParam_out( StringInfo str, NestLoopParam * node )
{
    appendStringInfoString( str, "\"name\":\"NESTLOOPPARAM\"" );
    appendStringInfo( str, ",\"paramno\":%d", node->paramno );
    appendStringInfo( str, ",\"paramval\":" );
    Node_out( str, node->paramval );
    return;
}

static void NestLoop_out( StringInfo str, NestLoop * node )
{
    appendStringInfoString( str, "\"name\":\"NESTLOOP\"" );
    JoinInfo_out( str, ( Join * ) node );
    appendStringInfo( str, ",\"nestParams\":" );
    Node_out( str, node->nestParams );
    return;
}

static void NestPath_out( StringInfo str, NestPath * node )
{
    appendStringInfoString( str, "\"name\":\"NESTPATH\"" );
    JoinPathInfo_out( str, ( JoinPath * ) node );
    return;
}

static void Node_out( StringInfo string, void * object )
{
    if( object == NULL )
    {
        appendStringInfo( string, "null" );
    }
    else if(
                IsA( object, List )
             || IsA( object, IntList )
             || IsA( object, OidList )
           )
    {
        List_out( string, object );
    }
    else if(
                IsA( object, Integer )
             || IsA( object, Float )
             || IsA( object, String )
             || IsA( object, BitString )
           )
    {
        Value_out( string, object );
    }
    else
    {
        appendStringInfoChar( string, '{' );

        switch( nodeTag( object ) )
        {
            case T_Aggref:
                Aggref_out( string, object );
                break;
            case T_Agg:
                Agg_out( string, object );
                break;
            case T_Alias:
                Alias_out( string, object );
                break;
            case T_AlternativeSubPlan:
                AlternativeSubPlan_out( string, object );
                break;
            case T_AppendPath:
                AppendPath_out( string, object );
                break;
            case T_AppendRelInfo:
                AppendRelInfo_out( string, object );
                break;
            case T_Append:
                Append_out( string, object );
                break;
            case T_ArrayCoerceExpr:
                ArrayCoerceExpr_out( string, object );
                break;
            case T_ArrayExpr:
                ArrayExpr_out( string, object );
                break;
            case T_A_ArrayExpr:
                A_ArrayExpr_out( string, object );
                break;
            case T_A_Const:
                A_Const_out( string, object );
                break;
            case T_A_Expr:
                A_Expr_out( string, object );
                break;
            case T_A_Indices:
                A_Indices_out( string, object );
                break;
            case T_A_Indirection:
                A_Indirection_out( string, object );
                break;
            case T_A_Star:
                A_Star_out( string, object );
                break;
            case T_BitmapAndPath:
                BitmapAndPath_out( string, object );
                break;
            case T_BitmapAnd:
                BitmapAnd_out( string, object );
                break;
            case T_BitmapHeapPath:
                BitmapHeapPath_out( string, object );
                break;
            case T_BitmapHeapScan:
                BitmapHeapScan_out( string, object );
                break;
            case T_BitmapIndexScan:
                BitmapIndexScan_out( string, object );
                break;
            case T_BitmapOrPath:
                BitmapOrPath_out( string, object );
                break;
            case T_BitmapOr:
                BitmapOr_out( string, object );
                break;
            case T_BooleanTest:
                BooleanTest_out( string, object );
                break;
            case T_BoolExpr:
                BoolExpr_out( string, object );
                break;
            case T_CaseExpr:
                CaseExpr_out( string, object );
                break;
            case T_CaseTestExpr:
                CaseTestExpr_out( string, object );
                break;
            case T_CaseWhen:
                CaseWhen_out( string, object );
                break;
            case T_CoalesceExpr:
                CoalesceExpr_out( string, object );
                break;
            case T_CoerceToDomainValue:
                CoerceToDomainValue_out( string, object );
                break;
            case T_CoerceToDomain:
                CoerceToDomain_out( string, object );
                break;
            case T_CoerceViaIO:
                CoerceViaIO_out( string, object );
                break;
            case T_CollateClause:
                CollateClause_out( string, object );
                break;
            case T_CollateExpr:
                CollateExpr_out( string, object );
                break;
            case T_ColumnDef:
                ColumnDef_out( string, object );
                break;
            case T_ColumnRef:
                ColumnRef_out( string, object );
                break;
            case T_CommonTableExpr:
                CommonTableExpr_out( string, object );
                break;
            case T_Const:
                Const_out( string, object );
                break;
            case T_ConvertRowtypeExpr:
                ConvertRowtypeExpr_out( string, object );
                break;
            case T_Constraint:
                Constraint_out( string, object );
                break;
            case T_CreateForeignTableStmt:
                CreateForeignTableStmt_out( string, object );
                break;
            case T_CreateStmt:
                CreateStmt_out( string, object );
                break;
            case T_CteScan:
                CteScan_out( string, object );
                break;
            case T_CurrentOfExpr:
                CurrentOfExpr_out( string, object );
                break;
            case T_DeclareCursorStmt:
                DeclareCursorStmt_out( string, object );
                break;
            case T_DefElem:
                DefElem_out( string, object );
                break;
            case T_DistinctExpr:
                DistinctExpr_out( string, object );
                break;
            case T_EquivalenceClass:
                EquivalenceClass_out( string, object );
                break;
            case T_EquivalenceMember:
                EquivalenceMember_out( string, object );
                break;
            case T_FieldSelect:
                FieldSelect_out( string, object );
                break;
            case T_FieldStore:
                FieldStore_out( string, object );
                break;
            case T_ForeignPath:
                ForeignPath_out( string, object );
                break;
            case T_ForeignScan:
                ForeignScan_out( string, object );
                break;
            case T_FromExpr:
                FromExpr_out( string, object );
                break;
            case T_FuncCall:
                FuncCall_out( string, object );
                break;
            case T_FuncExpr:
                FuncExpr_out( string, object );
                break;
            case T_FunctionScan:
                FunctionScan_out( string, object );
                break;
            case T_Group:
                Group_out( string, object );
                break;
            case T_HashJoin:
                HashJoin_out( string, object );
                break;
            case T_HashPath:
                HashPath_out( string, object );
                break;
            case T_Hash:
                Hash_out( string, object );
                break;
            case T_IndexElem:
                IndexElem_out( string, object );
                break;
            case T_IndexOptInfo:
                IndexOptInfo_out( string, object );
                break;
            case T_IndexPath:
                IndexPath_out( string, object );
                break;
            case T_IndexScan:
                IndexScan_out( string, object );
                break;
            case T_IndexStmt:
                IndexStmt_out( string, object );
                break;
            case T_IntoClause:
                IntoClause_out( string, object );
                break;
            case T_JoinExpr:
                JoinExpr_out( string, object );
                break;
            case T_Join:
                Join_out( string, object );
                break;
            case T_Limit:
                Limit_out( string, object );
                break;
            case T_LockingClause:
                LockingClause_out( string, object );
                break;
            case T_LockRows:
                LockRows_out( string, object );
                break;
            case T_MaterialPath:
                MaterialPath_out( string, object );
                break;
            case T_Material:
                Material_out( string, object );
                break;
            case T_MergeAppendPath:
                MergeAppendPath_out( string, object );
                break;
            case T_MergeAppend:
                MergeAppend_out( string, object );
                break;
            case T_MergeJoin:
                MergeJoin_out( string, object );
                break;
            case T_MergePath:
                MergePath_out( string, object );
                break;
            case T_MinMaxAggInfo:
                MinMaxAggInfo_out( string, object );
                break;
            case T_MinMaxExpr:
                MinMaxExpr_out( string, object );
                break;
            case T_ModifyTable:
                ModifyTable_out( string, object );
                break;
            case T_NamedArgExpr:
                NamedArgExpr_out( string, object );
                break;
            case T_NestLoopParam:
                NestLoopParam_out( string, object );
                break;
            case T_NestLoop:
                NestLoop_out( string, object );
                break;
            case T_NestPath:
                NestPath_out( string, object );
                break;
            case T_NotifyStmt:
                NotifyStmt_out( string, object );
                break;
            case T_NullIfExpr:
                NullIfExpr_out( string, object );
                break;
            case T_NullTest:
                NullTest_out( string, object );
                break;
            case T_OpExpr:
                OpExpr_out( string, object );
                break;
            case T_ParamRef:
                ParamRef_out( string, object );
                break;
            case T_Param:
                Param_out( string, object );
                break;
            case T_PathKey:
                PathKey_out( string, object );
                break;
            case T_Path:
                Path_out( string, object );
                break;
            case T_PlaceHolderInfo:
                PlaceHolderInfo_out( string, object );
                break;
            case T_PlaceHolderVar:
                PlaceHolderVar_out( string, object );
                break;
            case T_PlanInvalItem:
                PlanInvalItem_out( string, object );
                break;
            case T_PlannedStmt:
                PlannedStmt_out( string, object );
                break;
            case T_PlannerGlobal:
                PlannerGlobal_out( string, object );
                break;
            case T_PlannerInfo:
                PlannerInfo_out( string, object );
                break;
            case T_PlannerParamItem:
                PlannerParamItem_out( string, object );
                break;
            case T_PlanRowMark:
                PlanRowMark_out( string, object );
                break;
            case T_Plan:
                Plan_out( string, object );
                break;
            case T_Query:
                Query_out( string, object );
                break;
            case T_RangeFunction:
                RangeFunction_out( string, object );
                break;
            case T_RangeSubselect:
                RangeSubselect_out( string, object );
                break;
            case T_RangeTblEntry:
                RangeTblEntry_out( string, object );
                break;
            case T_RangeTblRef:
                RangeTblRef_out( string, object );
                break;
            case T_RangeVar:
                RangeVar_out( string, object );
                break;
            case T_RawStmt:
                RawStmt_out( string, object );
                break;
            case T_RecursiveUnion:
                RecursiveUnion_out( string, object );
                break;
            case T_RelabelType:
                RelabelType_out( string, object );
                break;
            case T_RelOptInfo:
                RelOptInfo_out( string, object );
                break;
            case T_RestrictInfo:
                RestrictInfo_out( string, object );
                break;
            case T_ResTarget:
                ResTarget_out( string, object );
                break;
            case T_Result:
                Result_out( string, object );
                break;
            case T_RowCompareExpr:
                RowCompareExpr_out( string, object );
                break;
            case T_RowExpr:
                RowExpr_out( string, object );
                break;
            case T_RowMarkClause:
                RowMarkClause_out( string, object );
                break;
            case T_ScalarArrayOpExpr:
                ScalarArrayOpExpr_out( string, object );
                break;
            case T_Scan:
                Scan_out( string, object );
                break;
            case T_SelectStmt:
                SelectStmt_out( string, object );
                break;
            case T_SeqScan:
                SeqScan_out( string, object );
                break;
            case T_SetOperationStmt:
                SetOperationStmt_out( string, object );
                break;
            case T_SetOp:
                SetOp_out( string, object );
                break;
            case T_SetToDefault:
                SetToDefault_out( string, object );
                break;
            case T_SortBy:
                SortBy_out( string, object );
                break;
            case T_SortGroupClause:
                SortGroupClause_out( string, object );
                break;
            case T_Sort:
                Sort_out( string, object );
                break;
            case T_SpecialJoinInfo:
                SpecialJoinInfo_out( string, object );
                break;
            case T_SubLink:
                SubLink_out( string, object );
                break;
            case T_SubPlan:
                SubPlan_out( string, object );
                break;
            case T_SubqueryScan:
                SubqueryScan_out( string, object );
                break;
            case T_TargetEntry:
                TargetEntry_out( string, object );
                break;
            case T_TidPath:
                TidPath_out( string, object );
                break;
            case T_TidScan:
                TidScan_out( string, object );
                break;
            case T_TypeCast:
                TypeCast_out( string, object );
                break;
            case T_TypeName:
                TypeName_out( string, object );
                break;
            case T_UniquePath:
                UniquePath_out( string, object );
                break;
            case T_Unique:
                Unique_out( string, object );
                break;
            case T_ValuesScan:
                ValuesScan_out( string, object );
                break;
            case T_Var:
                Var_out( string, object );
                break;
            case T_WindowAgg:
                WindowAgg_out( string, object );
                break;
            case T_WindowClause:
                WindowClause_out( string, object );
                break;
            case T_WindowDef:
                WindowDef_out( string, object );
                break;
            case T_WindowFunc:
                WindowFunc_out( string, object );
                break;
            case T_WithClause:
                WithClause_out( string, object );
                break;
            case T_WorkTableScan:
                WorkTableScan_out( string, object );
                break;
            case T_XmlExpr:
                XmlExpr_out( string, object );
                break;
            case T_XmlSerialize:
                XmlSerialize_out( string, object );
                break;
            default:
                elog(
                    WARNING,
                    "Unrecognized node type: %d (%s)",
                    (int) nodeTag( object ),
                    enum_NodeTag( nodeTag( object ) )
                );
                break;
        }

        appendStringInfoChar( string, '}' );
    }

    return;
}

static void NotifyStmt_out( StringInfo str, NotifyStmt * node )
{
    appendStringInfoString( str, "\"name\":\"NOTIFY\"" );
    appendStringInfo( str, ",\"conditionname\":" );
    Token_out( str, node->conditionname );
    appendStringInfo( str, ",\"payload\":" );
    Token_out( str, node->payload );
    return;
}

static void NullIfExpr_out( StringInfo str, NullIfExpr * node )
{
    appendStringInfoString( str, "\"name\":\"NULLIFEXPR\"" );
    appendStringInfo( str, ",\"opno\":%u", node->opno );
    appendStringInfo( str, ",\"opfuncid\":%u", node->opfuncid );
    appendStringInfo( str, ",\"opresulttype\":%u", node->opresulttype );
    appendStringInfo( str, ",\"opretset\":%s", node->opretset ? "true" : "false" );
    appendStringInfo( str, ",\"opcollid\":%u", node->opcollid );
    appendStringInfo( str, ",\"inputcollid\":%u", node->inputcollid );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void NullTest_out( StringInfo str, NullTest * node )
{
    appendStringInfoString( str, "\"name\":\"NULLTEST\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"nulltesttype\":\"%s\"", enum_NullTestType( node->nulltesttype ) );
    appendStringInfo( str, ",\"argisrow\":%s", node->argisrow ? "true" : "false" );
    return;
}

static void OpExpr_out( StringInfo str, OpExpr * node )
{
    appendStringInfoString( str, "\"name\":\"OPEXPR\"" );
    appendStringInfo( str, ",\"opno\":%u", node->opno );
    appendStringInfo( str, ",\"opfuncid\":%u", node->opfuncid );
    appendStringInfo( str, ",\"opresulttype\":%u", node->opresulttype );
    appendStringInfo( str, ",\"opretset\":%s", node->opretset ? "true" : "false" );
    appendStringInfo( str, ",\"opcollid\":%u", node->opcollid );
    appendStringInfo( str, ",\"inputcollid\":%u", node->inputcollid );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void ParamRef_out( StringInfo str, ParamRef * node )
{
    appendStringInfoString( str, "\"name\":\"PARAMREF\"" );
    appendStringInfo( str, ",\"number\":%d", node->number );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void Param_out( StringInfo str, Param * node )
{
    appendStringInfoString( str, "\"name\":\"PARAM\"" );
    appendStringInfo( str, ",\"paramkind\":\"%s\"", enum_ParamKind( node->paramkind ) );
    appendStringInfo( str, ",\"paramid\":%d", node->paramid );
    appendStringInfo( str, ",\"paramtype\":%u", node->paramtype );
    appendStringInfo( str, ",\"paramtypmod\":%d", node->paramtypmod );
    appendStringInfo( str, ",\"paramcollid\":%u", node->paramcollid );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void PathInfo_out( StringInfo str, Path * node )
{
    // Write out common path node stuff before returning
    // and callee writing out specific information
    appendStringInfo( str, ",\"pathtype\":\"%s\"", enum_NodeTag( node->pathtype ) );
    appendStringInfo(str, " :parent_relids ");
    Bitmapset_out( str, node->parent->relids );
    appendStringInfo( str, ",\"startup_cost\":%.2f", node->startup_cost );
    appendStringInfo( str, ",\"total_cost\":%.2f", node->total_cost );
    appendStringInfo( str, ",\"pathkeys\":" );
    Node_out( str, node->pathkeys );
    return;
}

static void PathKey_out( StringInfo str, PathKey * node )
{
    appendStringInfoString( str, "\"name\":\"PATHKEY\"" );
    appendStringInfo( str, ",\"pk_eclass\":" );
    Node_out( str, node->pk_eclass );
    appendStringInfo( str, ",\"pk_opfamily\":%u", node->pk_opfamily );
    appendStringInfo( str, ",\"pk_strategy\":%d", node->pk_strategy );
    appendStringInfo( str, ",\"pk_nulls_first\":%s", node->pk_nulls_first ? "true" : "false" );
    return;
}

static void Path_out( StringInfo str, Path * node )
{
    appendStringInfoString( str, "\"name\":\"PATH\"" );
    PathInfo_out( str, ( Path * ) node );
    return;
}

static void PlaceHolderInfo_out( StringInfo str, PlaceHolderInfo * node )
{
    appendStringInfoString( str, "\"name\":\"PLACEHOLDERINFO\"" );
    appendStringInfo( str, ",\"phid\":%u", node->phid );
    appendStringInfo( str, ",\"ph_var\":" );
    Node_out( str, node->ph_var );
    appendStringInfo( str, ",\"ph_eval_at\":" );
    Bitmapset_out( str, node->ph_eval_at );
    appendStringInfo( str, ",\"ph_lateral\":" );
    Bitmapset_out( str, node->ph_lateral );
    appendStringInfo( str, ",\"ph_needed\":" );
    Bitmapset_out( str, node->ph_needed );
    appendStringInfo( str, ",\"ph_width\":%d", node->ph_width );
    return;
}

static void PlaceHolderVar_out( StringInfo str, PlaceHolderVar * node )
{
    appendStringInfoString( str, "\"name\":\"PLACEHOLDERVAR\"" );
    appendStringInfo( str, ",\"phexpr\":" );
    Node_out( str, node->phexpr );
    appendStringInfo( str, ",\"phrels\":" );
    Bitmapset_out( str, node->phrels );
    appendStringInfo( str, ",\"phid\":%u", node->phid );
    appendStringInfo( str, ",\"phlevelsup\":%u", node->phlevelsup );
    return;
}

static void PlanInfo_out( StringInfo str, Plan * node )
{
    // Write out common plan node stuff before returning
    // and callee writing out specific information
    appendStringInfo( str, ",\"startup_cost\":%.2f", node->startup_cost );
    appendStringInfo( str, ",\"total_cost\":%.2f", node->total_cost );
    appendStringInfo( str, ",\"plan_rows\":%.0f", node->plan_rows );
    appendStringInfo( str, ",\"plan_width\":%d", node->plan_width );
    appendStringInfo( str, ",\"targetlist\":" );
    Node_out( str, node->targetlist );
    appendStringInfo( str, ",\"qual\":" );
    Node_out( str, node->qual );
    appendStringInfo( str, ",\"lefttree\":" );
    Node_out( str, node->lefttree );
    appendStringInfo( str, ",\"righttree\":" );
    Node_out( str, node->righttree );
    appendStringInfo( str, ",\"initPlan\":" );
    Node_out( str, node->initPlan );
    appendStringInfo( str, ",\"extParam\":" );
    Bitmapset_out( str, node->extParam );
    appendStringInfo( str, ",\"allParam\":" );
    Bitmapset_out( str, node->allParam );
    return;
}

static void PlanInvalItem_out( StringInfo str, PlanInvalItem * node )
{
    appendStringInfoString( str, "\"name\":\"PLANVALITEM\"" );
    appendStringInfo( str, ",\"cacheId\":%d", node->cacheId );
    return;
}

static void PlannedStmt_out( StringInfo str, PlannedStmt * node )
{
    appendStringInfoString( str, "\"name\":\"PLANNEDSTMT\"" );
    appendStringInfo( str, ",\"commandType\":\"%s\"", enum_CmdType( node->commandType ) );
#if PG_VERSION_NUM > 14000
    appendStringInfo( str, ",\"queryId\":%lu", node->queryId );
#endif // PG_VERSION_NUM
    appendStringInfo( str, ",\"hasReturning\":%s", node->hasReturning ? "true" : "false" );
    appendStringInfo( str, ",\"hasModifyingCTE\":%s", node->hasModifyingCTE ? "true" : "false" );
    appendStringInfo( str, ",\"canSetTag\":%s", node->canSetTag ? "true" : "false" );
    appendStringInfo( str, ",\"transientPlan\":%s", node->transientPlan ? "true" : "false" );
    appendStringInfo( str, ",\"dependsOnRole\":%s", node->dependsOnRole ? "true" : "false" );
    appendStringInfo( str, ",\"parallelModeNeeded\":%s", node->parallelModeNeeded ? "true" : "false" );
#if PG_VERSION_NUM > 11000
    appendStringInfo( str, ",\"jitFlags\":%d", node->jitFlags );
#endif // PG_VERSION_NUM
    appendStringInfo( str, ",\"planTree\":" );
    Node_out( str, node->planTree );
    appendStringInfo( str, ",\"rtable\":" );
    Node_out( str, node->rtable );
    appendStringInfo( str, ",\"resultRelations\":" );
    Node_out( str, node->resultRelations );
    appendStringInfo( str, ",\"appendRelations\":" );
    Node_out( str, node->appendRelations );
    appendStringInfo( str, ",\"subplans\":" );
    Node_out( str, node->subplans );
    appendStringInfo( str, ",\"rewindPlanIDs\":" );
    Bitmapset_out( str, node->rewindPlanIDs );
    appendStringInfo( str, ",\"rowMarks\":" );
    Node_out( str, node->rowMarks );
    appendStringInfo( str, ",\"relationOids\":" );
    Node_out( str, node->relationOids );
    appendStringInfo( str, ",\"invalItems\":" );
    Node_out( str, node->invalItems );
    appendStringInfo( str, ",\"paramExecTypes\":" );
    Node_out( str, node->paramExecTypes );
    appendStringInfo( str, ",\"utilityStmt\":" );
    Node_out( str, node->utilityStmt );
    appendStringInfo( str, ",\"stmt_location\":%d", node->stmt_location );
    appendStringInfo( str, ",\"stmt_len\":%d", node->stmt_len );
    return;
}

// This is a partial set of node attributes
static void PlannerGlobal_out( StringInfo str, PlannerGlobal * node )
{
    appendStringInfoString( str, "\"name\":\"PLANNERGLOBAL\"" );
    appendStringInfo( str, ",\"boundParams\":" );
    Node_out( str, node->boundParams );
    appendStringInfo( str, ",\"subplans\":" );
    Node_out( str, node->subplans );
    appendStringInfo( str, ",\"rewindPlanIDs\":" );
    Bitmapset_out( str, node->rewindPlanIDs );
    appendStringInfo( str, ",\"finalrtable\":" );
    Node_out( str, node->finalrtable );
    appendStringInfo( str, ",\"finalrowmarks\":" );
    Node_out( str, node->finalrowmarks );
    appendStringInfo( str, ",\"resultRelations\":" );
    Node_out( str, node->resultRelations );
    appendStringInfo( str, ",\"appendRelations\":" );
    Node_out( str, node->appendRelations );
    appendStringInfo( str, ",\"relationOids\":" );
    Node_out( str, node->relationOids );
    appendStringInfo( str, ",\"invalItems\":" );
    Node_out( str, node->invalItems );
    appendStringInfo( str, ",\"lastPHId\":%u", node->lastPHId );
    appendStringInfo( str, ",\"lastRowMarkId\":%u", node->lastRowMarkId );
    appendStringInfo( str, ",\"dependsOnRole\":%s", node->dependsOnRole ? "true" : "false" );
    appendStringInfo( str, ",\"parallelModeOK\":%s", node->parallelModeOK ? "true" : "false" );
    appendStringInfo( str, ",\"parallelModeNeeded\":%s", node->parallelModeNeeded ? "true" : "false" );
    appendStringInfo( str, ",\"maxParallelHazard\":\"%c\"", node->maxParallelHazard );
    // Figure out partition_directory
    return;
}

// This is a partial set of node attributes
static void PlannerInfo_out( StringInfo str, PlannerInfo * node )
{
    appendStringInfoString( str, "\"name\":\"PLANNERINFO\"" );
    appendStringInfo( str, ",\"parse\":" );
    Node_out( str, node->parse );
    appendStringInfo( str, ",\"glob\":" );
    Node_out( str, node->glob );
    appendStringInfo( str, ",\"query_level\":%u", node->query_level );
    appendStringInfo( str, ",\"plan_params\":" );
    Node_out( str, node->plan_params );
    appendStringInfo( str, ",\"outer_params\":" );
    Bitmapset_out( str, node->outer_params );
    // This has a TON of information - we'll need to fill this out
    return;
}

static void PlannerParamItem_out( StringInfo str, PlannerParamItem * node )
{
    appendStringInfoString( str, "\"name\":\"PLANNERPARAMITEM\"" );
    appendStringInfo( str, ",\"item\":" );
    Node_out( str, node->item );
    appendStringInfo( str, ",\"paramId\":%u", node->paramId );
    return;
}

static void PlanRowMark_out( StringInfo str, PlanRowMark * node )
{
    appendStringInfoString( str, "\"name\":\"PLANROWMARK\"" );
    appendStringInfo( str, ",\"rti\":%u", node->rti );
    appendStringInfo( str, ",\"prti\":%u", node->prti );
    appendStringInfo( str, ",\"rowmarkId\":%u", node->rowmarkId );
    appendStringInfo( str, ",\"markType\":%d", node->markType ); // enum - RowMarkType
    appendStringInfo( str, ",\"allMarkTypes\":%d", node->allMarkTypes );
    // Add strength and waitPolicy
    appendStringInfo( str, ",\"isParent\":%s", node->isParent ? "true" : "false" );
    return;
}

static void Plan_out( StringInfo str, Plan * node )
{
    appendStringInfoString( str, "\"name\":\"PLAN\"" );
    PlanInfo_out( str, ( Plan * ) node );
    return;
}

static void Query_out( StringInfo str, Query * node )
{
    appendStringInfoString( str, "\"name\":\"QUERY\"" );
    appendStringInfo( str, ",\"commandType\":\"%s\"", enum_CmdType( node->commandType ) );
    appendStringInfo( str, ",\"querySource\":%d", node->querySource ); // enum - QuerySource
#if PG_VERSION_NUM >= 14000
    appendStringInfo( str, ",\"queryId\":%lu", node->queryId );
#endif // PG_VERSION_NUM
    appendStringInfo( str, ",\"canSetTag\":%s", node->canSetTag ? "true" : "false" );

    if( node->utilityStmt )
    {
        appendStringInfo( str, ",\"utilityStmt\":" );
        switch( nodeTag( node->utilityStmt ) )
        {
            case T_CreateStmt:
            case T_IndexStmt:
            case T_NotifyStmt:
            case T_DeclareCursorStmt:
                Node_out( str, node->utilityStmt );
                break;
            default:
                appendStringInfo( str, "\"UNKNOWN\"" );
                break;
        }
    }
    else
    {
        appendStringInfo( str, ",\"utilityStmt\":NULL" );
    }

    appendStringInfo( str, ",\"resultRelation\":%d", node->resultRelation );
    appendStringInfo( str, ",\"hasAggs\":%s", node->hasAggs ? "true" : "false" );
    appendStringInfo( str, ",\"hasWindowFuncs\":%s", node->hasWindowFuncs ? "true" : "false" );
    appendStringInfo( str, ",\"hasTargetSRFs\":%s", node->hasTargetSRFs ? "true" : "false" );
    appendStringInfo( str, ",\"hasSubLinks\":%s", node->hasSubLinks ? "true" : "false" );
    appendStringInfo( str, ",\"hasDistinctOn\":%s", node->hasDistinctOn ? "true" : "false" );
    appendStringInfo( str, ",\"hasRecursive\":%s", node->hasRecursive ? "true" : "false" );
    appendStringInfo( str, ",\"hasModifyingCTE\":%s", node->hasModifyingCTE ? "true" : "false" );
    appendStringInfo( str, ",\"hasForUpdate\":%s", node->hasForUpdate ? "true" : "false" );
    appendStringInfo( str, ",\"hasRowSecurity\":%s", node->hasRowSecurity ? "true" : "false" );
    appendStringInfo( str, ",\"isReturn\":%s", node->isReturn ? "true" : "false" );
    appendStringInfo( str, ",\"cteList\":" );
    Node_out( str, node->cteList );
    appendStringInfo( str, ",\"rtable\":" );
    Node_out( str, node->rtable );
    appendStringInfo( str, ",\"jointree\":" );
    Node_out( str, node->jointree );
    appendStringInfo( str, ",\"targetList\":" );
    Node_out( str, node->targetList );
    // override
    appendStringInfo( str, ",\"onConflict\":" );
    Node_out( str, node->onConflict );
    appendStringInfo( str, ",\"returningList\":" );
    Node_out( str, node->returningList );
    appendStringInfo( str, ",\"groupClause\":" );
    Node_out( str, node->groupClause );
    appendStringInfo( str, ",\"groupDistinct\":%s", node->groupDistinct ? "true" : "false" );
    appendStringInfo( str, ",\"havingQual\":" );
    Node_out( str, node->havingQual );
    appendStringInfo( str, ",\"windowClause\":" );
    Node_out( str, node->windowClause );
    appendStringInfo( str, ",\"distinctClause\":" );
    Node_out( str, node->distinctClause );
    appendStringInfo( str, ",\"sortClause\":" );
    Node_out( str, node->sortClause );
    appendStringInfo( str, ",\"limitOffset\":" );
    Node_out( str, node->limitOffset );
    appendStringInfo( str, ",\"limitCount\":" );
    Node_out( str, node->limitCount );
    // limitOption
    appendStringInfo( str, ",\"rowMarks\":" );
    Node_out( str, node->rowMarks );
    appendStringInfo( str, ",\"setOperations\":" );
    Node_out( str, node->setOperations );
    //appendStringInfo( str, ",\"constraintDeps\":" );
    //Node_out( str, node->constraintDepts );
    appendStringInfo( str, ",\"withCheckOptions\":" );
    Node_out( str, node->withCheckOptions );
    appendStringInfo( str, ",\"stmt_location\":%d", node->stmt_location );
    appendStringInfo( str, ",\"stmt_len\":%d", node->stmt_len );
    return;
}

static void RangeFunction_out( StringInfo str, RangeFunction * node )
{
    appendStringInfoString( str, "\"name\":\"RANGEFUNCTION\"" );
    appendStringInfo( str, ",\"lateral\":%s", node->lateral ? "true" : "false" );
    appendStringInfo( str, ",\"ordinality\":%s", node->ordinality ? "true" : "false" );
    appendStringInfo( str, ",\"is_rowsfrom\":%s", node->is_rowsfrom ? "true" : "false" );
    appendStringInfo( str, ",\"functions\":" );
    Node_out( str, node->functions );
    appendStringInfo( str, ",\"alias\":" );
    Node_out( str, node->alias );
    appendStringInfo( str, ",\"coldeflist\":" );
    Node_out( str, node->coldeflist );
    return;
}

static void RangeSubselect_out( StringInfo str, RangeSubselect * node )
{
    appendStringInfoString( str, "\"name\":\"RANGESUBSELECT\"" );
    appendStringInfo( str, ",\"subquery\":" );
    Node_out( str, node->subquery );
    appendStringInfo( str, ",\"alias\":" );
    Node_out( str, node->alias );
    return;
}

static void RangeTblEntry_out( StringInfo str, RangeTblEntry * node )
{
    appendStringInfoString( str, "\"name\":\"RTE\"" );
    appendStringInfo( str, ",\"alias\":" );
    Node_out( str, node->alias );
    appendStringInfo( str, ",\"eref\":" );
    Node_out( str, node->eref );
    appendStringInfo( str, ",\"rtekind\":\"%s\"", enum_RTEKind( node->rtekind ) );

    switch( node->rtekind )
    {
        case RTE_RELATION:
            appendStringInfo( str, ",\"relid\":%u", node->relid );
            appendStringInfo( str, ",\"relkind\":\"%c\"", node->relkind );
            appendStringInfo( str, ",\"rellockmode\":%d", node->rellockmode );
            break;
        case RTE_SUBQUERY:
            appendStringInfo( str, ",\"subquery\":" );
            Node_out( str, node->subquery );
            break;
        case RTE_JOIN:
            appendStringInfo( str, ",\"jointype\":\"%s\"", enum_JoinType( node->jointype ) );
            appendStringInfo( str, ",\"joinaliasvars\":" );
            Node_out( str, node->joinaliasvars );
            break;
        case RTE_FUNCTION:
            appendStringInfo( str, ",\"functions\":" );
            Node_out( str, node->functions );
            appendStringInfo( str, ",\"funcordinality\":%s", node->funcordinality ? "true" : "false" );
            break;
        case RTE_VALUES:
            appendStringInfo( str, ",\"values_lists\":" );
            Node_out( str, node->values_lists );
            break;
        case RTE_CTE:
            appendStringInfo( str, ",\"ctename\":" );
            Token_out( str, node->ctename );
            appendStringInfo( str, ",\"ctelevelsup\":%u", node->ctelevelsup );
            appendStringInfo( str, ",\"self_reference\":%s", node->self_reference ? "true" : "false" );
            break;
        default:
            elog( ERROR, "Unknown rtekind: %d", ( int ) node->rtekind );
            break;
    }

    appendStringInfo( str, ",\"inh\":%s", node->inh ? "true" : "false" );
    appendStringInfo( str, ",\"lateral\":%s", node->lateral ? "true" : "false" );
    appendStringInfo( str, ",\"inFromCl\":%s", node->inFromCl ? "true" : "false" );
    appendStringInfo( str, ",\"requiredPerms\":%u", node->requiredPerms );
    appendStringInfo( str, ",\"checkAsUser\":%u", node->checkAsUser );
    appendStringInfo( str, ",\"selectedCols\":" );
    Bitmapset_out( str, node->selectedCols );
    appendStringInfo( str, ",\"insertedCols\":" );
    Bitmapset_out( str, node->insertedCols );
    appendStringInfo( str, ",\"updatedCols\":" );
    Bitmapset_out( str, node->updatedCols );
    appendStringInfo( str, ",\"extraUpdatedCols\":" );
    Bitmapset_out( str, node->extraUpdatedCols );
    appendStringInfo( str, ",\"securityQuals\":" );
    Node_out( str, node->securityQuals );
    return;
}

static void RangeTblRef_out( StringInfo str, RangeTblRef * node )
{
    appendStringInfoString( str, "\"name\":\"RANGETBLREF\"" );
    appendStringInfo( str, ",\"rtindex\":%d", node->rtindex );
    return;
}

static void RangeVar_out( StringInfo str, RangeVar * node )
{
    appendStringInfoString( str, "\"name\":\"RANGEVAR\"" );
    // 'catalogname' is ignored - cross db is not supported so this will always be current_database()
    if( node->schemaname )
    {
        appendStringInfo( str, ",\"schemaname\":" );
        Token_out( str, node->schemaname );
    }

    if( node->relname )
    {
        appendStringInfo( str, ",\"relname\":" );
        Token_out( str, node->relname );
    }

    appendStringInfo( str, ",\"inh\":%s", node->inh ? "true" : "false" );
    appendStringInfo( str, ",\"relpersistence\":\"%c\"", node->relpersistence );

    if( node->alias )
    {
        appendStringInfo( str, ",\"alias\":" );
        Node_out( str, node->alias );
    }
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void RawStmt_out( StringInfo str, RawStmt * node )
{
    appendStringInfoString( str, "\"name\":\"RAWSTMT\"" );
    appendStringInfoString( str, ",\"stmt\":" );
    Node_out( str, node->stmt );
    appendStringInfo( str, ",\"stmt_location\":%d", node->stmt_location );
    appendStringInfo( str, ",\"stmt_len\":%d", node->stmt_len );
    return;
}

static void RecursiveUnion_out( StringInfo str, RecursiveUnion * node )
{
    int i     = 0;
    int first = 1;

    appendStringInfoString( str, "\"name\":\"RECURSIVEUNION\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"wtParam\":%d", node->wtParam );
    appendStringInfo( str, ",\"numCols\":%d", node->numCols );
    appendStringInfo( str, "\"dupColIdx\":[" );

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%d", node->dupColIdx[i] );
    }

    appendStringInfo( str, "],\"dupOperators\":[" );
    first = 1;

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, " %u", node->dupOperators[i] );
    }

    appendStringInfo( str, "],\"numGroups\":%ld", node->numGroups );
    return;
}

static void RelabelType_out( StringInfo str, RelabelType * node )
{
    appendStringInfoString( str, "\"name\":\"RELABELTYPE\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"resulttype\":%u", node->resulttype );
    appendStringInfo( str, ",\"resulttypmod\":%d", node->resulttypmod );
    appendStringInfo( str, ",\"resultcollid\":%u", node->resultcollid );
    appendStringInfo( str, ",\"relabelformat\":\"%s\"", enum_CoercionForm( node->relabelformat ) );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

// This is a partial set of node attributes
static void RelOptInfo_out( StringInfo str, RelOptInfo * node )
{
    appendStringInfoString( str, "\"name\":\"RELOPTINFO\"" );
    appendStringInfo( str, ",\"reloptkind\":%d", node->reloptkind ); // enum - RelOptKind
    appendStringInfo( str, ",\"relids\":" );
    Bitmapset_out( str, node->relids );
    appendStringInfo( str, ",\"rows\":%.0f", node->rows );
    appendStringInfo( str, ",\"pathlist\":" );
    Node_out( str, node->pathlist );
    appendStringInfo( str, ",\"cheapest_startup_path\":" );
    Node_out( str, node->cheapest_startup_path );
    appendStringInfo( str, ",\"cheapest_total_path\":" );
    Node_out( str, node->cheapest_total_path );
    appendStringInfo( str, ",\"cheapest_unique_path\":" );
    Node_out( str, node->cheapest_unique_path );
    appendStringInfo( str, ",\"relid\":%u", node->relid );
    appendStringInfo( str, ",\"reltablespace\":%u", node->reltablespace );
    appendStringInfo( str, ",\"rtekind\":\"%s\"", enum_RTEKind( node->rtekind ) );
    appendStringInfo( str, ",\"min_attr\":%d", node->min_attr );
    appendStringInfo( str, ",\"max_attr\":%d", node->max_attr );
    appendStringInfo( str, ",\"indexlist\":" );
    Node_out( str, node->indexlist );
    appendStringInfo( str, ",\"pages\":%u", node->pages );
    appendStringInfo( str, ",\"tuples\":%.0f", node->tuples );
    appendStringInfo( str, ",\"baserestrictinfo\":" );
    Node_out( str, node->baserestrictinfo );
    appendStringInfo( str, ",\"joininfo\":" );
    Node_out( str, node->joininfo );
    appendStringInfo( str, ",\"has_eclass_joins\":%s", node->has_eclass_joins ? "true" : "false" );
    return;
}

// This is a partial set of node attributes
static void RestrictInfo_out( StringInfo str, RestrictInfo * node )
{
    appendStringInfoString( str, "\"name\":\"RESTRICTINFO\"" );
    appendStringInfo( str, ",\"clause\":" );
    Node_out( str, node->clause );
    appendStringInfo( str, ",\"is_pushed_down\":%s", node->is_pushed_down ? "true" : "false" );
    appendStringInfo( str, ",\"outerjoin_delayed\":%s", node->outerjoin_delayed ? "true" : "false" );
    appendStringInfo( str, ",\"can_join\":%s", node->can_join ? "true" : "false" );
    appendStringInfo( str, ",\"pseudoconstant\":%s", node->pseudoconstant ? "true" : "false" );
    appendStringInfo( str, ",\"clause_relids\":" );
    Bitmapset_out( str, node->clause_relids );
    appendStringInfo( str, ",\"required_relids\":" );
    Bitmapset_out( str, node->required_relids );
    appendStringInfo( str, ",\"nullable_relids\":" );
    Bitmapset_out( str, node->nullable_relids );
    appendStringInfo( str, ",\"left_relids\":" );
    Bitmapset_out( str, node->left_relids );
    appendStringInfo( str, ",\"right_relids\":" );
    Bitmapset_out( str, node->right_relids );
    appendStringInfo( str, ",\"orclause\":" );
    Node_out( str, node->orclause );
    // Ignore parent_ec
    appendStringInfo( str, ",\"norm_selec\":%.4f", node->norm_selec );
    appendStringInfo( str, ",\"outer_selec\":%.4f", node->outer_selec );
    appendStringInfo( str, ",\"mergeopfamilies\":" );
    Node_out( str, node->mergeopfamilies );
    // ignore left_ec and right_ec
    appendStringInfo( str, ",\"left_em\":" );
    Node_out( str, node->left_em );
    appendStringInfo( str, ",\"right_em\":" );
    Node_out( str, node->right_em );
    appendStringInfo( str, ",\"outer_is_left\":%s", node->outer_is_left ? "true" : "false" );
    appendStringInfo( str, ",\"hashjoinoperator\":%u", node->hashjoinoperator );
    return;
}

static void ResTarget_out( StringInfo str, ResTarget * node )
{
    appendStringInfoString( str, "\"name\":\"RESTARGET\"" );

    if( node->name )
    {
        appendStringInfo( str, ",\"name\":" );
        Token_out( str, node->name );
    }

    if( node->indirection )
    {
        appendStringInfo( str, ",\"indirection\":" );
        Node_out( str, node->indirection );
    }

    appendStringInfo( str, ",\"val\":" );
    Node_out( str, node->val );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void Result_out( StringInfo str, Result * node )
{
    appendStringInfoString( str, "\"name\":\"RESULT\"" );
    PlanInfo_out( str, (Plan *) node );
    appendStringInfo( str, ",\"resconstantqual\":" );
    Node_out( str, node->resconstantqual );
    return;
}

static void RowCompareExpr_out( StringInfo str, RowCompareExpr * node )
{
    appendStringInfoString( str, "\"name\":\"ROWCOMPARE\"" );
    appendStringInfo( str, ",\"rctype\":%d", node->rctype ); // enum - RowCompareType
    appendStringInfo( str, ",\"opnos\":" );
    Node_out( str, node->opnos );
    appendStringInfo( str, ",\"opfamilies\":" );
    Node_out( str, node->opfamilies );
    appendStringInfo( str, ",\"inputcollids\":" );
    Node_out( str, node->inputcollids );
    appendStringInfo( str, ",\"largs\":" );
    Node_out( str, node->largs );
    appendStringInfo( str, ",\"rargs\":" );
    Node_out( str, node->rargs );
    return;
}

static void RowExpr_out( StringInfo str, RowExpr * node )
{
    appendStringInfoString( str, "\"name\":\"ROW\"" );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"row_typeid\":%u", node->row_typeid );
    appendStringInfo( str, ",\"row_format\":\"%s\"", enum_CoercionForm( node->row_format ) );
    appendStringInfo( str, ",\"colnames\":" );
    Node_out( str, node->colnames );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void RowMarkClause_out( StringInfo str, RowMarkClause * node )
{
    appendStringInfoString( str, "\"name\":\"ROWMARKCLAUSE\"" );
    appendStringInfo( str, ",\"rti\":%u", node->rti );
    // Need strength and waitPolicy
    appendStringInfo( str, ",\"pushedDown\":%s", node->pushedDown ? "true" : "false" );
    return;
}

static void ScalarArrayOpExpr_out( StringInfo str, ScalarArrayOpExpr * node )
{
    appendStringInfoString( str, "\"name\":\"SCALARARRAYOPEXPR\"" );
    appendStringInfo( str, ",\"opno\":%u", node->opno );
    appendStringInfo( str, ",\"opfuncid\":%u", node->opfuncid );
    appendStringInfo( str, ",\"useOr\":%s", node->useOr ? "true" : "false" );
    appendStringInfo( str, ",\"inputcollid\":%u", node->inputcollid );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void ScanInfo_out( StringInfo str, Scan * node )
{
    // Write out common scan node stuff before returning
    // and callee writing out specific information
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"scanrelid\":%u", node->scanrelid );
    return;
}

static void Scan_out( StringInfo str, Scan * node )
{
    appendStringInfoString( str, "\"name\":\"SCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    return;
}

static void SelectStmt_out( StringInfo str, SelectStmt * node )
{
    appendStringInfoString( str, "\"name\":\"SELECTSTMT\"" );
    if( node->distinctClause )
    {
        appendStringInfo( str, ",\"distinctClause\":" );
        Node_out( str, node->distinctClause );
    }

    if( node->intoClause )
    {
        appendStringInfo( str, ",\"intoClause\":" );
        Node_out( str, node->intoClause );
    }

    if( node->targetList )
    {
        appendStringInfo( str, ",\"targetList\":" );
        Node_out( str, node->targetList );
    }

    if( node->fromClause )
    {
        appendStringInfo( str, ",\"fromClause\":" );
        Node_out( str, node->fromClause );
    }

    if( node->whereClause )
    {
        appendStringInfo( str, ",\"whereClause\":" );
        Node_out( str, node->whereClause );
    }

    if( node->groupClause )
    {
        appendStringInfo( str, ",\"groupClause\":" );
        Node_out( str, node->groupClause );
    }

    if( node->havingClause )
    {
        appendStringInfo( str, ",\"havingClause\":" );
        Node_out( str, node->havingClause );
    }

    if( node->windowClause )
    {
        appendStringInfo( str, ",\"windowClause\":" );
        Node_out( str, node->windowClause );
    }

    if( node->withClause )
    {
        appendStringInfo( str, ",\"withClause\":" );
        Node_out( str, node->withClause );
    }

    if( node->valuesLists )
    {
        appendStringInfo( str, ",\"valuesLists\":" );
        Node_out( str, node->valuesLists );
    }

    if( node->sortClause )
    {
        appendStringInfo( str, ",\"sortClause\":" );
        Node_out( str, node->sortClause );
    }

    if( node->limitOffset )
    {
        appendStringInfo( str, ",\"limitOffset\":" );
        Node_out( str, node->limitOffset );
    }

    if( node->limitCount )
    {
        appendStringInfo( str, ",\"limitCount\":" );
        Node_out( str, node->limitCount );
    }

    if( node->lockingClause )
    {
        appendStringInfo( str, ",\"lockingClause\":" );
        Node_out( str, node->lockingClause );
    }

    appendStringInfo( str, ",\"op\":%d", node->op ); // enum - SetOperation
    appendStringInfo( str, ",\"all\":%s", node->all ? "true" : "false" );
    
    if( node->larg )
    {
        appendStringInfo( str, ",\"larg\":" );
        Node_out( str, node->larg );
    }

    if( node->rarg )
    {
        appendStringInfo( str, ",\"rarg\":" );
        Node_out( str, node->rarg );
    }
    return;
}

static void SeqScan_out( StringInfo str, SeqScan * node )
{
    appendStringInfoString( str, "\"name\":\"SEQSCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    return;
}

static void SetOperationStmt_out( StringInfo str, SetOperationStmt * node )
{
    appendStringInfoString( str, "\"name\":\"SETOPERATIONSTMT\"" );
    appendStringInfo( str, ",\"op\":%d", node->op ); // enum - SetOperation
    appendStringInfo( str, ",\"all\":%s", node->all ? "true" : "false" );
    appendStringInfo( str, ",\"larg\":" );
    Node_out( str, node->larg );
    appendStringInfo( str, ",\"rarg\":" );
    Node_out( str, node->rarg );
    appendStringInfo( str, ",\"colTypes\":" );
    Node_out( str, node->colTypes );
    appendStringInfo( str, ",\"colTypmods\":" );
    Node_out( str, node->colTypmods );
    appendStringInfo( str, ",\"colCollations\":" );
    Node_out( str, node->colCollations );
    appendStringInfo( str, ",\"groupClauses\":" );
    Node_out( str, node->groupClauses );
    return;
}

static void SetOp_out( StringInfo str, SetOp * node )
{
    int     i = 0;
    int first = 1;

    appendStringInfoString( str, "\"name\":\"SETOP\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"cmd\":%d", node->cmd ); // enum - SetOpCmd
    appendStringInfo( str, ",\"strategy\":%d", node->strategy ); // enum - SetOpStrategy
    appendStringInfo( str, ",\"numCols\":%d", node->numCols );
    appendStringInfo( str, ",\"dupColIdx\":[" );

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%d", node->dupColIdx[i] );
    }

    appendStringInfo( str, "],\"dupOperators\":[" );
    first = 1;

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->dupOperators[i] );
    }

    appendStringInfo( str, "],\"flagColIdx\":%d", node->flagColIdx );
    appendStringInfo( str, ",\"firstFlag\":%d", node->firstFlag );
    appendStringInfo( str, ",\"numGroups\":%ld", node->numGroups );
    return;
}

static void SetToDefault_out( StringInfo str, SetToDefault * node )
{
    appendStringInfoString( str, "\"name\":\"SETTODEFAULT\"" );
    appendStringInfo( str, ",\"typeId\":%u", node->typeId );
    appendStringInfo( str, ",\"typeMod\":%d", node->typeMod );
    appendStringInfo( str, ",\"collation\":%u", node->collation );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void SortBy_out( StringInfo str, SortBy * node )
{
    appendStringInfoString( str, "\"name\":\"SORTBY\"" );
    appendStringInfo( str, ",\"node\":" );
    Node_out( str, node->node );
    appendStringInfo( str, ",\"sortby_dir\":\"%s\"", enum_SortByDir( node->sortby_dir ) );
    appendStringInfo( str, ",\"sortby_nulls\":\"%s\"", enum_SortByNulls( node->sortby_nulls ) );
    appendStringInfo( str, ",\"useOp\":" );
    Node_out( str, node->useOp );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void SortGroupClause_out( StringInfo str, SortGroupClause * node )
{
    appendStringInfoString( str, "\"name\":\"SORTGROUPCLAUSE\"" );
    appendStringInfo( str, ",\"tleSortGroupRef\":%u", node->tleSortGroupRef );
    appendStringInfo( str, ",\"eqop\":%u", node->eqop );
    appendStringInfo( str, ",\"sortop\":%u", node->sortop );
    appendStringInfo( str, ",\"nulls_first\":%s", node->nulls_first ? "true" : "false" );
    appendStringInfo( str, ",\"hashable\":%s", node->hashable ? "true" : "false" );
    return;
}

static void Sort_out( StringInfo str, Sort * node )
{
    int i     = 0;
    int first = 1;

    appendStringInfoString( str, "\"name\":\"SORT\"" );
    PlanInfo_out( str, (Plan *) node );
    appendStringInfo( str, ",\"numCols\":%d", node->numCols );
    appendStringInfo( str, ",\"sortColIdx\":[" );

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%d", node->sortColIdx[i] );
    }

    appendStringInfo( str, "],\"sortOperators\":[" );
    first = 1;

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->sortOperators[i] );
    }

    appendStringInfo( str, "],\"collations\":[" );
    first = 1;

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->collations[i] );
    }

    appendStringInfo( str, "],\"nullsFirst\":[" );
    first = 1;

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%s", node->nullsFirst[i] ? "true" : "false" );
    }

    appendStringInfoChar( str, ']' );
    return;
}

static void SpecialJoinInfo_out( StringInfo str, SpecialJoinInfo * node )
{
    appendStringInfoString( str, "\"name\":\"SPECIALJOININFO\"" );
    appendStringInfo( str, ",\"min_lefthand\":" );
    Bitmapset_out( str, node->min_lefthand );
    appendStringInfo( str, ",\"min_righthand\":" );
    Bitmapset_out( str, node->min_righthand );
    appendStringInfo( str, ",\"syn_lefthand\":" );
    Bitmapset_out( str, node->syn_lefthand );
    appendStringInfo( str, ",\"syn_righthand\":" );
    Bitmapset_out( str, node->syn_righthand );
    appendStringInfo( str, ",\"jointype\":\"%s\"", enum_JoinType( node->jointype ) );
    appendStringInfo( str, ",\"lhs_strict\":%s", node->lhs_strict ? "true" : "false" );
    appendStringInfo( str, ",\"delay_upper_joins\":%s", node->delay_upper_joins ? "true" : "false" );
    appendStringInfo( str, ",\"semi_can_hash\":%s", node->semi_can_hash ? "true" : "false" );
    appendStringInfo( str, ",\"semi_can_btree\":%s", node->semi_can_btree ? "true" : "false" );
    appendStringInfo( str, ",\"semi_operators\":" );
    Node_out( str, node->semi_operators );
    appendStringInfo( str, ",\"semi_rhs_exprs\":" );
    Node_out( str, node->semi_rhs_exprs );
    return;
}

static void SubLink_out( StringInfo str, SubLink * node )
{
    appendStringInfoString( str, "\"name\":\"SUBLINK\"" );
    appendStringInfo( str, ",\"subLinkType\":\"%s\"", enum_SubLinkType( node->subLinkType ) );
    appendStringInfo( str, ",\"testexpr\":" );
    Node_out( str, node->testexpr );
    appendStringInfo( str, ",\"operName\":" );
    Node_out( str, node->operName );
    appendStringInfo( str, ",\"subselect\":" );
    Node_out( str, node->subselect );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void SubPlan_out( StringInfo str, SubPlan * node )
{
    appendStringInfoString( str, "\"name\":\"SUBPLAN\"" );
    appendStringInfo( str, ",\"subLinkType\":\"%s\"", enum_SubLinkType( node->subLinkType ) );
    appendStringInfo( str, ",\"testexpr\":" );
    Node_out( str, node->testexpr );
    appendStringInfo( str, ",\"paramIds\":" );
    Node_out( str, node->paramIds );
    appendStringInfo( str, ",\"plan_id\":%d", node->plan_id );
    appendStringInfo( str, ",\"plan_name\":" );
    Token_out( str, node->plan_name );
    appendStringInfo( str, ",\"firstColType\":%u", node->firstColType );
    appendStringInfo( str, ",\"firstColTypmod\":%d", node->firstColTypmod );
    appendStringInfo( str, ",\"firstColCollation\":%u", node->firstColCollation );
    appendStringInfo( str, ",\"useHashTable\":%s", node->useHashTable ? "true" : "false" );
    appendStringInfo( str, ",\"unknownEqFalse\":%s", node->unknownEqFalse ? "true" : "false" );
    appendStringInfo( str, ",\"setParam\":" );
    Node_out( str, node->setParam );
    appendStringInfo( str, ",\"parParam\":" );
    Node_out( str, node->parParam );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"startup_cost\":%.2f", node->startup_cost );
    appendStringInfo( str, ",\"per_col_cost\":%.2f", node->per_call_cost );
    return;
}

static void SubqueryScan_out( StringInfo str, SubqueryScan * node )
{
    appendStringInfoString( str, "\"name\":\"SUBQUERYSCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    appendStringInfo( str, ",\"subplan\":" );
    Node_out( str, node->subplan );
    return;
}

static void TargetEntry_out( StringInfo str, TargetEntry * node )
{
    appendStringInfoString( str, "\"name\":\"TARGETENTRY\"" );
    appendStringInfo( str, ",\"expr\":" );
    Node_out( str, node->expr );
    appendStringInfo( str, ",\"resno\":%d", node->resno );
    appendStringInfo( str, ",\"resname\":" );
    Token_out( str, node->resname );
    appendStringInfo( str, ",\"ressortgroupref\":%u", node->ressortgroupref );
    appendStringInfo( str, ",\"resorigtbl\":%u", node->resorigtbl );
    appendStringInfo( str, ",\"resorigcol\":%d", node->resorigcol );
    appendStringInfo( str, ",\"resjunk\":%s", node->resjunk ? "true" : "false" );
    return;
}

static void TidPath_out( StringInfo str, TidPath * node )
{
    appendStringInfoString( str, "\"name\":\"TIDPATH\"" );
    PathInfo_out( str, ( Path * ) node );
    appendStringInfo( str, ",\"tidquals\":" );
    Node_out( str, node->tidquals );
    return;
}

static void TidScan_out( StringInfo str, TidScan * node )
{
    appendStringInfoString( str, "\"name\":\"TIDSCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    appendStringInfo( str, ",\"tidquals\":" );
    Node_out( str, node->tidquals );
    return;
}

static void Token_out( StringInfo str, char * s )
{
    if( s == NULL || *s == '\0' )
    {
        appendStringInfo( str, "null" );
        return;
    }

    appendStringInfoChar( str, '"' );
    // Treat tokens similar to read.c (nodeRead wrapper or underlying pg_strtok function)
    if(
            *s == '<'
         || *s == '\"'
         || isdigit( ( unsigned char ) *s )
         || (
                (*s == '+' || *s == '-' )
             && (
                     isdigit( ( unsigned char ) s[1] )
                  || s[1] == '.'
                )
            )
      )
        appendStringInfoChar( str, '\\' );

    while( *s )
    {
        // Handle regular escapes
        if(
                *s == ' '
             || *s == '\n'
             || *s == '\t'
             || *s == '('
             || *s == ')'
             || *s == '{'
             || *s == '}'
             || *s == '\\'
          )
        {
            appendStringInfoChar( str, '\\' );
        }

        appendStringInfoChar( str, *s++ );
    }

    appendStringInfoChar( str, '"' );
    return;
}

static void TypeCast_out( StringInfo str, TypeCast * node )
{
    appendStringInfoString( str, "\"name\":\"TYPECAST\"" );
    appendStringInfo( str, ",\"arg\":" );
    Node_out( str, node->arg );
    appendStringInfo( str, ",\"typeName\":" );
    Node_out( str, node->typeName );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void TypeName_out( StringInfo str, TypeName * node )
{
    appendStringInfoString( str, "\"name\":\"TYPENAME\"" );
    appendStringInfo( str, ",\"names\":" );
    Node_out( str, node->names );
    appendStringInfo( str, ",\"typeOid\":%u", node->typeOid );
    appendStringInfo( str, ",\"setof\":%s", node->setof ? "true" : "false" );
    appendStringInfo( str, ",\"pct_type\":%s", node->pct_type ? "true" : "false" );
    appendStringInfo( str, ",\"typmods\":" );
    Node_out( str, node->typmods );
    appendStringInfo( str, ",\"typemod\":%d", node->typemod );
    appendStringInfo( str, ",\"arrayBounds\":" );
    Node_out( str, node->arrayBounds );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void UniquePath_out( StringInfo str, UniquePath * node )
{
    appendStringInfoString( str, "\"name\":\"UNIQUEPATH\"" );
    PathInfo_out( str, ( Path * ) node );
    appendStringInfo( str, ",\"subpath\":" );
    Node_out( str, node->subpath );
    appendStringInfo( str, ",\"umethod\":%d", node->umethod ); // enum - UniquePathMethod
    appendStringInfo( str, ",\"in_operators\":" );
    Node_out( str, node->in_operators );
    appendStringInfo( str, ",\"uniq_exprs\":" );
    Node_out( str, node->uniq_exprs );
    return;
}

static void Unique_out( StringInfo str, Unique * node )
{
    int i     = 0;
    int first = 1;

    appendStringInfoString( str, "\"name\":\"UNIQUE\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"numCols\":%d", node->numCols );
    appendStringInfo(str, ",\"uniqColIdx\":[");

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%d", node->uniqColIdx[i] );
    }

    appendStringInfo( str, "],\"uniqOperators\":[" );
    first = 1;

    for( i = 0; i < node->numCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->uniqOperators[i] );
    }

    appendStringInfoChar( str, ']' );
    return;
}

static void ValuesScan_out( StringInfo str, ValuesScan * node )
{
    appendStringInfoString( str, "\"name\":\"VALUESCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    appendStringInfo( str, ",\"values_lists\":" );
    Node_out( str, node->values_lists );
    return;
}

static void Value_out( StringInfo str, Value * value )
{
    switch( value->type )
    {
        case T_Integer:
            appendStringInfo( str, "%d", value->val.ival );
            break;
        case T_Float:
            // Will break if NaN / Inf / -Inf
            appendStringInfoString( str, value->val.str );
            break;
        case T_String:
            Token_out( str, value->val.str );
            break;
        case T_BitString:
            appendStringInfoChar( str, '"' );
            appendStringInfoString( str, value->val.str );
            appendStringInfoChar( str, '"' );
            break;
        case T_Null:
            appendStringInfoString( str, "null" );
            break;
        default:
            elog( ERROR, "unrecognized value type: %d", ( int ) value->type );
            break;
    }

    return;
}

static void Var_out( StringInfo str, Var * node )
{
    appendStringInfoString( str, "\"name\":\"VAR\"" );
    appendStringInfo( str, ",\"varno\":%u", node->varno );
    appendStringInfo( str, ",\"varattno\":%d", node->varattno );
    appendStringInfo( str, ",\"vartype\":%u", node->vartype );
    appendStringInfo( str, ",\"vartypmod\":%d", node->vartypmod );
    appendStringInfo( str, ",\"varcollid\":%u", node->varcollid );
    appendStringInfo( str, ",\"varlevelsup\":%u", node->varlevelsup );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void WindowAgg_out( StringInfo str, WindowAgg * node )
{
    int i     = 0;
    int first = 1;

    appendStringInfoString( str, "\"name\":\"WINDOWAGG\"" );
    PlanInfo_out( str, ( Plan * ) node );
    appendStringInfo( str, ",\"winref\":%u", node->winref );
    appendStringInfo( str, ",\"partNumCols\":%d", node->partNumCols );
    appendStringInfo( str, "\"partColIdx\":[" );

    for( i = 0; i < node->partNumCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%d", node->partColIdx[i] );
    }

    appendStringInfo( str, "],\"partOperations\":[" );
    first = 1;

    for( i = 0; i < node->partNumCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->partOperators[i] );
    }

    appendStringInfo( str, "],\"ordNumCols\":%d", node->ordNumCols );
    appendStringInfo( str, " ,\"ordColIdx\":[" );
    first = 1;

    for( i = 0; i < node->ordNumCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%d", node->ordColIdx[i] );
    }

    appendStringInfo( str, "],\"ordOperations\":[" );
    first = 1;

    for( i = 0; i < node->ordNumCols; i++ )
    {
        if( first )
            first = 0;
        else
            appendStringInfoChar( str, ',' );

        appendStringInfo( str, "%u", node->ordOperators[i] );
    }

    appendStringInfo( str, "],\"frameOptions\":%d", node->frameOptions );
    appendStringInfo( str, ",\"startOffset\":" );
    Node_out( str, node->startOffset );
    appendStringInfo( str, ",\"endOffset\":" );
    Node_out( str, node->endOffset );
    return;
}

static void WindowClause_out( StringInfo str, WindowClause * node )
{
    appendStringInfoString( str, "\"name\":\"WINDOWCLAUSE\"" );
    appendStringInfo( str, ",\"name\":" );
    Token_out( str, node->name );
    appendStringInfo( str, ",\"refname\":" );
    Token_out( str, node->refname );
    appendStringInfo( str, ",\"partitionClause\":" );
    Node_out( str, node->partitionClause );
    appendStringInfo( str, ",\"orderClause\":" );
    Node_out( str, node->orderClause );
    appendStringInfo( str, ",\"frameOptions\":%d", node->frameOptions );
    appendStringInfo( str, ",\"startOffset\":" );
    Node_out( str, node->startOffset );
    appendStringInfo( str, ",\"endOffset\":" );
    Node_out( str, node->endOffset );
    appendStringInfo( str, ",\"winref\":%u", node->winref );
    appendStringInfo( str, ",\"copiedOrder\":%s", node->copiedOrder ? "true" : "false" );
    return;
}

static void WindowDef_out( StringInfo str, WindowDef * node )
{
    appendStringInfoString( str, "\"name\":\"WINDOWDEF\"" );
    appendStringInfo( str, ",\"name\":" );
    Token_out( str, node->name );
    appendStringInfo( str, ",\"refname\":" );
    Token_out( str, node->refname );
    appendStringInfo( str, ",\"partitionClause\":" );
    Node_out( str, node->partitionClause );
    appendStringInfo( str, ",\"orderClause\":" );
    Node_out( str, node->orderClause );
    appendStringInfo( str, ",\"frameOptions\":%d", node->frameOptions );
    appendStringInfo( str, ",\"startOffset\":" );
    Node_out( str, node->startOffset );
    appendStringInfo( str, ",\"endOffset\":" );
    Node_out( str, node->endOffset );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void WindowFunc_out( StringInfo str, WindowFunc * node )
{
    appendStringInfoString( str, "\"name\":\"WINDOWFUNC\"" );
    appendStringInfo( str, ",\"winfnoid\":%u", node->winfnoid );
    appendStringInfo( str, ",\"wintype\":%u", node->wintype );
    appendStringInfo( str, ",\"wincollid\":%u", node->wincollid );
    appendStringInfo( str, ",\"inputcollid\":%u", node->inputcollid );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"winref\":%u", node->winref );
    appendStringInfo( str, ",\"winstar\":%s", node->winstar ? "true" : "false" );
    appendStringInfo( str, ",\"winagg\":%s", node->winagg ? "true" : "false" );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void WithClause_out( StringInfo str, WithClause * node )
{
    appendStringInfoString( str, "\"name\":\"WITHCLAUSE\"" );
    appendStringInfo( str, ",\"ctes\":" );
    Node_out( str, node->ctes );
    appendStringInfo( str, ",\"recursive\":%s", node->recursive ? "true" : "false" );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void WorkTableScan_out( StringInfo str, WorkTableScan * node )
{
    appendStringInfoString( str, "\"name\":\"WORKTABLESCAN\"" );
    ScanInfo_out( str, ( Scan * ) node );
    appendStringInfo( str, ",\"wtParam\":%d", node->wtParam );
    return;
}

static void XmlExpr_out( StringInfo str, XmlExpr * node )
{
    appendStringInfoString( str, "\"name\":\"XMLEXPR\"" );
    appendStringInfo( str, ",\"op\":%d", node->op ); // enum - XmlExprOp
    appendStringInfo( str, ",\"name\":" );
    Token_out( str, node->name );
    appendStringInfo( str, ",\"named_args\":" );
    Node_out( str, node->named_args );
    appendStringInfo( str, ",\"arg_names\":" );
    Node_out( str, node->arg_names );
    appendStringInfo( str, ",\"args\":" );
    Node_out( str, node->args );
    appendStringInfo( str, ",\"xmloption\":%d", node->xmloption ); // enum - XmlOptionType
    appendStringInfo( str, ",\"type\":%u", node->type );
    appendStringInfo( str, ",\"typmod\":%d", node->typmod );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
}

static void XmlSerialize_out( StringInfo str, XmlSerialize * node )
{
    appendStringInfoString( str, "\"name\":\"XMLSERIALIZE\"" );
    appendStringInfo( str, ",\"xmloption\":%d", node->xmloption ); // enum - XmlOptionType
    appendStringInfo( str, ",\"expr\":" );
    Node_out( str, node->expr );
    appendStringInfo( str, ",\"typeName\":" );
    Node_out( str, node->typeName );
    appendStringInfo( str, ",\"location\":%d", node->location );
    return;
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
