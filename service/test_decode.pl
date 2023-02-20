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
    SELECT n.nspname::VARCHAR AS schema_name,
           c.relname::VARCHAR AS obj_name,
           c.oid,
           'r' AS type
      FROM pg_class c
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR != 'pg_toast'
     UNION ALL
    SELECT n.nspname::VARCHAR AS schema_name,
           p.proname::VARCHAR AS obj_name,
           p.oid,
           'f' AS type
      FROM pg_proc p
INNER JOIN pg_namespace n
        ON n.oid = p.pronamespace
       AND n.nspname::VARCHAR != 'pg_toast'
END_SQL

Readonly::Scalar my $GET_TEST_VIEW_PARSE_TREE => <<"END_SQL";
    SELECT ${SCHEMA_NAME}.fn_get_parse_tree( \$_\$__DEFINITION__\$_\$ )::JSONB AS tree
END_SQL

my $CONNECTION_MAP->{connection_string} = 'dbi:Pg:dbname=__pgc_testing__;host=localhost;port=5432';
$CONNECTION_MAP->{user_name} = 'postgres';
my $definition = <<END_SQL;
SELECT a.foo
  FROM tb_a a
  JOIN tb_b b
    ON b.bar = a.bar
 LIMIT 10
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

    my $sth = try_query( $handle, $query, undef );

    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to get relcache for cache table" );
    }

    my $cache = {};
    while( my $row = $sth->fetchrow_hashref() )
    {
        my $schema = $row->{schema_name};
        my $name   = $row->{obj_name};
        my $oid    = $row->{oid};
        $cache->{rels}->{$schema}->{$name} = $oid if( $row->{type} eq 'r' );
        $cache->{func}->{$schema}->{$name} = $oid if( $row->{type} eq 'f' );
    }

    $sth->finish();


    return $cache;
}

sub resolve_relation($$)
{
    my( $relcache, $relation ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR },
    );

    my $relation_schema = '';
    my $relation_name   = '';

    if( $relation =~ m/\./ )
    {
        $relation_schema = $relation;
        $relation_schema =~ s/\..*$//;
        $relation_name   = $relation;
        $relation_name   =~ s/^.*?\.//;

        if(
              defined( $relcache->{rels}->{$relation_schema}->{$relation_name} )
           || defined( $relcache->{func}->{$relation_schema}->{$relation_name} )
          )
        {
            return { schema => $relation_schema, name => $relation_name };
        }
    }
    else
    {
        $relation_name = $relation;

        foreach my $schema( keys %{$relcache->{rels}} )
        {
            if( defined( $relcache->{rels}->{$schema}->{$relation_name} ) )
            {
                $relation_schema = $schema;
                return { schema => $relation_schema, name => $relation_name };
            }
        }

        foreach my $schema( keys %{$relcache->{func}} )
        {
            if( defined( $relcache->{func}->{$schema}->{$relation_name} ) )
            {
                $relation_schema = $schema;
                return { schema => $relation_schema, name => $relation_name };
            }
        }
    }

    return;
}

