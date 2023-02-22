#!/usr/bin/perl

use strict;
use utf8;
use warnings;

use DBI;
use JSON::XS; # Can't use JSON:PP because it tried to redefine simple bools as a blessed class that other packages aren't aware of
use Readonly;
use Params::Validate qw( :all );
use Data::Dumper;
use English qw( -no_match_vars );
use Data::Search;

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
WITH tt_foo AS
(
    WITH tt_union_test AS
    (
        SELECT c.bar FROM public.tb_c c
         UNION
        SELECT b.bar FROM public.tb_b b
    )
        SELECT bar
          FROM tt_union_test
),
tt_bar AS
(
    SELECT b.bar
      FROM tb_b b
      JOIN public.tb_a a
        ON a.baz = b.baz
     WHERE a.baz = 2
       AND TRUE
       AND a.bar = 1
)
SELECT a.foo
  FROM tb_a a
  JOIN tb_b b
    ON b.bar = a.bar
  JOIN tb_c c
    ON c.baz = ANY( ARRAY[ 1,2,3] )
  JOIN tt_bar ttb
    ON ttb.bar = a.bar
  JOIN tt_foo ttf
    ON ttf.bar = a.bar
 WHERE a.baz IS NOT NULL
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

    my $parse_tree_obj = decode_json( $query_tree );
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

