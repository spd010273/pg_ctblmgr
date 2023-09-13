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

Readonly::Scalar our $BATCHED_CREATE     :Export( :MANDATORY ) => 1;
Readonly::Scalar our $BATCH_SIZE         :Export( :MANDATORY ) => 1000000;
Readonly::Scalar our $BULK_ACTION_CUTOFF :Export( :MANDATORY ) => 100000;

Readonly::Scalar my $TCP_KEEPALIVE          => 60;
Readonly::Scalar my $TCP_KEEPALIVE_INTERVAL => 5; # seconds
Readonly::Scalar my $TCP_KEEPALIVE_COUNT    => 200; #720;
Readonly::Scalar my $TCP_USER_TIMEOUT       => 1000 * 60 * 5;

Readonly::Scalar my $DEFAULT_SEEK_COUNT => 100;
Readonly::Scalar my $CREATE_REPLICATION_SLOT => <<"END_SQL";
    SELECT *
      FROM pg_catalog.pg_create_logical_replication_slot(
               ?,
               '$EXTENSION_NAME'
           );
END_SQL

Readonly::Scalar my $GET_SLOT_NAME => <<"END_SQL";
    SELECT lower(
                pg_catalog.regexp_replace(
                    current_database()::VARCHAR,
                    '[^[:alnum:]]',
                    '_',
                    'g'
                )
             || '__'
             || '$EXTENSION_NAME'
           )::VARCHAR AS slot_name
END_SQL

Readonly::Scalar my $CHECK_REPLICATION_SLOT => <<'END_SQL';
    SELECT plugin,
           slot_type
      FROM pg_catalog.pg_replication_slots
     WHERE slot_name = ?
END_SQL

Readonly::Scalar my $DROP_REPLICATION_SLOT => <<'END_SQL';
    SELECT pg_drop_replication_slot( slot_name )
      FROM pg_catalog.pg_stat_replication_slots
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
           mo.maintenance_object,
           mo.name
      FROM ${SCHEMA_NAME}.__pgctblmgr_repl_slot rs
INNER JOIN ${SCHEMA_NAME}.tb_maintenance_object mo
        ON mo.maintenance_object = rs.id
INNER JOIN ${SCHEMA_NAME}.tb_maintenance_group mg
        ON mg.maintenance_group = mo.maintenance_group
END_SQL

Readonly::Scalar my $REPLICATION_PEEK_QUERY_NO_FT => <<END_SQL;
    SELECT lsn,
           xid,
           data::JSONB AS data
      FROM pg_catalog.pg_logical_slot_peek_changes(
               ?::NAME,
               NULL::PG_LSN,
               ${DEFAULT_SEEK_COUNT}::INTEGER,
               'wal-level'::VARCHAR,
               ?::VARCHAR,
               'include-transaction'::VARCHAR,
               'TRUE'::VARCHAR
           )
     WHERE ?::PG_LSN IS NULL OR lsn > ?::PG_LSN
  ORDER BY lsn ASC
END_SQL

