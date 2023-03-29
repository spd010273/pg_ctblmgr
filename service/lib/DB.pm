package DB;

use strict;
use warnings;
use utf8;

use DBI;
use Perl6::Export::Attrs;
use FindBin;
use English qw( -no_match_vars );
use Params::Validate qw( :all );
use JSON::XS;

use Data::Dumper;

use lib "$FindBin::Bin";
use Util;

$OUTPUT_AUTOFLUSH = 1;
our $CONNECTION_MAP :Export( :MANDATORY );

Readonly::Scalar my $CREATE_REPLICATION_SLOT => <<'END_SQL';
    SELECT *
      FROM pg_catalog.pg_create_logical_replication_slot(
               ?,
               'pg_ctblmgr'
           );
END_SQL

Readonly::Scalar my $CHECK_REPLICATION_SLOT => <<'END_SQL';
    SELECT plugin,
           slot_type
      FROM pg_catalog.pg_replication_slots
     WHERE slot_name = ?
END_SQL

Readonly::Scalar my $CHECK_EXTENSION_RUNNING_QUERY => <<"END_SQL";
    SELECT pg_try_advisory_lock(
               c.oid::BIGINT
           ) AS lock_acquired
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
     WHERE c.relname::VARCHAR = ?
       AND c.relkind = 'r'
END_SQL

Readonly::Scalar my $CACHE_TABLE_COLUMNS => <<"END_SQL";
    SELECT a.attname::VARCHAR AS column_name
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_attribute a
        ON a.attrelid = c.oid
       AND a.attnum > 0
       AND a.attisdropped IS FALSE
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
     WHERE c.relname::VARCHAR = ?
  ORDER BY a.attnum ASC
END_SQL

Readonly::Scalar my $CACHE_TABLE_UNIQUE => <<END_SQL;
    SELECT array_agg( a.attname::VARCHAR ) AS unique_keys
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
INNER JOIN pg_catalog.pg_constraint co
        ON co.contype = 'u'
       AND co.conrelid = c.oid
INNER JOIN pg_catalog.pg_attribute a
        ON a.attrelid = c.oid
       AND a.attnum = ANY( co.conkey )
       AND a.attnum > 0
       AND a.attisdropped IS FALSE
     WHERE c.relname::VARCHAR = ?
  GROUP BY co.oid
     UNION
    SELECT array_agg( a.attname::VARCHAR ) AS unique_keys
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
INNER JOIN pg_catalog.pg_index i
        ON i.indisunique IS TRUE
       AND i.indislive IS TRUE
       AND i.indisready IS TRUE
       AND i.indrelid = c.oid
INNER JOIN pg_catalog.pg_attribute a
        ON a.attrelid = c.oid
       AND a.attnum > 0
       AND a.attisdropped IS FALSE
       AND a.attnum = ANY( i.indkey )
INNER JOIN pg_catalog.pg_class ci
        ON ci.oid = i.indexrelid
 LEFT JOIN pg_catalog.pg_constraint co
        ON co.contype = 'u'
       AND co.conrelid = c.oid
       AND co.conindid = ci.oid
     WHERE c.relname::VARCHAR = ?
       AND co.oid IS NULL
  GROUP BY ci.oid
END_SQL

Readonly::Scalar my $GET_CT_SHA => <<"END_SQL";
    SELECT regexp_replace(
               digest(
                   mo.definition,
                   'sha256'::VARCHAR
               )::VARCHAR,
               '\\\\x',
               ''
           ) AS hash
      FROM ${SCHEMA_NAME}.tb_maintenance_object mo
     WHERE mo.maintenance_object = ?
END_SQL

Readonly::Scalar my $EXTENSION_CHECK_QUERY => <<END_SQL;
    SELECT n.oid
      FROM pg_namespace n
     WHERE n.nspname = ?
END_SQL

Readonly::Scalar my $GET_WORKER_LIST => <<"END_SQL";
    SELECT rs.maintenance_channel,
           rs.filter,
           mg.wal_level,
           mo.maintenance_object
      FROM ${SCHEMA_NAME}.__pgctblmgr_repl_slot rs