sub add_table_mapping($$$$$$$;$)
{
    my(
        $relcache,
        $table_mapping,
        $obj_name,
        $obj_alias,
        $parent,
        $location,
        $is_function,
        $is_cte
      ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => HASHREF },
        { type => SCALAR },
        { type => SCALAR | UNDEF },
        { type => SCALAR | UNDEF },
        { type => SCALAR },
        { type => SCALAR },
        { type => SCALAR | UNDEF, optional => 1 },
    );

    if( defined $is_cte && $is_cte )
    {
        $table_mapping->{CTES}->{$obj_name} = { parent => $parent, location => $location };

        # CTEs get added to the rellist because we recurse before adding table mapping for them
        # this cleans up rels that were added to the cte list
        if( defined( $table_mapping->{RELS}->{$obj_name} ) )
        {
            delete $table_mapping->{RELS}->{$obj_name};
        }
        return;
    }

    my $obj_data = resolve_relation( $relcache, $obj_name );

    unless( $obj_data )
    {
        warn "Unable to resolve relation '$obj_name' in relcache";
        return;
    }

    $obj_name = $obj_data->{name};
    my $obj_schema = $obj_data->{schema};

    if( defined $is_function && $is_function )
    {
        if(
               !defined( $table_mapping->{FUNCTIONS}->{$obj_schema}->{$obj_name} )
            && !defined( $table_mapping->{FUNCTIONS}->{$obj_schema}->{$obj_name}->{$obj_alias} )
          )
        {
            $table_mapping->{FUNCTIONS}->{$obj_schema}->{$obj_name}->{$obj_alias} = [ { parent=> $parent, location => $location } ];
            return;
        }

        if(
               defined( $parent )
            && not grep(
                   /^$parent$/,
                   @{$table_mapping->{FUNCTIONS}->{$obj_schema}->{$obj_name}->{$obj_alias}}
               )
          )
        {
            push(
                @{$table_mapping->{FUNCTIONS}->{$obj_schema}->{$obj_name}->{$obj_alias}},
                { parent=> $parent, location => $location }
            );
        }

        return;
    }

    if( grep( /^$obj_name$/, keys %{$table_mapping->{CTES}} ) )
    {
        return;
    }

    if(
           !defined( $table_mapping->{RELS}->{$obj_schema}->{$obj_name} )
        && !defined( $table_mapping->{RELS}->{$obj_schema}->{$obj_name}->{$obj_alias} )
      )
    {
        $table_mapping->{RELS}->{$obj_schema}->{$obj_name}->{$obj_alias} = [ { parent => $parent, location => $location } ];
        return;
    }

    if(
            defined( $parent )
         && not grep(
                /^$parent$/,
                @{$table_mapping->{RELS}->{$obj_schema}->{$obj_name}->{$obj_alias}}
            )
      )
    {
        push(
            @{$table_mapping->{RELS}->{$obj_schema}->{$obj_name}->{$obj_alias}},
            { parent=>$parent, location => $location }
        );
        return;
    }

    return;
}

sub get_joined_rels($$$$)
{
    my( $json_fragment, $parent, $table_mapping, $relcache ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
    );

    #NOTE: We parse location to determine where the WHERE clause should go
    #Location parsing here is important if our parent statement does not possess
    #a WHERE clause. Chris note: Currently our location parsing gets us CLOSE but it still includee
    #fragments of the join predicate, and we need to possibly move the location 'forward' to skip past boolean
    #predicate expressions
    my $location = 0;
    if( defined( $json_fragment->{location} ) )
    {
        $location = $json_fragment->{location};
        print "Found location $location for $json_fragment->{name}\n";
    }

    if( defined $json_fragment && defined( $json_fragment->{larg} ) )
    {
        my $from_list = &get_joined_rels(
            $json_fragment->{larg},
            $parent,
            $table_mapping,
            $relcache
        );

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

                push(
                    @$from_list,
                    {
                        $function_alias => {
                            obj      => $function_name,
                            type     => 'FUNCTION',
                            location => $function_call->{rarg}->{location},
                        }
                    }
                );
                add_table_mapping(
                    $relcache,
                    $table_mapping,
                    $function_name,
                    $function_alias,
                    $parent,
                    $location,
                    1
                );
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

                push(
                    @$from_list,
                    {
                        $alias => {
                            obj      => $right_relation,
                            type     => 'RELATION',
                            location => $json_fragment->{rarg}->{location},
                        }
                    }
                );
                add_table_mapping(
                    $relcache,
                    $table_mapping,
                    $right_relation,
                    $alias,
                    $parent,
                    $location,
                    0
                );
            }
            elsif( $json_fragment->{rarg}->{name} eq 'RANGESUBSELECT' )
            {
                my $alias = $json_fragment->{rarg}->{alias}->{aliasname};
                push(
                    @$from_list,
                    {
                        $alias => {
                            obj      => &parse_select(
                                $json_fragment->{rarg}->{subquery},
                                $parent,
                                $table_mapping,
                                $relcache
                            ),
                            type     => 'SUBSELECT',
                            location => $json_fragment->{rarg}->{location},
                        }
                    }
                );
            }
            else
            {
                warn( "get_joined_rels: Unknown right RTE $json_fragment->{rarg}->{name}\n" );
                return;
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

            add_table_mapping(
                $relcache,
                $table_mapping,
                $function_name,
                $function_alias,
                $parent,
                $location,
                1
            );
            return [
                {
                    $function_alias => {
                        obj      => $function_name,
                        type     => 'FUNCTION',
                        location => $json_fragment->{location},
                    }
                }
            ];
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

            add_table_mapping(
                $relcache,
                $table_mapping,
                $left_relation,
                $alias,
                $parent,
                $location,
                0
            );
            return [
                {
                    $alias => {
                        obj      => $left_relation,
                        type     => 'RELATION',
                        location => $json_fragment->{location},
                    }
                }
            ];
        }
        elsif( $json_fragment->{name} eq 'RANGESUBSELECT' )
        {
            my $alias = $json_fragment->{alias}->{aliasname};
            return [
                {
                    $alias => {
                        obj      => &parse_select( $json_fragment, $parent, $table_mapping, $relcache ),
                        type     => 'SUBSELECT',
                        location => $json_fragment->{location},
                    }
                }
            ];
        }
        else
        {
            warn( "get_joined_rels: Unknown recursed left RTE $json_fragment->{name}\n" );
            return;
        }
    }
}

