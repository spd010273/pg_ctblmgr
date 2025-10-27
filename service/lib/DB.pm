package DB;

use strict;
use warnings;
use utf8;

use DBI;
use Perl6::Export::Attrs;
use FindBin;
use English qw( -no_match_vars );
use Params::Validate qw( :all );
use Readonly;

use JSON::XS;
use IO::Select;
use IO::Handle;

use Data::Dumper;

use lib "$FindBin::Bin";
use Util;
use ConfigManager;

my $PRINT_QUERIES = 0; # Debug only

# Default configuration values from ConfigManager catalog
# This is to allow the lib functions to be used without initializing the ConfigManager
Readonly my $DEFAULT_BATCHED_CREATE           => $CONFIG_CATALOG{'batched_create'}->{value};
Readonly my $DEFAULT_BATCH_SIZE               => $CONFIG_CATALOG{'batch_size'}->{value};
Readonly my $DEFAULT_CONSERVATIVE_FAST_DELETE => $CONFIG_CATALOG{'conservative_fast_delete'}->{value};
Readonly my $DEFAULT_MAX_QUERY_RETRIES        => $CONFIG_CATALOG{'max_query_retries'}->{value};
Readonly my $DEFAULT_TCP_KEEPALIVES_IDLE      => $CONFIG_CATALOG{'db_conn_tcp_keepalives_idle'}->{value};
Readonly my $DEFAULT_TCP_KEEPALIVES_INTERVAL  => $CONFIG_CATALOG{'db_conn_tcp_keepalives_interval'}->{value};
Readonly my $DEFAULT_TCP_USER_TIMEOUT         => $CONFIG_CATALOG{'db_conn_tcp_user_timeout'}->{value};

$OUTPUT_AUTOFLUSH = 1;
our $CONNECTION_MAP              :Export( :MANDATORY );
our $LOCAL_PK_MAINTENANCE_OBJECT :Export( :MANDATORY );
our $SKIP_SHM_CLEANUP            :Export( :MANDATORY ) = 0;
our $MAINTENANCE_CHANNEL         :Export( :MANDATORY ) = '';
our $SELECTOR                    :Export( :MANDATORY );
our $FILE_DESCRIPTOR             :Export( :MANDATORY );
our $SKIP_LOCK_CHECK             :Export( :MANDATORY ) = 0;
our $BACKEND_PID                 :Export( :MANDATORY ) = 0;
our $NO_TEMP_TABLES              :Export( :MANDATORY ) = 0;
our $FORCE_MATERIALIZE           :Export( :MANDATORY ) = 0;
our $ENABLE_FAST_DELETE          :Export( :MANDATORY ) = 0;

# Only use global locking when global snapshots are enabled. Currently cross db snapshot imports are not allowed
#our $PGCTBLMGR_XID_MAGIC_1       :Export( :MANDATORY ) = 82163684;
#our $PGCTBLMGR_XID_MAGIC_2       :Export( :MANDATORY ) = 33128049;

Readonly::Scalar my $GET_TABLE_COLUMNS_DATATYPES => <<END_SQL;
    SELECT a.attname AS column_name,
           t.typname AS datatype
      FROM pg_class c
      JOIN pg_namespace n
        ON n.oid = c.relnamespace
      JOIN pg_attribute a
        ON a.attnum > 0
       AND a.attrelid = c.oid
      JOIN pg_type t
        ON t.oid = a.atttypid
     WHERE n.nspname = ?
       AND c.relname = ?
END_SQL

#    SELECT pg_try_advisory_lock( ?::INTEGER, ?::INTEGER ) AS locked
Readonly::Scalar my $XID_SERVICE_LOCK => <<"END_SQL";
    SELECT pg_try_advisory_lock( c.oid::INTEGER, -1::INTEGER ) AS locked
      FROM pg_class c
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname = '${SCHEMA_NAME}'
     WHERE c.relname = '__pgctblmgr_repl_slot'
END_SQL

# old global query
#Readonly::Scalar my $XID_SERVICE_CHECK => <<"END_SQL";
#    SELECT l.*
#      FROM pg_locks l
#     WHERE l.classid = ?::INTEGER -- magic 1
#       AND l.pid != pg_backend_pid()
#       AND l.objid = ?::INTEGER -- magic 2
#END_SQL
#
Readonly::Scalar my $XID_SERVICE_CHECK => <<"END_SQL";
    SELECT l.*
      FROM pg_locks l
INNER JOIN pg_class c
        ON c.oid = l.classid
       AND l.pid != pg_backend_pid()
INNER JOIN pg_database d
        ON d.oid = l.database
       AND d.datname = current_database()
     WHERE l.objid = -1::INTEGER
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
           mo.name,
           regexp_replace(
               digest(
                   mo.definition,
                   'sha256'::VARCHAR
               )::VARCHAR,
               '\\\\x',
               ''
           ) AS hash
      FROM ${SCHEMA_NAME}.__pgctblmgr_repl_slot rs
INNER JOIN ${SCHEMA_NAME}.tb_maintenance_object mo
        ON mo.maintenance_object = rs.id
INNER JOIN ${SCHEMA_NAME}.tb_maintenance_group mg
        ON mg.maintenance_group = mo.maintenance_group
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
    SELECT a.attname::VARCHAR as column_name,
           a.atttypid AS data_type
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
    SELECT a.attname::VARCHAR AS column_name,
           a.atttypid AS data_type
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
       AND e.data_type = r.data_type
     WHERE e.column_name IS NULL
        OR r.column_name IS NULL
END_SQL


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
    SELECT d.name AS driver_name,
           rs.filter,
           rs.maintenance_channel,
           mo.*
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
    SELECT COALESCE( ns.nspname::VARCHAR, 'public' )
        || '.'
        || c.relname::VARCHAR AS object_name,
           CASE WHEN c.relkind = 'm'
                THEN 'MATERIALIZED'
                ELSE ''
                 END AS view_type,
           regexp_replace(
               pg_get_viewdef( c.oid, TRUE ),
               ';\\s*\$',
               ''
           ) AS definition,
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
     SELECT 'DROP '
         || view_type
         || ' VIEW '
         || object_name AS drop_statement,
            'CREATE '
         || view_type
         || ' VIEW '
         || object_name
         || ' AS ( '
         || definition
         || ')' AS create_statement,
            object_name,
            TRUE,
            rank
       FROM tt_def_prep
END_SQL

