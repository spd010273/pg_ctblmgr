package DB;

use strict;
use warnings;
use utf8;

use DBI;
use Perl6::Export::Attrs;
use FindBin;
use English qw( -no_match_vars );
use Params::Validate qw( :all );

use lib "$FindBin::Bin";
use Util;

our $CONNECTION_MAP :Export( :MANDATORY );

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

Readonly::Scalar my $REPLICATION_SEEK_QUERY => <<END_SQL;
    SELECT lsn,
           xid,
           data::JSONB AS data
      FROM pg_catalog.pg_logical_slot_get_changes(
               ?,
               ?,
               NULL,
               'wal-level',
               ?,
               'filter-tables',
               ?,
               'include-transaction',
               TRUE
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

    RETRY_CONN:
    $retry_counter++;
    return undef if( $retry_counter > $MAX_QUERY_RETRIES );

    until( defined( $handle ) && $handle->pg_ping > 0 )
    {
        _log( $LOG_LEVEL_INFO, 'Not connected to DB, attempting to reconnect...' ) if( $DEBUG );
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
            _log( $LOG_LEVEL_FATAL, "Failed to acquire lock after reconnecting to database" );
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
        _log( $LOG_LEVEL_ERROR, 'Failed to execute statement, retrying...' ) if( $DEBUG );

        $try_count++;
        goto RETRY_CONN if( $handle->pg_ping <= 0 );
        my $query_state = $handle->state;

        if( $query_state eq $SQL_STATE_ADMIN_CANC )
        {
            _log( $LOG_LEVEL_ERROR, 'Query canceled by administrator. Retrying...' );
        }
        elsif( $query_state eq $SQL_STATE_ADMIN_TERM )
        {
            _log( $LOG_LEVEL_ERROR, 'Query terminated by administrator. Retrying...' );
        }

        sleep( $sleep_backoff );

        goto RETRY_CONN if( $handle->pg_ping <= 0 );
        $last_backoff_time = $sleep_backoff;
        $sleep_backoff    += int( rand( 2 ** $try_count - 1 ) );

        return undef if( $try_count >= $MAX_QUERY_RETRIES );
    }

    return $sth;
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

sub replication_seek($$$) :Export( :MANDATORY )
{
    my( $handle, $filter_tables, $lsn ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALAR },
    );

    my $sth = try_query(
        $handle,
        $REPLICATION_SEEK_QUERY,
        [ $SLOT_NAME, $lsn, 'F', $filter_tables ]
    );

    unless( $sth )
    {
        return 0;
    }

    if( $sth->rows() > 0 )
    {
        while( my $row = $sth->fetchrow_hashref() )
        {
            my $lsn = $row->{lsn};
            my $xid = $row->{xid};
            my $data = $row->{data};
        }

        $sth->finish();
        return;
    }

    $sth->finish();
    return 0;
}

sub replication_peek($$$) :Export( :MANDATORY )
{
    my( $handle, $filter_tables, $max_lsn ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALARREF },
        { type => SCALAR | UNDEF, optional => 1 },
    );

    my $sth = try_query(
        $handle,
        $REPLICATION_PEEK_QUERY,
        [ $SLOT_NAME, 'F', $filter_tables, $$max_lsn, $$max_lsn ]
    );

    unless( $sth )
    {
        return 0;
    }

    if( $sth->rows() > 0 )
    {
        my $intermediate_data = {};
        my $xids              = [];

        while( my $row = $sth->fetchrow_hashref() )
        {
            my $lsn  = $row->{lsn};

            if( !defined $$max_lsn )
            {
                $$max_lsn = $lsn;
            }
            else
            {
                if( lsn_cmp( $$max_lsn, $lsn ) < 0 )
                {
                    $$max_lsn = $lsn;
                }
            }

            my $xid  = $row->{xid};
            my $data = from_json( $row->{data} );
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

    my $sth = try_query( $handle, $CHECK_CACHE_TABLE_EXISTS, [ $schema, $name ] );

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

    my $create_query = $CREATE_CACHE_TABLE;
    $create_query =~ s/__TABLE__/${schema}.${name}/;
    $create_query =~ s/__DEFINITION__/$definition/;

    $sth = try_query( $handle, $create_query, undef );

    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to create cache table $schema.$name" );
    }

    _log( $LOG_LEVEL_DEBUG, "Cache Table $schema.$name created" );
    $sth->finish();
    return;
}

1;