sub parse_union($$$$)
{
    # Unions are expressed in node trees as
    # rarg => { fromClause => [] },
    # larg => {
    #   rarg => { fromClause => [] ),
    #   larg => ...
    # }
    my( $json_fragment, $parent, $table_mapping, $relcache ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
    );

    if(
           $json_fragment->{name} eq 'SELECTSTMT'
        && $json_fragment->{op} eq 'NONE'
      )
    {
        # Regular union element - we're likely at an end element in the union tree
        return [ parse_select( $json_fragment, $parent, $table_mapping, $relcache ) ];
    }
    elsif(
              defined( $json_fragment->{larg} )
           && defined( $json_fragment->{rarg} )
           && $json_fragment->{op} ne 'NONE'
         )
    { # no fromclause - it's broken down into an larg/rarg tree
        my $union_from_a = &parse_union(
            $json_fragment->{larg},
            $parent,
            $table_mapping,
            $relcache
        );

        my $union_from_b = &parse_union(
            $json_fragment->{rarg},
            $parent,
            $table_mapping,
            $relcache
        );

        if(
               ref( $union_from_a ) eq 'ARRAY'
          )
        {
            if( ref( $union_from_b ) eq 'ARRAY' )
            {
                $union_from_b = $union_from_b->[0];
            }
            push( @$union_from_a, $union_from_b );
            return $union_from_a;
        }

        return [ $union_from_a, $union_from_b ];
    }
    elsif( $json_fragment->{name} eq 'SELECTSTMT' )
    {
        return &parse_select(
            $json_fragment,
            $parent,
            $table_mapping,
            $relcache
        );
    }
    else
    {
        warn "parse_union: Invalid structure in $json_fragment->{name} node\n";
    }
}

sub parse_cte($$$$)
{
    my( $json_fragment, $parent, $table_mapping, $relcache ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
    );

    my $index = 0;
    my $ctes = [];
    foreach my $cte( @{$json_fragment->{ctes}} )
    {
        my $cte_obj = $json_fragment->{ctes}->[$index];
        my $cte_data = {};

        $cte_data->{name} = $cte_obj->{ctename};
        my $local_parent = $cte_data->{name};
        if( $cte_obj->{cterecursive} )
        {
            $cte_data->{from_base} = &parse_select(
                $cte_obj->{ctequery}->{larg},
                $local_parent,
                $table_mapping,
                $relcache
            );
            $cte_data->{from_recursive} = &parse_select(
                $cte_obj->{ctequery}->{rarg},
                $local_parent,
                $table_mapping,
                $relcache
            );
        }
        else
        {
            $cte_data = &parse_select(
                $cte_obj->{ctequery},
                $local_parent,
                $table_mapping,
                $relcache
            );
        }

        $index++;
        my $location = $cte_obj->{location};
        add_table_mapping(
            $relcache,
            $table_mapping,
            $local_parent,
            undef,
            $parent,
            $location,
            0,
            1
        );
        push( @$ctes, $cte_data );
    }


    return $ctes;
}