Readonly::Scalar my $REPLICATION_PEEK_QUERY => <<END_SQL;
    SELECT lsn,
           xid,
           data::JSONB AS data
      FROM pg_catalog.pg_logical_slot_peek_changes(
               ?::NAME,
               NULL::PG_LSN,
               ${DEFAULT_SEEK_COUNT}::INTEGER,
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

Readonly::Scalar my $REPLICATION_SEEK_QUERY_NO_FT => <<END_SQL;
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

Readonly::Scalar my $REPLICATION_PEEK_FOR_CATCHUP => <<END_SQL;
    SELECT lsn
      FROM pg_catalog.pg_logical_slot_peek_changes(
               ?::NAME,
               NULL::PG_LSN,
               NULL::INTEGER,
               'include-transaction'::VARCHAR,
               'TRUE'::VARCHAR,
               'filter-tables'::VARCHAR,
               ?::VARCHAR
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
               'TRUE'::VARCHAR,
               'filter-tables'::VARCHAR,
               ?
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

Readonly::Scalar my $CREATE_COLUMN_CHECK_TABLE => <<END_SQL;
CREATE TEMP TABLE tt_column_verify AS
(
    WITH tt_foo AS
    (
        __DEFINITION__
    )
        SELECT *
          FROM tt_foo
         LIMIT 0
)
END_SQL

Readonly::Scalar my $CHECK_CACHE_TABLE_COLUMNS => <<END_SQL;
WITH tt_existing AS
(
    SELECT a.attname::VARCHAR as column_name
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
INNER JOIN pg_catalog.pg_attribute a
        ON a.attrelid = c.oid
       AND a.attnum > 0
       AND a.attisdropped IS FALSE
     WHERE c.relname::VARCHAR = ?
),
tt_requested AS
(
    SELECT a.attname::VARCHAR AS column_name
      FROM pg_catalog.pg_class c
INNER JOIN pg_catalog.pg_namespace n
        ON n.oid = c.relnamespace
       AND n.oid = pg_catalog.pg_my_temp_schema()
INNER JOIN pg_attribute a
        ON a.attrelid = c.oid
       AND a.attnum > 0
       AND a.attisdropped IS FALSE
     WHERE c.relname::VARCHAR = 'tt_column_verify'
)
    SELECT r.column_name AS r,
           e.column_name AS e
      FROM tt_existing e
FULL OUTER JOIN tt_requested r
        ON e.column_name = r.column_name
     WHERE e.column_name IS NULL
        OR r.column_name IS NULL
END_SQL

my $CREATE_CACHE_TABLE;
if( $BATCHED_CREATE )
{
    $CREATE_CACHE_TABLE = <<END_SQL;
    CREATE TABLE IF NOT EXISTS __TABLE__ AS
    (
        WITH tt_foo AS
        (
            __DEFINITION__
        )
            SELECT *
              FROM tt_foo
             LIMIT 0
    );
END_SQL
}
else
{
    $CREATE_CACHE_TABLE = <<END_SQL;
    CREATE TABLE IF NOT EXISTS __TABLE__ AS
    (
        __DEFINITION__
    );
END_SQL
}

Readonly::Scalar my $CREATE_POPULATE => <<END_SQL;
WITH tt_def AS
(
    __DEFINITION__
)
    INSERT INTO __TABLE__
         SELECT *
           FROM tt_def
       ORDER BY __ORDERBY__
          LIMIT __LIMIT__
         OFFSET __OFFSET__
END_SQL

Readonly::Scalar my $GET_CACHE_TABLE_DEFINITION => <<"END_SQL";
    SELECT d.name AS driver,
           mo.namespace,
           mo.name,
           mo.definition,
           rs.filter,
           mo.unique_index,
           mo.indexes
      FROM ${SCHEMA_NAME}.tb_driver d
INNER JOIN ${SCHEMA_NAME}.tb_maintenance_object mo
        ON mo.driver = d.driver
INNER JOIN ${SCHEMA_NAME}.__pgctblmgr_repl_slot rs
        ON rs.id = mo.maintenance_object
     WHERE mo.maintenance_object = ?
END_SQL

Readonly::Scalar my $CREATE_CT_DEPENDENT_OBJECT_TT => <<'END_SQL';
CREATE TEMP TABLE tt_dependent_objects
(
    drop_statement   TEXT,
    create_statement TEXT,
    object_name      VARCHAR,
    is_base_obj      BOOLEAN,
    rank             INTEGER
)
END_SQL

Readonly::Scalar my $GET_DEPENDENT_VIEWS => <<"END_SQL";
WITH RECURSIVE tt_viewdefs AS
(
    SELECT DISTINCT ON( dc.oid, sc.oid )
           dc.oid AS dependent_oid,
           sc.oid,
           1 AS rank
      FROM pg_depend d
INNER JOIN pg_rewrite rw
        ON rw.oid = d.objid
INNER JOIN pg_class dc
        ON dc.oid = rw.ev_class
       AND dc.relkind IN( 'm', 'v' )
INNER JOIN pg_class sc
        ON sc.oid = d.refobjid
INNER JOIN pg_namespace sns
        ON sns.oid = sc.relnamespace
     WHERE sns.nspname::VARCHAR = ?
       AND sc.relname::VARCHAR = ?
     UNION
    SELECT DISTINCT ON( dc.oid, sc.oid )
           dc.oid AS dependent_oid,
           sc.oid,
           tt.rank + 1 AS rank
      FROM pg_depend d
INNER JOIN pg_rewrite rw
        ON rw.oid = d.objid
INNER JOIN pg_class dc
        ON dc.oid = rw.ev_class
       AND dc.relkind IN( 'm', 'v' )
INNER JOIN pg_class sc
        ON sc.oid = d.refobjid
INNER JOIN tt_viewdefs tt
        ON tt.dependent_oid = sc.oid
     WHERE sc.oid IS DISTINCT FROM dc.oid
),
tt_def_prep AS
(
    SELECT COALESCE( ns.nspname::VARCHAR, 'public' ) || '.' || c.relname::VARCHAR AS object_name,
           CASE WHEN c.relkind = 'm'
                THEN 'MATERIALIZED'
                ELSE ''
                 END AS view_type,
           regexp_replace( pg_get_viewdef( c.oid, TRUE ), ';\\s*\$', '' ) AS definition,
           tt.rank
      FROM tt_viewdefs tt
INNER JOIN pg_class c
        ON c.oid = tt.dependent_oid
INNER JOIN pg_namespace ns
        ON ns.oid = c.relnamespace
)
INSERT INTO tt_dependent_objects
            (
                drop_statement,
                create_statement,
                object_name,
                is_base_obj,
                rank
            )
     SELECT 'DROP ' || view_type || ' VIEW ' || object_name AS drop_statement,
            'CREATE ' || view_type || ' VIEW ' || object_name || ' AS ( ' || definition || ')' AS create_statement,
            object_name,
            TRUE,
            rank
       FROM tt_def_prep
END_SQL

Readonly::Scalar my $GET_FK_DEPENDENCIES => <<"END_SQL";
WITH tt_fk_constraints AS
(
    SELECT co.conname::VARCHAR AS constraint_name,
           COALESCE( nr.nspname::VARCHAR, 'public' ) || '.' || cr.relname::VARCHAR AS object,
           pg_get_constraintdef( co.oid, TRUE ) AS definition,
           2 AS rank
      FROM pg_constraint co
INNER JOIN pg_class c
        ON c.oid = co.confrelid
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
INNER JOIN pg_class cr
        ON cr.oid = co.conrelid
INNER JOIN pg_namespace nr
        ON nr.oid = cr.relnamespace
     WHERE co.contype = 'f'
       AND n.nspname::VARCHAR = ?
       AND c.relname::VARCHAR = ?
     UNION
    SELECT co.conname::VARCHAR AS constraint_name,
           COALESCE( nr.nspname::VARCHAR, 'public' ) || '.' || cr.relname::VARCHAR AS object,
           pg_get_constraintdef( co.oid, TRUE ) AS definition,
           tt.rank + 1 AS rank
      FROM pg_constraint co
INNER JOIN pg_class c
        ON c.oid = co.confrelid
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
INNER JOIN pg_class cr
        ON cr.oid = co.conrelid
INNER JOIN pg_namespace nr
        ON nr.oid = cr.relnamespace
INNER JOIN tt_dependent_objects tt
        ON tt.object_name = COALESCE( nr.nspname::VARCHAR, 'public' ) || '.' || cr.relname::VARCHAR
       AND tt.is_base_obj IS TRUE
     WHERE co.contype = 'f'
)
INSERT INTO tt_dependent_objects
            (
                drop_statement,
                create_statement,
                object_name,
                is_base_obj,
                rank
            )
     SELECT 'ALTER TABLE ' || object || ' DROP CONSTRAINT ' || constraint_name AS drop_statement,
            'ALTER TABLE ' || object || ' ADD CONSTRAINT ' || constraint_name || ' ' || definition AS create_statement,
            constraint_name,
            FALSE,
            rank
       FROM tt_fk_constraints tt;
END_SQL

Readonly::Scalar my $GET_DEPENDENT_CHECK_CONSTRAINTS => <<"END_SQL";
WITH tt_check_constraints AS
(
    SELECT co.conname::VARCHAR AS constraint_name,
           COALESCE( n.nspname::VARCHAR, 'public' ) || '.' || c.relname::VARCHAR AS object,
           pg_get_constraintdef( co.oid, TRUE ) AS definition,
           2 AS rank
      FROM pg_constraint co
INNER JOIN pg_class c
        ON c.oid = co.conrelid
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
     WHERE co.contype != 'f'
       AND co.contype != 'p'
       AND n.nspname::VARCHAR = ?
       AND c.relname::VARCHAR = ?
     UNION
    SELECT co.conname::VARCHAR AS constraint_name,
           COALESCE( n.nspname::VARCHAR, 'public' ) || '.' || c.relname::VARCHAR AS object,
           pg_get_constraintdef( co.oid, TRUE ) AS definition,
           tt.rank + 1 AS rank
      FROM pg_constraint co
INNER JOIN pg_class c
        ON c.oid = co.conrelid
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
INNER JOIN tt_dependent_objects tt
        ON tt.object_name = COALESCE( n.nspname::VARCHAR, 'public' ) || '.' || c.relname::VARCHAR
       AND tt.is_base_obj IS TRUE
     WHERE co.contype != 'f'
       AND co.contype != 'p'
)
INSERT INTO tt_dependent_objects
            (
                drop_statement,
                create_statement,
                object_name,
                is_base_obj,
                rank
            )
     SELECT 'ALTER TABLE ' || object || ' DROP CONSTRAINT ' || constraint_name AS drop_statement,
            'ALTER TABLE ' || object || ' ADD CONSTRAINT ' || constraint_name || ' ' || definition AS create_statement,
            constraint_name,
            FALSE,
            rank
       FROM tt_check_constraints tt;
END_SQL

Readonly::Scalar my $GET_DEPENDENT_TRIGGERS => <<"END_SQL";
WITH tt_triggers AS
(
    SELECT t.tgname AS trigger_name,
           COALESCE( n.nspname::VARCHAR, 'public' ) || '.' || c.relname::VARCHAR AS object,
           pg_get_triggerdef( t.oid, TRUE ) AS definition,
           2 AS rank
      FROM pg_trigger t
INNER JOIN pg_class c
        ON c.oid = t.tgrelid
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
     WHERE n.nspname::VARCHAR = ?
       AND c.relname::VARCHAR = ?
     UNION
    SELECT t.tgname AS trigger_name,
           COALESCE( n.nspname::VARCHAR, 'public' ) || '.' || c.relname::VARCHAR AS object,
           pg_get_triggerdef( t.oid, TRUE ) AS definition,
           2 AS rank
      FROM pg_trigger t
INNER JOIN pg_class c
        ON c.oid = t.tgrelid
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
INNER JOIN tt_dependent_objects tt
        ON tt.object_name = COALESCE( n.nspname::VARCHAR, 'public' ) || '.' || c.relname::VARCHAR
       AND tt.is_base_obj IS TRUE
)
INSERT INTO tt_dependent_objects
            (
                drop_statement,
                create_statement,
                object_name,
                is_base_obj,
                rank
            )
     SELECT 'DROP TRIGGER ' || trigger_name || ' ON ' || object AS drop_statement,
            definition AS create_statement,
            trigger_name,
            FALSE,
            rank
       FROM tt_triggers;
END_SQL

Readonly::Scalar my $GET_DEPENDENT_INDEXES => <<"END_SQL";
    WITH tt_indexes AS
    (
        SELECT pg_get_indexdef( ci.oid ) AS create_statement,
               'DROP INDEX ' || ci.relname::VARCHAR AS drop_statement,
               ci.relname::VARCHAR AS object_name,
               tt.rank + 1 AS rank
          FROM pg_index i
    INNER JOIN pg_class ci
            ON ci.oid = i.indexrelid
    INNER JOIN pg_class c
            ON c.oid = i.indrelid
    INNER JOIN pg_namespace n
            ON n.oid = c.relnamespace
    INNER JOIN tt_dependent_objects tt
            ON tt.object_name = COALESCE( n.nspname::VARCHAR, 'public' ) || '.' || c.relname::VARCHAR
           AND tt.is_base_obj IS TRUE
    )
    INSERT INTO tt_dependent_objects
                (
                    drop_statement,
                    create_statement,
                    object_name,
                    is_base_obj,
                    rank
                )
         SELECT drop_statement,
                create_statement,
                object_name,
                FALSE,
                rank
           FROM tt_indexes;
END_SQL

sub get_slot_name($) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $slot_name_sth = &try_query( $handle, $GET_SLOT_NAME );

    return undef unless( $slot_name_sth );
    my $slot_name_row = $slot_name_sth->fetchrow_hashref();

    my $slot_name = $slot_name_row->{slot_name};

    $slot_name_sth->finish();
    return $slot_name;
}

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
        $cache_hash->{indexes}       = $row->{indexes};
        $cache_hash->{unique_index}  = $row->{unique_index};
        $cache_hash->{maintenance_object} = $pk_maintenance_object;
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

sub drop_replication_slot($) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $drop_sth = $handle->prepare( $DROP_REPLICATION_SLOT );

    return -1 unless( $drop_sth );

    $drop_sth->bind_param( 1, $SLOT_NAME );

    return -1 unless( $drop_sth->execute() );

    if( $drop_sth->rows() == 0 )
    {
        $drop_sth->finish();
        return 0;
    }

    $drop_sth->finish();
    return 1;
}