INNER JOIN ${SCHEMA_NAME}.tb_maintenance_object mo
        ON mo.maintenance_object = rs.id
INNER JOIN ${SCHEMA_NAME}.tb_maintenance_group mg
        ON mg.maintenance_group = mo.maintenance_group
END_SQL

Readonly::Scalar my $REPLICATION_PEEK_QUERY => <<END_SQL;
    SELECT lsn,
           xid,
           data::JSONB AS data
      FROM pg_catalog.pg_logical_slot_peek_changes(
               ?::NAME,
               NULL::PG_LSN,
               NULL::INTEGER,
               'wal-level'::VARCHAR,
               ?::VARCHAR,
               'filter-tables'::VARCHAR,
               ?::VARCHAR,
               'include-transaction'::VARCHAR,
               'TRUE'::VARCHAR
           )
     WHERE ?::PG_LSN IS NULL OR lsn > ?::PG_LSN
  ORDER BY lsn ASC
END_SQL

Readonly::Scalar my $REPLICATION_PEEK_FOR_CATCHUP => <<END_SQL;
    SELECT lsn
      FROM pg_catalog.pg_logical_slot_peek_changes(
               ?::NAME,
               NULL::PG_LSN,
               NULL::INTEGER,
               'include-transactions'::VARCHAR,
               'TRUE'::VARCHAR
           )
  ORDER BY lsn DESC
     LIMIT 1
END_SQL

Readonly::Scalar my $REPLICATION_SEEK_QUERY => <<END_SQL;
    SELECT lsn,
           xid,
           data::JSONB AS data
      FROM pg_catalog.pg_logical_slot_get_changes(
               ?::NAME,
               ?::PG_LSN,
               NULL::INTEGER,
               'wal-level'::VARCHAR,
               ?::VARCHAR,
               'include-transaction'::VARCHAR,
               'TRUE'::VARCHAR
           )
  ORDER BY lsn ASC
END_SQL

Readonly::Scalar my $CHECK_CACHE_TABLE_EXISTS => <<END_SQL;
    SELECT c.oid
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
     WHERE c.relkind = 'r'
       AND c.relname::VARCHAR = ?
END_SQL

Readonly::Scalar my $CREATE_CACHE_TABLE => <<END_SQL;
CREATE TABLE IF NOT EXISTS __TABLE__ AS
(
    __DEFINITION__
);
END_SQL

Readonly::Scalar my $GET_CACHE_TABLE_DEFINITION => <<"END_SQL";
    SELECT d.name AS driver,
           mo.namespace,
           mo.name,
           mo.definition,
           rs.filter
      FROM ${SCHEMA_NAME}.tb_driver d
INNER JOIN ${SCHEMA_NAME}.tb_maintenance_object mo
        ON mo.driver = d.driver
INNER JOIN ${SCHEMA_NAME}.__pgctblmgr_repl_slot rs
        ON rs.id = mo.maintenance_object
     WHERE mo.maintenance_object = ?
END_SQL

sub get_ct_definition($$$) :Export( :MANDATORY )
{
    my( $handle, $pk_maintenance_object, $cache_hash ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => HASHREF | UNDEF },
    );

    my $ct_sth = &try_query(
        $handle,
        $GET_CACHE_TABLE_DEFINITION,
        [ $pk_maintenance_object ]
    );

    if( $ct_sth )
    {
        my $row = $ct_sth->fetchrow_hashref();
        $cache_hash->{schema}        = $row->{namespace};
        $cache_hash->{driver}        = $row->{driver};
        $cache_hash->{name}          = $row->{name};
        $cache_hash->{definition}    = $row->{definition};
        $cache_hash->{filter_tables} = $row->{filter};
        $ct_sth->finish();

        $cache_hash->{digest} = get_ct_digest(
            $handle,
            $pk_maintenance_object
        );

        if( !defined( $cache_hash->{digest} ) )
        {
            _log(
                $LOG_LEVEL_FATAL,
                'Failed to get SHA256 checksum for cache_table'
            );
        }

        return 1;
    }

    return 0;
}