Readonly::Scalar my $GET_FK_DEPENDENCIES => <<"END_SQL";
WITH tt_fk_constraints AS
(
    SELECT co.conname::VARCHAR AS constraint_name,
           COALESCE( nr.nspname::VARCHAR, 'public' )
        || '.'
        || cr.relname::VARCHAR AS object,
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
           COALESCE( nr.nspname::VARCHAR, 'public' )
        || '.'
        || cr.relname::VARCHAR AS object,
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
        ON tt.object_name = COALESCE( nr.nspname::VARCHAR, 'public' )
        || '.'
        || cr.relname::VARCHAR
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
     SELECT 'ALTER TABLE '
         || object
         || ' DROP CONSTRAINT '
         || constraint_name AS drop_statement,
            'ALTER TABLE '
         || object
         || ' ADD CONSTRAINT '
         || constraint_name
         || ' '
         || definition AS create_statement,
            constraint_name,
            FALSE,
            rank
       FROM tt_fk_constraints tt;
END_SQL

Readonly::Scalar my $GET_DEPENDENT_CHECK_CONSTRAINTS => <<"END_SQL";
WITH tt_check_constraints AS
(
    SELECT co.conname::VARCHAR AS constraint_name,
           COALESCE( n.nspname::VARCHAR, 'public' )
        || '.'
        || c.relname::VARCHAR AS object,
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
           COALESCE( n.nspname::VARCHAR, 'public' )
        || '.'
        || c.relname::VARCHAR AS object,
           pg_get_constraintdef( co.oid, TRUE ) AS definition,
           tt.rank + 1 AS rank
      FROM pg_constraint co
INNER JOIN pg_class c
        ON c.oid = co.conrelid
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
INNER JOIN tt_dependent_objects tt
        ON tt.object_name = COALESCE( n.nspname::VARCHAR, 'public' )
        || '.'
        || c.relname::VARCHAR
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
     SELECT 'ALTER TABLE '
         || object
         || ' DROP CONSTRAINT '
         || constraint_name AS drop_statement,
            'ALTER TABLE '
         || object
         || ' ADD CONSTRAINT '
         || constraint_name
         || ' '
         || definition AS create_statement,
            constraint_name,
            FALSE,
            rank
       FROM tt_check_constraints tt;
END_SQL

Readonly::Scalar my $GET_DEPENDENT_TRIGGERS => <<"END_SQL";
WITH tt_triggers AS
(
    SELECT t.tgname AS trigger_name,
           COALESCE( n.nspname::VARCHAR, 'public' )
        || '.'
        || c.relname::VARCHAR AS object,
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
           COALESCE( n.nspname::VARCHAR, 'public' )
        || '.'
        || c.relname::VARCHAR AS object,
           pg_get_triggerdef( t.oid, TRUE ) AS definition,
           2 AS rank
      FROM pg_trigger t
INNER JOIN pg_class c
        ON c.oid = t.tgrelid
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
INNER JOIN tt_dependent_objects tt
        ON tt.object_name = COALESCE( n.nspname::VARCHAR, 'public' )
        || '.'
        || c.relname::VARCHAR
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
     SELECT 'DROP TRIGGER '
         || trigger_name
         || ' ON '
         || object AS drop_statement,
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
            ON tt.object_name = COALESCE( n.nspname::VARCHAR, 'public' )
            || '.'
            || c.relname::VARCHAR
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

Readonly::Scalar my $MANAGE_INDEXES => <<"END_SQL";
WITH tt_mo AS
(
    SELECT mo.namespace,
           mo.name,
           mo.indexes,
           'ix_' || mo.name AS unique_index,
           'ix_null_' || mo.name AS nullable_index
      FROM ${SCHEMA_NAME}.tb_maintenance_object mo
     WHERE mo.maintenance_object = ?
),
tt_needed AS
(
    SELECT ( 'ix_' || mo.name || '_' || regexp_replace( x, '[^[:alnum:]]', '_', 'g' ) )::VARCHAR(63) AS needed_index,
           'CREATE INDEX IF NOT EXISTS ' || ( 'ix_' || mo.name || '_' || regexp_replace( x, '[^[:alnum:]]', '_', 'g' ) )::VARCHAR(63) || ' ON ' || mo.namespace || '.' || mo.name || '( ' || x || ' )' AS statement
      FROM tt_mo mo
INNER JOIN unnest( mo.indexes ) x
        ON TRUE
),
tt_existing_indexes AS
(
    SELECT ci.relname AS existing_index,
           'DROP INDEX IF EXISTS ' || ci.relname AS statement
      FROM tt_mo mo
INNER JOIN pg_class c
        ON c.relname = mo.name
       AND c.relkind = 'r'
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname = mo.namespace
INNER JOIN pg_index i
        ON i.indrelid = c.oid
INNER JOIN pg_class ci
        ON ci.oid = i.indexrelid
       AND ci.relkind = 'i'
       AND ci.relname != mo.unique_index
       AND ci.relname != mo.nullable_index
),
tt_create_drop AS
(
         SELECT COALESCE( ttn.needed_index, tte.existing_index ) AS index_name,
                CASE WHEN tte.existing_index IS NULL AND ttn.needed_index IS NOT NULL
                     THEN TRUE
                     WHEN tte.existing_index IS NOT NULL AND ttn.needed_index IS NULL
                     THEN FALSE
                     ELSE NULL
                      END AS create_drop,
                COALESCE( ttn.statement, tte.statement ) AS statement
           FROM tt_existing_indexes tte
FULL OUTER JOIN tt_needed ttn
             ON ttn.needed_index = tte.existing_index
)
    SELECT index_name,
           statement
      FROM tt_create_drop
     WHERE create_drop IS NOT NULL
END_SQL

Readonly::Scalar my $UPDATE_FILTERS => <<"END_SQL";
    UPDATE ${SCHEMA_NAME}.__pgctblmgr_repl_slot
       SET filter = ?
     WHERE id = ?
END_SQL

Readonly::Scalar my $CHECK_PARENT_LOCK => <<"END_SQL";
    SELECT l.*
      FROM pg_locks l
INNER JOIN pg_class c
        ON c.oid = l.classid
       AND c.relname = '__pgctblmgr_repl_slot'
INNER JOIN pg_namespace n
        ON n.nspname = '${SCHEMA_NAME}'
       AND n.oid = c.relnamespace
INNER JOIN pg_database d
        ON d.oid = l.database
       AND d.datname = current_database()
     WHERE l.pid = pg_backend_pid()
END_SQL

Readonly::Scalar my $CHECK_WORKER_LOCK => <<"END_SQL";
    SELECT l.*
      FROM pg_locks l
INNER JOIN pg_class c
        ON c.oid = l.classid
       AND c.relname = '__pgctblmgr_repl_slot'
INNER JOIN pg_namespace n
        ON n.nspname = '${SCHEMA_NAME}'
       AND n.oid = c.relnamespace
INNER JOIN pg_database d
        ON d.oid = l.database
       AND d.datname = current_database()
     WHERE l.objid = ?
       AND l.pid = pg_backend_pid()
END_SQL

Readonly::Scalar my $GET_LOCK => <<"END_SQL";
    SELECT pg_try_advisory_lock( c.oid::INTEGER, rs.id::INTEGER ) AS locked
      FROM pg_class c
INNER JOIN pg_namespace n
        ON n.nspname = '${SCHEMA_NAME}'
       AND n.oid = c.relnamespace
INNER JOIN ${SCHEMA_NAME}.__pgctblmgr_repl_slot rs
        ON rs.id = ?::INTEGER
     WHERE c.relname = '__pgctblmgr_repl_slot'
END_SQL

Readonly::Scalar my $FETCH_TYPMODS_QUERY => <<"END_SQL";
    SELECT a.attname::VARCHAR AS column,
           t.typname::VARCHAR AS type
      FROM pg_class c
INNER JOIN pg_attribute a
        ON a.attrelid = c.oid
       AND a.attisdropped IS FALSE
       AND a.attnum > 0
INNER JOIN pg_type t
        ON t.oid = a.atttypid
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
     WHERE n.nspname::VARCHAR || '.' || c.relname::VARCHAR = ?
END_SQL

Readonly::Scalar my $GET_CT_INDEX_SHA => <<"END_SQL";
    SELECT regexp_replace(
               digest(
                   COALESCE(
                       array_to_string(
                           array_agg( x ORDER BY x ),
                           ''
                       ),
                       ''
                   ),
                   'sha256'::VARCHAR
               )::VARCHAR,
               '\\\\x',
               ''
           ) AS hash
      FROM ${SCHEMA_NAME}.tb_maintenance_object mo
INNER JOIN unnest( mo.indexes ) x
        ON TRUE
     WHERE mo.maintenance_object = ?
END_SQL

Readonly::Scalar my $GET_XID_SERVICE_HOST => <<"END_SQL";
    SELECT host
      FROM ${SCHEMA_NAME}.tb_location
     WHERE namespace = 'XID_SERVICE'
       AND location = 0
END_SQL

Readonly::Scalar my $GET_XID_SERVICE_PORT => <<"END_SQL";
    SELECT port
      FROM ${SCHEMA_NAME}.tb_location
     WHERE namespace = 'XID_SERVICE'
       AND location = 0
END_SQL

sub get_xid_service_host($) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    if( defined( $CONFIG_MANAGER ) )
    {
        my $potential_host = $CONFIG_MANAGER->get_config_value( 'xid_service_host' );

        if( defined( $potential_host ) && length( $potential_host ) > 0 )
        {
            return $potential_host;
        }
    }

    my $sth = $handle->prepare( $GET_XID_SERVICE_HOST );

    return unless( $sth && $sth->execute() );
    my $host_row = $sth->fetchrow_hashref();
    my $host = $host_row->{host};
    return $host;
}

sub get_xid_service_port($) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    if( defined( $CONFIG_MANAGER ) )
    {
        my $potential_port = $CONFIG_MANAGER->get_config_value( 'xid_service_port' );

        if( defined( $potential_port ) && $potential_port =~ m/^\d+$/ && $potential_port > 0 && $potential_port < 65536 )
        {
            return $potential_port;
        }
    }

    my $sth = $handle->prepare( $GET_XID_SERVICE_PORT );

    return unless( $sth && $sth->execute() );
    my $port_row = $sth->fetchrow_hashref();
    my $port = $port_row->{port};
    return $port;
}

# Note: This routine returns 1 if we /should/ start the XID service,
#  IE the DB lock is missing
sub xid_service_check($) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    return 0 unless( $ENABLE_FAST_DELETE );
    my $lock_check_sth = $handle->prepare( $XID_SERVICE_CHECK );

    return 0 unless( defined( $lock_check_sth ) );
    #$lock_check_sth->bind_param( 1, $PGCTBLMGR_XID_MAGIC_1 );
    #$lock_check_sth->bind_param( 2, $PGCTBLMGR_XID_MAGIC_2 );
    return 0 unless( $lock_check_sth->execute() );
    return 0 unless( $lock_check_sth->rows() == 0 );

    $lock_check_sth->finish();
    return 1;
}

