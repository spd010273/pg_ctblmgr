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
           'r' AS type,
           NULL::OID AS dep
      FROM pg_class c
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
       AND n.nspname::VARCHAR != 'pg_toast'
     UNION ALL
    SELECT n.nspname::VARCHAR AS schema_name,
           p.proname::VARCHAR AS obj_name,
           p.oid,
           'f' AS type,
           NULL::OID AS dep
      FROM pg_proc p
INNER JOIN pg_namespace n
        ON n.oid = p.pronamespace
       AND n.nspname::VARCHAR != 'pg_toast'
     UNION ALL
    SELECT n.nspname::VARCHAR AS schema_name,
           c.relname::VARCHAR AS obj_name,
           c.oid,
           'p' AS type,
           inh.inhrelid AS dep
      FROM pg_class c
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
INNER JOIN pg_inherits inh
        ON inh.inhparent = c.oid
END_SQL

Readonly::Scalar my $FK_COLUMN_CACHE => <<END_SQL;
    SELECT n.nspname AS schema,
           c.relname AS table,
           a.attname AS column,
           nf.nspname AS foreign_schema,
           cf.relname AS foreign_table,
           array_agg( af.attname ) AS foreign_columns
      FROM pg_class c
INNER JOIN pg_attribute a
        ON a.attrelid = c.oid
       AND a.attnum > 0
       AND a.attisdropped IS FALSE
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
INNER JOIN pg_constraint co
        ON co.contype = 'f'
       AND co.conrelid = c.oid
       AND a.attnum = ANY( co.conkey )
INNER JOIN pg_class cf
        ON cf.oid = co.confrelid
INNER JOIN pg_attribute af
        ON af.attrelid = cf.oid
       AND af.attnum > 0
       AND af.attisdropped IS FALSE
       AND af.attnum = ANY( co.confkey )
INNER JOIN pg_namespace nf
        ON nf.oid = cf.relnamespace
  GROUP BY co.oid,
           n.nspname,
           c.relname,
           a.attname,
           nf.nspname,
           cf.relname
     UNION ALL
    SELECT n.nspname AS schema,
           c.relname AS table,
           a.attname AS column,
           n.nspname AS foreign_schema,
           c.relname AS foreign_table,
           array_agg( a.attname ) AS foreign_columns
      FROM pg_class c
INNER JOIN pg_attribute a
        ON a.attrelid = c.oid
       AND a.attnum > 0
       AND a.attisdropped IS FALSE
INNER JOIN pg_namespace n
        ON n.oid = c.relnamespace
INNER JOIN pg_constraint co
        ON co.contype = 'p'
       AND co.conrelid = c.oid
       AND a.attnum = ANY( co.conkey )
  GROUP BY n.nspname,
           c.relname,
           a.attname
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

    my $sth = try_query( $handle, $OID_CACHE, undef );

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
        my $dep    = $row->{dep};
        $cache->{rels}->{$schema}->{$name} = $oid if( $row->{type} eq 'r' );
        $cache->{func}->{$schema}->{$name} = $oid if( $row->{type} eq 'f' );
        $cache->{oid}->{$oid} = { schema => $schema, name => $name };

        if( $row->{type} eq 'p' )
        {
            if( !defined( $cache->{deps}->{$schema}->{$name} ) )
            {
                $cache->{deps}->{$schema}->{$name} = [ $dep ];
            }
            else
            {
                push( @{$cache->{deps}->{$schema}->{$name}}, $dep );
            }
        }
    }

    $sth->finish();

    $sth = try_query( $handle, $FK_COLUMN_CACHE, undef );

    unless( $sth )
    {
        _log( $LOG_LEVEL_FATAL, "Failed to get foreign keys for cache table" );
    }

    while( my $row = $sth->fetchrow_hashref() )
    {
        my $schema          = $row->{schema};
        my $table           = $row->{table};
        my $column          = $row->{column};
        my $foreign_schema  = $row->{foreign_schema};
        my $foreign_table   = $row->{foreign_table};
        my $foreign_columns = $row->{foreign_columns};

        foreach my $f_column( @$foreign_columns )
        {
            $cache->{fks}->{$schema}->{$table}->{$column}->{$foreign_schema}->{$foreign_table}->{$f_column} = 1;
        }
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

sub resolve_fk($$$$)
{
    my( $relcache, $schema, $table, $column ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR },
        { type => SCALAR },
        { type => SCALAR },
    );

    my $fks = {};
    unless(
               defined( $relcache->{fks}->{$schema} )
            && defined( $relcache->{fks}->{$schema}->{$table} )
            && defined( $relcache->{fks}->{$schema}->{$table}->{$column} )
          )
    {
        return undef;
    }

    my $keys = $relcache->{fks}->{$schema}->{$table}->{$column};

    foreach my $f_schema( keys %$keys )
    {
        foreach my $f_table( keys %{$keys->{$f_schema}} )
        {
            my $relname = "$f_schema.$f_table";

            if( !defined( $fks->{$relname} ) )
            {
                $fks->{$relname} = [];
            }

            foreach my $f_column( keys %{$keys->{$f_schema}->{$f_table}} )
            {
                push( @{$fks->{$relname}}, $f_column );
            }
        }
    }
    # For models with surrogate foreign keys, we expect keys to be 1:1, but using natural keys we can get odd multivariate results

    return $fks;
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

sub get_inheritance($$)
{
    my( $relcache, $relation ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALAR }
    );

    my $obj_data = resolve_relation( $relcache, $relation );

    return if( !defined( $obj_data ) );

    my $schema = $obj_data->{schema};
    my $name   = $obj_data->{name};

    # The passed-in item is assumed to be a parent of inheritence - so we need to locate all children
    my $ret = [];
    if(
           defined( $relcache->{deps}->{$schema} )
        && defined( $relcache->{deps}->{$schema}->{$name} )
      )
    {
        foreach my $dep( @{$relcache->{deps}->{$schema}->{$name}} )
        {
            my $dep_schema = $relcache->{oid}->{$dep}->{schema};
            my $dep_name   = $relcache->{oid}->{$dep}->{name};
            push( @$ret, "${dep_schema}.${dep_name}" );
        }
    }

    return $ret if( scalar( $ret ) > 0 );
    return;
}