sub replace_cache_table($$)
{
    my( $handle, $pk_maintenance_object ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
    );

    my $ct_hash = {};

    unless( &get_ct_definition( $handle, $pk_maintenance_object, $ct_hash ) )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to get cache table definition" );
        return 0;
    }

    my $name          = $ct_hash->{name};
    $ct_hash->{name} .= '_temp';
    my $temp_name     = $ct_hash->{name};
    my $schema        = $ct_hash->{schema};
    my $definition    = $ct_hash->{definition};

    $handle->do( 'BEGIN' );
    &create_cache_table( $handle, $ct_hash );
    my $sth = &try_query( $handle, "DROP TABLE $schema.$name" );

    unless( $sth )
    {
        $handle->do( 'ROLLBACK' );
        _log(
            $LOG_LEVEL_FATAL,
            'Cache table replacement failed - could not drop old definition'
        );
    }

    $sth = &try_query(
        $handle,
        "ALTER TABLE $schema.$temp_name RENAME TO $schema.$name"
    );

    unless( $sth )
    {
        $handle->do( 'ROLLBACK' );
        _log(
            $LOG_LEVEL_FATAL,
            'Cache table replacement failed - could not rename new table'
        );
    }

    unless( $handle->do( "ANALYZE $schema.$name" ) )
    {
        $handle->do( 'ROLLBACK' );
        _log( $LOG_LEVEL_FATAL, "Failed to analyze replacement cache table" );
    }

    $handle->do( 'COMMIT' );

    return;
}

sub try_query($$;$) :Export( :MANDATORY )
{
    my( $handle, $query, $params ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => ARRAYREF | UNDEF, optional => 1 },
    );

    my $sth;
    my $retry_counter     = 0;
    my $last_backoff_time = 0;
    my $last_sql_state    = '';
    my $sleep_backoff     = 1;
    my $try_count         = 0;

    #_log( $LOG_LEVEL_DEBUG, "Executing '$query'" );
    RETRY_CONN:
    $retry_counter++;
    return undef if( $retry_counter > $MAX_QUERY_RETRIES );

    until( defined( $handle ) && $handle->pg_ping > 0 )
    {
        if( $DEBUG )
        {
            _log(
                $LOG_LEVEL_INFO,
                'Not connected to DB, attempting to reconnect...'
            );
        }

        $try_count++;
        sleep( $sleep_backoff );
        $handle = DBI->connect(
            $CONNECTION_MAP->{connection_string},
            $CONNECTION_MAP->{user_name},
            undef
        );

        $last_backoff_time = $sleep_backoff;
        $sleep_backoff    += int( rand( 2 ** $try_count - 1 ) );

        if( $try_count > 5 )
        {
            ## XXX Check if DB was dropped
        }
    }

    # We're connected to the DB at this point
    if( $PARENT_PID == $PROCESS_ID )
    {
        unless( check_extension_running( $handle ) )
        {
            _log(
                $LOG_LEVEL_FATAL,
                'Failed to acquire lock after reconnecting to database'
            );
        }
    }

    until( $handle->pg_ping > 0 && ( $sth = $handle->prepare( $query ) ) )
    {
        goto RETRY_CONN;
    }

    goto RETRY_CONN unless( defined( $sth ) );

    if(
          defined( $params )
       && ref( $params ) eq 'ARRAY'
       && scalar( @$params ) > 0
      )
    {
        # Bind Params
        my $bind_index = 1;

        foreach my $param( @$params )
        {
            $sth->bind_param( $bind_index, $param );
            $bind_index++;
        }
    }

    $sleep_backoff     = 1;
    $try_count         = 0;
    $last_backoff_time = 0;

    until( $sth->execute() )
    {
        if( $DEBUG )
        {
            _log(
                $LOG_LEVEL_ERROR,
                'Failed to execute statement, retrying...'
            );
        }

        $try_count++;
        goto RETRY_CONN if( $handle->pg_ping <= 0 );
        my $query_state = $handle->state;

        if( $query_state eq $SQL_STATE_ADMIN_CANC )
        {
            _log(
                $LOG_LEVEL_ERROR,
                'Query canceled by administrator. Retrying...'
            );
        }
        elsif( $query_state eq $SQL_STATE_ADMIN_TERM )
        {
            _log(
                $LOG_LEVEL_ERROR,
                'Query terminated by administrator. Retrying...'
            );
        }

        sleep( $sleep_backoff );

        goto RETRY_CONN if( $handle->pg_ping <= 0 );
        $last_backoff_time = $sleep_backoff;
        $sleep_backoff    += int( rand( 2 ** $try_count - 1 ) );

        return undef if( $try_count >= $MAX_QUERY_RETRIES );
    }

    return $sth;
}