sub xid_service_lock($) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $lock_sth = $handle->prepare( $XID_SERVICE_LOCK );

    return 0 unless( defined( $lock_sth ) );
    #$lock_sth->bind_param( 1, $PGCTBLMGR_XID_MAGIC_1 );
    #$lock_sth->bind_param( 2, $PGCTBLMGR_XID_MAGIC_2 );
    return 0 unless( $lock_sth->execute() );
    return 0 unless( $lock_sth->rows() > 0 );

    my $lock_row = $lock_sth->fetchrow_hashref();
    my $locked   = $lock_row->{locked};
    $lock_sth->finish();

    unless( $locked && ( $locked =~ m/t/i || $locked =~ m/1/ ) )
    {
        return 0;
    }

    return 1;
}

sub try_lock($) :Export( :MANDATORY )
{
    #NOTE: Using try_query here will lead to deep recursion
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    return 1 if( defined $SKIP_LOCK_CHECK && $SKIP_LOCK_CHECK );
    my $sth = $handle->prepare( $CHECK_WORKER_LOCK );
    return 0 unless( $sth );

    $sth->bind_param( 1, $LOCAL_PK_MAINTENANCE_OBJECT );
    return 0 unless( $sth->execute() );

    if( $sth->rows() > 0 )
    {
        $sth->finish();
        return 1;
    }

    $sth->finish();

    $sth = $handle->prepare( $GET_LOCK );

    return 0 unless( $sth );
    $sth->bind_param( 1, $LOCAL_PK_MAINTENANCE_OBJECT );
    return 0 unless( $sth->execute() );
    return 0 if( $sth->rows() <= 0 );

    my $row = $sth->fetchrow_hashref();
    my $locked = $row->{locked};

    $sth->finish();

    if( $locked && ( $locked =~ m/t/i || $locked =~ m/1/ ) )
    {
        return 1;
    }

    return 0;
}

sub _get_create_cache_table_query
{
    my ( $batched_create ) = validate_pos(
        @_,
        { type => SCALAR },
    );

    my $create_cache_table_query;

    if( $batched_create )
    {
        $create_cache_table_query = <<END_SQL;
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
        $create_cache_table_query = <<END_SQL;
        CREATE TABLE IF NOT EXISTS __TABLE__ AS
        (
            __DEFINITION__
        );
END_SQL
    }

    return $create_cache_table_query;
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
        my $row                            = $ct_sth->fetchrow_hashref();
        $cache_hash->{schema}              = $row->{namespace};
        $cache_hash->{driver}              = $row->{driver_name};
        $cache_hash->{name}                = $row->{name};
        $cache_hash->{definition}          = $row->{definition};
        $cache_hash->{filter_tables}       = $row->{filter};
        $cache_hash->{indexes}             = $row->{indexes};
        $cache_hash->{unique_index}        = $row->{unique_index};
        $cache_hash->{maintenance_object}  = $pk_maintenance_object;
        $cache_hash->{maintenance_channel} = $row->{maintenance_channel};
        $cache_hash->{not_null_unique}     = [];
        $cache_hash->{not_null_unique}     = $row->{not_null_unique} if( $row->{not_null_unique} );
        $cache_hash->{null_unique}         = [];

        $ct_sth->finish();

        foreach my $unique_column( @{$row->{unique_index}} )
        {
            unless( grep /^$unique_column$/, @{$row->{not_null_unique}} )
            {
                push( @{$cache_hash->{null_unique}}, $unique_column );
            }
        }

        if( $cache_hash->{null_unique} )
        {
            @{$cache_hash->{null_unique}} = sort { $a cmp $b } @{$cache_hash->{null_unique}};
        }

        if( $cache_hash->{not_null_unique} )
        {
            @{$cache_hash->{not_null_unique}} = sort { $a cmp $b } @{$cache_hash->{not_null_unique}};
        }

        if( $cache_hash->{unique_index} )
        {
            @{$cache_hash->{unique_index}} = sort { $a cmp $b } @{$cache_hash->{unique_index}};
        }

        $cache_hash->{digest} = get_ct_digest(
            $handle,
            $pk_maintenance_object
        );

        $cache_hash->{index_digest} = &get_ct_index_digest(
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

        $ct_sth = &try_query( $handle, $GET_TABLE_COLUMNS_DATATYPES, [ $cache_hash->{schema}, $cache_hash->{name} ] );
        while( $row = $ct_sth->fetchrow_hashref() )
        {
            $cache_hash->{columns}->{$row->{column_name}} = $row->{datatype};
        }

        $ct_sth->finish();
        return 1;
    }

    return 0;
}

sub populate_typmods($$) :Export( :MANDATORY )
{
    my( $handle, $filter_tables ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => ARRAYREF },
    );

    my $result_typmods = {};
    foreach my $filter_table( @$filter_tables )
    {
        my $sth = try_query( $handle, $FETCH_TYPMODS_QUERY, [ $filter_table ] );

        unless( $sth )
        {
            _log( $LOG_LEVEL_ERROR, "Unable to get typmods for '$filter_table'" );
        }

        while( my $row = $sth->fetchrow_hashref )
        {
            $result_typmods->{$filter_table}->{$row->{column}} = $row->{type};
        }
    }

    return $result_typmods;
}