sub parse_from_clause($$$$)
{
    my( $json_fragment, $parent, $table_mapping, $relcache ) = validate_pos(
        @_,
        { type => ARRAYREF },
        { type => SCALAR | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
    );

    return &get_joined_rels( $json_fragment->[0], $parent, $table_mapping, $relcache );
}

sub parse_select($$$$)
{
    my( $json_fragment, $parent, $table_mapping, $relcache ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
    );
    
    #The conditionals around location here are to narrow down the location (or possible location)
    # of a WHERE clause
    if( $json_fragment->{name} ne 'SELECTSTMT' )
    {
        warn "parse_select: Invalid node $json_fragment->{name}\n";
        return;
    }

    my $statement_info = {};
    my $where_start;
    my $where_end;
    if( defined( $json_fragment->{fromClause} ) )
    { # From clause with / without joins
        $statement_info->{from} = &parse_from_clause(
            $json_fragment->{fromClause},
            $parent,
            $table_mapping,
            $relcache
        );

    }
    elsif(
            defined( $json_fragment->{rarg} )
         && defined( $json_fragment->{larg} )
         )
    { # UNION / UNION ALL / EXCEPT / INTERSECT
        $statement_info->{union} = &parse_union(
            $json_fragment,
            $parent,
            $table_mapping,
            $relcache
        );
    }

    if( defined( $json_fragment->{withClause} ) )
    { # Common Table Expressions incl recursive
        $statement_info->{ctes} = &parse_cte(
            $json_fragment->{withClause},
            $parent,
            $table_mapping,
            $relcache
        );
    }

    if( defined( $json_fragment->{whereClause} ) )
    {
        $statement_info->{has_where} = 1;
        # Possibly parse out where clause BoolExpr locations
        if(
                defined( $json_fragment->{whereClause}->{location} )
          )
        {
            if(
                    defined( $json_fragment->{whereClause}->{args} )
                 && ref( $json_fragment->{whereClause}->{args} ) eq 'ARRAY'
              )
            {
                foreach my $arg( @{$json_fragment->{whereClause}->{args}} )
                {
                    if(
                           (
                                 defined( $arg->{lexpr}->{location} )
                              && defined( $where_start )
                              && $arg->{lexpr}->{location} < $where_start
                           )
                        || (
                                 !defined( $where_start )
                              && defined( $arg->{lexpr}->{location} )
                           )
                      )
                    {
                        $where_start = $arg->{lexpr}->{location};
                    }
                }
            }
            elsif(
                     defined( $json_fragment->{whereClause}->{lexpr} )
                  && defined( $json_fragment->{whereClause}->{lexpr}->{location} )
                 )
            {
                if(
                      (
                          defined( $where_start )
                       && $json_fragment->{whereClause}->{lexpr}->{location} < $where_start
                      )
                   || ( !defined $where_start )
                  )
                {
                    $where_start = $json_fragment->{whereClause}->{lexpr}->{location};
                }
            }
            else
            {
                $where_start = $json_fragment->{whereClause}->{location};
            }
        }
    }
    else
    {
        # We need to find the END of the from clause to determine where the WHERE clause should go
        my $max_location = 0;
        foreach my $from( @{$statement_info->{from}} )
        {
            foreach my $alias( keys %$from )
            {
                if( $from->{$alias}->{location} > $max_location )
                {
                    $max_location = $from->{$alias}->{location};
                }
            }
        }

        $where_start = $max_location;
    }
     
    # it's important we set the where_start after parsing the whereclause,
    # if it exists, as its where start takes precedence but the logic is
    # counter-intuitive
    if( defined( $json_fragment->{fromClause} ) )
    {
        if(
                defined( $json_fragment->{fromClause}->[0] )
             && defined( $json_fragment->{fromClause}->[0]->{location} )
             && !defined( $where_start )
          )
        {
            $where_start = $json_fragment->{fromClause}->[0]->{location};
        }
    }

    if( defined( $json_fragment->{groupClause} ) )
    {
        $statement_info->{has_group} = 1;

        if( defined( $json_fragment->{groupClause}->[0]->{location} ) )
        {
            if(
                   (
                        defined( $where_end )
                     && $json_fragment->{groupClause}->[0]->{location} < $where_end
                   )
                || !defined( $where_end )
              )
            {
                $where_end = $json_fragment->{groupClause}->[0]->{location};
            }
        }
    }

    if( defined( $json_fragment->{havingClause} ) )
    {
        $statement_info->{has_having} = 1;

        if( defined( $json_fragment->{havingClause}->{location} ) )
        {
            if(
                   (
                        defined( $where_end )
                     && $json_fragment->{havingClause}->{location} < $where_end
                   )
                || !defined( $where_end )
              )
            {
                $where_end = $json_fragment->{havingClause}->{location};
            }
        }
    }

    if( defined( $json_fragment->{sortClause} ) )
    {
        $statement_info->{has_sort} = 1;

        if(
               defined( $json_fragment->{sortClause}->[0]->{location} ) 
            && $json_fragment->{sortClause}->[0]->{location} > 0
          )
        {
            if(
                   (
                        defined( $where_end )
                     && $json_fragment->{sortClause}->[0]->{location} < $where_end
                   )
                || !defined( $where_end )
              )
            {
                $where_end = $json_fragment->{sortClause}->[0]->{location};
            }
        }
    }

    if( defined( $json_fragment->{limitOffset} ) )
    {
        $statement_info->{has_offset} = 1;

        if( defined( $json_fragment->{limitOffset}->{location} ) )
        {
            if(
                   (
                        defined( $where_end )
                     && $json_fragment->{limitOffset}->{location} < $where_end
                   )
                || !defined( $where_end )
              )
            {
                $where_end = $json_fragment->{limitOffset}->{location};
            }
        }
    }

    if( defined( $json_fragment->{limitCount} ) )
    {
        $statement_info->{has_limit} = 1;

        if( defined( $json_fragment->{limitCount}->{location} ) )
        {
            if(
                   (
                        defined( $where_end )
                     && $json_fragment->{limitCount}->{location} < $where_end
                   )
                || !defined( $where_end )
              )
            {
                $where_end = $json_fragment->{limitCount}->{location};
            }
        }
    }

    # determine the location of the select statement based on the location
    # of the targetList ResTarget entries (if they exist)
    my $min_location;
    if( defined( $json_fragment->{targetList} ) )
    { # we're always expected to enter this
        foreach my $restarget( @{$json_fragment->{targetList}} )
        {
            my $location = $restarget->{location};

            if( !defined $min_location || $min_location > $location )
            {
                $min_location = $location;
            }
        }
    }

    if( $min_location && !defined( $where_start ) )
    {
        $where_start = $min_location;
    }

    if( defined( $where_start ) )
    {
        $statement_info->{where_start} = $where_start;
    }

    if( defined( $where_end ) )
    {
        $statement_info->{where_end} = $where_end;
    }

    return $statement_info;
}

sub find_table_aliases($$$$$)
{
    my( $handle, $relcache, $definition, $filter_tables, $table_mapping ) = validate_pos(
        @_,
        { type => OBJECT   },
        { type => HASHREF  },
        { type => SCALAR   },
        { type => ARRAYREF },
        { type => HASHREF },
    );

    my $parse_tree_obj = get_query_parsetree( $handle, $definition );

    return unless( defined $parse_tree_obj );
    #print Dumper( $parse_tree_obj );

    # Sanity check top-level-node
    unless( defined $parse_tree_obj && ref( $parse_tree_obj ) eq 'HASH' )
    {
        die( "Invalid structure returned\n" );
    }

    print Dumper( $parse_tree_obj );

    unless(
                exists( $parse_tree_obj->{stmt} )
             && exists( $parse_tree_obj->{name} )
             && $parse_tree_obj->{name} eq 'RAWSTMT'
          )
    {
        die( "Unexpected top level node $parse_tree_obj->{name}\n" );
    }

    my $statement = $parse_tree_obj->{stmt};
    my $query_data = parse_select( $statement, undef, $table_mapping, $relcache );

    return $query_data;
}

sub apply_filters($$$$)
{
    my( $query_data, $table_mapping, $definition, $filters ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => HASHREF },
        { type => SCALAR },
        { type => HASHREF },
    );

    # Lets use the filters we've received and search for the tables, their aliases, and the objects they are present in within the query,
    # then attempt to modify the query such that we habe a filtered query
    # Phase I will result in a keyed array telling us which CTE or query will need a filter applied
    my $filter_locations = {};
    foreach my $schema( keys %$filters )
    { #iterate over changed schema
        # skip if we don't have a table mapping - change doesn't appear in the query
        unless( defined( $table_mapping->{RELS}->{$schema} ) )
        {
            warn "Found change to a schema not in the query!\n";
            next;
        }

        foreach my $table( keys %{$filters->{$schema}} )
        {
            unless( defined( $table_mapping->{RELS}->{$schema}->{$table} ) )
            {
                warn "Found change to a table not in the query!\n";
                next;
            }

            foreach my $alias( keys %{$table_mapping->{RELS}->{$schema}->{$table}} )
            {
                my $filter_entries = [];

                foreach my $key( keys %{$filters->{$schema}->{$table}} )
                {
                    foreach my $changed_key( @{$filters->{$schema}->{$table}->{$key}} )
                    {
                        my $entry = "$alias\.$table = $changed_key";
                        push( @$filter_entries, $entry );
                    }

                    foreach my $filter_location( @{$table_mapping->{RELS}->{$schema}->{$table}->{$alias}} )
                    {
                        unless( defined( $filter_location->{parent} ) ) # undef == main query
                        {
                            if( !defined( $filter_locations->{MAIN} ) )
                            {
                                $filter_locations->{MAIN}->{where} = $filter_entries;
                                $filter_locations->{MAIN}->{location} = $filter_location->{location};
                            }
                            else
                            {
                                push( @{$filter_locations}->{MAIN}->{where}, @$filter_entries );
                            }
                        }
                        else
                        {
                            if( !defined( $filter_locations->{$filter_location->{parent}} ) )
                            {
                                $filter_locations->{$filter_location->{parent}}->{where} = $filter_entries;
                                $filter_locations->{$filter_location->{parent}}->{location} = $filter_location->{location};
                            }
                            else
                            {
                                push( @{$filter_locations->{$filter_location->{parent}}->{where}}, @$filter_entries );
                            }
                        }
                    }
                }
            }
        }
    }

    #print Dumper( $filter_locations );
    #print Dumper( $query_data );

    my $where_fragments = {};
    foreach my $filter_location( keys %{$filter_locations} )
    {
        my $where_fragment = '( ( ' . join( ' ) OR ( ', @{$filter_locations->{$filter_location}->{where}} ) . ' ) )';

        if( $filter_location  eq 'MAIN' )
        {   #note for later - for the main query - we may have to store the location of the closest clause to the where to assist the replcement later
            if( $query_data->{has_where} )
            {
                $where_fragment = ' AND ' . $where_fragment;
            }
            else
            {
                $where_fragment = ' WHERE ' . $where_fragment;
            }
        }
        else
        {
            # need to traverse $query_data to determine if the given CTE / subquery has a WHERE
        }

        $where_fragments->{$filter_location} = {
            where => $where_fragment,
            location => $filter_locations->{$filter_location}->{location}
        };
    }

    foreach my $where_fragment( keys %$where_fragments )
    {
        my $portion = '';

        if( $where_fragment eq 'MAIN' )
        {
            # locate the last of the CTEs so that we can slice the main query out
            my $largest_location = 0;

            foreach my $cte( keys %{$table_mapping->{CTES}} )
            {
                if( $table_mapping->{CTES}->{$cte}->{location} > $largest_location )
                {
                    $largest_location = $table_mapping->{CTES}->{$cte}->{location};
                }
            }
            
            $portion = substr( $definition, $largest_location, length( $definition ) );

            print "$portion\n";
        }
    }

    return;
}

my $handle = DBI->connect( $CONNECTION_MAP->{connection_string}, $CONNECTION_MAP->{user_name}, undef );

unless( $handle )
{
    die( "failed to connect\n" );
}

my $test_change = { 'public' => { 'tb_a' => { 'foo' => [ 1 ] }, 'tb_b' => { 'foo'=>[2,3]}, 'tb_c' => {'foo'=>[4,5]} } };
my $filter_tables = [ 'public.tb_a', 'public.tb_b', 'public.tb_c' ];
my $relcache = get_relcache( $handle, $filter_tables );
my $table_mapping = {};
my $data = find_table_aliases( $handle, $relcache, $definition, $filter_tables, $table_mapping );
print Dumper( $data );
#my $q = apply_filters( $data, $table_mapping, $definition, $test_change );
#print Dumper( $table_mapping );