sub get_ct_digest($$) :Export( :MANDATORY )
{
    my( $handle, $pk_maintenance_object ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
    );

    my $sth = try_query( $handle, $GET_CT_SHA, [ $pk_maintenance_object ] );

    if( $sth )
    {
        my $hash_row = $sth->fetchrow_hashref();
        my $hash = $hash_row->{hash};
        $sth->finish();
        return $hash;
    }

    return;
}

sub check_extension($) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $sth = try_query(
        $handle,
        $EXTENSION_CHECK_QUERY,
        [ $SCHEMA_NAME ]
    );

    return 0 unless( $sth );

    if( $sth->rows() > 0 )
    {
        $sth->finish();
        return 1;
    }

    $sth->finish();
    return 0;
}

sub create_replication_slot($) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $check_sth = &try_query(
        $handle,
        $CHECK_REPLICATION_SLOT,
        [ $SLOT_NAME ]
    );

    return 0 unless( $check_sth );

    if( $check_sth->rows() == 0 )
    {
        my $create_sth = &try_query(
            $handle,
            $CREATE_REPLICATION_SLOT,
            [ $SLOT_NAME ]
        );

        if( !$create_sth )
        {
            $check_sth->finish();
            return 0;
        }

        $create_sth->finish();
    }

    $check_sth->finish();

    return 1;
}

sub check_extension_running($) :Export( :MANDATORY )
{
    # This subroutine needs to use DBI methods to avoid deep recursion
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $sth = $handle->prepare(
        $CHECK_EXTENSION_RUNNING_QUERY
    );

    return 0 unless( $sth );

    $sth->bind_param( 1, $SCHEMA_NAME );
    $sth->bind_param( 2, '__pgctblmgr_repl_slot' );

    return 0 unless( $sth->execute() );

    if( $sth->rows() > 0 )
    {
        my $row    = $sth->fetchrow_hashref();
        my $result = $row->{lock_acquired};
        $sth->finish();
        return $result;
    }

    $sth->finish();
    return 0;
}

sub get_worker_list($) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $sth = try_query( $handle, $GET_WORKER_LIST );

    unless( $sth )
    {
        return undef;
    }

    if( $sth->rows() > 0 )
    {
        my $worker_data = [];

        while( my $row = $sth->fetchrow_hashref() )
        {
            my $maintenance_channel = $row->{maintenance_channel};
            my $filter_tables       = $row->{filter};
            my $wal_level           = $row->{wal_level};
            my $maintenance_object  = $row->{maintenance_object};

            push(
                @$worker_data,
                {
                    maintenance_channel => $maintenance_channel,
                    filter_tables       => $filter_tables,
                    wal_level           => $wal_level,
                    maintenance_object  => $maintenance_object,
                }
            );
        }

        $sth->finish();
        return $worker_data;
    }

    $sth->finish();
    return undef;
}

sub replication_seek($$) :Export( :MANDATORY )
{
    my( $handle, $lsn ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
    );

    my $sth = try_query(
        $handle,
        $REPLICATION_SEEK_QUERY,
        [ $SLOT_NAME, $lsn, 'F' ]
    );

    unless( $sth )
    {
        return 0;
    }

    if( $sth->rows() > 0 )
    {
        $sth->finish();
        return 1;
    }

    $sth->finish();
    return 0;
}

