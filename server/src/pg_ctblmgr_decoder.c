#include "pg_ctblmgr_decoder.h"

void _PG_init( void )
{
    // this is a stub for now, we may need to read in GUCs
    // that determine whether minimal JSON records are output or not
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
#endif
}

// Initialize the decoder
static void pg_ctblmgr_decode_startup(
    LogicalDecodingContext * context,
    OutputPluginOptions *    options,
    bool                     is_init
)
{
    decode_data * data         = NULL;

    data = palloc0( sizeof( decode_data ) );

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
    data->context = AllocSetContextCreate(
        TopMemoryContext,
        "pg_ctblmgr decoder context",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE
    );

    data->num_changes = 0;

    context->output_plugin_private = data;
    options->output_type           = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;

    return;
}

static void pg_ctblmgr_decode_shutdown( LogicalDecodingContext * context )
{
    decode_data * data = NULL;

    data = context->output_plugin_private;
    MemoryContextDelete( data->context );
    return;
}

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

static void pg_ctblmgr_decode_begin_tx( LogicalDecodingContext * context, ReorderBufferTXN * txn )
{
    decode_data * data = NULL;

    data = context->output_plugin_private;
    data->wrote_tx_changes = false;
    
    OutputPluginPrepareWrite( context, true );
    appendStringInfo(
        context->out,
        transaction_boundary,
        "BEGIN",
        txn->xid,
        timestamptz_to_str( txn->commit_time )
    );

    OutputPluginWrite( context, true );
    return;
}

static void pg_ctblmgr_decode_commit_tx(
    LogicalDecodingContext * context,
    ReorderBufferTXN *       txn,
    XLogRecPtr               commit_lsn
)
{
    OutputPluginPrepareWrite( context, true );

    appendStringInfo(
        context->out,
        transaction_boundary,
        "COMMIT",
        txn->xid,
        timestamptz_to_str( txn->commit_time )
    );

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
 */
static void pg_ctblmgr_decode_change(
    LogicalDecodingContext * context,
    ReorderBufferTXN *       txn,
    Relation                 relation,
    ReorderBufferChange *    change
)
{
    decode_data *     data             = NULL;
    Relation          index            = {0};
    Form_pg_class     class_form       = {0};
    Form_pg_attribute attribute_form   = {0};
    TupleDesc         tuple_descriptor = {0};
    HeapTuple         old_tuple        = {0};
    HeapTuple         new_tuple        = {0};
    HeapTuple         tuple            = {0};
    MemoryContext     old_context      = {0};
    char *            table_name       = NULL;
    char *            schema_name      = NULL;
    char *            dml_type         = NULL;
    unsigned int      i                = 0;
    unsigned int      j                = 0;

    data = context->output_plugin_private;
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

    if(      change->action == REORDER_BUFFER_CHANGE_INSERT ) { dml_type = "INSERT";  }
    else if( change->action == REORDER_BUFFER_CHANGE_UPDATE ) { dml_type = "UPDATE";  }
    else if( change->action == REORDER_BUFFER_CHANGE_DELETE ) { dml_type = "DELETE";  }
    else                                                      { dml_type = "UNKNOWN"; }

    old_context = MemoryContextSwitchTo( data->context );

    appendStringInfo(
        context->out,
        dml_preamble,
        dml_type,
        txn->xid,
        timestamptz_to_str( txn->commit_time ),
        schema_name,
        table_name
    );

    if( change->data.tp.oldtuple != NULL )
    {
        old_tuple = &change->data.tp.oldtuple->tuple;
        tuple     = old_tuple; // Set tuple for deletes
    }
    
    if( change->data.tp.newtuple != NULL )
    {
        new_tuple = &change->data.tp.newtuple->tuple;
        tuple     = new_tuple; // Set tuple for update / insert
    }

    // Append key information
    appendStringInfoString( context->out, ",\"key\":{" );
    RelationGetIndexList( relation );
    // search relation for a natural or surrogate key
    if( OidIsValid( relation->rd_replidindex ) )
    {
        // we may need to cache the index entries - though this should be
        // cached already on most databases
        index = index_open( relation->rd_replidindex, ShareLock );
    
        for( i = 0; i < index->rd_index->indnatts; i++ )
        {
            j              = index->rd_index->indkey.values[i];
            attribute_form = tuple_descriptor->attrs[j - 1];

            if( i > 0 )
            {
                appendStringInfoChar( context->out, ',' );
            }

            appendStringInfo(
                context->out,
                "\"%s\"",
                NameStr( attribute_form->attname )
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
    else
    {
        appendStringInfoString( context->out, "\"ERROR\":\"ERROR\"" );
        // Shouldnt get here
    }

    appendStringInfoString( context->out, "},\"data\":{" );

    if( new_tuple != NULL )
    {
        appendStringInfoString( context->out, "\"new\":{" );
        append_tuple( context->out, tuple_descriptor, new_tuple );
        appendStringInfoChar( context->out, '}' );
    }

    if( old_tuple != NULL )
    {
        if( new_tuple != NULL )
        {
            appendStringInfoChar( context->out, ',' );
        }

        appendStringInfoString( context->out, "\"old\":{" );
        append_tuple( context->out, tuple_descriptor, old_tuple );
        appendStringInfoChar( context->out, '}' );
    }

    appendStringInfoChar( context->out, '}' );

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
    bool              type_is_variable_length = false;
    bool              is_null                 = false;
    Oid               type_output             = {0};
    Form_pg_attribute attribute_form          = {0};
    Datum             original_value          = {0};
    Oid               type_id                 = {0};
    Datum             value                   = {0};

    attribute_form = tuple_descriptor->attrs[index];
    original_value = fastgetattr(
        tuple,
        index,
        tuple_descriptor,
        &is_null
    );

    type_id = attribute_form->atttypid;

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
    const char * value;
    char character;

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
    unsigned int      i              = 0;
    Form_pg_attribute attribute_form = {0};

    for( i = 0; i < tuple_descriptor->natts; i++ )
    {
        attribute_form = tuple_descriptor->attrs[i];
        
        if(
              attribute_form->attisdropped    
           || attribute_form->attnum < 0
          )
        {
            continue;
        }

        appendStringInfo(
            string,
            "\"%s\":",
            NameStr(
                attribute_form->attname
            )
        );

        append_tuple_value( string, tuple_descriptor, tuple, i - 1 );
        
        if( i < tuple_descriptor->natts - 1 )
        {
            appendStringInfoChar( string, ',' );
        }
    }

    return;
}