sub get_relcache($)
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
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

    return unless( $obj_data ); # Likely a CTE

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
    }

    my $supplemental_location;

    if( defined( $json_fragment->{quals} ) )
    {
        my $old_warn = $SIG{__WARN__};
        $SIG{__WARN__} = sub { };
        my @locs = datasearch( data => $json_fragment->{quals}, search => 'keys', find => qr/location/ );
        $SIG{__WARN__} = $old_warn;
        if( scalar( @locs ) > 0 )
        {
            foreach my $loc( @locs )
            {
                if( !defined( $supplemental_location ) || $supplemental_location < $loc )
                {
                    $supplemental_location = $loc;
                }
            }
        }
    }

    if( defined( $supplemental_location ) && $supplemental_location > $location )
    {
        $location = $supplemental_location;
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

                if( $location < $function_call->{rarg}->{location} )
                {
                    $location = $function_call->{rarg}->{location};
                }

                push(
                    @$from_list,
                    {
                        $function_alias => {
                            obj      => $function_name,
                            type     => 'FUNCTION',
                            location => $location,
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

                if( $location < $json_fragment->{rarg}->{location} )
                {
                    $location = $json_fragment->{rarg}->{location};
                }

                push(
                    @$from_list,
                    {
                        $alias => {
                            obj      => $right_relation,
                            type     => 'RELATION',
                            location => $location,
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
                if( $location < $json_fragment->{rarg}->{location} )
                {
                    $location = $json_fragment->{rarg}->{location};
                }

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
                            location => $location,
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

sub parse_union($$$$$;$)
{
    # Unions are expressed in node trees as
    # rarg => { fromClause => [] },
    # larg => {
    #   rarg => { fromClause => [] ),
    #   larg => ...
    # }
    my( $json_fragment, $parent, $table_mapping, $relcache, $is_rarg, $union_flag ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
        { type => SCALAR },
        { type => SCALAR, optional => 1 },
    );

    if(
           $json_fragment->{name} eq 'SELECTSTMT'
        && $json_fragment->{op} eq 'NONE'
      )
    {
        # Regular union element - we're likely at an end element in the union tree
        if( $is_rarg )
        {
            # for anchoring unions (final where clause) we need to know if this is the last union member
            return [ &parse_select( $json_fragment, $parent, $table_mapping, $relcache, 'NONE' ) ];
        }

        return [ &parse_select( $json_fragment, $parent, $table_mapping, $relcache, $union_flag ) ];
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
            $relcache,
            0,
            $json_fragment->{op}
        );

        my $union_from_b = &parse_union(
            $json_fragment->{rarg},
            $parent,
            $table_mapping,
            $relcache,
            1
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
    { # catchall
        return &parse_select(
            $json_fragment,
            $parent,
            $table_mapping,
            $relcache,
            $union_flag
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

    my $result = &get_joined_rels( $json_fragment->[0], $parent, $table_mapping, $relcache );

    return $result;
}

sub parse_select($$$$;$)
{
    my( $json_fragment, $parent, $table_mapping, $relcache, $union_flag ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
        { type => SCALAR | UNDEF, optional => 1 },
    );

    my $is_union_member;
    $is_union_member = $union_flag if( defined( $union_flag ) );
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
            $relcache,
            0
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

                if( $where_start == -1 )
                {
                    my $old_warn = $SIG{__WARN__};
                    $SIG{__WARN__} = sub { };
                    my @locs = datasearch( data => $json_fragment->{whereClause}, search => 'keys', find => qr/location/ );
                    $SIG{__WARN__} = $old_warn;
                    my $new_where_start;
                    foreach my $loc( @locs )
                    {
                        next if( $loc == -1 );
                        if( !defined( $new_where_start ) || $loc < $new_where_start )
                        {
                            $new_where_start = $loc;
                        }
                    }

                    $where_start = $new_where_start;
                }
            }
        }
        else
        {
            my $old_warn = $SIG{__WARN__};
            $SIG{__WARN__} = sub { };
            my @locs = datasearch( data => $json_fragment->{whereClause}, search => 'keys', find => qr/location/ );
            $SIG{__WARN__} = $old_warn;
            my $new_where_start;

            foreach my $loc( @locs )
            {
                next if( $loc == -1 );
                if( !defined( $new_where_start ) || $loc < $new_where_start )
                {
                    $new_where_start = $loc;
                }
            }

            $where_start = $new_where_start;
        }
    }
    else
    {
        if( defined( $statement_info->{union} ) )
        {
            $where_start = -1;
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
    my $select_location;
    my $min_location;
    if( defined( $json_fragment->{targetList} ) )
    { # we're always expected to enter this
        my $old_warn = $SIG{__WARN__};
        $SIG{__WARN__} = sub { };
        my @locs = datasearch( data => $json_fragment->{targetList}, search => 'keys', find => qr/location/ );
        $SIG{__WARN__} = $old_warn;

        foreach my $loc( @locs )
        {
            if( !defined( $select_location ) || $select_location > $loc )
            {
                $select_location = $loc;
            }
        }

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

    $table_mapping->{BINDS}->{$where_start} = {
        start           => $where_start,
        end             => $where_end,
        parent          => $parent,
        has_where       => $statement_info->{has_where} // 0,
        has_limit       => $statement_info->{has_limit} // 0,
        has_offset      => $statement_info->{has_offset} // 0,
        has_group       => $statement_info->{has_group} // 0,
        has_sort        => $statement_info->{has_sort} // 0,
        has_having      => $statement_info->{has_having} // 0,
        is_union        => $is_union_member,
        select_location => $select_location,
        rels            => {},
    };

    foreach my $rel( @{$statement_info->{from}} )
    {
        foreach my $alias( keys %$rel )
        {
            my $obj_name = $rel->{$alias}->{obj};
            my $qual = resolve_relation( $relcache, $obj_name );
            next unless( defined $qual->{schema} && defined $qual->{name} );
            my $schema = $qual->{schema};
            my $name   = $qual->{name};
            $table_mapping->{BINDS}->{$where_start}->{rels}->{$schema}->{$alias}->{$name} = 1;
        }
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
    #print Dumper( $statement );
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
    my $where_expressions = {};

    foreach my $position( keys %{$table_mapping->{BINDS}} )
    {
        next if( $position < 0 );

        my $RELS          = $table_mapping->{BINDS}->{$position}->{rels};
        my $where_entries = [];

        foreach my $schema( keys %$RELS )
        {
            foreach my $alias( keys %{$RELS->{$schema}} )
            {
                foreach my $table_name( keys %{$RELS->{$schema}->{$alias}} )
                {
                    if( defined( $filters->{$schema}->{$table_name} ) )
                    {
                        foreach my $key( keys %{$filters->{$schema}->{$table_name}} )
                        {
                            foreach my $value( @{$filters->{$schema}->{$table_name}->{$key}} )
                            {
                                # TODO: Get typmod cache and use that to correctly bind stuff
                                push( @$where_entries, "${alias}.${key} = ${value}" );
                            }
                        }
                    }

                }
            }
        }

        my $where_entry;

        if( scalar( @$where_entries ) > 0 )
        {
            if( $table_mapping->{BINDS}->{$position}->{has_where} )
            {
                $where_entry = ' AND ( ( ' . join( ' ) OR ( ', @$where_entries ) . ' ) ) ';
            }
            else
            {
                $where_entry = ' WHERE ( ( ' . join( ' ) OR ( ', @$where_entries ) . ' ) ) ';
            }

            $where_expressions->{$position} = $where_entry;
        }
    }

    my @starts = sort { $b <=> $a } keys( %{$table_mapping->{BINDS}} );
    # Assmple where expressions structure keyed based on the bind position
    # for much easier substitution later

    #print Dumper( $where_expressions );
    my $new_q = $definition;
    my $index = 0;

    foreach my $bind_start( @starts )
    {
        next if( $bind_start < 0 ); # Skip if unbindable (no relevent relations)
        next if( !defined( $where_expressions->{$bind_start} ) ); # Skip if no filters to be applied
        my $bind_end = $table_mapping->{BINDS}->{$bind_start}->{end};
        my $next_cte_name;

        if( $index - 1 >= 0 )
        {
            $next_cte_name = $table_mapping->{BINDS}->{$starts[$index-1]}->{parent};
        }

        if( !defined( $bind_end ) )
        {
            my $parent = $table_mapping->{BINDS}->{$bind_start}->{parent};
            if( !defined( $parent ) || $index == 0 )
            {
                $bind_end = length( $new_q );
            }
            else
            {
                # find the location of the proceeding select statement
                my $next = $starts[$index - 1];
                $bind_end = $table_mapping->{BINDS}->{$next}->{select_location};
            }
        }

        my $bind_location = substr( $new_q, $bind_start, $bind_end - $bind_start );
        # note for union parsing - we need to constrain by adding where_end in select parsing :(
        # also, we need to corelate against the $table_mapping->binds itself

        # Find the end of the last expression (if has_where) or the last join predicate (if !has_where)
        my $where_expression = $where_expressions->{$bind_start};
        my $is_in_cte = defined( $table_mapping->{BINDS}->{$bind_start}->{parent} );
        my $where_proceeding_clause_mark;
        if(    $table_mapping->{BINDS}->{$bind_start}->{has_group}  ) { $where_proceeding_clause_mark = 'group\s+by';                 }
        elsif( $table_mapping->{BINDS}->{$bind_start}->{has_having} ) { $where_proceeding_clause_mark = 'having';                     }
        # TODO add WINDOW
        elsif(
                  defined( $table_mapping->{BINDS}->{$bind_start}->{is_union} )
               && $table_mapping->{BINDS}->{$bind_start}->{is_union} ne 'NONE'
             )
        {
            $where_proceeding_clause_mark = '\s+' . lc( $table_mapping->{BINDS}->{$bind_start}->{is_union} );
        }
        elsif( # handle case where we union at the end of a CTE def
                  defined( $table_mapping->{BINDS}->{$bind_start}->{is_union} )
             )
        {
            # NOTE This may need to be expanded - there are many cases where unions can be used / abused and
            # a union can appear in the form of:

            $where_proceeding_clause_mark = '\)';
        }
        elsif( $table_mapping->{BINDS}->{$bind_start}->{has_sort}   ) { $where_proceeding_clause_mark = 'order\s+by';                 }
        elsif( $table_mapping->{BINDS}->{$bind_start}->{has_limit}  ) { $where_proceeding_clause_mark = 'limit';                      }
        elsif( $table_mapping->{BINDS}->{$bind_start}->{has_offset} ) { $where_proceeding_clause_mark = 'offset';                     }
        # TODO add FETCH
        # TODO add FOR <lock statement>
        elsif( $is_in_cte && defined( $next_cte_name )              ) { $where_proceeding_clause_mark = '\)\s*,\s*' . $next_cte_name; }
        elsif( $is_in_cte && !defined( $next_cte_name )             ) { $where_proceeding_clause_mark = '\)\s*select';                }
        else
        {
            warn "Could not determine proceeding where clause mark\n";
            return;
        }

        my $preceeding_query = substr( $new_q, 0, $bind_start );
        my $proceeding_query = substr( $new_q, $bind_end, length( $new_q ) - $bind_end );
        my $substituted_where = $bind_location;
        #print "---------------POS: $bind_start\n";
        #print "Binding\n$where_proceeding_clause_mark\nto\n$bind_location\n";
        $substituted_where =~ s/($where_proceeding_clause_mark)/${where_expression}$1/i;
        #print "---------------\n";
        $new_q = $preceeding_query . $substituted_where . $proceeding_query;

        $index++;
    }

    return $new_q;
}
my $handle = DBI->connect( $CONNECTION_MAP->{connection_string}, $CONNECTION_MAP->{user_name}, undef );

unless( $handle )
{
    die( "failed to connect\n" );
}

my $test_change = { 'public' => { 'tb_a' => { 'foo' => [ 1 ] }, 'tb_b' => { 'foo'=>[2,3]}, 'tb_c' => {'foo'=>[4,5]} } };
my $filter_tables = [ 'public.tb_a', 'public.tb_b', 'public.tb_c' ];
my $relcache = get_relcache( $handle );
my $table_mapping = {};
my $data = find_table_aliases( $handle, $relcache, $definition, $filter_tables, $table_mapping );
#print Dumper( $data );
#print Dumper( $table_mapping );
my $substituted_query = apply_filters( $data, $table_mapping, $definition, $test_change );
print "$substituted_query\n";
