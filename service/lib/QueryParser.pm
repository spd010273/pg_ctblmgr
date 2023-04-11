package QueryParser;

use strict;
use utf8;
use warnings;

# Can't use JSON:PP because it tried to redefine simple bools as a blessed
# class that other packages aren't aware of
use JSON::XS;
use Readonly;
use Params::Validate qw( :all );
use Data::Dumper;
use English qw( -no_match_vars );
use Data::Search; # Possibly replace with generic subroutine
use Perl6::Export::Attrs;
use FindBin;

use lib "$FindBin::Bin";

use DB;
use Util;

$OUTPUT_AUTOFLUSH = 1;

Readonly::Scalar my $OID_CACHE => <<END_SQL;
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

Readonly::Scalar my $GET_RELATION_TYPEMODS => <<END_SQL;
    SELECT a.attname::VARCHAR AS column,
           t.typname::VARCHAR AS type
      FROM pg_class c
INNER JOIN pg_attribute a
        ON a.attrelid = c.oid
INNER JOIN pg_type t
        ON t.oid = a.atttypid
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR = ?
     WHERE c.relname::VARCHAR = ?
END_SQL

Readonly::Scalar my $GET_PARSE_TREE => <<"END_SQL";
    SELECT ${SCHEMA_NAME}.fn_get_parse_tree(
        \$_\$__DEFINITION__\$_\$
    )::JSONB AS tree
END_SQL

my $PARSE_ERROR = 0;

sub get_query_parsetree($$) :Export( :MANDATORY )
{
    my( $handle, $definition ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
    );

    my $get_parse_tree_query = $GET_PARSE_TREE;
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

sub get_relcache($) :Export( :MANDATORY )
{
    my( $handle ) = validate_pos(
        @_,
        { type => OBJECT },
    );

    my $query = $OID_CACHE;

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

sub get_typmods($$$;$)
{
    my( $handle, $schema, $table, $column ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => SCALAR },
        { type => SCALAR },
        { type => SCALAR | UNDEF, optional => 1 },
    );

    my $query = $GET_RELATION_TYPEMODS;
    my @binds;

    push( @binds, $schema );
    push( @binds, $table );

    if( defined( $column ) )
    {
        $query .= 'AND a.attname::VARCHAR = ?';
        push( @binds, $column );
    }

    my $sth = try_query( $handle, $query, \@binds );

    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to get typmods for '$table'" );
    }

    if( $sth->rows() > 0 )
    {
        my $ret = { };

        while( my $row = $sth->fetchrow_hashref() )
        {
            my $column = $row->{column};
            my $type   = $row->{type};

            $ret->{$column} = $type;
        }

        return $ret;
    }

    return;
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
        $table_mapping->{CTES}->{$obj_name} = {
            parent   => $parent,
            location => $location
        };

        # CTEs get added to the rellist because we recurse before adding table
        # mapping for them. this cleans up rels that were added to the cte list
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
        my $target = $table_mapping->{FUNCTIONS}->{$obj_schema}->{$obj_name};
        if(
               !defined( $target )
            && !defined( $target->{$obj_alias} )
          )
        {
            $target->{$obj_alias} = [
                {
                    parent   => $parent,
                    location => $location
                }
            ];
            return;
        }

        if(
               defined( $parent )
            && not grep(
                   /^$parent$/,
                   @{$target->{$obj_alias}}
               )
          )
        {
            push(
                @{$target->{$obj_alias}},
                { parent=> $parent, location => $location }
            );
        }

        return;
    }

    if( grep( /^$obj_name$/, keys %{$table_mapping->{CTES}} ) )
    {
        return;
    }

    my $reltarg = $table_mapping->{RELS}->{$obj_schema}->{$obj_name};
    if(
           !defined( $reltarg )
        && !defined( $reltarg->{$obj_alias} )
      )
    {
        $reltarg->{$obj_alias} = [
            {
                parent   => $parent,
                location => $location
            }
        ];
        return;
    }

    if(
            defined( $parent )
         && not grep(
                /^$parent$/,
                @{$reltarg->{$obj_alias}}
            )
      )
    {
        push(
            @{$reltarg->{$obj_alias}},
            { parent => $parent, location => $location }
        );
        return;
    }

    return;
}