# Dependent object logic
sub create_dependent_temp_table($$)
{
    my( $handle, $ct_hash ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => HASHREF },
    );

    my $sth = &try_query( $handle, $CREATE_CT_DEPENDENT_OBJECT_TT );

    unless( $sth )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to create dependency temp table" );
        return 0;
    }

    $sth->finish();

    $sth = &try_query( $handle, $GET_DEPENDENT_VIEWS, [ $ct_hash->{schema}, $ct_hash->{name} ] );

    unless( $sth )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to get view dependencies for $ct_hash->{name}" );
        return 0;
    }

    $sth->finish();
    $sth = &try_query( $handle, $GET_FK_DEPENDENCIES, [ $ct_hash->{schema}, $ct_hash->{name} ] );

    unless( $sth )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to get fk-dependent objects for $ct_hash->{name}" );
        return 0;
    }

    $sth->finish();
    $sth = &try_query( $handle, $GET_DEPENDENT_CHECK_CONSTRAINTS, [ $ct_hash->{schema}, $ct_hash->{name} ] );

    unless( $sth )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to get dependent check constraints on $ct_hash->{name}" );
        return 0;
    }

    $sth->finish();
    $sth = &try_query( $handle, $GET_DEPENDENT_TRIGGERS, [ $ct_hash->{schema}, $ct_hash->{name} ] );

    unless( $sth )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to get dependent triggers for $ct_hash->{name}" );
        return 0;
    }

    $sth->finish();
    $sth = &try_query( $handle, $GET_DEPENDENT_INDEXES );

    unless( $sth )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to get dependent triggers for $ct_hash->{name}" );
        return 0;
    }

    $sth->finish();

    return 1;
}

sub drop_dependencies($)
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $q = <<'END_SQL';
        SELECT drop_statement,
               object_name
          FROM tt_dependent_objects
      ORDER BY rank DESC
END_SQL

    my $sth = &try_query( $handle, $q );

    unless( $sth )
    {
        _log( $LOG_LEVEL_ERROR, 'Failed to get results from dependency table' );
        return 0;
    }

    while( my $row = $sth->fetchrow_hashref() )
    {
        unless( $handle->do( $row->{drop_statement} ) )
        {
            _log( $LOG_LEVEL_ERROR, "Failed to drop dependent object $row->{object_name}" );
            return 0;
        }
    }

    return 1;
}

sub recreate_dependencies($)
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $q = <<'END_SQL';
        SELECT create_statement,
               object_name
          FROM tt_dependent_objects
      ORDER BY rank ASC
END_SQL

    my $sth = &try_query( $handle, $q );

    unless( $sth )
    {
        _log( $LOG_LEVEL_ERROR, 'Failed to get create results from dependency table' );
        return 0;
    }

    while( my $row = $sth->fetchrow_hashref() )
    {
        unless( $handle->do( $row->{create_statement} ) )
        {
            _log( $LOG_LEVEL_ERROR, "Failed to recreate dependent object $row->{object_name}" );
            return 0;
        }
    }

    return 1;
}

sub drop_dependency_temp_table($)
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    $handle->do( 'DROP TABLE tt_dependent_objects' );

    return 1;
}