sub replication_slot_peek_unneeded_changes($$) :Export( :MANDATORY )
{
    my( $handle, $lsn ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALARREF },
    );

    my $sth = try_query(
        $handle,
        $REPLICATION_PEEK_FOR_CATCHUP,
        [ $SLOT_NAME ]
    );

    if( $sth->rows() == 0 )
    {
        $sth->finish();
        return;
    }

    my $row = $sth->fetchrow_hashref();
    $sth->finish();
    $$lsn = $row->{lsn};
    return;
}

sub replication_peek($$$) :Export( :MANDATORY )
{
    my( $handle, $filter_tables, $max_lsn ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALARREF },
    );

    my $sth = try_query(
        $handle,
        $REPLICATION_PEEK_QUERY,
        [ $SLOT_NAME, 'F', $filter_tables, $$max_lsn, $$max_lsn ]
    );

    return 0 unless( $sth );

    if( $sth->rows() > 0 )
    {
        my $intermediate_data = {};
        my $xids              = [];

        while( my $row = $sth->fetchrow_hashref() )
        {
            my $lsn  = $row->{lsn};

            if( !defined( $$max_lsn ) || lsn_cmp( $$max_lsn, $lsn ) < 0 )
            {
                $$max_lsn = $lsn;
            }

            my $xid  = $row->{xid};
            my $data;
            $data = decode_json( $row->{data} ) if( $row->{data} );
            my $out  = { lsn => $lsn, xid => $xid, data => $data };

            unless( $xid ~~ @$xids )
            {
                push( @$xids, $xid );
            }

            if(
                  $data->{type} ne 'COMMIT'
               && $data->{type} ne 'BEGIN'
               && $data->{type} ne 'ROLLBACK'
              )
            {
                push( @{$intermediate_data->{$xid}->{DML}}, $out );
            }
            else
            {
                # Assume transaction demarcation
                $intermediate_data->{$xid}->{$data->{type}} = $out->{lsn};
            }
        }

        $sth->finish();

        my $out_data = [];
        # Step through transactional data and only output DML if we detect both a valid
        # BEGIN and COMMIT for the DML's XID
        foreach my $xid( @$xids )
        {
            if(
                    exists( $intermediate_data->{$xid}->{COMMIT} )
                 && exists( $intermediate_data->{$xid}->{BEGIN} )
              )
            {
                if(
                      exists( $intermediate_data->{$xid}->{DML} )
                   && scalar( @{$intermediate_data->{$xid}->{DML}} )
                  )
                {
                    foreach my $dml( @{$intermediate_data->{$xid}->{DML}} )
                    {
                        $dml->{commit_lsn} = $intermediate_data->{$xid}->{COMMIT};
                        push( @$out_data, $dml )
                    }
                }
            }
        }

        return if( scalar( @$out_data ) == 0 );
        return $out_data;
    }

    $sth->finish();
    return 0;
}

sub check_ct_exists($$$$) :Export( :MANDATORY )
{
    my( $handle, $schema, $name, $definition ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALAR },
        { type => SCALAR },
    );

    my $sth = try_query(
        $handle,
        $CHECK_CACHE_TABLE_EXISTS,
        [ $schema, $name ]
    );

    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to verify that $schema.$name exists" );
    }

    if( $sth->rows() > 0 )
    {
        $sth->finish();
        _log( $LOG_LEVEL_DEBUG, "Cache Table $schema.$name already exists" );
        return;
    }

    $sth->finish();
    &create_cache_table(
        $handle,
        {
            name       => $name,
            definition => $definition,
            schema     => $schema
        }
    );

    return;
}

sub create_cache_table($$)
{
    my( $handle, $ct_hash ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => HASHREF },
    );

    my $schema       = $ct_hash->{schema};
    my $name         = $ct_hash->{name};
    my $definition   = $ct_hash->{definition};
    my $create_query = $CREATE_CACHE_TABLE;
    $create_query    =~ s/__TABLE__/${schema}.${name}/;
    $create_query    =~ s/__DEFINITION__/$definition/;

    my $sth = try_query( $handle, $create_query, undef );

    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to create cache table $schema.$name" );
    }

    _log( $LOG_LEVEL_DEBUG, "Cache Table $schema.$name created" );
    $sth->finish();
    return;
}