sub update_filter_tables($$$) :Export( :MANDATORY )
{
    my( $handle, $new_filter_tables, $pk_maintenance_object ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => ARRAYREF },
        { type => SCALAR },
    );

    unless( try_query( $handle, $UPDATE_FILTERS, [ $new_filter_tables, $pk_maintenance_object ] ) )
    {
        return;
    }

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

    $handle->do( "SET client_min_messages = 'ERROR'" );
    $handle->do( "DROP TABLE IF EXISTS tt_dependent_objects" );
    $handle->do( "SET client_min_messages TO DEFAULT" );
    my $sth = &try_query( $handle, $CREATE_CT_DEPENDENT_OBJECT_TT );

    unless( $sth )
    {
        _log( $LOG_LEVEL_ERROR, 'Failed to create dependency temp table' );
        return 0;
    }

    $sth->finish();

    $sth = &try_query(
        $handle,
        $GET_DEPENDENT_VIEWS,
        [ $ct_hash->{schema}, $ct_hash->{name} ]
    );

    unless( $sth )
    {
        _log(
            $LOG_LEVEL_ERROR,
            "Failed to get view dependencies for $ct_hash->{name}"
        );
        return 0;
    }

    $sth->finish();
    $sth = &try_query(
        $handle,
        $GET_FK_DEPENDENCIES,
        [ $ct_hash->{schema}, $ct_hash->{name} ]
    );

    unless( $sth )
    {
        _log(
            $LOG_LEVEL_ERROR,
            "Failed to get fk-dependent objects for $ct_hash->{name}"
        );
        return 0;
    }

    $sth->finish();
    $sth = &try_query(
        $handle,
        $GET_DEPENDENT_CHECK_CONSTRAINTS,
        [ $ct_hash->{schema}, $ct_hash->{name} ]
    );

    unless( $sth )
    {
        _log(
            $LOG_LEVEL_ERROR,
            "Failed to get dependent check constraints on $ct_hash->{name}"
        );
        return 0;
    }

    $sth->finish();
    $sth = &try_query(
        $handle,
        $GET_DEPENDENT_TRIGGERS,
        [ $ct_hash->{schema}, $ct_hash->{name} ]
    );

    unless( $sth )
    {
        _log(
            $LOG_LEVEL_ERROR,
            "Failed to get dependent triggers for $ct_hash->{name}"
        );
        return 0;
    }

    $sth->finish();
    $sth = &try_query( $handle, $GET_DEPENDENT_INDEXES );

    unless( $sth )
    {
        _log(
            $LOG_LEVEL_ERROR,
            "Failed to get dependent triggers for $ct_hash->{name}"
        );
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
            _log(
                $LOG_LEVEL_ERROR,
                "Failed to drop dependent object $row->{object_name}"
            );
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
        _log(
            $LOG_LEVEL_ERROR,
            'Failed to get create results from dependency table'
        );
        return 0;
    }

    while( my $row = $sth->fetchrow_hashref() )
    {
        unless( $handle->do( $row->{create_statement} ) )
        {
            _log(
                $LOG_LEVEL_ERROR,
                "Failed to recreate dependent object $row->{object_name}"
            );
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

    $handle->do( 'DROP TABLE  IF EXISTS tt_dependent_objects' );

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

    $handle->do( "SET application_name = 'Replace $ct_hash->{name}'" );

    my $name          = $ct_hash->{name};
    my $schema        = $ct_hash->{schema};
    $handle->do( 'BEGIN' );
    $handle->do( "SET client_min_messages = 'ERROR'" );

    until( &create_dependent_temp_table( $handle, $ct_hash ) )
    {
        $handle->do( 'ROLLBACK' );
        _log(
            $LOG_LEVEL_ERROR,
            'Cache table replacement failed - '
          . 'could not collect dependent objects'
        );
        $handle->do( 'BEGIN' );
        $handle->do( "SET client_min_messages = 'ERROR'" );
    }

    $ct_hash->{name} .= '_temp';
    my $temp_name     = $ct_hash->{name};

    # note: This statement is intentionally not set to cascade - it's a safety
    # mechanism in case we did not drop dependent objects.
    $handle->do( "DROP TABLE IF EXISTS $temp_name" );

    &create_cache_table( $handle, $ct_hash );

    unless( &drop_dependencies( $handle ) )
    {
        $handle->do( 'ROLLBACK' );
        _log( $LOG_LEVEL_ERROR, "Failed to drop dependent objects" );
        return 0;
    }

    my $sth = &try_query(
        $handle,
        "DROP TABLE IF EXISTS $schema.$name CASCADE"
    );

    unless( $sth )
    {
        $handle->do( 'ROLLBACK' );
        _log(
            $LOG_LEVEL_ERROR,
            'Cache table replacement failed - could not drop old definition'
        );
        return 0;
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
            $LOG_LEVEL_ERROR,
            'Cache table replacement failed - could not rename new table'
        );
        return 0;
    }

    unless( $handle->do( "ANALYZE $schema.$name" ) )
    {
        $handle->do( 'ROLLBACK' );
        _log( $LOG_LEVEL_ERROR, "Failed to analyze replacement cache table" );
        return 0;
    }

    unless( &recreate_dependencies( $handle ) )
    {
        $handle->do( 'ROLLBACK' );
        _log( $LOG_LEVEL_ERROR, "Failed to recreate dependencies\n" );
        return 0;
    }

    unless(
            $handle->do(
                "ALTER INDEX IF EXISTS ix_$temp_name RENAME TO ix_$name"
            )
          )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to rename index for $name" );
    }

    unless(
            $handle->do(
                "ALTER INDEX IF EXISTS ix_null_$temp_name RENAME TO ix_null_$name"
            )
          )
    {
        _log( $LOG_LEVEL_ERROR, "Faild to rename nullable unique index for $name" );
    }
    &drop_dependency_temp_table( $handle );
    $ct_hash->{name} = $name;
    &rename_cache_table_indexes( $handle, $ct_hash );
    $handle->do( 'SET client_min_messages TO DEFAULT' );
    $handle->do( 'COMMIT' );

    _log(
        $LOG_LEVEL_DEBUG,
        "Cache table $name has been successfully replaced"
    );
    return 1;
}

sub db_connect(;$$) :Export( :MANDATORY )
{
    my( $handle, $is_aged ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF, optional => 1 },
        { type => SCALAR | UNDEF, optional => 1 },
    );

    # Fast conn check & ret
    if( defined( $handle ) && $handle->ping() > 0 && $handle->pg_ping() > 0 )
    {
        return $handle;
    }
    else
    {
        _log( $LOG_LEVEL_DEBUG, "P: " . $handle->ping() . " PGP: " . $handle->pg_ping() ) if( defined( $handle ) );
        if( defined( $handle ) )
        {
            $handle->disconnect();
            undef( $handle );
        }

        # Cleanup pid's globals to avoid any leaks via orphaned objs
        if( $PROCESS_ID != $PARENT_PID && !defined( $is_aged ) )
        {
            if( defined( $FILE_DESCRIPTOR ) && fileno( $FILE_DESCRIPTOR ) )
            {
                close( $FILE_DESCRIPTOR );
                undef( $FILE_DESCRIPTOR );
            }

            if( $SELECTOR )
            {
                undef( $SELECTOR );
            }
        }

        if(
              !defined( $CONNECTION_MAP )
           || !defined( $CONNECTION_MAP->{connection_string} )
           || !defined( $CONNECTION_MAP->{user_name} )
          )
        {
            return undef;
        }

        my $old_warn = local $SIG{__WARN__};
        local $SIG{__WARN__} = sub { };

        eval {
            $handle = DBI->connect(
                $CONNECTION_MAP->{connection_string},
                $CONNECTION_MAP->{user_name},
                undef
            );
        };

        if( $EVAL_ERROR || !defined( $handle ) )
        {
            _log(
                $LOG_LEVEL_ERROR,
                "Failed to connect to database '$CONNECTION_MAP->{connection_string}'"
            );
        }

        local $SIG{__WARN__} = $old_warn;
    }

    # We're hitting this section iff initial connection does not succeed
    my $connect_count = 0;
    my $sleep_backoff = 1;

    until( defined( $handle ) && $handle->ping() > 0 && $handle->pg_ping() > 0 )
    {
        if( defined $CONFIG_MANAGER && $CONFIG_MANAGER->get_config_value( 'debug' ) )
        {
            _log( $LOG_LEVEL_INFO, 'Not connected to DB, reconnecting...' );
        }

        $handle->disconnect() if( defined( $handle ) );
        undef( $handle );

        $connect_count++;
        sleep( $sleep_backoff );
        $sleep_backoff += int( rand( 2 ** $connect_count - 1 ) );

        my $old_warn = local $SIG{__WARN__};
        local $SIG{__WARN__} = sub { };

        eval {
            $handle = DBI->connect(
                $CONNECTION_MAP->{connection_string},
                $CONNECTION_MAP->{user_name},
                undef
            );
        };

        if( $EVAL_ERROR || !defined( $handle ) )
        {
            _log(
                $LOG_LEVEL_ERROR,
                "Failed to connect to database '$CONNECTION_MAP->{connection_string}'"
            );
        }

        local $SIG{__WARN__} = $old_warn;

        if( $connect_count > 5 )
        {
            # is our database still here? is the server down??
            return undef;
        }

        if( $connect_count == 3 )
        {
            my $old_warn = local $SIG{__WARN__};
            local $SIG{__WARN__} = sub { };
            # Sanity check - does the DB exist?
            my $pg_handle;

            eval {
                $pg_handle = DBI->connect(
                    $CONNECTION_MAP->{pg_connection_string},
                    $CONNECTION_MAP->{user_name},
                    undef
                );
            };

            if( $EVAL_ERROR || !defined( $pg_handle ) )
            {
                _log(
                    $LOG_LEVEL_ERROR,
                    "Failed to connect to database '$CONNECTION_MAP->{pg_connection_string}'"
                );
            }

            local $SIG{__WARN__} = $old_warn;

            next unless( $pg_handle );
            my $check_sth = $pg_handle->prepare( 'SELECT datallowconn FROM pg_database WHERE datname = ?' );
            next unless( $check_sth );
            $check_sth->bind_param( 1, $CONNECTION_MAP->{dbname} );
            next unless( $check_sth->execute() );

            if( $check_sth->rows() == 0 )
            {
                _log(
                    $LOG_LEVEL_FATAL,
                    "Database '$CONNECTION_MAP->{dbname}' does not exist! "
                  . 'Check your connection settings and restart the service'
                );
            }

            my $allowconn_row = $check_sth->fetchrow_hashref();
            my $allowconn = $allowconn_row->{datallowconn};

            unless( $allowconn )
            {
                _log(
                    $LOG_LEVEL_ERROR,
                    "Database '$CONNECTION_MAP->{dbname}' is not accepting connections. "
                  . 'The service will sleep for 15 minutes and retry its connection.'
                );
                $connect_count = 0;
                sleep( 54000 );
            }
        }
    }

    my $tcp_keepalives_idle     = defined( $CONFIG_MANAGER ) ? $CONFIG_MANAGER->get_config_value( 'db_conn_tcp_keepalives_idle' ) : $DEFAULT_TCP_KEEPALIVES_IDLE;
    my $tcp_keepalives_interval = defined( $CONFIG_MANAGER ) ? $CONFIG_MANAGER->get_config_value( 'db_conn_tcp_keepalives_interval' ) : $DEFAULT_TCP_KEEPALIVES_INTERVAL;
    my $tcp_user_timeout        = defined( $CONFIG_MANAGER ) ? $CONFIG_MANAGER->get_config_value( 'db_conn_tcp_user_timeout' ) : $DEFAULT_TCP_USER_TIMEOUT;

	_log( $LOG_LEVEL_INFO, 'Reconnected to database' ) if( $connect_count > 0 );
	$handle->do( "SET tcp_keepalives_idle = ?", undef, $tcp_keepalives_idle );
	$handle->do( "SET tcp_keepalives_interval = ?", undef, $tcp_keepalives_interval );
	$handle->do( "SET tcp_user_timeout = ?", undef, $tcp_user_timeout );
    $handle->{PrintError} = 1;
