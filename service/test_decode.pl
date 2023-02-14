#!/usr/bin/perl

use strict;
use utf8;
use warnings;

use DBI;
use JSON;
use Readonly;
use Params::Validate qw( :all );
use Data::Dumper;
use English qw( -no_match_vars );

Readonly::Scalar my $DEBUG                  => 1;
Readonly::Scalar my $CLEAN_UP               => 0; # Emergency shm cleanup
Readonly::Scalar my $LOG_LEVEL_FATAL        => 5;
Readonly::Scalar my $LOG_LEVEL_ERROR        => 4;
Readonly::Scalar my $LOG_LEVEL_WARNING      => 3;
Readonly::Scalar my $LOG_LEVEL_INFO         => 2;
Readonly::Scalar my $LOG_LEVEL_DEBUG        => 1;
Readonly::Scalar my $WORKER_STATUS_STARTUP  => 1;
Readonly::Scalar my $WORKER_STATUS_RUNNING  => 2;
Readonly::Scalar my $WORKER_STATUS_UPDATING => 3;
Readonly::Scalar my $WORKER_STATUS_EXITED   => 4;
Readonly::Scalar my $EXTENSION_NAME         => 'pg_ctblmgr';
Readonly::Scalar my $SCHEMA_NAME            => 'pgctblmgr';
Readonly::Scalar my $SQL_STATE_ADMIN_TERM   => '57P01';
Readonly::Scalar my $SQL_STATE_ADMIN_CANC   => '57014';
Readonly::Scalar my $MAX_QUERY_RETRIES      => 5;
my $PARENT_PID = $$;

Readonly::Scalar my $FILTER_TABLE_OID_CACHE => <<END_SQL;
    SELECT n.nspname::VARCHAR || '.' || c.relname::VARCHAR AS name,
           c.oid
      FROM pg_class c
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
     WHERE n.nspname::VARCHAR || '.' || c.relname::VARCHAR = ANY( ARRAY[ __BINDPOINTS__ ]::VARCHAR[] )
END_SQL

Readonly::Scalar my $GET_TEST_VIEW_PARSE_TREE => <<"END_SQL";
    SELECT ${SCHEMA_NAME}.fn_get_parse_tree( '__DEFINITION__' )::JSONB AS tree
END_SQL

my $CONNECTION_MAP->{connection_string} = 'dbi:Pg:dbname=__pgc_testing__;host=localhost;port=5432';
$CONNECTION_MAP->{user_name} = 'postgres';
my $definition = <<END_SQL;
WITH tt_foo AS
(
    SELECT a.foo
      FROM public.tb_a a
     UNION
    SELECT b.foo
      FROM public.tb_b b
     UNION
    SELECT c.foo
      FROM public.tb_c c
),
tt_bar AS
(
    WITH tt_nested AS
    (
        SELECT a.bar
          FROM tb_a a
    )
        SELECT b.bar
          FROM tt_nested tt
          JOIN tb_b b
            ON b.bar = tt.bar
),
tt_test AS
(
    SELECT e.foo
      FROM tb_e e
 INTERSECT
    SELECT d.foo
      FROM tb_d d
)
    SELECT c.baz
      FROM tb_c c
      JOIN tt_bar ttb
        ON ttb.bar = c.bar
      JOIN tt_foo ttf
        ON ttf.foo = c.foo
END_SQL
sub try_query($$;$)
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
#        unless( check_extension_running( $handle ) )
#        {
#            _log( $LOG_LEVEL_FATAL, "Failed to acquire lock after reconnecting to database" );
#        }
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

sub get_query_parsetree($$)
{
    my( $handle, $definition ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
    );

    my $get_parse_tree_query = $GET_TEST_VIEW_PARSE_TREE;
    $get_parse_tree_query =~ s/__DEFINITION__/$definition/;

    my $sth = try_query( $handle, $get_parse_tree_query, undef );

    my $defrow = $sth->fetchrow_hashref();
    my $query_tree = $defrow->{tree};
    $sth->finish();

    my $parse_tree_obj = from_json( $query_tree );
    return unless( $parse_tree_obj );

    if( ref( $parse_tree_obj ) eq 'ARRAY' )
    {
        if( scalar( @$parse_tree_obj ) == 1 )
        {
            $parse_tree_obj = $parse_tree_obj->[0];
        }
        else
        {
            warn "Multiple parse trees returned\n";
        }
    }
    else
    {
        warn "Ref unexpected. not an array\n";
    }

    return $parse_tree_obj;
}