sub replace_cache_table($$) :Export( :MANDATORY )
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

    $handle->do( "SET application_name = 'replace $ct_hash->{name}'" );

    my $name          = $ct_hash->{name};
    my $schema        = $ct_hash->{schema};
    my $definition    = $ct_hash->{definition};

    $handle->do( 'BEGIN' );
    $handle->do( "SET client_min_messages = 'ERROR'" );

    unless( &create_dependent_temp_table( $handle, $ct_hash ) )
    {
        $handle->do( 'ROLLBACK' );
        _log(
            $LOG_LEVEL_FATAL,
            'Cache table replacement failed - could not collect dependent objects'
        );
    }

    $ct_hash->{name} .= '_temp';
    my $temp_name     = $ct_hash->{name};

    # note: This statement is intentionally not set to cascade - it's a safety mechanism in case we did not
    # drop dependent objects.
    $handle->do( "DROP TABLE IF EXISTS $temp_name" );

    &create_cache_table( $handle, $ct_hash );

    unless( &drop_dependencies( $handle ) )
    {
        $handle->do( 'ROLLBACK' );
        _log( $LOG_LEVEL_FATAL, "Failed to drop dependent objects" );
    }

    my $sth = &try_query( $handle, "DROP TABLE IF EXISTS $schema.$name CASCADE" );

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
        "ALTER TABLE $schema.$temp_name RENAME TO $name"
    );

    unless( $sth )
    {
        $handle->do( 'ROLLBACK' );
        $ct_hash->{name} = $name;
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

    unless( &recreate_dependencies( $handle ) )
    {
        $handle->do( 'ROLLBACK' );
        _log( $LOG_LEVEL_FATAL, "Failed to recreate dependencies\n" );
    }

    unless( $handle->do( "ALTER INDEX IF EXISTS ix_$temp_name RENAME TO ix_$name" ) )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to rename index for $name" );
    }

    &drop_dependency_temp_table( $handle );
    $ct_hash->{name} = $name;
    &rename_cache_table_indexes( $handle, $ct_hash );
    $handle->do( 'SET client_min_messages TO DEFAULT' );
    $handle->do( 'COMMIT' );
    return;
}

sub db_connect(;$) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF, optional => 1 }
    );

    # Fast conn check & ret
    if( defined( $handle ) && $handle->ping() > 0 && $handle->pg_ping() > 0 )
    {
        return $handle;
    }
    else
    {
        if(
              !defined( $CONNECTION_MAP )
           || !defined( $CONNECTION_MAP->{connection_string} )
           || !defined( $CONNECTION_MAP->{user_name} )
          )
        {
            return undef;
        }

        $handle = DBI->connect(
            $CONNECTION_MAP->{connection_string},
            $CONNECTION_MAP->{user_name},
            undef
        );
    }

    # We're hitting this section iff initial connection does not succeed
    my $connect_count = 0;
    my $sleep_backoff = 1;

    until( defined( $handle ) && $handle->ping() > 0 && $handle->pg_ping() > 0 )
    {
        if( $DEBUG )
        {
            _log( $LOG_LEVEL_INFO, "Not connected to DB, reconnecting..." );
        }

        $handle->disconnect() if( defined( $handle ) );
        undef( $handle );

        $connect_count++;
        sleep( $sleep_backoff );
        $sleep_backoff += int( rand( 2 ** $connect_count - 1 ) );

        $handle = DBI->connect(
            $CONNECTION_MAP->{connection_string},
            $CONNECTION_MAP->{user_name},
            undef
        );

        if( $connect_count > 5 )
        {
            # is our database still here? is the server down??
            return undef;
        }
    }

	_log( $LOG_LEVEL_INFO, "Reconnected to database" ) if( $connect_count > 0 );
	$handle->do( "SET tcp_keepalives_idle = $TCP_KEEPALIVE" );
	$handle->do( "SET tcp_keepalives_interval = $TCP_KEEPALIVE_INTERVAL" );
	$handle->do( "SET tcp_keepalives_count = $TCP_KEEPALIVE_COUNT" );
	$handle->do( "SET tcp_user_timeout = $TCP_USER_TIMEOUT" );
    return $handle;
}

