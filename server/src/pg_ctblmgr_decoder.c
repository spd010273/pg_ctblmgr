#include "pg_ctblmgr_decoder.h"

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
#endif
}

// Initialize the decoder
static void pg_ctblmgr_decode_startup(
    LogicalDecodingContext * context,
    OutputPluginOptions *    options,
    bool                     is_init
)
{
    decode_data * data = NULL;

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
#endif

static void pg_ctblmgr_decode_begin_tx(
    LogicalDecodingContext * context,
    ReorderBufferTXN *       txn
)
{
    decode_data * data = NULL;

    data = ( decode_data * ) context->output_plugin_private;
    data->wrote_tx_changes = false;
    
    OutputPluginPrepareWrite( context, true );
    appendStringInfo(
        context->out,
        transaction_boundary,
        "BEGIN",
        txn->xid,
        timestamptz_to_str(
            txn->commit_time
        )
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
 * the session.
 *
 * More than likely - the best place to intercept this is at the tcop, because it
 * has access to xid information as well as directing the SET command to guc.c
 * routines, a good starting point is the standard_ProcessUtility in
 * backend/tcop/utility.c
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

    if(      change->action == REORDER_BUFFER_CHANGE_INSERT )
        dml_type = "INSERT";
    else if( change->action == REORDER_BUFFER_CHANGE_UPDATE )
        dml_type = "UPDATE";
    else if( change->action == REORDER_BUFFER_CHANGE_DELETE )
        dml_type = "DELETE";
    else
        dml_type = "UNKNOWN";

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

Datum _hook_set_config_by_name( PG_FUNCTION_ARGS )
{
    char *        name      = NULL;
    char *        value     = NULL;
    char *        new_value = NULL;
    bool          is_local  = false;
    txid          val       = 0;
    TxidEpoch     state     = {0}; // struct: { TransactionId last_xid; uint32 epoch; }
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
    if( should_forward_guc_to_wal( name ) ) // need to check if the guc name is what we're interested in
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