sub test_query($$) :Export( :MANDATORY )
{
    my( $handle, $query ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
    );

    my $test_query = "WITH tt_test AS( $query ) SELECT * FROM tt_test LIMIT 0";

    my $sth = $handle->prepare( $test_query );

    return 0 if( !defined( $sth ) );
    return 0 unless( $sth->execute() );

    $sth->finish();
    return 1;
}

sub generate_temp_table($$) :Export( :MANDATORY )
{
    my( $handle, $query ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
    );

    my $temp_table_name = 'tt_foo';
    my $tt_query = "CREATE TEMP TABLE $temp_table_name AS( $query );";

    my $sth = try_query( $handle, $tt_query );

    if( $sth )
    {
        $sth->finish();
        return $temp_table_name;
    }

    return;
}

sub drop_temp_table($$) :Export( :MANDATORY )
{
    my( $handle, $temp_table ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
    );

    my $query = "DROP TABLE $temp_table";
    my $sth   = try_query( $handle, $query );

    return 0 unless( $sth );

    $sth->finish();
    return 1;
}

sub get_cache_table_columns($$$) :Export( :MANDATORY )
{
    my( $handle, $cache_table_schema, $cache_table_name ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALAR },
    );

    my $sth = &try_query(
        $handle,
        $CACHE_TABLE_COLUMNS,
        [ $cache_table_schema, $cache_table_name ]
    );

    return unless( $sth );
    my $columns = [];

    while( my $row = $sth->fetchrow_hashref() )
    {
        push( @$columns, $row->{column_name} );
    }

    $sth->finish();
    return $columns;
}

sub get_cache_table_unique($$$) :Export( :MANDATORY )
{
    my( $handle, $cache_table_schema, $cache_table_name ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALAR },
    );

    my $sth = &try_query(
        $handle,
        $CACHE_TABLE_UNIQUE,
        [
            $cache_table_schema,
            $cache_table_name,
            $cache_table_schema,
            $cache_table_name
        ]
    );

    return unless( $sth );
    my $uniques = [];
    while( my $row = $sth->fetchrow_hashref() )
    {
        # each row represents a different unique constraint
        my $unique_columns = $row->{unique_keys};
        push( @$uniques, $unique_columns );
    }

    $sth->finish();
    return $uniques;
}

sub generate_update_statement($$$$$$) :Export( :MANDATORY )
{
    my(
        $handle,
        $temp_table,
        $cache_table_schema,
        $cache_table_name,
        $table_columns,
        $uniques
      ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALAR },
        { type => SCALAR },
        { type => ARRAYREF },
        { type => ARRAYREF },
    );

    my $join_clauses       = [];
    my $where_clauses      = [];
    my $distinct_uniques   = [];
    my $non_unique_columns = [];

    foreach my $unique_columns( @$uniques )
    {
        my $join_clause  = join(
            ' AND ',
            map { "tt.$_ IS NOT DISTINCT FROM vw.$_" } @$unique_columns
        );

        my $where_clause = join(
            ' AND ',
            map { "ct.$_ IS NOT DISTINCT FROM tt.$_" } @$unique_columns
        );

        push( @$join_clauses,  $join_clause  );
        push( @$where_clauses, $where_clause );

        foreach my $unique_column( @$unique_columns )
        {
            unless( grep /^$unique_column$/, @$distinct_uniques )
            {
                push( @$distinct_uniques, $unique_column );
            }
        }
    }

    foreach my $column_name( @$table_columns )
    {
        next if( grep( /^$column_name$/, @$distinct_uniques ) );
        push( @$non_unique_columns, $column_name );
    }

    my $join_predicate  = '( ( ' . join( ' ) OR ( ', @$join_clauses ) . ' ) )';
    my $update_fragment = join( ', ', map { "$_ = tt.$_" } @$table_columns );
    my $columns         = join( ', ', map { "vw.$_" } @$table_columns );
    my $where_clause    = '( ( ' . join( ' ) OR ( ', @$where_clauses ) . ' ) )';

    my $UPDATE_Q = <<END_SQL;
    WITH tt_records_to_update AS
    (
        SELECT $columns
          FROM $temp_table vw
    INNER JOIN $cache_table_schema.$cache_table_name tt
            ON $join_predicate
    )
        UPDATE $cache_table_schema.$cache_table_name ct
           SET $update_fragment
          FROM tt_records_to_update tt
         WHERE $where_clause