sub try_query($$;$) :Export( :MANDATORY )
{
    my( $handle, $query, $params ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
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

    $handle = &db_connect( $handle );

    # We're connected to the DB at this point
    if( $PARENT_PID == $PROCESS_ID )
    {
        unless( check_extension_running( $handle ) )
        {
            _log(
                $LOG_LEVEL_FATAL,
                'Failed to acquire lock after reconnecting to database or extension not installed'
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

    $SLOT_NAME = get_slot_name( $handle );
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
        _log( $LOG_LEVEL_DEBUG, "slot $SLOT_NAME created" );
        $create_sth->finish();
    }
    else
    {
        # slot exists
        _log( $LOG_LEVEL_DEBUG, "Slot $SLOT_NAME exists!" );
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

sub get_worker_list($;$) :Export( :MANDATORY )
{
    my( $handle, $pk_maintenance_object ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR, optional => 1 },
    );

    my $query = $GET_WORKER_LIST;
    my $sth;

    if( defined $pk_maintenance_object )
    {
        $query .= ' WHERE mo.maintenance_object = ?';
        $sth = try_query( $handle, $query, [ $pk_maintenance_object] );
    }
    else
    {
        $sth = try_query( $handle, $query );
    }

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
            my $name                = $row->{name};
            push(
                @$worker_data,
                {
                    maintenance_channel => $maintenance_channel,
                    filter_tables       => $filter_tables,
                    wal_level           => $wal_level,
                    maintenance_object  => $maintenance_object,
                    name                => $name,
                }
            );
        }

        $sth->finish();
        return $worker_data;
    }

    $sth->finish();
    return undef;
}

sub replication_seek($$;$) :Export( :MANDATORY )
{
    my( $handle, $lsn, $all_filter_tables ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALAR, optional => 1 },
    );

    $handle = &db_connect( $handle );
    my $seek_query;
    my $params = [];
    if( defined( $all_filter_tables ) )
    {
        $seek_query = $REPLICATION_SEEK_QUERY;
        $params = [ $SLOT_NAME, $lsn, 'M', $all_filter_tables ];
    }
    else
    {
        $seek_query = $REPLICATION_SEEK_QUERY_NO_FT;
        $params = [ $SLOT_NAME, $lsn, 'M' ];
    }

    my $sth = try_query(
        $handle,
        $seek_query,
        $params
    );

    my $rows = 0;
    unless( $sth )
    {
        return -1;
    }

    $rows = $sth->rows();
    $sth->finish();
    return $rows;
}

sub replication_slot_peek_unneeded_changes($$$) :Export( :MANDATORY )
{
    my( $handle, $lsn, $all_filter_tables ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => SCALARREF },
        { type => SCALAR },
    );

    my $sth = try_query(
        $handle,
        $REPLICATION_PEEK_FOR_CATCHUP,
        [ $SLOT_NAME, $all_filter_tables ]
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

sub replication_peek($$$$) :Export( :MANDATORY )
{
    my( $handle, $filter_tables, $wal_level, $max_lsn ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => SCALAR },
        { type => SCALAR },
        { type => SCALARREF },
    );

    my $sth;
    $handle = &db_connect( $handle );
    if( defined( $filter_tables ) && length( $filter_tables ) > 0 )
    {
        $sth = try_query(
            $handle,
            $REPLICATION_PEEK_QUERY,
            [ $SLOT_NAME, $wal_level, $filter_tables, $$max_lsn, $$max_lsn ]
        );
    }
    else
    {
        $sth = try_query(
            $handle,
            $REPLICATION_PEEK_QUERY_NO_FT,
            [ $SLOT_NAME, $wal_level, $$max_lsn, $$max_lsn ]
        );
    }

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

            my $xid = $row->{xid};
            my $data;
            $data = decode_json( $row->{data} ) if( $row->{data} );
            my $out  = { lsn => $lsn, xid => $xid };

            if( $wal_level eq 'F' )
            {
                $out->{data} = $data;
            }
            elsif( $wal_level eq 'M' )
            {
                if( defined( $data->{d} ) )
                {
                    #inflate data
                    $out->{data}->{table_name} = $data->{t};
                    $out->{data}->{schema_name} = $data->{s};
                    $out->{data}->{key} = $data->{key};
                    $out->{data}->{type} = 'INSERT' if( $data->{d} eq 'I' );
                    $out->{data}->{type} = 'UPDATE' if( $data->{d} eq 'U' );
                    $out->{data}->{type} = 'DELETE' if( $data->{d} eq 'D' );
                }
                $out->{data}->{xid} = $data->{x};
            }

            unless( grep /^$xid$/, @$xids )
            {
                push( @$xids, $xid );
            }

            if(
                  (
                    $wal_level eq 'F'
                 && defined( $data->{type} )
                 && $data->{type} ne 'COMMIT'
                 && $data->{type} ne 'BEGIN'
                 && $data->{type} ne 'ROLLBACK'
                  )
               || (
                    $wal_level eq 'M'
                 && !defined( $data->{b} )
                  )
              )
            {
                push( @{$intermediate_data->{$xid}->{DML}}, $out );
            }
            else
            {
                # Assume transaction demarcation
                my $type;
                if( $wal_level eq 'F' )
                {
                    $type = $data->{type};
                }
                elsif( $wal_level eq 'M' )
                {
                    $type = 'BEGIN' if( $data->{b} eq 'B' );
                    $type = 'COMMIT' if( $data->{b} eq 'C' );
                    $type = 'ROLLBACK' if( $data->{b} eq 'R' );
                }

                $intermediate_data->{$xid}->{$type} = $out->{lsn};
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
                        $dml->{begin_lsn}  = $intermediate_data->{$xid}->{BEGIN};
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

sub check_ct_exists($) :Export( :MANDATORY )
{
    my( $handle, $ct_hash ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => HASHREF },
    );

    $handle = &db_connect( $handle );
    my $sth = try_query(
        $handle,
        $CHECK_CACHE_TABLE_EXISTS,
        [ $ct_hash->{schema}, $ct_hash->{name} ]
    );

    # If there are anny issues, we'll fail through to replacement sub
    unless( $sth )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to verify that $ct_hash->{schema}.$ct_hash->{name} exists" );
    }

    if( $sth && $sth->rows() > 0 )
    {
        $sth->finish();
        _log( $LOG_LEVEL_DEBUG, "Cache Table $ct_hash->{schema}.$ct_hash->{name} already exists" );

        my $create_tt = $CREATE_COLUMN_CHECK_TABLE;
        $create_tt =~ s/__DEFINITION__/$ct_hash->{definition}/;
        $sth = try_query(
            $handle,
            $create_tt
        );

        unless( $sth )
        {
            _log( $LOG_LEVEL_FATAL, "Failed to create test table using definition to validate columns" );
        }

        $sth->finish();

        $sth = try_query(
            $handle,
            $CHECK_CACHE_TABLE_COLUMNS,
            [ $ct_hash->{schema}, $ct_hash->{name} ]
        );

        unless( $sth )
        {
            _log( $LOG_LEVEL_FATAL, "Failed to check cache table columns against definition" );
        }

        $handle->do( 'DROP TABLE IF EXISTS tt_column_verify' );

        return if( $sth->rows() == 0 );

        _log(
            $LOG_LEVEL_ERROR,
            "There is a discrepency between the existing cache table and its definition. The cache table will be rebuilt"
        );
    }

    $sth->finish();
    &replace_cache_table( $handle, $ct_hash->{maintenance_object} );

    return;
}

sub create_cache_table($$)
{
    my( $handle, $ct_hash ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => HASHREF },
    );

    $handle = &db_connect( $handle );
    my $schema       = $ct_hash->{schema};
    my $name         = $ct_hash->{name};
    my $definition   = $ct_hash->{definition};
    $handle->do( "SET application_name = 'create: $name'" );
    my $create_query = $CREATE_CACHE_TABLE;
    $create_query    =~ s/__TABLE__/${schema}.${name}/;
    $create_query    =~ s/__DEFINITION__/$definition/;

    my $sth = try_query( $handle, $create_query, undef );

    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to create cache table $schema.$name" );
        return;
    }

    if( $BATCHED_CREATE )
    {
        _log( $LOG_LEVEL_DEBUG, "Performing batch population of $name" );
        $handle->do( "SET application_name = 'batch populate: $name'" );
        my $done            = 0;
        my $offset          = 0;
        my $populate_q      = $CREATE_POPULATE;
        my $initial_orderby = join( ',', @{$ct_hash->{unique_index}} );

        $populate_q =~ s/__TABLE__/${schema}.${name}/;
        $populate_q =~ s/__DEFINITION__/$definition/;
        $populate_q =~ s/__ORDERBY__/$initial_orderby/;
        $populate_q =~ s/__LIMIT__/$BATCH_SIZE/;

        while( !$done )
        {
            my $populate_iter_q = $populate_q;
            $populate_iter_q =~ s/__OFFSET__/$offset/;
            my $check_sth = &try_query( $handle, $populate_iter_q );

            unless( $check_sth )
            {
                _log( $LOG_LEVEL_ERROR, "Batched insert to ${name} failed!" );
                return;
            }

            $done = 1 if( $check_sth->rows() == 0 );
            $offset += $BATCH_SIZE;
        }
    }

    unless( &create_cache_table_unique( $handle, $ct_hash ) )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to create cache table unique index" );
    }

    $sth->finish();
    $handle->do( "ANALYZE $schema.$name" );

    foreach my $columns( @{$ct_hash->{indexes}} )
    {
        my $index_name = $columns;
        $index_name =~ s/[^[:alnum:]]/_/g;
        my $full_index_name = "ix_$ct_hash->{name}_$index_name";

        my $def = "CREATE INDEX $full_index_name ON $ct_hash->{schema}.$ct_hash->{name}( $columns )";

        unless( $handle->do( $def ) )
        {
            _log(
                $LOG_LEVEL_ERROR,
                "Failure when creating index for columns ($columns) on $ct_hash->{schema}.$ct_hash->{name}"
            );
        }
    }

    _log( $LOG_LEVEL_DEBUG, "Cache Table $schema.$name created" );
    return;
}