#    $handle->do( "SET client_min_messages = 'DEBUG1'" ) if( $CONFIG_MANAGER->get_config_value( 'debug' ) );

    if( !defined( $is_aged ) && $PROCESS_ID != $PARENT_PID )
    {
        &do_listen( $handle );
        my $row = $handle->selectrow_hashref( 'SELECT pg_backend_pid() AS pid' );
        $BACKEND_PID = $row->{pid};

        unless( &try_lock( $handle ) )
        {
            _log(
                $LOG_LEVEL_FATAL,
                'There seems to be another worker using maintenance object '
              . $LOCAL_PK_MAINTENANCE_OBJECT . ' on this database'
            );
        }
    }

    return $handle;
}

sub do_listen($) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    if( $PROCESS_ID != $PARENT_PID && length( $MAINTENANCE_CHANNEL ) > 0 )
    {
        if(
                defined( $SELECTOR )
             || defined( $FILE_DESCRIPTOR )
          )
        {
            print( "Cleaning up selector\n" );
            $SELECTOR->remove( $FILE_DESCRIPTOR ) if( defined( $SELECTOR ) );
            undef( $SELECTOR );
            if(
                    defined( $FILE_DESCRIPTOR )
                 && defined( fileno( $FILE_DESCRIPTOR ) )
                 && fileno( $FILE_DESCRIPTOR ) >= 0
              )
            {
                print( "Closing old FD\n" );
                close( $FILE_DESCRIPTOR );
            }
            undef( $FILE_DESCRIPTOR );
        }

        $handle->do( "LISTEN $MAINTENANCE_CHANNEL" );

        $FILE_DESCRIPTOR = $handle->func( 'getfd' );
        $SELECTOR        = IO::Select->new( $FILE_DESCRIPTOR );
        _log( $LOG_LEVEL_INFO, "Worker $PROCESS_ID listening on $MAINTENANCE_CHANNEL" );
    }

    return;
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
    my $max_query_retries = defined( $CONFIG_MANAGER ) ? $CONFIG_MANAGER->get_config_value( 'max_query_retries' ) : $DEFAULT_MAX_QUERY_RETRIES;

    #_log( $LOG_LEVEL_DEBUG, "Executing '$query'" );
    RETRY_CONN:
    $retry_counter++;
    return undef if( $retry_counter > $max_query_retries );

    $handle = &db_connect( $handle );

    # We're connected to the DB at this point
    if( $PARENT_PID == $PROCESS_ID )
    {
        unless( check_extension_running( $handle ) )
        {
            $SKIP_SHM_CLEANUP = 1;
            _log(
                $LOG_LEVEL_FATAL,
                'Failed to acquire lock after reconnecting to database or '
              . 'extension not installed'
            );
        }
    }
    else
    {
        unless( &try_lock( $handle ) )
        {
            _log(
                $LOG_LEVEL_FATAL,
                'There seems to be another worker using maintenance object '
              . $LOCAL_PK_MAINTENANCE_OBJECT . ' on this database'
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
        if( defined $CONFIG_MANAGER && $CONFIG_MANAGER->get_config_value( 'debug' ) )
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

        return undef if( $try_count >= $max_query_retries );
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

sub get_ct_index_digest($$) :Export( :MANDATORY )
{
    my( $handle, $pk_maintenance_object ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
    );

    my $sth = try_query( $handle, $GET_CT_INDEX_SHA, [ $pk_maintenance_object ] );

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

sub check_extension_running($) :Export( :MANDATORY )
{
    # This subroutine needs to use DBI methods to avoid deep recursion
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $sth = $handle->prepare( $CHECK_PARENT_LOCK );
    return 0 unless( $sth );
    return 0 unless( $sth->execute() );
    return 1 if( $sth->rows() > 0 );
    $sth->finish();
    $sth = $handle->prepare(
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
            push(
                @$worker_data,
                {
                    maintenance_channel => $row->{maintenance_channel},
                    filter_tables       => $row->{filter},
                    wal_level           => $row->{wal_level},
                    maintenance_object  => $row->{maintenance_object},
                    name                => $row->{name},
                    hash                => $row->{hash},
                }
            );
        }

        $sth->finish();
        return $worker_data;
    }

    $sth->finish();
    return undef;
}

sub check_indexes($$) :Export( :MANDATORY )
{
    my( $handle, $pk_maintenance_object ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
    );

    my $sth = $handle->prepare( $MANAGE_INDEXES );

    return unless( $sth );

    $sth->bind_param( 1, $pk_maintenance_object );

    return unless( $sth->execute() );
    return unless( $sth->rows() > 0 );

    while( my $row = $sth->fetchrow_hashref() )
    {
        my $statement = $row->{statement};
        my $indname   = $row->{index_name};
        _log( $LOG_LEVEL_DEBUG, "Modifying index '$indname'" );

        unless( $handle->do( $statement ) )
        {
            _log( $LOG_LEVEL_ERROR, "Failed to create supplimental index '$indname'" );
        }
    }

    return;
}

sub check_ct_exists($$;$) :Export( :MANDATORY )
{
    my( $handle, $ct_hash, $short_validation ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => HASHREF },
        { type => SCALAR | UNDEF, optional => 1 },
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
        _log(
            $LOG_LEVEL_ERROR,
            "Failed to verify that $ct_hash->{schema}.$ct_hash->{name} exists"
        );
    }

    if( $sth && $sth->rows() > 0 )
    {
        $sth->finish();
        return if( defined $short_validation && $short_validation );
        _log(
            $LOG_LEVEL_DEBUG,
            "Cache Table $ct_hash->{schema}.$ct_hash->{name} already exists"
        );

        my $create_tt = $CREATE_COLUMN_CHECK_TABLE;
        $handle->do( "SET client_min_messages = 'ERROR'" );
        $handle->do( "DROP TABLE IF EXISTS tt_column_verify" );
        $handle->do( "SET client_min_messages TO DEFAULT" );
        $create_tt =~ s/__DEFINITION__/$ct_hash->{definition}/;
        $sth = try_query(
            $handle,
            $create_tt
        );

        unless( $sth )
        {
            _log(
                $LOG_LEVEL_FATAL,
                'Failed to create test table using definition to validate '
              . 'columns'
            );
        }

        $sth->finish();

        $sth = try_query(
            $handle,
            $CHECK_CACHE_TABLE_COLUMNS,
            [ $ct_hash->{schema}, $ct_hash->{name} ]
        );

        unless( $sth )
        {
            _log(
                $LOG_LEVEL_FATAL,
                'Failed to check cache table columns against definition'
            );
        }

        $handle->do( 'DROP TABLE IF EXISTS tt_column_verify' );

        return if( $sth->rows() == 0 );

        _log(
            $LOG_LEVEL_ERROR,
            'There is a discrepency between the existing cache table and its '
          . 'definition. The cache table will be rebuilt'
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

    my $batched_create = defined( $CONFIG_MANAGER ) ? $CONFIG_MANAGER->get_config_value( 'batched_create' ) : $DEFAULT_BATCHED_CREATE;

    $handle->do( "SET application_name = 'Create: $name'" );
    my $create_query = _get_create_cache_table_query( $batched_create );
    $create_query    =~ s/__TABLE__/${schema}.${name}/;
    $create_query    =~ s/__DEFINITION__/$definition/;

    my $sth = try_query( $handle, $create_query, undef );

    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to create cache table $schema.$name" );
        return;
    }

    if( $batched_create )
    {
        _log( $LOG_LEVEL_DEBUG, "Performing batch population of $name" );
        $handle->do( "SET application_name = 'Batch populate: $name'" );
        my $done            = 0;
        my $offset          = 0;
        my $populate_q      = $CREATE_POPULATE;
        my $initial_orderby = join( ',', @{$ct_hash->{unique_index}} );

        $populate_q =~ s/__TABLE__/${schema}.${name}/;
        $populate_q =~ s/__DEFINITION__/$definition/;
        $populate_q =~ s/__ORDERBY__/$initial_orderby/;

        my $batch_size = defined( $CONFIG_MANAGER ) ? $CONFIG_MANAGER->get_config_value( 'batch_size' ) : $DEFAULT_BATCH_SIZE;

        $populate_q =~ s/__LIMIT__/$batch_size/;

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
            $offset += $batch_size;
        }
    }

    unless( &create_cache_table_unique( $handle, $ct_hash ) )
    {
        _log( $LOG_LEVEL_ERROR, 'Failed to create cache table unique index' );
    }

    $sth->finish();
    $handle->do( "ANALYZE $schema.$name" );

    foreach my $columns( @{$ct_hash->{indexes}} )
    {
        my $index_name = $columns;
        $index_name =~ s/[^[:alnum:]]/_/g;
        my $full_index_name = "ix_$ct_hash->{name}_$index_name";

        my $def = "CREATE INDEX $full_index_name "
                . "ON $ct_hash->{schema}.$ct_hash->{name}( $columns )";

        unless( $handle->do( $def ) )
        {
            _log(
                $LOG_LEVEL_ERROR,
                "Failure when creating index for columns ($columns) on "
              . "$ct_hash->{schema}.$ct_hash->{name}"
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

        my $def = "ALTER INDEX IF EXISTS $temp_index_name "
                . "RENAME TO $new_index_name";

        unless( $handle->do( $def ) )
        {
            _log(
                $LOG_LEVEL_ERROR,
                'Failed to rename index - this may cause issues the next time '
              . 'a table is redefined'
            );
        }
    }

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
    my $sth           = try_query(
        $handle,
        "CREATE UNIQUE INDEX IF NOT EXISTS ix_$ct_hash->{name} "
      . "ON $ct_hash->{schema}.\"$ct_hash->{name}\"( $index_columns )"
    );

    return 0 unless( $sth );
    $sth->finish();

    if( $ct_hash->{null_unique} && scalar( @{$ct_hash->{null_unique}} ) > 0 )
    {
        $index_columns = join( ',', @{$ct_hash->{null_unique}} );
        $sth = try_query(
            $handle,
            "CREATE INDEX IF NOT EXISTS ix_null_$ct_hash->{name} "
          . "ON $ct_hash->{schema}.\"$ct_hash->{name}\"( $index_columns )"
        );

        $sth->finish() if( $sth );
    }
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
    unless( $sth->execute() )
    {
        _log( $LOG_LEVEL_ERROR, "Failed to exec: $test_query\n" );
        return 0;
    }

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
    my $temp_table_name = 'tt_' . $ct_hash->{name};
    _log( $LOG_LEVEL_DEBUG, "Generating TT $temp_table_name" );
    my $tt_query        = "CREATE TEMP TABLE $temp_table_name AS( $query );";
    print "$tt_query\n" if( $PRINT_QUERIES );
    $handle->do( "SET client_min_messages = 'ERROR'" );
    $handle->do( "DROP TABLE IF EXISTS $temp_table_name" );
    $handle->do( "SET client_min_messages TO DEFAULT" );

    my $sth  = try_query( $handle, $tt_query );
    return unless( $sth );
    $sth->finish();

    my $tt_count = get_table_count( $handle, $temp_table_name );
    return undef if( $tt_count < 0 );

    my $return_data = {
        count => $tt_count,
        name  => $temp_table_name,
        index => "ix_$temp_table_name"
    };
    my $uniques = join( ',', @{$ct_hash->{unique_index}} );

    $sth = try_query(
        $handle,
        "CREATE UNIQUE INDEX ix_$temp_table_name "
      . "ON $temp_table_name( $uniques )"
    );

    unless( $sth )
    {
        _log(
            $LOG_LEVEL_WARNING,
            'Failed to create unique index on comparrison table. '
          . 'Please verify the cardinality of this index provided!'
        );
    }

    $sth->finish();

    if( $ct_hash->{null_unique} && scalar( @{$ct_hash->{null_unique}} ) > 0 )
    {
        $sth = try_query(
            $handle,
            "CREATE INDEX ix_null_${temp_table_name} ON $temp_table_name( " . join( ',', @{$ct_hash->{null_unique}} ) . ' )'
        );

        if( $sth )
        {
            $sth->finish();
        }
        else
        {
            _log( $LOG_LEVEL_WARNING, "Failed to generate complementary null index on ${temp_table_name}" );
        }
    }

    return $return_data;
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

sub generate_update_statement($$$$$) :Export( :MANDATORY )
{
    my(
        $handle,
        $temp_table,
        $query,
        $cache_hash,
        $bulk_action_cutoff,
      ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => HASHREF },
        { type => SCALAR },
        { type => HASHREF },
        { type => SCALAR },
    );

    my $cache_table_schema = $cache_hash->{schema};
    my $cache_table_name   = $cache_hash->{name};
    my $table_columns      = $cache_hash->{cache_table_columns};

    my $uniques         = $cache_hash->{unique_index};
    my $not_null_unique = $cache_hash->{not_null_unique};
    my $null_unique     = $cache_hash->{null_unique};

    $handle = &db_connect( $handle );
    $handle->do( "SET application_name = 'Update: $cache_table_name'" );
    my $join_clauses       = [];
    my $where_clauses      = [];
    my $distinct_uniques   = [];
    my $non_unique_columns = [];

    push( @$join_clauses, map { "tt.$_ = vw.$_" } @$not_null_unique );
    push( @$join_clauses, map { "( ( tt.$_ = vw.$_ ) OR ( tt.$_ IS NULL AND vw.$_ IS NULL ) )" } @$null_unique );
    push( @$where_clauses, map { "ct.$_ = tt.$_" } @$not_null_unique );
    push( @$where_clauses, map { "( ( ct.$_ = tt.$_ ) OR ( ct.$_ IS NULL AND tt.$_ IS NULL ) )" } @$null_unique );

    foreach my $column_name( @$table_columns )
    {
        next if( grep( /^$column_name$/, @$uniques ) );
        push( @$non_unique_columns, $column_name );
    }

    my $tt;
    my $materialized_cte = '';

    if( $NO_TEMP_TABLES )
    {
        $tt = "( $query )";
    }
    else
    {
        # use of a temp table may have been dictated elsewhere
        $tt = $temp_table->{name};
    }

    if( $FORCE_MATERIALIZE && ( $NO_TEMP_TABLES || defined( $temp_table->{virtual} ) ) )
    {
        # if we definitely aren't using a temp table, use a materialized CTE to
        # stabilize the query runtimes
        $tt               = 'tt_update';
        $materialized_cte = "WITH $tt AS MATERIALIZED ( $query )"
    }

    if( $temp_table->{count} > $bulk_action_cutoff )
    {
        my $columns = join( ',', @$table_columns );
        _log(
            $LOG_LEVEL_DEBUG,
            "Performing large update optimization ($temp_table->{count} "
          . ' possible rows)'
        );
        $handle->do( 'BEGIN' );
        #$handle->do( "DROP INDEX IF EXISTS ix_$cache_hash->{name}" );
        my $delete_where = join( ' AND ', @$where_clauses );
        my $DELETE_Q = <<END_SQL;
        DELETE FROM $cache_table_schema.$cache_table_name ct
              USING $tt tt
              WHERE $delete_where
END_SQL
        print "======================== BULK UPDATE - DELETE QUERY ===========================\n" if( $PRINT_QUERIES );
        print "$DELETE_Q\n" if( $PRINT_QUERIES );
        unless( &try_query( $handle, $DELETE_Q, [] ) )
        {
            _log(
                $LOG_LEVEL_ERROR,
                'Failed to bulk delete rows for fast update'
            );
            $handle->do( 'ROLLBACK' );
            return 0;
        }

        my $INSERT_Q = <<END_SQL;
        INSERT INTO $cache_table_schema.$cache_table_name
                    (
                        $columns
                    )
             SELECT $columns
               FROM $tt
END_SQL
        print "======================== BULK UPDATE - INSERT QUERY ===========================\n" if( $PRINT_QUERIES );
        print "$INSERT_Q\n" if( $PRINT_QUERIES );
        unless( &try_query( $handle, $INSERT_Q ) )
        {
            $handle->do( 'ROLLBACK' );
            _log(
                $LOG_LEVEL_ERROR,
                'failed to bulk insert rows for fast update'
            );
            return 0
        }

        #unless( create_cache_table_unique( $handle, $cache_hash ) )
        #{
        #    $handle->do( 'ROLLBACK' );
        #    _log( $LOG_LEVEL_ERROR, "Failed to recreate unique index" );
        #    return 0;
        #}
        $handle->do( 'COMMIT' );
        $handle->do( "ANALYZE $cache_table_schema.$cache_table_name" );
    }
    else
    {
        my $update_fragment = join(
            ', ',
            map { "$_ = tt.$_" } @$non_unique_columns
        );
        my $where_clause    = join( ' AND ', @$where_clauses );
        my $diff_distinct   = '( '
                            . join(
                                ' OR ',
                                map {
                                    "ct.$_ IS DISTINCT FROM tt.$_"
                                } @$non_unique_columns
                             )
                            . ' )';

        my $UPDATE_Q        = <<END_SQL;
        $materialized_cte
        UPDATE $cache_table_schema.$cache_table_name ct
           SET $update_fragment
          FROM $tt tt
         WHERE $where_clause
           AND $diff_distinct
END_SQL
        print "======================== UPDATE QUERY ===========================\n" if( $PRINT_QUERIES );
        print "$UPDATE_Q\n" if( $PRINT_QUERIES );
        my $sth = &try_query( $handle, $UPDATE_Q, [] );

        return 0 unless( $sth );

        $sth->finish();
    }

    return 1;
}

sub generate_insert_statement($$$$) :Export( :MANDATORY )
{
    my(
        $handle,
        $temp_table,
        $query,
        $cache_hash
      ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => HASHREF },
        { type => SCALAR },
        { type => HASHREF },
    );

    my $cache_table_schema = $cache_hash->{schema};
    my $cache_table_name   = $cache_hash->{name};
    my $table_columns      = $cache_hash->{cache_table_columns};

    my $uniques         = $cache_hash->{unique_index};
    my $not_null_unique = $cache_hash->{not_null_unique};
    my $null_unique     = $cache_hash->{null_unique};

    $handle = &db_connect( $handle );
    $handle->do( "SET application_name = 'Insert: $cache_table_name'" );

    my $join_clauses  = [];
    my $where_clauses = [];
    my $join_clause;
    my $where_clause_elem;

    push( @$join_clauses, map { "tt.$_ = vw.$_" } @$not_null_unique );
    push( @$join_clauses, map { "( ( tt.$_ = vw.$_ ) OR ( tt.$_ IS NULL AND vw.$_ IS NULL ) )" } @$null_unique );
    push( @$where_clauses, map { "tt.$_ IS NULL" } @$uniques );

    my $ins_columns      = join( ', ', @$table_columns );
    my $columns          = join( ', ', map { "vw.$_" } @$table_columns );
    my $join_predicate   = join( ' AND ', @$join_clauses );
    my $where_clause     = join( ' AND ', @$where_clauses );
    my $tt               = '';
    my $materialized_cte = '';
    my $materialize      = '';
    $materialize         = 'MATERIALIZED' if( $FORCE_MATERIALIZE );

    if( $NO_TEMP_TABLES )
    {
        $tt = "( $query )";
    }
    else
    {
        $tt = $temp_table->{name};
    }

    if( $FORCE_MATERIALIZE && ( $NO_TEMP_TABLES || defined( $temp_table->{virtual} ) ) )
    {
        # if we definitely aren't using a temp table, use a materialized CTE to
        # stabilize the query runtimes
        $tt               = 'tt_insert';
        $materialized_cte = "WITH $tt AS MATERIALIZED ( $query )"
    }

    my $INSERT_Q = <<END_SQL;
    WITH tt_records_to_insert AS $materialize
    (
        $materialized_cte
        SELECT $columns
          FROM $tt vw
     LEFT JOIN $cache_table_schema.$cache_table_name tt
            ON $join_predicate
         WHERE $where_clause
    )
    INSERT INTO $cache_table_schema.$cache_table_name ( $ins_columns )
         SELECT $columns
           FROM tt_records_to_insert vw
END_SQL
    print "======================== INSERT QUERY ===========================\n" if( $PRINT_QUERIES );
    print "$INSERT_Q\n" if( $PRINT_QUERIES );
    my $sth = &try_query( $handle, $INSERT_Q, [] );

    return 0 unless( $sth );

    $sth->finish();
    return 1;
}

sub get_change_volume($$$) :Export( :MANDATORY )
{
    my( $handle, $query, $cache_hash ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => HASHREF },
    );

    my $materialize = '';
    $materialize    = 'MATERIALIZED' if( $FORCE_MATERIALIZE );

    my $q = <<"END_SQL";
    WITH tt_count AS $materialize
    (
        $query
    )
        SELECT COUNT( * ) AS change_volume
          FROM tt_count
END_SQL

    my $sth = &try_query( $handle, $q, [] );

    return 0 unless( $sth );
    my $count_row = $sth->fetchrow_hashref();

    my $count = $count_row->{change_volume};
    $sth->finish();
    return $count;
}

sub fast_forward_aged_data($$$$$$) :Export( :MANDATORY )
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
    my $uniques            = $cache_hash->{cache_table_uniques};

    $current_handle->do(
        "SET application_name = 'Fast delete: $cache_table_name'"
    );
    $aged_handle->do(
        "SET application_name = 'Lookback: $cache_table_name'"
    );

    my $column_data_types = [];

    foreach my $unique_column( @{$cache_hash->{unique_index}} )
    {
        push(
            @$column_data_types,
            $unique_column . ' ' . $cache_hash->{columns}->{$unique_column}
        );
    }

    my $past_temp_table = "tt_past_data_${PROCESS_ID}";

    unless( $NO_TEMP_TABLES )
    {
        $current_handle->do( "SET client_min_messages = 'ERROR'" );
        $current_handle->do( "DROP TABLE IF EXISTS $past_temp_table" );
        $current_handle->do( "SET client_min_messages TO DEFAULT" );
        my $temp_table_q = "CREATE TEMP TABLE $past_temp_table ( "
                         . join( ',', @$column_data_types )
                         . ' )';

        unless( $current_handle->do( $temp_table_q ) )
        {
            _log( $LOG_LEVEL_ERROR, "Failed to create $past_temp_table" );
            $aged_handle->do( 'ROLLBACK' );
            $aged_handle->disconnect();
            return;
        }
    }

    my $unique_columns = join( ',', @{$cache_hash->{unique_index}} );
    my $AGED_DATA_QUERY = <<END_SQL;
        SELECT $unique_columns
          FROM $aged_temp_table->{name} x
END_SQL

    my $aged_sth = $aged_handle->prepare( $AGED_DATA_QUERY );

    unless( $aged_sth )
    {
        _log(
            $LOG_LEVEL_ERROR,
            'Failed to prepare aged data query for fast delete'
        );
        return;
    }

    unless( $aged_sth->execute() )
    {
        _log(
            $LOG_LEVEL_DEBUG,
            'Failed to execute aged data query for fast delete'
        );
        return;
    }

    my $aged_data   = [];
    my $bind_points = '?'
                    . (
                        ',?' x ( scalar( @$column_data_types ) - 1 )
                      );
    my $insert_q    = "INSERT INTO $past_temp_table( "
                    . join(
                          ',',
                          @{$cache_hash->{unique_index}}
                      )
                    . " ) VALUES ( $bind_points )";
    my $insert_sth;
    unless( $NO_TEMP_TABLES )
    {
        $insert_sth = $current_handle->prepare( $insert_q );
        unless( $insert_sth )
        {
            _log(
                $LOG_LEVEL_ERROR,
                'Failed to prepared insert statement for past data transfer'
            );
            $aged_sth->finish();
            $aged_handle->do( 'ROLLBACK' );
            $aged_handle->disconnect();
            return;
        }
    }

    my $aged_rows = $aged_sth->rows();

    if( $aged_rows == 0 )
    {
        my $conservative_fast_delete = defined( $CONFIG_MANAGER ) ? $CONFIG_MANAGER->get_config_value( 'conservative_fast_delete' ) : $DEFAULT_CONSERVATIVE_FAST_DELETE;

        if( $conservative_fast_delete )
        {
            _log( $LOG_LEVEL_DEBUG, "Insufficient data ( $aged_rows rows ) in aged handle" );
            $insert_sth->finish() if( $insert_sth );
            $aged_sth->finish();
            $aged_handle->do( 'ROLLBACK' );
            $aged_handle->disconnect();
            return;
        }
        else
        {
            _log( $LOG_LEVEL_DEBUG, "Skipping fast/slow delete, $aged_rows rows in aged set" );
            $insert_sth->finish() if( $insert_sth );
            $aged_sth->finish();
            return [];
        }
    }
    _log( $LOG_LEVEL_DEBUG, "Moving $aged_rows rows to present timeline" );
    while( my $row = $aged_sth->fetchrow_hashref() )
    {
        my $row_arr = [];
        my $index = 1;
        foreach my $unique( @{$cache_hash->{unique_index}} )
        {
            if( $NO_TEMP_TABLES )
            {
                if( defined( $row->{$unique} ) )
                {
                    push( @$row_arr, "'" . $row->{$unique} . "'::" . $cache_hash->{columns}->{$unique} );
                }
                else
                {
                    push( @$row_arr, 'NULL::' . $cache_hash->{columns}->{$unique} );
                }
            }
            else
            {
                $insert_sth->bind_param( $index, $row->{$unique} );
                $index++;
            }
        }

        if( $NO_TEMP_TABLES )
        {
            push( @$aged_data, $row_arr );
        }
        else
        {
            unless( $insert_sth->execute() )
            {
                _log(
                    $LOG_LEVEL_ERROR,
                    'Failed to insert aged data into current timeline'
                );
                return;
            }
        }
    }

    $current_handle->do( "ANALYZE $past_temp_table" ) unless( $NO_TEMP_TABLES );
    $insert_sth->finish() if( $insert_sth );
    $aged_sth->finish();
    $aged_handle->do( 'ROLLBACK' );
    $aged_handle->disconnect();
    return $aged_data;
}

sub generate_aged_delete_statement($$$$$$) :Export( :MANDATORY )
{
    my(
        $current_handle,
        $aged_temp_table,
        $current_temp_table,
        $cache_hash,
        $aged_data
      ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => HASHREF },
        { type => HASHREF },
        { type => HASHREF },
        { type => ARRAYREF },
    );

    my $definition         = $cache_hash->{definition};
    my $cache_table_schema = $cache_hash->{schema};
    my $cache_table_name   = $cache_hash->{name};

    my $uniques         = $cache_hash->{unique_index};
    my $not_null_unique = $cache_hash->{not_null_unique};
    my $null_unique     = $cache_hash->{null_unique};
    # at this point, past_temp_table contains data from a historic timeline but
    # is in the present timeline
    my $past_temp_table = "tt_past_data_${PROCESS_ID}";
    my $materialize     = '';
    $materialize        = 'MATERIALIZED' if( $FORCE_MATERIALIZE );

    if( $NO_TEMP_TABLES )
    {
        return 1 if( scalar( @$aged_data ) == 0 );

        $past_temp_table = '( VALUES ';
        my $rows = [];
        foreach my $row( @$aged_data )
        {
            push( @$rows, '( ' . join( ',', @$row ) . ' )' );
        }

        $past_temp_table .= join( ',', @$rows );
        $past_temp_table .= ') AS tt (' . join( ',', @{$cache_hash->{unique_index}} )  . ' )' ;
    }
    else
    {
        $current_handle->do( "ANALYZE $past_temp_table" );
        $current_handle->do( "ANALYZE $current_temp_table->{name}" );
    }
    my $unique_column_select = join(
        ',',
        map { "tt.$_" } @{$cache_hash->{unique_index}}
    );

    my $left_join_clauses = [];
    my $left_join_wheres  = [];
    my $ind = 0;

    my $join_clause;
    my $where_clause;

    push( @$left_join_wheres, map { "vw.$_ IS NULL" } @$uniques );
    push( @$left_join_clauses, map { "vw.$_ = tt.$_" } @$not_null_unique );
    push( @$left_join_clauses, map { "( ( vw.$_ = tt.$_ ) OR ( vw.$_ IS NULL AND tt.$_ IS NULL ) )" } @$null_unique );

    $current_handle->do( "CREATE INDEX ix_past_data ON $past_temp_table( " . join( ',', @$uniques ) . " ) " ) unless( $NO_TEMP_TABLES );
    $current_handle->do( "CREATE INDEX ix_past_data_null ON $past_temp_table( " . join( ',', @$null_unique ) . ' )' ) if( !$NO_TEMP_TABLES && scalar( @$null_unique ) > 0 );
    my $left_join_predicate  = join( ' AND ', @$left_join_clauses );
    my $left_join_where      = join( ' AND ', @$left_join_wheres );

    $past_temp_table .= ' tt' unless( $NO_TEMP_TABLES );;
    my $delete_query = <<END_SQL;
    WITH tt_rows_to_delete AS $materialize
    (
        SELECT $unique_column_select
          FROM $past_temp_table
     LEFT JOIN $current_temp_table->{name} vw
            ON $left_join_predicate
         WHERE $left_join_where
    )
        DELETE FROM $cache_table_schema.$cache_table_name vw
              USING tt_rows_to_delete tt
              WHERE $left_join_predicate
END_SQL
    print "========================= FAST DELETE QUERY ===========================\n" if( $PRINT_QUERIES );
    print "$delete_query\n" if( $PRINT_QUERIES );
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

    my $sth;
    my $definition         = $cache_hash->{definition};
    my $cache_table_schema = $cache_hash->{schema};
    my $cache_table_name   = $cache_hash->{name};

    my $null_uniques     = $cache_hash->{null_unique};
    my $not_null_uniques = $cache_hash->{not_null_unique};
    my $uniques          = $cache_hash->{unique_index};

    $handle->do( "SET application_name = 'Slow delete: $cache_table_name'" );
    my $DELETE_Q;
    my $join_preds = [];

    push( @$join_preds, map { "tt.$_ = vw.$_" } @$not_null_uniques );
    push( @$join_preds, map { "( ( tt.$_ = vw.$_ ) OR ( tt.$_ IS NULL AND vw.$_ IS NULL ) )" } @$null_uniques );

    my $tt_sel         = join( ',', map { "tt.$_" } @$uniques );
    my $vw_sel         = join( ',', map { "vw.$_" } @$uniques );
    my $join_predicate = join( ' AND ', @$join_preds );
    my $materialize    = '';
    $materialize       = 'MATERIALIZED' if( $FORCE_MATERIALIZE );

    if( scalar( @$join_preds ) > 1 )
    {
        $join_predicate = '(' . $join_predicate . ')';
    }

    $DELETE_Q = <<"END_SQL";
WITH tt_rows_to_delete AS $materialize
(
    WITH tt_del AS $materialize
    (
        $definition
    )
    SELECT $vw_sel
      FROM $cache_table_schema.$cache_table_name vw
    EXCEPT
    SELECT $tt_sel
      FROM tt_del tt
)
    DELETE FROM $cache_table_schema.$cache_table_name vw
          USING tt_rows_to_delete tt
          WHERE $join_predicate
END_SQL

    print "======================== SLOW DELETE QUERY ===========================\n" if( $PRINT_QUERIES );
    print "$DELETE_Q\n" if( $PRINT_QUERIES );
    $sth = &try_query( $handle, $DELETE_Q, [] );
    return 0 unless( $sth );
    $sth->finish();
    return 1;
}

1;