sub add_table_mapping($)
{
    my( $map ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    my $relcache      = $map->{relcache};
    my $table_mapping = $map->{table_mapping};
    my $obj_name      = $map->{obj_name};
    my $obj_alias     = $map->{obj_alias};
    my $parent        = $map->{parent};
    my $location      = $map->{location};
    my $is_function   = $map->{is_function};
    my $is_cte        = $map->{is_cte};
    my $find_inh      = $map->{find_inh};

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

    $obj_name      = $obj_data->{name};
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

    if( $find_inh )
    {
        # uh i forgot why this is here
        if(
               defined( $relcache->{deps}->{$obj_schema} )
            && defined( $relcache->{deps}->{$obj_schema}->{$obj_name} )
          )
        {
            foreach my $dep( @{$relcache->{deps}->{$obj_schema}->{$obj_name}} )
            {
                my $dep_schema = $relcache->{oid}->{$dep}->{schema};
                my $dep_name   = $relcache->{oid}->{$dep}->{name};

                # XXX
                if( $table_mapping->{DEPS}->{$obj_schema}->{$obj_name} )
                {
                    push(
                        @{$table_mapping->{DEPS}->{$obj_schema}->{$obj_name}},
                        { schema => $dep_schema, name => $dep_name }
                    );
                }
                else
                {
                    $table_mapping->{DEPS}->{$obj_schema}->{$obj_name} = [ {
                        schema => $dep_schema,
                        name   => $dep_name
                    } ];
                }
            }
        }
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

sub get_dependent_column($$$;$)
{
    my( $json_fragment, $relcache, $alias, $relation ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => HASHREF },
        { type => SCALAR  },               # the outer relation's alias
        { type => SCALAR, optional => 1 }, # the outer relation's name
    );

    # Parsed join expression to determine dependent keys
    # For example, if we determine a relation is dependent on another (is outer join'd - right, left, or full outer)
    # we need to determine a way to filter the temp table later, so we examine the predicate used in the outer join
    # to determine which key(s) the outer table depends on. Concrete example:
    #
    #     SELECT f.baz
    #       FROM tb_foo f
    #  LEFT JOIN tb_bar b
    #         ON b.bar = f.foo
    #
    #  In this example, tb_bar is the outer relation and is dependent on tb_foo through the predicate
    #  `ON b.bar = f.foo`. We have determined that tb_bar is dependent on tb_foo outside of this function (look where this is called)
    #  but now  we need to examine the predicate.
    #
    #  Much later in the filtering process, when we're processing changes, we may receive a change to tb_bar. We cannot filter tb_bar directly
    #  as it is involved in an outer join. We can, however, use the nature of its join to reduce the set we'll be operating on.
    #  We would like to filter the definition query, given a change to tb_bar, using a statement such as:
    #
    #  SELECT bar AS foo FROM tb_bar WHERE <condition>
    #
    #  We can then treat an update of one or more key(s) in tb_bar to be an update to tb_foo instead, which we can then successfully filter
    #  without impacting the query's correctness (only the number of tuples output). This can safely be used on anti joins as well.

    my $schema;
    my $table;

    if( $relation )
    {
        my @components = split( '.', $relation );

        if( scalar( @components ) > 2 )
        {
            return; # screw this. someone used a period in their table/schema names. SELECT * FROM "try.renaming.youre"."stuff__pl.ease"
        }

        if( scalar( @components ) == 2 )
        {
            # we would have previously looked up the relation in the relcache so we should have the fully qual'd name
            $schema = shift( @components );
            $table  = shift( @components );
        }
        else
        {
            $table = shift( @components );
        }
    }

    my $exprs = [];
    if( exists( $json_fragment->{args} ) && ref( $json_fragment->{args} ) eq 'ARRAY' )
    {
        # compound boolean expression
        my $parsed_subexprs = [];
        foreach my $subfrag( @{$json_fragment->{args}} )
        {
            my $subexprs = &get_dependent_column( $subfrag, $relcache, $alias, $relation );
            if( defined $subexprs && ref( $subexprs ) eq 'ARRAY' && scalar( @$subexprs ) > 0 )
            {
                push( @$parsed_subexprs, @$subexprs );
            }
        }

        return $parsed_subexprs if( scalar( @$parsed_subexprs ) > 0 );
        return undef;
    }
    elsif( exists( $json_fragment->{lexpr} ) && exists( $json_fragment->{rexpr} ) )
    {
        if( !exists( $json_fragment->{name} ) || ref( $json_fragment->{name} ) ne 'ARRAY' || $json_fragment->{name}->[0] ne '=' )
        {
            _log( $LOG_LEVEL_WARNING, "Failed to parse outer join predicate - no equality op" );
            return;
        }

        my $subexpr = {};
        if( $json_fragment->{lexpr}->{name} eq 'COLUMNREF' )
        {
            if( exists( $json_fragment->{lexpr}->{fields} ) && ref( $json_fragment->{lexpr}->{fields} ) eq 'ARRAY' )
            {
                $subexpr->{l} = $json_fragment->{lexpr}->{fields};
            }
        }

        if( $json_fragment->{rexpr}->{name} eq 'COLUMNREF' )
        {
            if( exists( $json_fragment->{rexpr}->{fields} ) && ref( $json_fragment->{rexpr}->{fields} ) eq 'ARRAY' )
            {
                $subexpr->{r} = $json_fragment->{rexpr}->{fields};
            }
        }

        if( defined $subexpr->{r} && defined $subexpr->{l} )
        {
            push( @$exprs, $subexpr );
        }
    }

    # We've filtered down to equality ops, we hope for one or more possible comparisons IE:
    #  ...
    # LEFT JOIN tb_foo f
    #        ON f.foo = a.foo  <- exprs will contain this
    #       AND f.foo = b.foo  <- exprs will contain this
    #       AND TRUE
    #       AND f.foo > random()::INTEGER

    my $parsed_exprs = [];
    foreach my $expr( @$exprs )
    {
        next unless( exists( $expr->{l} ) && exists( $expr->{r} ) );
        my $parsed_expr  = {};
        my $left_column  = $expr->{l}->[scalar(@{$expr->{l}}) - 1];
        my $right_column = $expr->{r}->[scalar(@{$expr->{r}}) - 1];

        if( $schema )
        {
            if( $expr->{l}->[0] eq $schema )
            {
                if( defined( $table ) && $expr->{l}->[1] eq $table )
                {
                    $parsed_expr->{outer}->{column} = $left_column;
                    $parsed_expr->{outer}->{alias}  = $alias;
                    $parsed_expr->{outer}->{rel}    = $relation if( $relation );

                    $parsed_expr->{other}->{column} = $right_column;

                    if( scalar( @{$expr->{r}} ) == 2 )
                    {
                        #could be an alias or table name
                        my $rel = resolve_relation( $relcache, $expr->{r}->[0] );

                        if( $rel )
                        {
                            $parsed_expr->{other}->{rel} = $rel->{schema} . '.' . $rel->{name};
                        }
                        else
                        {
                            $parsed_expr->{other}->{alias} = $expr->{r}->[0];
                        }
                    }
                    elsif( scalar( @{$expr->{r}} ) == 3 )
                    {
                        $parsed_expr->{other}->{rel} = $expr->{r}->[0] . '.' . $expr->{r}->[1];
                    }
                }
            }
            elsif( $expr->{r}->[0] eq $schema )
            {
                if( defined( $table ) && $expr->{r}->[1] eq $table )
                {
                    $parsed_expr->{outer}->{column} = $right_column;
                    $parsed_expr->{outer}->{alias}  = $alias;
                    $parsed_expr->{outer}->{rel}    = $relation if( $relation );

                    $parsed_expr->{other}->{column} = $left_column;

                    if( scalar( @{$expr->{l}} ) == 2 )
                    {
                        my $rel = resolve_relation( $relcache, $expr->{l}->[0] );

                        if( $rel )
                        {
                            $parsed_expr->{other}->{rel} = $rel->{schema} . '.' . $rel->{name};
                        }
                        else
                        {
                            $parsed_expr->{other}->{alias} = $expr->{l}->[0];
                        }
                    }
                    elsif( scalar( @{$expr->{l}} ) == 3 )
                    {
                        $parsed_expr->{other}->{rel} = $expr->{l}->[0] . '.' . $expr->{l}->[1];
                    }
                }
            }
        }
        else
        {
            if( ( defined( $table ) && $expr->{l}->[0] eq $table ) || $expr->{l}->[0] eq $alias )
            {
                $parsed_expr->{outer}->{column} = $left_column;
                $parsed_expr->{outer}->{alias}  = $alias;
                $parsed_expr->{outer}->{rel}    = $relation if( $relation );

                $parsed_expr->{other}->{column} = $right_column;

                if( scalar( @{$expr->{r}} ) == 2 )
                {
                    my $rel = resolve_relation( $relcache, $expr->{r}->[0] );

                    if( $rel )
                    {
                        $parsed_expr->{other}->{rel} = $rel->{schema} . '.' . $rel->{name};
                    }
                    else
                    {
                        $parsed_expr->{other}->{alias} = $expr->{r}->[0];
                    }
                }
                elsif( scalar( @{$expr->{r}} ) == 3 )
                {
                    $parsed_expr->{other}->{rel} = $expr->{r}->[0] . '.' . $expr->{r}->[1];
                }
            }
            elsif( ( defined( $table ) && $expr->{r}->[0] eq $table ) || $expr->{r}->[0] eq $alias )
            {
                $parsed_expr->{outer}->{column} = $right_column;
                $parsed_expr->{outer}->{alias}  = $alias;
                $parsed_expr->{outer}->{rel}    = $relation if( $relation );

                $parsed_expr->{other}->{column} = $left_column;

                if( scalar( @{$expr->{l}} ) == 2 )
                {
                    my $rel = resolve_relation( $relcache, $expr->{l}->[0] );

                    if( $rel )
                    {
                        $parsed_expr->{other}->{rel} = $rel->{schema} . '.' . $rel->{name};
                    }
                    else
                    {
                        $parsed_expr->{other}->{alias} = $expr->{l}->[0];
                    }
                }
                elsif( scalar( @{$expr->{l}} ) == 3 )
                {
                    $parsed_expr->{other}->{rel} = $expr->{l}->[0] . '.' . $expr->{l}->[1];
                }
            }
        }

        next unless( exists( $parsed_expr->{outer} ) && exists( $parsed_expr->{other} ) );
        push( @$parsed_exprs, $parsed_expr );
    }

    return undef if( scalar( @$parsed_exprs ) == 0 );
    return $parsed_exprs;
}

sub get_joined_rels($$)
{
    my( $map, $number ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => SCALARREF },
    );

    my $json_fragment = $map->{fragment};
    my $parent        = $map->{parent};
    my $table_mapping = $map->{table_mapping};
    my $relcache      = $map->{relcache};
    my $union_flag    = $map->{union_flag};
    my $is_outer      = $map->{is_outer};

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

    my $outer_dep      = '';
    my $right_is_outer = 0;
    my $left_is_outer  = 0;
    my $join_expr;

    if( defined $json_fragment && defined( $json_fragment->{larg} ) )
    {
        # Note on handedness - left / right is reference to larg / rarg. A relation is considered outer if it is the non-inner joined side of the relation
        # IE:
        #     SELECT foo
        #       FROM tb_bar b
        #  LEFT JOIN tb_foo f <- This is the outer relation, and would be rarg, right_is_outer would be set
        #         ON b.baz = f.baz
        #
        #     SELECT foo
        #       FROM tb_bar b <- This is the outer relation, and would be larg. left_is_outer would be set
        # RIGHT JOIN tb_foo f
        #         ON f.baz = b.baz
        if( defined( $json_fragment->{name} ) && $json_fragment->{name} eq 'JOINEXPR' )
        {
            if(
                  defined( $json_fragment->{jointype} )
               && $json_fragment->{jointype} ne 'INNER' # X join Y, X natural join Y, X inner join Y
              )
            {
                # We've located an outer join'd relation(s)
                if( $json_fragment->{jointype} eq 'LEFT' )
                {
                    $right_is_outer = 1;
                    $outer_dep      = 'l';
                    $join_expr      = $json_fragment->{quals};
                }
                elsif( $json_fragment->{jointype} eq 'RIGHT' )
                {
                    $left_is_outer  = 1;
                    $outer_dep      = 'r';
                    $join_expr      = $json_fragment->{quals};
                }
                elsif( $json_fragment->{jointype} eq 'FULL' )
                {
                    $right_is_outer = 1;
                    $left_is_outer  = 1;
                    $outer_dep      = 'b';
                    $join_expr      = $json_fragment->{quals};
                }
                else
                {
                    _log( $LOG_LEVEL_WARNING, "Unimplemented join type detected: $json_fragment->{jointype}" );
                }

                # XXX We need to figure out the join predicate

            }
            elsif(
                    defined( $json_fragment->{jointype} )
                 && $json_fragment->{jointype} eq 'INNER'
                 )
            {
                $left_is_outer  = 0;
                $right_is_outer = 0;
                $is_outer       = 0;
                $outer_dep      = '';
                undef( $join_expr );
            }
        }

        my $from_list = &get_joined_rels(
            {
                fragment      => $json_fragment->{larg},
                parent        => $parent,
                table_mapping => $table_mapping,
                relcache      => $relcache,
                union_flag    => $union_flag,
                is_outer      => $left_is_outer,
                join_expr     => $join_expr,
            },
            $number
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

                if( defined( $function_call->{rarg}->{location} ) && $location < $function_call->{rarg}->{location} )
                {
                    $location = $function_call->{rarg}->{location};
                }

                $$number++;
                my $frag = {
                    $function_alias => {
                        obj      => $function_name,
                        type     => 'FUNCTION',
                        location => $location,
                        number   => $$number,
                    }
                };

                if( $is_outer || $right_is_outer )
                {
                    $frag->{$function_alias}->{is_outer} = 1;
                    $frag->{$function_alias}->{outer_dep} = $outer_dep;
                    $frag->{$function_alias}->{expr} = get_dependent_column( $join_expr, $relcache, $function_alias ) if( $join_expr );
                }

                push(
                    @$from_list,
                    $frag
                );

                add_table_mapping(
                    {
                        relcache      => $relcache,
                        table_mapping => $table_mapping,
                        obj_name      => $function_name,
                        obj_alias     => $function_alias,
                        parent        => $parent,
                        location      => $location,
                        is_function   => 1,
                        is_cte        => 0,
                        find_inh      => 0
                    }
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
                my $inh;
                my $find_inh = 0;
                if(
                       defined( $json_fragment->{rarg}->{inh} )
                    && $json_fragment->{rarg}->{inh}
                  )
                {
                    $inh = get_inheritance( $relcache, $right_relation );
                }

                if( defined( $json_fragment->{rarg}->{alias} ) )
                {
                    $alias = $json_fragment->{rarg}->{alias}->{aliasname};
                }

                if( $location < $json_fragment->{rarg}->{location} )
                {
                    $location = $json_fragment->{rarg}->{location};
                }

                $$number++;
                my $frag = {
                    $alias => {
                        obj      => $right_relation,
                        type     => 'RELATION',
                        location => $location,
                        number   => $$number,
                    }
                };

                if( defined( $inh ) && scalar( @$inh ) > 0 )
                {
                    $frag->{$alias}->{inh} = $inh;
                    $find_inh = 1;
                }

                if( $is_outer || $right_is_outer )
                {
                    $frag->{$alias}->{is_outer} = 1;
                    $frag->{$alias}->{outer_dep} = $outer_dep;
                    $frag->{$alias}->{expr} = get_dependent_column( $join_expr, $relcache, $alias, $right_relation ) if( $join_expr );
                }

                push(
                    @$from_list,
                    $frag
                );

                add_table_mapping(
                    {
                        relcache      => $relcache,
                        table_mapping => $table_mapping,
                        obj_name      => $right_relation,
                        obj_alias     => $alias,
                        parent        => $parent,
                        location      => $location,
                        is_function   => 0,
                        is_cte        => 0,
                        find_inh      => $find_inh,
                    }
                );
            }
            elsif( $json_fragment->{rarg}->{name} eq 'RANGESUBSELECT' )
            {
                my $alias = $json_fragment->{rarg}->{alias}->{aliasname};
                if( $location < $json_fragment->{rarg}->{location} )
                {
                    $location = $json_fragment->{rarg}->{location};
                }

                $$number++;

                my $frag = {
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
                        number   => $$number,
                    }
                };

                if( $is_outer || $right_is_outer )
                {
                    $frag->{$alias}->{is_outer} = 1;
                    $frag->{$alias}->{outer_dep} = $outer_dep;
                    $frag->{$alias}->{expr} = get_dependent_column( $join_expr, $relcache, $alias ) if( $join_expr );
                }

                push( @$from_list, $frag );
            }
            elsif( $json_fragment->{rarg}->{name} eq 'JOINEXPR' )
            {
                my $sub_join = &get_joined_rels(
                    {
                        fragment      => $json_fragment->{rarg},
                        parent        => $parent,
                        table_mapping => $table_mapping,
                        relcache      => $relcache,
                        union_flag    => $union_flag,
                        is_outer      => $right_is_outer,
                        join_expr     => $join_expr,
                    },
                    $number
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
                {
                    relcache      => $relcache,
                    table_mapping => $table_mapping,
                    obj_name      => $function_name,
                    obj_alias     => $function_alias,
                    parent        => $parent,
                    location      => $location,
                    is_function   => 1,
                    is_cte        => 0,
                    find_inh      => 0
                }
            );

            $$number++;
            my $frag = {
                $function_alias => {
                    obj => $function_name,
                    type => 'FUNCTION',
                    location => $json_fragment->{location},
                    number   => $$number,
                }
            };

            if( $is_outer || $left_is_outer )
            {
                $outer_dep = 'r' unless( $outer_dep && length( $outer_dep ) > 0 );
                $frag->{$function_alias}->{is_outer} = 1;
                $frag->{$function_alias}->{outer_dep} = $outer_dep;
                if( $join_expr || $map->{join_expr} )
                {
                    $frag->{$function_alias}->{expr} = get_dependent_column( $map->{join_expr}, $relcache, $function_alias ) if( $is_outer );
                    $frag->{$function_alias}->{expr} = get_dependent_column( $join_expr, $relcache, $function_alias ) if( $left_is_outer );
                }
            }

            return [ $frag ];
        }
        elsif( $json_fragment->{name} eq 'RANGEVAR' )
        {
            my $left_relation = $json_fragment->{relname};
            my $alias         = $left_relation;
            my $find_inh      = 0;

            # TODO, add support for partitions / inheritance here
            $find_inh = 1 if( defined( $json_fragment->{inh} ) && $json_fragment->{inh} );

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
                {
                    relcache      => $relcache,
                    table_mapping => $table_mapping,
                    obj_name      => $left_relation,
                    obj_alias     => $alias,
                    parent        => $parent,
                    location      => $location,
                    is_function   => 0,
                    is_cte        => 0,
                    find_inh      => $find_inh
                }
            );

            $$number++;
            my $frag = {
                    $alias => {
                        obj      => $left_relation,
                        type     => 'RELATION',
                        location => $json_fragment->{location},
                        number   => $$number,
                    }
            };

            if( $find_inh )
            {
                $frag->{$alias}->{inh} = get_inheritance( $relcache, $left_relation );
                # XXX another spot to inject deps
            }

            if( $is_outer || $left_is_outer )
            {
                $outer_dep = 'r' unless( $outer_dep && length( $outer_dep ) > 0 );
                $frag->{$alias}->{is_outer} = 1;
                $frag->{$alias}->{outer_dep} = $outer_dep;
                if( $join_expr || $map->{join_expr} )
                {
                    $frag->{$alias}->{expr} = get_dependent_column( $map->{join_expr}, $relcache, $alias, $left_relation ) if( $is_outer );
                    $frag->{$alias}->{expr} = get_dependent_column( $join_expr, $relcache, $alias, $left_relation ) if( $left_is_outer );
                }
            }

            return [ $frag ];
        }
        elsif( $json_fragment->{name} eq 'RANGESUBSELECT' )
        {
            my $alias = $json_fragment->{alias}->{aliasname};
            $$number++;
            my $frag = {
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
                        number   => $$number,
                    }
            };

            if( $is_outer || $left_is_outer )
            {
                $outer_dep = 'r' unless( $outer_dep && length( $outer_dep ) > 0 );
                $frag->{$alias}->{is_outer} = 1;
                $frag->{$alias}->{outer_dep} = $outer_dep;
                if( $join_expr || $map->{join_expr} )
                {
                    $frag->{$alias}->{expr} = get_dependent_column( $map->{join_expr}, $relcache, $alias ) if( $is_outer );
                    $frag->{$alias}->{expr} = get_dependent_column( $join_expr, $relcache, $alias ) if( $left_is_outer );
                }
            }

            return [ $frag ];
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
            {
                relcache      => $relcache,
                table_mapping => $table_mapping,
                obj_name      => $local_parent,
                obj_alias     => undef,
                parent        => $parent,
                location      => $location,
                is_function   => 0,
                is_cte        => 1,
                find_inh      => 0
            }
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

    my $recur_number = 0;
    my $result = &get_joined_rels(
        {
            fragment      => $json_fragment->[0],
            parent        => $parent,
            table_mapping => $table_mapping,
            relcache      => $relcache,
            union_flag    => $union_flag,
            is_outer      => 0,
        },
        \$recur_number
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

sub check_cte_has_relation($$$$)
{
    my( $table_mapping, $relcache, $dep_rel, $rel ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => HASHREF },
        { type => SCALAR },
        { type => SCALAR },
    );

    my $relinfo = resolve_relation( $relcache, $rel );

    unless( defined( $relinfo ) )
    {
        return 0;
    }

    foreach my $bindpoint( keys %{$table_mapping->{BINDS}} )
    {
        my $bindinfo = $table_mapping->{BINDS}->{$bindpoint};
        if(
                defined( $bindinfo->{rels} )
             && defined( $bindinfo->{rels}->{$relinfo->{schema}} )
          )
        {
            foreach my $alias( keys %{$bindinfo->{rels}->{$relinfo->{schema}}} )
            {
                if( defined( $bindinfo->{rels}->{$relinfo->{schema}}->{$alias}->{$relinfo->{name}} ) )
                {
                    return { alias => $alias };
                }
            }
        }
    }

    return 0;
}

sub recursive_from_finder($$$)
{
    my( $relcache, $table_mapping, $json_fragment ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => HASHREF },
        { type => HASHREF },
    );

    # NOTE that we CANNOT allow outer joins to have binds
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
                    ## New change - marker values are now multi-purpose:
                    # 1 = free to bind to this relation
                    # ARRAYREF = additional information to handle outer joins - bind with caution
                    # 0 = outer join with no additional information - DO NOT BIND
                    my $marker = 1;

                    $qual = resolve_relation( $relcache, $obj_name );

                    if( defined( $rel->{$alias}->{is_outer} ) && $rel->{$alias}->{is_outer} eq 1 )
                    {
                        $marker     = 0; # Outer join - set to DO NOT BIND unless we can satisfy requirements
                        my $number  = $rel->{$alias}->{number};
                        my $next    = $rel->{$alias}->{outer_dep}; # l = number - 1, r = number + 1, b = number - 1
                        my $desired;

                        if( $next eq 'r' )
                        {
                            $desired = $number + 1;
                        }
                        elsif( $next eq 'b' || $next eq 'l' )
                        {
                            $desired = $number - 1;
                        }


                        if( defined( $rel->{$alias}->{expr} ) && scalar( @{$rel->{$alias}->{expr}} ) > 0 && $desired )
                        {
                            my $cannot_bind = 0;
                            foreach my $expr( @{$rel->{$alias}->{expr}} )
                            {
                                my $other_alias  = $expr->{other}->{alias};
                                my $other_column = $expr->{other}->{column};
                                my $dep_obj;

                                REL: foreach my $next_rel( @{$json_fragment->{from}} )
                                {
                                    if( defined( $next_rel->{$other_alias} ) )
                                    {
                                        $dep_obj = $next_rel->{$other_alias}->{obj};
                                        last REL if( $dep_obj );
                                        #locate by alias
                                    }
                                    else
                                    {
                                        #locate by number
                                        foreach my $next_alias( keys %$next_rel )
                                        {
                                            if( $next_rel->{$next_alias}->{number} && $next_rel->{$next_alias}->{number} == $desired )
                                            {
                                                if( $next_alias ne $other_alias )
                                                {
                                                    # this can happen for subselects / weirdly nested expressions - solve on a case-by-case
                                                    _log(
                                                        $LOG_LEVEL_WARNING,
                                                        "Skipping risky outer handling for relation $expr->{outer}->{rel} "
                                                      . "with alias $expr->{outer}->{alias}"
                                                    );
                                                    last REL;
                                                }

                                                $dep_obj = $next_rel->{$next_alias}->{obj};

                                                last REL;
                                            }
                                        }

                                    }
                                }

                                if( defined( $dep_obj ) )
                                {
                                    # setup $marker to have a mapping for this expression
                                    if( ref( $dep_obj ) eq 'HASH' )
                                    {
                                        # XXX This may need special handling - we may not be able to map inverse from multiple relations
                                        my $fake_table_mapping = {};
                                        &recursive_from_finder(
                                            $relcache,
                                            $fake_table_mapping,
                                            $dep_obj
                                        );

                                        if( scalar( keys %{$fake_table_mapping->{BINDS}} ) > 1 )
                                        {
                                            _log(
                                                $LOG_LEVEL_WARNING,
                                                "Unfilterable outer join from $expr->{outer}->{rel}, alias $expr->{outer}->{alias}"
                                            );
                                            next;
                                        }

                                        my @keys     = keys %{$fake_table_mapping->{BINDS}};
                                        my $only_key = shift @keys;
                                        $dep_obj     = $fake_table_mapping->{BINDS}->{$only_key}->{rels};
                                    }

                                    my $dep_info   = resolve_relation( $relcache, $dep_obj );
                                    my $outer_info = resolve_relation( $relcache, $expr->{outer}->{rel} );

                                    if( !defined( $outer_info ) || !defined( $outer_info->{schema} ) || !defined( $outer_info->{name} ) )
                                    {
                                        _log( $LOG_LEVEL_DEBUG, "Could not resolve outer joined relation $expr->{outer}->{rel}" );
                                        next;
                                    }

                                    my $target_alias    = $other_alias;
                                    my $target_schema   = $dep_info->{schema};
                                    my $target_relation = $dep_info->{name};
                                    my $target_column   = $other_column;

                                    my $dep_in_cte;
                                    if( !defined( $dep_info ) || !defined( $dep_info->{schema} ) || !defined( $dep_info->{name} ) )
                                    {
                                        # We have an outer join that is dependent on a CTE - do extra checks to see if this can be resolved
                                        my $info = &resolve_fk(
                                            $relcache,
                                            $outer_info->{schema},
                                            $outer_info->{name},
                                            $expr->{outer}->{column}
                                        );

                                        $cannot_bind = 1;

                                        if( defined( $info ) && defined( $table_mapping->{CTES}->{$dep_obj} ) )
                                        {
                                            # sanity check the target CTE expression to verify that the resolved FK info actually exists there
                                            foreach my $rel( keys %$info )
                                            {
                                                my $alternate_rels;
                                                my $result = &check_cte_has_relation(
                                                    $table_mapping,
                                                    $relcache,
                                                    $dep_obj,
                                                    $rel
                                                );
                                                my $relinfo     = resolve_relation( $relcache, $rel );
                                                $alternate_rels = get_inheritance( $relcache, $rel ) if( !$result );

                                                if(
                                                       !$result
                                                    && defined( $alternate_rels )
                                                    && scalar( @$alternate_rels ) > 0
                                                  )
                                                {
                                                    # Check the case where inherited relations are present, this can happen with self joins
                                                    foreach my $alternate_rel( @$alternate_rels )
                                                    {
                                                        $result = &check_cte_has_relation(
                                                            $table_mapping,
                                                            $relcache,
                                                            $dep_obj,
                                                            $alternate_rel
                                                        );

                                                        if( ref( $result ) eq 'HASH' && $result->{alias} )
                                                        {
                                                            $target_alias = $result->{alias};
                                                        }

                                                        $relinfo = resolve_relation( $relcache, $alternate_rel ) if( $result );
                                                    }
                                                }

                                                if( $result && $relinfo )
                                                {
                                                    $dep_in_cte      = 1;
                                                    $cannot_bind     = 0;
                                                    $dep_in_cte      = $dep_obj;
                                                    $target_relation = $relinfo->{name};
                                                    $target_schema   = $relinfo->{schema};
                                                    $target_alias    = $result->{alias};
                                                }
                                            }
                                        }

                                        if( $cannot_bind )
                                        {
                                            _log(
                                                $LOG_LEVEL_WARNING,
                                                "Could not resolve outer target relation $dep_obj from $expr->{outer}->{rel}."
                                              . " Filters will not be able to be applied for this relation and updates will be slow."
                                              . " Consider using a resolvable relation between $expr->{outer}->{rel} and $dep_obj rather"
                                              . " than joining directly to $dep_obj"
                                            );
                                            next;
                                        }

                                        # XXX We need to add information that allows the bind data to be displaced to the correct CTE
                                    }

                                    # We have sufficient info, convert to arrayref of bind info
                                    $marker = [] if( ref( $marker ) eq '' );
                                    my $is_inh = 0;
                                    if( $rel->{$alias}->{inh} && scalar( @{$rel->{$alias}->{inh}} ) > 0 )
                                    {
                                        $is_inh = 1;
                                    }
                                    push(
                                        @$marker,
                                        {
                                            target_relation => $target_relation,
                                            target_column   => $target_column,
                                            target_alias    => $target_alias,
                                            target_schema   => $target_schema,
                                            outer_relation  => $outer_info->{name},
                                            outer_schema    => $outer_info->{schema},
                                            outer_alias     => $expr->{outer}->{alias},
                                            outer_column    => $expr->{outer}->{column},
                                            is_inh          => $is_inh,
                                            dep_in_cte      => $dep_in_cte,
                                        }
                                    );
                                }
                                else
                                {
                                    _log( $LOG_LEVEL_WARNING, "Failed to find outer predicate expression dependency" );
                                    print Dumper( $expr );
                                }
                            }

                            if( $cannot_bind )
                            {
                                $marker = 0;
                            }
                        }
                    }

                    unless(
                               defined( $qual->{schema} )
                            && defined( $qual->{name} )
                          )
                    {
                        # We're here because the object is likely either a CTE or temp relation
                        _log(
                            $LOG_LEVEL_DEBUG,
                            "Removing unresolvable relation $obj_name"
                        );
                        next;
                    }
                    my $schema = $qual->{schema};
                    my $name   = $qual->{name};
                    $table_mapping->{BINDS}->{$where_start}->{rels}->{$schema}->{$alias}->{$name} = $marker;

                    if( defined( $rel->{$alias}->{inh} ) )
                    {
                        foreach my $qual( @{$rel->{$alias}->{inh}} )
                        {
                            my $obj_data   = resolve_relation( $relcache, $qual );
                            my $dep_schema = $obj_data->{schema};
                            my $dep_name   = $obj_data->{name};

                            $table_mapping->{BINDS}->{$where_start}->{rels}->{$dep_schema}->{$alias}->{$dep_name} = $marker;
                        }
                    }
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

    # lets do a fixup for outer joined relations
    return $query_data;
}

sub bind_filters($)
{
    my( $map ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    my $handle   = $map->{handle};
    my $filters  = $map->{filters};
    my $schema   = $map->{schema};
    my $relation = $map->{relation};
    my $alias    = $map->{alias};
    my $entries  = [];

    foreach my $key( keys( %{$filters->{$schema}->{$relation}} ) )
    {
        my $typmod = &get_typmods(
            $handle,
            $schema,
            $relation,
            $key
        );

        if( !defined( $typmod ) )
        {
            warn(
                "Invalid column for relation $relation - "
              . "$key. Column appears to have no type\n"
            );
            next;
        }

        my $type = $typmod->{$key};
        my $vals;
        my $where_entry;
        my $vals_array = $filters->{$schema}->{$relation}->{$key};
        $where_entry   = "${alias}." if( $alias );
        $where_entry  .= $key;

        if( scalar( @$vals_array ) > 1 )
        {
            $vals = ' IN( '
                  . join(
                        ', ',
                        map { "'${_}'::${type}" } @$vals_array
                    )
                  . ' )';
        }
        elsif( scalar( @$vals_array ) > 0 )
        {
            $vals = " = ('" .  $vals_array->[0] . "' )::$type ";
        }
        else
        {
            warn "No binds for $schema.$relation.$key\n";
            next;
        }

        $where_entry .= $vals;
        push( @$entries, $where_entry );
    }

    return $entries if( scalar( @$entries ) > 0 );
    return undef;
}

sub chain_assembler($)
{
    my( $chain ) = validate_pos(
        @_,
        { type => HASHREF },
    );

    my $results = [];
    my $move_to;

    foreach my $chain_tail( @{$chain->{__ENDS__}} )
    {
        my $chain_end   = $chain_tail->{q};
        my $chain_alias = $chain_tail->{alias};
        my $length = 0;
        my $next_q = $chain->{$chain_end}->{q};

        $move_to = $chain_tail->{cte} if( $chain_tail->{cte} );

        while( defined( $next_q ) && $next_q ne $chain->{__START__} )
        {
            $length++;
            $chain_end = "${chain_end} ${next_q}";
            $next_q    = $chain->{$next_q}->{q};
        }

        if( $length == 0 )
        {
            $chain_end .= ' ' . $next_q if( $next_q );
            $length++;
        }

        $chain_end = "${chain_alias}.${chain_end}" if( $chain_alias );
        $chain_end .= ( ')' x $length );
        push( @$results, $chain_end );
    }

    my $frag = { q => $results };
    $frag->{move_to} = $move_to if( $move_to );
    print Dumper( $frag );
    return $frag;
}

sub recursive_bind_helper($$)
{
    my( $map, $chain ) = validate_pos(
        @_,
        { type => HASHREF },
        { type => HASHREF },
    );

    my $filters   = $map->{filters};
    my $next_bind = $map->{next_bind};
    my $handle    = $map->{handle};
    my $RELS      = $map->{RELS};
    my $BINDS     = $map->{BINDS};

    return unless( defined( $next_bind ) && ref( $next_bind ) eq 'ARRAY' );

    my $result_queries = [];

    foreach my $bind_info( @$next_bind )
    {
        my $outer_schema    = $bind_info->{outer_schema};
        my $outer_relation  = $bind_info->{outer_relation};
        my $outer_column    = $bind_info->{outer_column};
        my $outer_alias     = $bind_info->{outer_alias};
        my $target_schema   = $bind_info->{target_schema};
        my $target_relation = $bind_info->{target_relation};
        my $target_column   = $bind_info->{target_column};
        my $target_alias    = $bind_info->{target_alias};
        my $is_inh          = $bind_info->{is_inh};
        my $dep_in_cte      = $bind_info->{dep_in_cte};
        my $ONLY            = 'ONLY';

        $ONLY = '' if( defined( $is_inh ) && $is_inh == 1 );
        my $q = <<"END_SQL";
$target_column IN( SELECT $outer_column FROM $ONLY $outer_schema.$outer_relation WHERE
END_SQL
        my $new_rels = $RELS;

        if( $filters )
        {
            # ONLY SET for the first call
            my $binds = &bind_filters( {
                handle   => $handle,
                filters  => $filters,
                schema   => $outer_schema,
                relation => $outer_relation,
            } );

            $q .= ' ' . join( ' AND ', @$binds ) . ') ' if( $binds );;
            $chain->{__START__} = $q;
        }

        my $cte;
        if( $bind_info->{dep_in_cte} )
        {
            foreach my $pos( keys %$BINDS )
            {
                if( $BINDS->{$pos}->{parent} && $BINDS->{$pos}->{parent} eq $bind_info->{dep_in_cte} )
                {
                    ## XXX context switching here
                    $new_rels = $BINDS->{$pos}->{rels};
                    $cte = $bind_info->{dep_in_cte};
                    last;
                }
            }
        }

        my $q_frag = { q => $q };
        $q_frag->{cte} = $cte if( $cte );
        push( @$result_queries, $q );
        my $next_next_binds = $new_rels->{$target_schema}->{$target_alias}->{$target_relation};

        my $ret_qs = &recursive_bind_helper(
            {
                filters     => undef, # apply filters to only the outer-most call
                handle      => $handle,
                next_bind   => $next_next_binds,
                RELS        => $new_rels,
                BINDS       => $BINDS,
            },
            $chain,
        );

        if( !defined( $ret_qs ) )
        {
            my $frag = { q => $q, alias => $target_alias };
            $frag->{cte} = $cte if( $cte );
            if( defined( $chain->{__ENDS__} ) )
            {
                push( @{$chain->{__ENDS__}}, $frag );
            }
            else
            {
                $chain->{__ENDS__} = [ $frag ];
            }
        }

        foreach my $ret_q( @$ret_qs )
        {
            $chain->{$ret_q} = { q => $q };
        }

        # Tail recursive, iterative nightmare
        # We need to track query permutations that can happen so that we can appropriately nest them
        #
        # Consider the following structure
        #           /--D--F---
        #      /-B-<
        #     /     \--E------
        # -A-<
        #     \--C------------
        #
        # Each branch in the chain means we have another query nesting that we need to handle. In this
        # case our chains will be
        # [
        #  [ A, B, D, F ]
        #  [ A, B, E ]
        #  [ A, C ]
        # ]
    }

    return $result_queries;
}

sub apply_filters($$$$$;$) :Export( :MANDATORY )
{
    my(
        $handle,
        $query_data,
        $table_mapping,
        $definition,
        $filters,
        $outer_join_mode
      ) = validate_pos(
        @_,
        { type => OBJECT },
        { type => HASHREF },
        { type => HASHREF },
        { type => SCALAR },
        { type => HASHREF },
        { type => SCALAR, optional => 1 },
    );

    # Lets use the filters we've received and search for the tables, their
    # aliases, and the objects they are present in within the query, then
    # attempt to modify the query such that we habe a filtered query
    # Phase I will result in a keyed array telling us which CTE or query will
    # need a filter applied
    my $where_expressions = {};

    foreach my $position( keys %{$table_mapping->{BINDS}} )
    {
        #print "P: $position\n";
        next if( $position < 0 );
        my $BINDS         = $table_mapping->{BINDS};
        my $RELS          = $table_mapping->{BINDS}->{$position}->{rels};
        my $where_entries = [];

        foreach my $schema( keys %$RELS )
        {
            #print "S: $schema\n";
            foreach my $alias( keys %{$RELS->{$schema}} )
            {
                #print "A: $alias\n";
                foreach my $table_name( keys %{$RELS->{$schema}->{$alias}} )
                {
                    #print "T: $table_name\n";
                    my $bind_infos = $RELS->{$schema}->{$alias}->{$table_name};
                    if( ref( $bind_infos ) eq '' && $bind_infos == 0 )
                    {
                        _log( $LOG_LEVEL_DEBUG, "Skipping unbindable relation $table_name - involved in outer join" );
                        next;
                    }
                    elsif( ref( $bind_infos ) eq 'ARRAY' )
                    {
                        my $has_binds = 0;
                        my $inh_match = 0;
                        foreach my $bind_info( @$bind_infos )
                        {
                            $has_binds = 1 if( defined( $filters->{$bind_info->{outer_schema}}->{$bind_info->{outer_relation}} ) );
                            $inh_match = 1 if( $bind_info->{outer_schema} eq $schema && $bind_info->{outer_relation} eq $table_name );
                        }
                        next unless( $has_binds && $inh_match );
                        my $chain = {};
                        my $ret = &recursive_bind_helper(
                            {
                             handle     => $handle,
                             next_bind  => $bind_infos,
                             RELS       => $RELS,
                             filters    => $filters,
                             BINDS      => $BINDS,
                            },
                            $chain
                        );

                        my $results = &chain_assembler( $chain );

                        if( defined( $results->{move_to} ) )
                        {
                            # Relocate this query to another CTE / Expression
                            # to do this, we locate the position of the CTE found in {move_to} and inject our @$results there
                            my $cte_found = 0;
                            foreach my $target_position( keys %{$table_mapping->{BINDS}} )
                            {
                                if(
                                    defined( $table_mapping->{BINDS}->{$target_position}->{parent} )
                                    && $table_mapping->{BINDS}->{$target_position}->{parent} eq $results->{move_to}
                                  )
                                {
                                    $cte_found = 1;
                                    if( !defined( $where_expressions->{$target_position} ) )
                                    {
                                        $where_expressions->{$target_position} = [];
                                    }

                                    push( @{$where_expressions->{$target_position}}, @{$results->{q}} );

                                    last;
                                }
                            }

                            if( !$cte_found )
                            {
                                # there may be a case where we'll need to search the CTE parent->child LUT in table_mapping
                                _log( $LOG_LEVEL_ERROR, "Assemble outer join can be injected to $results->{move_to} but $results->{move_to} could not be found" );
                            }
                        }
                        else
                        {
                            push( @$where_entries, @{$results->{q}} );
                        }
                    }
                    else
                    {
                        if( defined( $filters->{$schema}->{$table_name} ) )
                        {
                            my $binds = &bind_filters(
                                {
                                    handle => $handle,
                                    filters => $filters,
                                    schema => $schema,
                                    relation => $table_name,
                                    alias    => $alias,
                                }
                            );

                            if( $binds )
                            {
                                push( @$where_entries, @$binds );
                            }
                        }
                    }
                }
            }
        }

        if( $where_expressions->{$position} )
        {
            push( @{$where_expressions->{$position}}, @$where_entries );
        }
        else
        {
            $where_expressions->{$position} = $where_entries;
        }
    }


    foreach my $position( keys %$where_expressions )
    {
        my $where_entries = $where_expressions->{$position};
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
        else
        {
            delete( $where_expressions->{$position} );
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

        if( $bind_start < 0 )
        {
            $index++;
            next;
        }
        # Skip if no filters to be applied
        if( !defined( $where_expressions->{$bind_start} ) )
        {
            $index++;
            next;
        }

        my $bind_end = $table_mapping->{BINDS}->{$bind_start}->{end};
        my $next_cte_name;

        if( $index - 1 >= 0 )
        {
            # Here we look for the next CTE in the statement, if present.
            # This includes a recursive search in the case we run into something like:
            # WITH ...
            # (
            #    <- This is the statement we're currently concerned with
            # ),
            # tt_foo AS  <- We want this CTE's name
            # (
            #     WITH tt_bar AS
            #     (
            #         WITH tt_baz AS <- Next statement's parent will be this
            #         (
            #             ...
            #         )
            #             ...
            #     )
            #         ...
            # )
            #     ...

            $next_cte_name = $table_mapping->{BINDS}->{$starts[$index-1]}->{parent};

            my $recur_cte = $next_cte_name;

            if( $recur_cte )
            {
                while( defined( $table_mapping->{CTES}->{$recur_cte}->{parent} ) )
                {
                    $recur_cte = $table_mapping->{CTES}->{$recur_cte}->{parent};
                }

                $next_cte_name = $recur_cte;
            }
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
            $PARSE_ERROR = 1;
            #print Dumper( $table_mapping );
            return;
        }

        #print "Proceeding mark: '$where_proceeding_clause_mark'\n";
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

    #print "$new_q\n";
    return $new_q;
}

1;