sub rename_cache_table_indexes($$)
{
    my( $handle, $ct_hash ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => HASHREF },
    );

    foreach my $columns( @{$ct_hash->{indexes}} )
    {
        my $index_name = $columns;

        $index_name =~ s/[^[:alnum:]]/_/g;

        my $temp_index_name = "ix_$ct_hash->{name}_temp_$index_name";
        my $new_index_name  = "ix_$ct_hash->{name}_$index_name";

        my $def = "ALTER INDEX IF EXISTS $temp_index_name RENAME TO $new_index_name";

        unless( $handle->do( $def ) )
        {
            _log(
                $LOG_LEVEL_ERROR,
                "Failed to rename index - this may cause issues the next time a table is redefined"
            );
        }
    }

    _log( $LOG_LEVEL_DEBUG, "Cache table indexes renamed" );

    return;
}

sub create_cache_table_unique($$) :Export( :MANDATORY )
{
    my( $handle, $ct_hash ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => HASHREF },
    );

    $handle = &db_connect( $handle );
    my $index_columns = join( ',', @{$ct_hash->{unique_index}} );
    my $sth = try_query( $handle, "CREATE UNIQUE INDEX IF NOT EXISTS ix_$ct_hash->{name} ON $ct_hash->{schema}.\"$ct_hash->{name}\"( $index_columns )" );

    return 0 unless( $sth );

    foreach my $col( $ct_hash->{unique_index} )
    {
        push( @{$ct_hash->{cache_table_uniques}}, $col );
    }

    $sth->finish();
    return 1;
}

sub test_query($$) :Export( :MANDATORY )
{
    my( $handle, $query ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => SCALAR },
    );

    $handle = &db_connect( $handle );
    my $test_query = "WITH tt_test AS( $query ) SELECT * FROM tt_test LIMIT 0";

    my $sth = $handle->prepare( $test_query );

    return 0 if( !defined( $sth ) );
    return 0 unless( $sth->execute() );

    $sth->finish();
    return 1;
}

sub get_table_count($$) :Export( :MANDATORY )
{
    my( $handle, $table_name ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => SCALAR },
    );

    $handle = &db_connect( $handle );
    my $query = "SELECT COUNT(*) as count FROM $table_name";

    my $sth = &try_query( $handle, $query );

    return -1 unless( $sth );

    my $row = $sth->fetchrow_hashref();

    my $count = $row->{count};
    $sth->finish();
    return $count;
}

sub get_def_count($$) :Export( :MANDATORY )
{
    my( $handle, $definition ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => SCALAR },
    );

    $handle = &db_connect( $handle );
    my $def_q = <<END_SQL;
    WITH tt_def AS
    (
        $definition
    )
        SELECT COUNT(*) AS count FROM tt_def
END_SQL

    my $sth = &try_query( $handle, $def_q );

    return -1 unless( $sth );

    my $row = $sth->fetchrow_hashref();

    $sth->finish();
    my $count = $row->{count};
    return $count;
}

sub generate_temp_table($$$) :Export( :MANDATORY )
{
    my( $handle, $query, $ct_hash ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => SCALAR },
        { type => HASHREF },
    );

    $handle = &db_connect( $handle );
    # TODO This can take some time
    my $temp_table_name = 'tt_' . $ct_hash->{name};
    my $tt_query        = "CREATE TEMP TABLE $temp_table_name AS( $query );";
    print "$tt_query\n";
    my $sth             = try_query( $handle, $tt_query );

    if( $sth )
    {
        $sth->finish();

        my $tt_count = get_table_count( $handle, $temp_table_name );
        return undef if( $tt_count < 0 );

        my $return_data = { count => $tt_count, name => $temp_table_name, index => "ix_$temp_table_name" };
        my $uniques     = join( ',', @{$ct_hash->{unique_index}} );

        $sth = try_query( $handle, "CREATE UNIQUE INDEX ix_$temp_table_name ON $temp_table_name( $uniques )" );

        if( $sth )
        {
            $sth->finish();
        }
        else
        {
            _log(
                $LOG_LEVEL_WARNING,
                "Failed to create unique index on comparrison table. "
              . "Please verify the cardinality of this index provided!"
            );
        }

        return $return_data;
    }

    return undef;
}

sub drop_temp_table($$) :Export( :MANDATORY )
{
    my( $handle, $temp_table ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => HASHREF },
    );

    my $query = 'DROP TABLE IF EXISTS ' . $temp_table->{name};
    my $sth   = try_query( $handle, $query );

    return 0 unless( $sth );

    $sth->finish();
    return 1;
}