sub get_joined_rels($$$$;$)
{
    my(
        $json_fragment,
        $parent,
        $table_mapping,
        $relcache,
        $union_flag
      ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
        { type => SCALAR | UNDEF, optional => 1 },
    );

    #NOTE: We parse location to determine where the WHERE clause should go
    # Location parsing here is important if our parent statement does not
    # possess a WHERE clause. Chris note: Currently our location parsing gets
    # us CLOSE but it still include fragments of the join predicate, and we
    # need to possibly move the location 'forward' to skip past boolean
    # predicate expressions
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
        my @locs = datasearch(
            data   => $json_fragment->{quals},
            search => 'keys',
            find   => qr/location/
        );
        $SIG{__WARN__} = $old_warn;
        if( scalar( @locs ) > 0 )
        {
            foreach my $loc( @locs )
            {
                if(
                      !defined( $supplemental_location )
                   || $supplemental_location < $loc
                  )
                {
                    $supplemental_location = $loc;
                }
            }
        }
    }

    if(
          defined( $supplemental_location )
       && $supplemental_location > $location
      )
    {
        $location = $supplemental_location;
    }

    if( defined $json_fragment && defined( $json_fragment->{larg} ) )
    {
        my $from_list = &get_joined_rels(
            $json_fragment->{larg},
            $parent,
            $table_mapping,
            $relcache,
            $union_flag
        );

        if( defined( $json_fragment->{rarg} ) )
        {

            if( $json_fragment->{rarg}->{name} eq 'RANGEFUNCTION' )
            { # SRF Function
                # According to parsenodes.h - each element of this List is a
                # two element sublist:
                #   - first element being the untransformed function call tree
                #   -  second element being a possibly-empty list of ColumnDef
                #      nodes representing any columndef list attached to that
                #      function within the ROWS FROM() syntax

                my $function_call = $json_fragment->{rarg}->{functions}->[0]->[0];
                my $function_name = $function_call->{funcname}->[0];

                if( scalar( @{$function_call->{funcname}} ) > 1 )
                {
                    $function_name = $function_name
                                   . '.'
                                   . $function_call->{funcname}->[1];
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
                    $right_relation = $json_fragment->{rarg}->{schemaname}
                                    . '.'
                                    . $right_relation;
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
                                $relcache,
                                $union_flag
                            ),
                            type     => 'SUBSELECT',
                            location => $location,
                        }
                    }
                );
            }
            elsif( $json_fragment->{rarg}->{name} eq 'JOINEXPR' )
            {
                my $sub_join = &get_joined_rels(
                    $json_fragment->{rarg},
                    $parent,
                    $table_mapping,
                    $relcache,
                    $union_flag
                );

                foreach my $rel( @$sub_join )
                {
                    push(
                        @$from_list,
                        $rel
                    );
                }
            }
            else
            {
                warn(
                    'get_joined_rels: Unknown right RTE '
                  . "$json_fragment->{rarg}->{name}\n"
                );
                $PARSE_ERROR = 1;
                print Dumper( $json_fragment->{rarg} );
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
                $function_name = $function_name
                               . '.'
                               . $function_call->{funcname}->[1];
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
                $left_relation = $json_fragment->{schemaname}
                               . '.'
                               . $left_relation;
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
                        obj      => &parse_select(
                                        $json_fragment->{subquery},
                                        $parent,
                                        $table_mapping,
                                        $relcache,
                                        $union_flag
                                     ),
                        type     => 'SUBSELECT',
                        location => -1,
                    }
                }
            ];
        }
        else
        {
            warn(
                'get_joined_rels: Unknown recursed left RTE '
              . "$json_fragment->{name}\n"
            );
            $PARSE_ERROR = 1;
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
    my(
        $json_fragment,
        $parent,
        $table_mapping,
        $relcache,
        $is_rarg,
        $union_flag
      ) = validate_pos(
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
        # Regular union element - we're likely at an end element in the
        # union tree
        $union_flag = 'NONE' if( !defined( $union_flag ) );
        if( $is_rarg )
        {
            # for anchoring unions (final where clause) we need to know if this
            # is the last union member
            return [
                &parse_select(
                    $json_fragment,
                    $parent,
                    $table_mapping,
                    $relcache,
                    $union_flag
                )
            ];
        }

        return [
            &parse_select(
                $json_fragment,
                $parent,
                $table_mapping,
                $relcache,
                $union_flag
            )
        ];
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

        $union_flag = 'NONE' if( !defined( $union_flag ) );
        my $union_from_b = &parse_union(
            $json_fragment->{rarg},
            $parent,
            $table_mapping,
            $relcache,
            1,
            $union_flag
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
        warn(
            "parse_union: Invalid structure in $json_fragment->{name} node\n"
        );
        $PARSE_ERROR = 1;
    }

    return;
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

sub parse_from_clause($$$$;$)
{
    my(
        $json_fragment,
        $parent,
        $table_mapping,
        $relcache,
        $union_flag
      ) = validate_pos(
        @_,
        { type => ARRAYREF },
        { type => SCALAR | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
        { type => SCALAR | UNDEF, optional => 1 },
    );

    my $result = &get_joined_rels(
        $json_fragment->[0],
        $parent,
        $table_mapping,
        $relcache,
        $union_flag
    );

    return $result;
}

sub parse_select($$$$;$)
{
    my(
        $json_fragment,
        $parent,
        $table_mapping,
        $relcache,
        $union_flag
      ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR | UNDEF },
        { type => HASHREF },
        { type => HASHREF },
        { type => SCALAR | UNDEF, optional => 1 },
    );

    my $is_union_member;
    $is_union_member = $union_flag if( defined( $union_flag ) );
    #The conditionals around location here are to narrow down the location
    # (or possible location) of a WHERE clause
    if( $json_fragment->{name} ne 'SELECTSTMT' )
    {
        warn "parse_select: Invalid node $json_fragment->{name}\n";
        $PARSE_ERROR = 1;
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
            $relcache,
            $union_flag
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
                    my @locs = datasearch(
                        data   => $json_fragment->{whereClause},
                        search => 'keys',
                        find   => qr/location/
                    );
                    $SIG{__WARN__} = $old_warn;
                    my $new_where_start;
                    foreach my $loc( @locs )
                    {
                        next if( $loc == -1 );
                        if(
                               !defined( $new_where_start )
                            || $loc < $new_where_start
                          )
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
            my @locs = datasearch(
                data   => $json_fragment->{whereClause},
                search => 'keys',
                find   => qr/location/
            );
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
            # We need to find the END of the from clause to determine where
            # the WHERE clause should go
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
        my @locs = datasearch(
            data   => $json_fragment->{targetList},
            search => 'keys',
            find   => qr/location/
        );
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

    # Locate FROM-clause elements nested in $statement_info->{from} and unroll
    # them into table_mapping
    &recursive_from_finder( $relcache, $table_mapping, $statement_info );

    return $statement_info;
}

sub recursive_from_finder($$$)
{
    my( $relcache, $table_mapping, $json_fragment ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => HASHREF },
        { type => HASHREF },
    );

    if(
          defined( $json_fragment->{from} )
       && ref( $json_fragment->{from} ) eq 'ARRAY'
      )
    {
        my $where_start = $json_fragment->{where_start};
        foreach my $rel( @{$json_fragment->{from}} )
        {
            foreach my $alias( keys %$rel )
            {
                my $obj_name = $rel->{$alias}->{obj};
                my $qual;

                if( ref( $obj_name ) eq 'HASH' )
                {
                    &recursive_from_finder(
                        $relcache,
                        $table_mapping,
                        $obj_name
                    );
                }
                else
                {
                    $qual = resolve_relation( $relcache, $obj_name );
                    unless(
                               defined( $qual->{schema} )
                            && defined( $qual->{name} )
                          )
                    {
                        _log( $LOG_LEVEL_DEBUG, "Removing unresolvable relation $obj_name" );
                        next;
                    }
                    my $schema = $qual->{schema};
                    my $name   = $qual->{name};
                    $table_mapping->{BINDS}->{$where_start}->{rels}->{$schema}->{$alias}->{$name} = 1;
                }
            }
        }
    }

    return;
}

sub find_table_aliases($$$$$) :Export( :MANDATORY )
{
    my(
        $handle,
        $relcache,
        $definition,
        $filter_tables,
        $table_mapping
      ) = validate_pos(
        @_,
        { type => OBJECT   },
        { type => HASHREF  },
        { type => SCALAR   },
        { type => ARRAYREF },
        { type => HASHREF },
    );

    my $parse_tree_obj = get_query_parsetree( $handle, $definition );

    $PARSE_ERROR = 0;
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
    my $query_data = &parse_select(
        $statement,
        undef,
        $table_mapping,
        $relcache
    );

    if( $PARSE_ERROR )
    {
        print "There was an error parsing the following query's parse tree:\n";
        print "$definition\n";
        $PARSE_ERROR = 0;
    }

    return $query_data;
}

sub apply_filters($$$$$) :Export( :MANDATORY )
{
    my(
        $handle,
        $query_data,
        $table_mapping,
        $definition,
        $filters
      ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => HASHREF },
        { type => HASHREF },
        { type => SCALAR },
        { type => HASHREF },
    );

    # Lets use the filters we've received and search for the tables, their
    # aliases, and the objects they are present in within the query, then
    # attempt to modify the query such that we habe a filtered query
    # Phase I will result in a keyed array telling us which CTE or query will
    # need a filter applied
    my $where_expressions = {};

    foreach my $position( keys %{$table_mapping->{BINDS}} )
    {
        print "P: $position\n";
        next if( $position < 0 );

        my $RELS          = $table_mapping->{BINDS}->{$position}->{rels};
        my $where_entries = [];

        foreach my $schema( keys %$RELS )
        {
            print "S: $schema\n";
            foreach my $alias( keys %{$RELS->{$schema}} )
            {
                print "A: $alias\n";
                foreach my $table_name( keys %{$RELS->{$schema}->{$alias}} )
                {
                    print "T: $table_name\n";
                    if( defined( $filters->{$schema}->{$table_name} ) )
                    {
                        foreach my $key( keys %{$filters->{$schema}->{$table_name}} )
                        {
                            print "K: $key\n";
                            my $typmod = &get_typmods(
                                $handle,
                                $schema,
                                $table_name,
                                $key
                            );

                            if( !defined( $typmod ) )
                            {
                                warn(
                                    "Invalid column for table $schema."
                                  . "$table_name - $key. Column appears "
                                  . "to have no type\n"
                                );
                                next;
                            }

                            my $type        = $typmod->{$key};
                            my $where_entry = "${alias}.${key} ";

                            if( scalar( @{$filters->{$schema}->{$table_name}->{$key}} ) > 1 )
                            {
                                my $values = [];

                                foreach my $value( @{$filters->{$schema}->{$table_name}->{$key}} )
                                {
                                    push( @$values, "( '${value}' )::$type" );
                                }

                                $where_entry .= 'IN( '
                                              . join( ', ', @$values )
                                              .' ) ';
                            }
                            elsif( scalar( @{$filters->{$schema}->{$table_name}->{$key}} ) > 0 )
                            {
                                my $value     = $filters->{$schema}->{$table_name}->{$key}->[0];
                                $where_entry .= "= ( '${value}' )::$type";
                            }
                            else
                            {
                                warn "No binds for $schema.$table_name.$key\n";
                                next;
                            }

                            push( @$where_entries, $where_entry );
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
                $where_entry = ' AND ( ( '
                             . join( ' ) OR ( ', @$where_entries )
                             . ' ) ) ';
            }
            else
            {
                $where_entry = ' WHERE ( ( '
                             . join( ' ) OR ( ', @$where_entries )
                             . ' ) ) ';
            }

            $where_expressions->{$position} = $where_entry;
        }
    }

    my @starts = sort { $b <=> $a } keys( %{$table_mapping->{BINDS}} );
    # Assmple where expressions structure keyed based on the bind position
    # for much easier substitution later

    #print Dumper( $where_expressions ) if( $DEBUG );
    my $new_q = $definition;
    my $index = 0;

    foreach my $bind_start( @starts )
    {
        # Skip if unbindable (no relevent relations)
        next if( $bind_start < 0 );
        # Skip if no filters to be applied
        next if( !defined( $where_expressions->{$bind_start} ) );

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

        my $bind_location = substr(
            $new_q,
            $bind_start,
            $bind_end - $bind_start
        );
        # note for union parsing - we need to constrain by adding where_end
        # in select parsing :( also, we need to corelate against the
        # $table_mapping->binds itself

        # Find the end of the last expression (if has_where) or the last join predicate (if !has_where)
        my $where_expression = $where_expressions->{$bind_start};
        my $is_in_cte        = defined(
            $table_mapping->{BINDS}->{$bind_start}->{parent}
        );

        my $where_proceeding_clause_mark;
        my $BS_HASH = $table_mapping->{BINDS}->{$bind_start};
        my $replace_where = 0;

        if( $BS_HASH->{has_group} )
        {
            $where_proceeding_clause_mark = 'group\s+by';
        }
        elsif( $BS_HASH->{has_having} )
        {
            $where_proceeding_clause_mark = 'having';
        }
        # TODO add WINDOW
        elsif(
                  defined( $BS_HASH->{is_union} )
               && $BS_HASH->{is_union} ne 'NONE'
             )
        {
            $where_proceeding_clause_mark = '\s+' . lc( $BS_HASH->{is_union} );
        }
        elsif( defined( $BS_HASH->{is_union} ) )
        { # handle case where we union at the end of a CTE def
            # NOTE This may need to be expanded - there are many cases where unions can be used / abused and
            # a union can appear in the form of:
            $where_proceeding_clause_mark = '\)';
        }
        elsif( $BS_HASH->{has_sort} )
        {
            $where_proceeding_clause_mark = 'order\s+by';
        }
        elsif( $BS_HASH->{has_limit} )
        {
            $where_proceeding_clause_mark = 'limit';
        }
        elsif( $BS_HASH->{has_offset} )
        {
            $where_proceeding_clause_mark = 'offset';
        }
        # TODO add FETCH
        # TODO add FOR <lock statement>
        elsif( $is_in_cte && defined( $next_cte_name ) )
        {
            $where_proceeding_clause_mark = '\)\s*,\s*' . $next_cte_name;
        }
        elsif( $is_in_cte && !defined( $next_cte_name ) )
        {
            $where_proceeding_clause_mark = '\)\s*select';
        }
        elsif(
                 !$BS_HASH->{has_where}
              && !defined( $BS_HASH->{parent} )
             )
        {
            $where_proceeding_clause_mark = '$';
        }
        elsif( $BS_HASH->{has_where} )
        {
            $where_proceeding_clause_mark = 'where';
            $replace_where = 1;
        }
        else
        {
            warn "Could not determine proceeding where clause mark\n";
            print "Query fragment info:\n";
            print Dumper( $BS_HASH );
            $PARSE_ERROR = 1;
            #print Dumper( $table_mapping );
            return;
        }

        print "Proceeding mark: '$where_proceeding_clause_mark'\n";
        my $preceeding_query  = substr( $new_q, 0, $bind_start );
        my $proceeding_query = substr(
            $new_q,
            $bind_end,
            length( $new_q ) - $bind_end
        );
        my $substituted_where = $bind_location;

        if( $replace_where )
        {
            if( $substituted_where =~ m/where/i )
            {
                $substituted_where =~ s/($where_proceeding_clause_mark)/$1 TRUE ${where_expression} AND /i;
            }
            else
            {   # We've likly latched on a where clause element
                $substituted_where .= $where_expression;
            }
        }
        else
        {
            $substituted_where =~ s/($where_proceeding_clause_mark)/${where_expression}$1/i;
        }

        $new_q = $preceeding_query
               . $substituted_where
               . $proceeding_query;

        $index++;
    }

    print "$new_q\n";
    return $new_q;
}

1;