sub get_relcache($$)
{
    my( $handle, $filter_tables ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => ARRAYREF },
    );

    my $query = $FILTER_TABLE_OID_CACHE;
    my $bindpoints = '?,' x ( scalar( @$filter_tables ) - 1 );
    $bindpoints .= '?';

    $query =~ s/__BINDPOINTS__/$bindpoints/;

    my $sth = try_query( $handle, $query, $filter_tables );

    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to get relcache for cache table" );
    }

    my $cache = {};
    while( my $row = $sth->fetchrow_hashref() )
    {
        my $name = $row->{name};
        my $oid  = $row->{oid};
        $cache->{$name} = $oid;
    }

    return $cache;
}

sub get_joined_rels($)
{
    my( $json_fragment ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    if( defined $json_fragment && defined( $json_fragment->{larg} ) )
    {
        my $from_list = &get_joined_rels( $json_fragment->{larg} );

        if( defined( $json_fragment->{rarg} ) )
        {
            if( $json_fragment->{rarg}->{name} eq 'RANGEFUNCTION' )
            { # SRF Function
                # According to parsenodes.h - each element of this List is a two element sublist
                #   - first element being the untransformed function call tree
                #   -  second element being a possibly-empty list of ColumnDef nodes representing
                #      any columndef list attached to that function within the ROWS FROM() syntax
                my $function_call = $json_fragment->{rarg}->{functions}->[0]->[0];
                my $function_name = $function_call->{funcname}->[0];

                if( scalar( @{$function_call->{funcname}} ) > 1 )
                {
                    $function_name = $function_name . '.' . $function_call->{funcname}->[1];
                }
                
                my $function_alias = $function_name;

                if( defined( $json_fragment->{rarg}->{alias} ) )
                {
                    $function_alias = $json_fragment->{rarg}->{alias}->{aliasname};
                }

                push( @$from_list, { $function_alias => $function_name } );
            }
            elsif( $json_fragment->{rarg}->{name} eq 'RANGEVAR' )
            { # table
                my $right_relation = $json_fragment->{rarg}->{relname};

                if( defined( $json_fragment->{rarg}->{schemaname} ) )
                {
                    $right_relation = $json_fragment->{rarg}->{schemaname} . '.' . $right_relation;
                }

                my $alias = $right_relation;

                if( defined( $json_fragment->{rarg}->{alias} ) )
                {
                    $alias = $json_fragment->{rarg}->{alias}->{aliasname};
                }

                push( @$from_list, { $alias => $right_relation } );
            }
        }

        return $from_list;
    }
    else
    {
        if( $json_fragment->{name} eq 'RANGEFUNCTION' )
        {
            my $function_call = $json_fragment->{functions}->[0]->[0];
            my $function_name = $function_call->{funcname}->[0];

            if( scalar( @{$function_call->{funcname}} ) > 1 )
            {
                $function_name = $function_name . '.' . $function_call->{funcname}->[1];
            }

            my $function_alias = $function_name;

            if( defined( $json_fragment->{alias} ) )
            {
                $function_alias = $json_fragment->{alias}->{aliasname};
            }

            return [ { $function_alias => $function_name } ];
        }
        elsif( $json_fragment->{name} eq 'RANGEVAR' )
        {
            my $left_relation = $json_fragment->{relname};
            my $alias = $left_relation;

            if( defined( $json_fragment->{schemaname} ) )
            {
                $left_relation = $json_fragment->{schemaname} . '.' . $left_relation;
            }

            if( defined( $json_fragment->{alias} ) )
            {
                $alias = $json_fragment->{alias}->{aliasname};
            }

            return [ { $alias => $left_relation } ];
        }
    }
}

sub parse_union($)
{
    # Unions are expressed in node trees as
    # rarg => { fromClause => [] },
    # larg => {
    #   rarg => { fromClause => [] ),
    #   larg => ...
    # }
    my( $json_fragment ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    if( defined( $json_fragment->{fromClause} ) )
    {
        # we're at a leaf node for a union subexpression
        my $from = $json_fragment->{fromClause}->[0];
        return get_joined_rels( $from );
    }
    else
    {
        my $union_from_a = &parse_union( $json_fragment->{larg} );
        my $union_from_b = &parse_union( $json_fragment->{rarg} );
        
        if( ref( $union_from_a ) eq 'ARRAY' && ref( $union_from_a->[0] ) eq 'ARRAY' )
        {
            push( @$union_from_a, $union_from_b );
            return $union_from_a;
        }
        
        return [ $union_from_a, $union_from_b ];
    }
}

sub parse_cte($)
{
    my( $json_fragment ) = validate_pos(
        @_,
        { type => HASHREF },
    );
        
    my $index = 0;
    my $ctes = []; 
    foreach my $cte( @{$json_fragment->{ctes}} )
    {
        my $cte_obj = $json_fragment->{ctes}->[$index];
        my $cte_data = {};

        if( defined( $cte_obj->{ctequery}->{withClause} ) )
        {
            $cte_data->{ctes} = &parse_cte( $cte_obj->{ctequery}->{withClause} );
        }

        $cte_data->{name} = $cte_obj->{ctename};

        if( $cte_obj->{cterecursive} )
        {
            my $base_set = get_joined_rels( $cte_obj->{ctequery}->{larg}->{fromClause}->[0] );
            my $recursive_set = get_joined_rels( $cte_obj->{ctequery}->{rarg}->{fromClause}->[0] );
            $cte_data->{from_base} = $base_set;
            $cte_data->{from_recursive} = $recursive_set;
        }
        else
        {
            if( defined( $cte_obj->{ctequery}->{fromClause} ) )
            {
                my $cte_from = $cte_obj->{ctequery}->{fromClause}->[0];
                my $cte_fromlist = get_joined_rels( $cte_from );
                $cte_data->{from} = $cte_fromlist;
            }
            elsif(
                    defined( $cte_obj->{ctequery}->{larg} )
                 && defined( $cte_obj->{ctequery}->{rarg} )
                 )
            {
                # Unioned statement?
                my $union = parse_union( $cte_obj->{ctequery} );
                $cte_data->{union} = $union;
            }
        }

        $index++;
        push( @$ctes, $cte_data );
    }

    return $ctes;
}

sub find_table_aliases($$$$)
{
    my( $handle, $relcache, $definition, $filter_tables ) = validate_pos(
        @_,
        { type => OBJECT   },
        { type => HASHREF  },
        { type => SCALAR   },
        { type => ARRAYREF },
    );

    my $parse_tree_obj = get_query_parsetree( $handle, $definition );

    return unless( defined $parse_tree_obj );
    print Dumper( $parse_tree_obj );
    
    # Sanity check top-level-node
    unless( defined $parse_tree_obj && ref( $parse_tree_obj ) eq 'HASH' )
    {
        die( "Invalid structure returned\n" );
    }

    unless(
                exists( $parse_tree_obj->{stmt} )
             && exists( $parse_tree_obj->{name} )
             && $parse_tree_obj->{name} eq 'RAWSTMT'
          )
    {
        die( "Unexpected top level node $parse_tree_obj->{name}\n" );
    }

    my $statement = $parse_tree_obj->{stmt};

    my $query_data = {};
    # Handle common table expressions
    my $statement_has_ctes = exists( $statement->{withClause} );
    my $number_of_ctes = 0;

    if( exists $statement->{withClause} )
    {
        $query_data->{ctes} = parse_cte( $statement->{withClause} );
    }

    # Handle from clause
    if( exists( $statement->{fromClause} ) )
    {
        my $from = $statement->{fromClause}->[0];
        my $from_list = get_joined_rels( $from );
        $query_data->{from} = $from_list;
    }
    elsif(
            defined( $statement->{larg} )
         && defined( $statement->{rarg} )
         )
    {
        #unioned statement
        my $union = parse_union( $statement );
        $query_data->{union} = $union;
    }

    return $query_data;
}


my $handle = DBI->connect( $CONNECTION_MAP->{connection_string}, $CONNECTION_MAP->{user_name}, undef );

unless( $handle )
{
    die( "failed to connect\n" );
}

my $filter_tables = [ 'public.tb_a', 'public.tb_b', 'public.tb_c' ];
my $relcache = get_relcache( $handle, $filter_tables );
my $data = find_table_aliases( $handle, $relcache, $definition, $filter_tables );
print Dumper( $data );