END_SQL

    my $sth = &try_query( $handle, $UPDATE_Q, [] );

    return 0 unless( $sth );

    $sth->finish();
    return 1;
}

sub generate_insert_statement($$$$$$) :Export( :MANDATORY )
{
    my(
        $handle,
        $temp_table,
        $cache_table_schema,
        $cache_table_name,
        $table_columns,
        $uniques
      ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALAR },
        { type => SCALAR },
        { type => ARRAYREF },
        { type => ARRAYREF },
    );

    my $join_clauses  = [];
    my $where_clauses = [];

    foreach my $unique_columns( @$uniques )
    {
        my $join_clause  = join(
            ' AND ',
            map { "tt.$_ IS NOT DISTINCT FROM vw.$_" } @$unique_columns
        );
        my $where_clause = join(
            ' AND ',
            map { "tt.$_ IS NULL" } @$unique_columns
        );
        push( @$join_clauses,  $join_clause  );
        push( @$where_clauses, $where_clause );
    }

    my $columns        = join( ', ', map { "vw.$_" } @$table_columns );
    my $join_predicate = '( ( ' . join( ' ) OR ( ', @$join_clauses ) . ' ) )';
    my $where_clause   = '( ( ' . join( ') AND (', @$where_clauses ) . ' ) )';

    my $INSERT_Q = <<END_SQL;
    WITH tt_records_to_insert AS
    (
        SELECT $columns
          FROM $temp_table vw
     LEFT JOIN $cache_table_schema.$cache_table_name tt
            ON $join_predicate
         WHERE $where_clause
    )
    INSERT INTO $cache_table_schema.$cache_table_name
         SELECT $columns
           FROM tt_records_to_insert vw
END_SQL

    my $sth = &try_query( $handle, $INSERT_Q, [] );

    return 0 unless( $sth );

    $sth->finish();
    return 1;
}

sub generate_delete_statement($$$$$$) :Export( :MANDATORY )
{
    my(
        $handle,
        $definition,
        $cache_table_schema,
        $cache_table_name,
        $table_columns,
        $uniques
      ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALAR },
        { type => SCALAR },
        { type => ARRAYREF },
        { type => ARRAYREF },
    );

    my $join_clauses  = [];
    my $where_clauses = [];

    foreach my $unique_columns( @$uniques )
    {
        my $join_clause  = join(
            ' AND ',
            map { "tt.$_ IS NOT DISTINCT FROM vw.$_" } @$unique_columns
        );
        my $where_clause = join(
            ' AND ',
            map { "tt.$_ IS NULL" } @$unique_columns
        );
        push( @$join_clauses,  $join_clause  );
        push( @$where_clauses, $where_clause );
    }

    my $columns        = join( ', ', map { "vw.$_" } @$table_columns );
    my $join_predicate = '( ( ' . join( ' ) OR ( ', @$join_clauses ) . ' ) )';
    my $where_clause   = '( ( ' . join( ' ) AND ( ', @$where_clauses ) . ' ) )';

    my $DELETE_Q = <<"END_SQL";
    WITH tt_base_data AS
    (
        $definition
    ),
    tt_rows_to_delete AS
    (
        SELECT $columns
          FROM $cache_table_schema.$cache_table_name vw
     LEFT JOIN tt_base_data tt
            ON $join_predicate
         WHERE $where_clause
    )
    DELETE FROM $cache_table_schema.$cache_table_name tt
          USING tt_rows_to_delete vw
          WHERE $join_predicate
END_SQL

    print "$DELETE_Q\n";
    my $sth = &try_query( $handle, $DELETE_Q, [] );

    return 0 unless( $sth );

    $sth->finish();
    return 1;
}

1;