sub get_cache_table_columns($$$) :Export( :MANDATORY )
{
    my( $handle, $cache_table_schema, $cache_table_name ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
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
        { type => OBJECT | UNDEF },
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

sub generate_update_statement($$$) :Export( :MANDATORY )
{
    my(
        $handle,
        $temp_table,
        $cache_hash
      ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
    );

    my $cache_table_schema = $cache_hash->{schema};
    my $cache_table_name   = $cache_hash->{name};
    my $table_columns      = $cache_hash->{cache_table_columns};
    my $uniques            = $cache_hash->{cache_table_uniques};

    $handle = &db_connect( $handle );
    $handle->do( "SET application_name = 'update: $cache_table_name'" );
    my $join_clauses       = [];
    my $where_clauses      = [];
    my $distinct_uniques   = [];
    my $non_unique_columns = [];

    foreach my $unique_columns( @$uniques )
    {
        my $join_clause  = join(
            ' AND ',
            map { "( ( tt.$_ IS NULL AND vw.$_ IS NULL ) OR ( tt.$_ = vw.$_ ) )" } @$unique_columns
        );

        my $where_clause = join(
            ' AND ',
            map { "( ( ct.$_ IS NULL AND tt.$_ IS NULL ) OR ( ct.$_ = tt.$_ ) )" } @$unique_columns
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

    if( $temp_table->{count} > $BULK_ACTION_CUTOFF )
    {
        my $columns = join( ',', @$table_columns );
        _log(
            $LOG_LEVEL_DEBUG,
            "Performing large update optimization ($temp_table->{count} possible rows)"
        );
        $handle->do( 'BEGIN' );
        $handle->do( "DROP INDEX IF EXISTS ix_$cache_hash->{name}" );
        my $delete_where = '( ( ' . join( ' ) OR ( ', @$where_clauses ) . ' ) )';
        my $DELETE_Q = <<END_SQL;
        DELETE FROM $cache_table_schema.$cache_table_name ct
              USING $temp_table->{name} tt
              WHERE $delete_where
END_SQL
        unless( &try_query( $handle, $DELETE_Q, [] ) )
        {
            _log( $LOG_LEVEL_ERROR, 'Failed to bulk delete rows for fast update' );
            $handle->do( 'ROLLBACK' );
            return 0;
        }

        my $INSERT_Q = <<END_SQL;
        INSERT INTO $cache_table_schema.$cache_table_name
                    (
                        $columns
                    )
             SELECT $columns
               FROM $temp_table->{name}
END_SQL

        unless( &try_query( $handle, $INSERT_Q ) )
        {
            $handle->do( 'ROLLBACK' );
            _log( $LOG_LEVEL_ERROR, 'failed to bulk insert rows for fast update' );
            return 0
        }

        unless( create_cache_table_unique( $handle, $cache_hash ) )
        {
            $handle->do( 'ROLLBACK' );
            _log( $LOG_LEVEL_ERROR, "Failed to recreate unique index" );
            return 0;
        }
        $handle->do( 'COMMIT' );
        $handle->do( "ANALYZE $cache_table_schema.$cache_table_name" );
    }
    else
    {
        my $update_fragment = join( ', ', map { "$_ = tt.$_" } @$non_unique_columns );
        my $where_clause    = '( ( ' . join( ' ) OR ( ', @$where_clauses ) . ' ) )';
        my $diff_distinct   = '( ' . join( ' OR ', map { "ct.$_ IS DISTINCT FROM tt.$_" } @$non_unique_columns ) . ' )';
        my $UPDATE_Q = <<END_SQL;
        UPDATE $cache_table_schema.$cache_table_name ct
           SET $update_fragment
          FROM $temp_table->{name} tt
         WHERE $where_clause
           AND $diff_distinct
END_SQL
        my $sth = &try_query( $handle, $UPDATE_Q, [] );

        return 0 unless( $sth );

        $sth->finish();
    }

    return 1;
}

sub generate_insert_statement($$$) :Export( :MANDATORY )
{
    my(
        $handle,
        $temp_table,
        $cache_hash
      ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
    );

    my $cache_table_schema = $cache_hash->{schema};
    my $cache_table_name   = $cache_hash->{name};
    my $table_columns      = $cache_hash->{cache_table_columns};
    my $uniques            = $cache_hash->{cache_table_uniques};

    $handle = &db_connect( $handle );
    $handle->do( "SET application_name = 'insert: $cache_table_name'" );

    my $join_clauses  = [];
    my $where_clauses = [];

    foreach my $unique_columns( @$uniques )
    {
        my $join_clause  = join(
            ' AND ',
            map { "( ( tt.$_ IS NULL AND vw.$_ IS NULL ) OR ( tt.$_ = vw.$_ ) )" } @$unique_columns
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
    WITH tt_records_to_insert AS MATERIALIZED
    (
        SELECT $columns
          FROM $temp_table->{name} vw
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

sub generate_aged_delete_statement($$$$$) :Export( :MANDATORY )
{
    my(
        $aged_handle,
        $current_handle,
        $aged_temp_table,
        $current_temp_table,
        $cache_hash
      ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => OBJECT },
        { type => HASHREF },
        { type => HASHREF },
        { type => HASHREF },
    );

    my $definition         = $cache_hash->{definition};
    my $cache_table_schema = $cache_hash->{schema};
    my $cache_table_name   = $cache_hash->{name};
    my $table_columns      = $cache_hash->{cache_table_columns};
    my $uniques            = $cache_hash->{cache_table_uniques};

    $current_handle->do( "SET application_name = 'Fast delete: $cache_table_name'" );
    $aged_handle->do( "SET application_name = 'Lookback: $cache_table_name'" );

    my $column_data_type_hash = {};
    my $column_data_types = [];
    my $get_type_q = <<END_SQL;
    SELECT t.typname AS datatype
      FROM pg_class c
      JOIN pg_attribute a
        ON a.attnum > 0
       AND a.attrelid = c.oid
      JOIN pg_type t
        ON t.oid = a.atttypid
     WHERE c.relname = ?
       AND a.attname = ?
END_SQL
    my $get_type_sth = $aged_handle->prepare( $get_type_q );

    unless( $get_type_sth )
    {
        _log( $LOG_LEVEL_ERROR, 'Failed to prepare type lookup query for aged handle' );
        return 0;
    }

    foreach my $unique_column_set( @$uniques )
    {
        foreach my $unique_column( @$unique_column_set )
        {
            next if( defined( $column_data_type_hash->{$unique_column} ) );
            $get_type_sth->bind_param( 1, $aged_temp_table->{name} );
            $get_type_sth->bind_param( 2, $unique_column );
            unless( $get_type_sth->execute() )
            {
                _log( $LOG_LEVEL_ERROR, "Failed to lookup datatype for unique column $unique_column on aged handle" );
                return 0;
            }

            unless( $get_type_sth->rows() > 0 )
            {
                _log( $LOG_LEVEL_ERROR, "No column found on aged handle for $unique_column" );
                return 0;
            }
            my $row = $get_type_sth->fetchrow_hashref();
            push( @$column_data_types, $unique_column . ' ' . $row->{datatype} );
            $column_data_type_hash->{$unique_column} = $row->{datatype};
        }
    }

    $get_type_sth->finish();
    my $past_temp_table = "tt_past_data_${PROCESS_ID}";
    $current_handle->do( "DROP TABLE IF EXISTS $past_temp_table" );
    my $temp_table_q = "CREATE TEMP TABLE $past_temp_table ( "
                     . join( ',', @$column_data_types )
                     . ' )';

    unless( $current_handle->do( $temp_table_q ) )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to create $past_temp_table" );
        $aged_handle->do( 'ROLLBACK' );
        $aged_handle->disconnect();
        return 0;
    }

    my $unique_columns = join( ',', keys %$column_data_type_hash );
    my $AGED_DATA_QUERY = <<END_SQL;
        SELECT $unique_columns
          FROM $aged_temp_table->{name}
END_SQL

    my $aged_sth = $aged_handle->prepare( $AGED_DATA_QUERY );

    unless( $aged_sth )
    {
        _log( $LOG_LEVEL_ERROR, 'Failed to prepare aged data query for fast delete' );
        return 0;
    }

    unless( $aged_sth->execute() )
    {
        _log( $LOG_LEVEL_DEBUG, 'Failed to execute aged data query for fast delete' );
        return 0;
    }

    my $bind_points = '?' . ( ',?' x ( scalar( keys %$column_data_type_hash ) - 1 ) );
    my $insert_q    = "INSERT INTO $past_temp_table( "
                    . join( ',', sort { $a cmp $b } keys %$column_data_type_hash )
                    . " ) VALUES ( $bind_points )";

    my $insert_sth = $current_handle->prepare( $insert_q );
    unless( $insert_sth )
    {
        _log( $LOG_LEVEL_ERROR, 'Failed to prepared insert statement for past data transfer' );
        $aged_sth->finish();
        $aged_handle->do( 'ROLLBACK' );
        $aged_handle->disconnect();
        return 0;
    }

    my $where_filters = [];
    # TODO move count here
    if( $current_temp_table->{count} == $aged_sth->rows() )
    {
        # no rows previously existed
        _log(
            $LOG_LEVEL_DEBUG,
            'Fast delete early exit - no rows previously existed '
          . 'matching this filter or subset rough match'
        );

        $aged_sth->finish();
        $insert_sth->finish();
        $aged_handle->do( 'ROLLBACK' );
        $aged_handle->disconnect();
        return 1;
    }

    unless( $aged_sth->rows() > 0 )
    {
         # Likely an anti-join involved - revert to slow delete
        _log( $LOG_LEVEL_WARNING, "Insufficient data in aged handle" );
        $insert_sth->finish();
        $aged_sth->finish();
        $aged_handle->do( 'ROLLBACK' );
        $aged_handle->disconnect();
        return 0;
    }

    while( my $row = $aged_sth->fetchrow_hashref() )
    {
        my $where_filter_elems = [];
        my $index = 1;

        foreach my $unique_columns( @$uniques )
        {
            my $where_elems = [];
            foreach my $unique( @$unique_columns )
            {
                my $value;
                if( $row->{$unique} )
                {
                    $value = "'" . $row->{$unique} . "'::" . $column_data_type_hash->{$unique};
                }
                else
                {
                    $value = 'NULL::' . $column_data_type_hash->{$unique};
                }

                push( @$where_elems, "( vw.$unique IS NULL AND  $value IS NULL ) OR ( vw.$unique = $value )" );
            }

            push( @$where_filter_elems, ' ( ( ' . join( ' ) AND ( ', @$where_elems ) . ' ) ) ' );
        }

        push( @$where_filters, ' ( ( ' . join( ' ) OR ( ', @$where_filter_elems ) . ' ) ) ' );

        foreach my $unique( sort { $a cmp $b } keys %$column_data_type_hash )
        {
            $insert_sth->bind_param( $index, $row->{$unique} );
            $index++;
        }

        unless( $insert_sth->execute() )
        {
            _log( $LOG_LEVEL_ERROR, 'Failed to insert aged data into current timeline' );
            return 0;
        }
    }

    $insert_sth->finish();
    $aged_sth->finish();
    $aged_handle->do( 'ROLLBACK' );
    $aged_handle->disconnect();

    # at this point, past_temp_table contains data from a historic timeline but is in the present timeline
    my $unique_column_select = join( ',', map { "vw.$_" } keys %$column_data_type_hash );

    my $left_join_clauses = [];
    my $left_join_wheres  = [];
    foreach my $unique_columns( @$uniques )
    {
        my $join_clause = join(
            ' AND ',
            map { "( vw.$_ IS NULL AND tt.$_ IS NULL ) OR ( vw.$_ = tt.$_ )" } @$unique_columns
        );

        my $where_clause = join(
            ' AND ',
            map { "tt.$_ IS NULL" } @$unique_columns
        );

        push( @$left_join_wheres, $where_clause );
        push( @$left_join_clauses, $join_clause );
    }

    my $left_join_predicate  = ' ( ( ' . join( ' ) OR ( ', @$left_join_clauses ) . ' ) ) ';
    my $left_join_where      = ' ( ( ' . join( ' ) OR ( ', @$left_join_wheres ) . ' ) ) ';
    my $main_filter          = '( ' . join( ') OR (', @$where_filters ) . ' )';
    my $delete_query = <<END_SQL;
    WITH tt_rows_to_delete AS
    (
        SELECT $unique_column_select
          FROM $cache_table_schema.$cache_table_name vw
     LEFT JOIN $past_temp_table tt
            ON $left_join_predicate
         WHERE $left_join_where
           AND $main_filter
    )
        DELETE FROM $cache_table_schema.$cache_table_name vw
              USING tt_rows_to_delete tt
              WHERE $left_join_predicate
END_SQL

    unless( $current_handle->do( $delete_query ) )
    {
        _log( $LOG_LEVEL_ERROR, 'Failed to execute fast delete query' );
        return 0;
    }

    return 1;
}

sub generate_delete_statement($$) :Export( :MANDATORY )
{
    my(
        $handle,
        $cache_hash,
      ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => HASHREF },
    );

    my $definition         = $cache_hash->{definition};
    my $cache_table_schema = $cache_hash->{schema};
    my $cache_table_name   = $cache_hash->{name};
    my $table_columns      = $cache_hash->{cache_table_columns};
    my $uniques            = $cache_hash->{cache_table_uniques};

    $handle->do( "SET application_name = 'delete: $cache_table_name'" );

    my $join_clauses  = [];
    my $where_clauses = [];
    my $index_elems   = [];

    foreach my $unique_columns( @$uniques )
    {
        my $join_clause  = join(
            ' AND ',
            map { "( ( tt.$_ IS NULL AND vw.$_ IS NULL ) OR ( tt.$_ = vw.$_ ) )" } @$unique_columns
        );
        my $where_clause = join(
            ' AND ',
            map { "tt.$_ IS NULL" } @$unique_columns
        );

        my $index_elem = join( ',', @$unique_columns );

        push( @$join_clauses,  $join_clause  );
        push( @$where_clauses, $where_clause );
        push( @$index_elems,   $index_elem   );
    }

    my $delete_tt_name = "tt_base_data_${PROCESS_ID}";
    my $columns        = join( ', ', map { "vw.$_" } @$table_columns );
    my $join_predicate = '( ( ' . join( ' ) OR ( ', @$join_clauses ) . ' ) )';
    my $where_clause   = '( ( ' . join( ' ) AND ( ', @$where_clauses ) . ' ) )';

    my $DELETE_TT_Q = << "END_SQL";
    CREATE TEMP TABLE ${delete_tt_name} AS
    (
        $definition
    )
END_SQL

    my $sth = &try_query( $handle, $DELETE_TT_Q, [] );
    my $ind_ind = 0;

    return 0 unless( $sth );
    $sth->finish();

    foreach my $ind( @$index_elems )
    {
        my $stmt = "CREATE INDEX ix_${delete_tt_name}_${ind_ind} ON ${delete_tt_name}( $ind ) ";
        $ind_ind++;
        unless( &try_query( $handle, $stmt, [] ) )
        {
            _log(
                $LOG_LEVEL_ERROR,
                "Failed to create slow delete index"
            );
        }
    }

    # Create some indexes

    my $DELETE_Q = <<"END_SQL";
    WITH tt_rows_to_delete AS
    (
        SELECT $columns
          FROM $cache_table_schema.$cache_table_name vw
     LEFT JOIN $delete_tt_name tt
            ON $join_predicate
         WHERE $where_clause
    )
    DELETE FROM $cache_table_schema.$cache_table_name tt
          USING tt_rows_to_delete vw
          WHERE $join_predicate
END_SQL

    $sth = &try_query( $handle, $DELETE_Q, [] );

    $handle->do( "DROP TABLE IF EXISTS ${delete_tt_name}" );
    return 0 unless( $sth );
    $sth->finish();
    return 1;
}

1;
