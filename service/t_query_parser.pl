#!/usr/bin/perl

use strict;
use warnings;
use utf8;

use Params::Validate qw( :all );
use Data::Dumper;

sub get_parse_subtree_obj($$$)
{
    my( $pg_node_tree, $current_index, $output ) = validate_pos(
        @_,
        { type => ARRAYREF },
        { type => SCALARREF },
        { type => SCALARREF },
    );

    my $local_index = $$current_index;
    my $local_output = {};
    my $is_key = 0;
    my $is_sublist = 0;
    my $key_name = '';

    foreach my $word( @$pg_node_tree )
    {
        print "obj: $word\n";
        if( $local_index < $$current_index )
        {
            $local_index++;
            next;
        }

        if( !$is_sublist && !$is_key && $word =~ m/^:/ )
        {
            my $cleaned_word = $word;
            $cleaned_word =~ s/^://;
            $is_key = 1;
            $key_name = $cleaned_word;
            $$current_index++;
            $local_index++;
            next;
        }
        elsif( $is_key && !$is_sublist && $word =~ m/^{/ )
        {
            $$current_index++;
            my $cleaned_word = $word;
            $cleaned_word =~ s/^{//;
            $local_output->{$key_name} = get_parse_subtree_obj( [ @$pg_node_tree[$current_index..( scalar(@$pg_node_tree) - 1 )] ], $current_index, $output );
            $is_key = 0;
            next;
        }
        elsif( $is_key && !$is_sublist && $word =~ m/^\({/ )
        {
            $$current_index++;
            my $cleaned_word
        }

        $local_index++;
        $$current_index++;
    }
}

sub get_parse_subtree_array($$$)
{
    my( $pg_node_tree, $current_index, $output ) = validate_pos(
        @_,
        { type => ARRAYREF },
        { type => SCALARREF },
        { type => SCALARREF },
    );

    my $local_index = $$current_index;
    my $local_output = [];
    my $element_output = {};
    my $is_key = 0;
    my $key_name = '';
    my $is_sublist = 0;
        
    foreach my $word( @$pg_node_tree )
    {
        if( $local_index < $$current_index )
        {
            $local_index++;
            print "Skipping index to discard word '$word'\n";
            next;
        }

        if( !$is_sublist && !$is_key && $word =~ m/^:/ )
        {
            my $cleaned_word = $word;
            $cleaned_word =~ s/^://;
            $is_key = 1;
            $key_name = $cleaned_word; 
            $local_index++;
            $$current_index++;
            next;
        }
        elsif( $is_key && !$is_sublist && $word =~ m/^{/ )
        {
            $$current_index++;
            my $cleaned_word = $word;
            $cleaned_word =~ s/^{//;
            $element_output->{$key_name}->{$cleaned_word} = get_parse_subtree_obj( [ @$pg_node_tree[$current_index..( scalar(@$pg_node_tree) - 1 )] ], $current_index, $output );
            $is_key = 0;
            next;
        }
        elsif( $is_key && !$is_sublist && $word =~ m/^\({/ )
        {
            $$current_index++;
            my $cleaned_word = $word;
            $cleaned_word =~ s/^\({//;
            $element_output->{$key_name}->{$cleaned_word} = get_parse_subtree_array( [ @$pg_node_tree[$current_index..( scalar(@$pg_node_tree) - 1 )] ], $current_index, $output );
            $is_key = 0;
            next;
        }
        elsif( $is_key && !$is_sublist && $word =~ m/}$/ )
        {
            my $value = $word;
            $value =~ s/}$//;
            $element_output->{$key_name} = $value;
            push( @$local_output, $element_output );
            print "Word $word triggered new elem\n";
            $element_output = {};
        }
        elsif( $is_key && !$is_sublist )
        {
            my $value = $word;
            $element_output->{$key_name} = $value;

            if( $value =~ /^\(/ && $value !~ /\)$/ )
            {
                $is_sublist = 1;
            }
            else
            {
                $is_key = 0;
            }
        }
        elsif( $is_key && $is_sublist )
        {
            if( $word =~ /\)$/ )
            {
                $is_sublist = 0;
                $is_key = 0;
            }

            $element_output->{$key_name} .= " $word";
        }

        $$current_index++;
        $local_index++;
    }

    return $local_output;
}

sub get_parse_tree_obj($)
{
    my( $pg_node_tree ) = validate_pos(
        @_,
        { type => SCALAR },
    );

    my $current_index = 0;
    my $array = [];
    @$array = split( /\s/, $pg_node_tree );
    my $output = {};
    my $local_index = 0;

    foreach my $word( @$array )
    {
        if( $current_index > $local_index )
        {
            $local_index++;
            next;
        }

        if( $word =~ m/^{/ )
        {
            my $cleaned_word = $word;
            $cleaned_word =~ s/^{//;
            $current_index++;
            $output->{$cleaned_word} = get_parse_subtree_obj( [ @$array[$current_index..( scalar( @$array ) - 1 )] ], \$current_index, \$output );
            next;
        }
        elsif( $word =~ m/^\({/ )
        {
            my $cleaned_word = $word;
            $cleaned_word =~ s/^\({//;
            $current_index++;
            $output->{$cleaned_word} = get_parse_subtree_array( [ @$array[$current_index..( scalar( @$array) - 1 )] ], \$current_index, \$output );
            next;
        }
        
        $current_index++;
        $local_index++;
    }

    print Dumper( $output );
}
sub foo()
{
    my $pg_node_tree;
    if( substr( $pg_node_tree, 0, 1 ) eq '(' && substr( $pg_node_tree, 1, 1 ) eq '{' )
    {
        # rip off outer '({' and '})'
        $pg_node_tree = substr( $pg_node_tree, 2, length( $pg_node_tree ) - 4 );
    }

    my $pg_node_tree;
    my $output = {};
    my @nest_stack;
    my $last_key = '';
    my $key_fudge_factor = 0;
    my $is_key = 0;
    my @parse_tree_chars;
    # second try
    foreach my $word( @parse_tree_chars )
    {
        my $temp_hr = $output;
        foreach my $nested_key( @nest_stack )
        {
            $temp_hr = $temp_hr->{$nested_key};
        }

        if( $is_key )
        {
            if( $word =~ m/^\({[A-Z]+$/ || $word =~ m/^{[A-Z]+$/ )
            {
                my $cleaned_word = $word;
                $cleaned_word =~ s/^\(//;
                $cleaned_word =~ s/^{//;
                $temp_hr->{$cleaned_word} = {};
                push( @nest_stack, $cleaned_word );
                $is_key = 0;
                next;
            }
            else
            {
                my $cleaned_word = $word;
                if( $word =~ m/}$/ || $word =~ m/}\)$/ )
                {
                    $cleaned_word =~ s/}[\)]?$//g;
                    pop @nest_stack;
                }

                $temp_hr->{$last_key} = $cleaned_word;
                $is_key = 0;
                next; 
            }
        }

        if( $word =~ m/^:\w+/ )
        {
            my $cleaned_word = $word;
            $cleaned_word =~ s/^://;
            $temp_hr->{$cleaned_word} = {};
            $is_key                   = 1;
            $last_key                 = $cleaned_word;
            next;
        }
    }

    print Dumper( $output );
    # first try
    foreach my $word( @parse_tree_chars )
    {
        my $temp_hr = $output;
        foreach my $nested_key( @nest_stack )
        {
            $temp_hr = $temp_hr->{$nested_key};
        }

        if( $word =~ /^[A-Z]+$/ || $word =~ /^{[A-Z]+$/ || $word =~ /^\({[A-Z]+$/ )
        {
            my $cleaned_word = $word;
            $cleaned_word =~ s/\(//g;
            $cleaned_word =~ s/{//g;
            print "Found key $cleaned_word\n";
         
            if( $last_key =~ /^:/ )
            {
                my $cleaned_last_key = $last_key;
                $cleaned_last_key =~ s/://g;
                $key_fudge_factor++;
                $temp_hr->{$cleaned_last_key}->{$cleaned_word} = {};
                $temp_hr = $temp_hr->{$cleaned_last_key}->{$cleaned_word};
            }
            else
            {
                $temp_hr->{$cleaned_word} = {};
                $temp_hr = $temp_hr->{$cleaned_word};
            }
                
            push( @nest_stack, $cleaned_word );
        }

        if( $word =~ /}\)$/ || $word =~ /}$/ )
        {
            pop( @nest_stack );

            if( $key_fudge_factor > 0 )
            {
                #pop( @nest_stack );
                #$key_fudge_factor--;
            }
        }

        if( $word =~ /^:w+$/ )
        {
            my $cleaned_word = $word;
            $cleaned_word =~ s/^://;
            $is_key = 1;
            $temp_hr->{$cleaned_word} = '';
        }

        $last_key = $word;
    }

    print Dumper( $output );
}

my $test_string = '({QUERY :commandType 1 :querySource 0 :canSetTag true :utilityStmt <> :resultRelation 0 :hasAggs false :hasWindowFuncs false :hasTargetSRFs false :hasSubLinks false :hasDistinctOn false :hasRecursive false :hasModifyingCTE false :hasForUpdate false :hasRowSecurity false :isReturn false :cteList ({COMMONTABLEEXPR :ctename tt_test :aliascolnames <> :ctematerialized 0 :ctequery {QUERY :commandType 1 :querySource 0 :canSetTag false :utilityStmt <> :resultRelation 0 :hasAggs false :hasWindowFuncs false :hasTargetSRFs false :hasSubLinks false :hasDistinctOn false :hasRecursive false :hasModifyingCTE false :hasForUpdate false :hasRowSecurity false :isReturn false :cteList <> :rtable ({RTE :alias {ALIAS :aliasname a :colnames <>} :eref {ALIAS :aliasname a :colnames ("foo" "bar" "baz")} :rtekind 0 :relid 18288 :relkind r :rellockmode 1 :tablesample <> :lateral false :inh true :inFromCl true :requiredPerms 2 :checkAsUser 0 :selectedCols (b 8) :insertedCols (b) :updatedCols (b) :extraUpdatedCols (b) :securityQuals <>}) :jointree {FROMEXPR :fromlist ({RANGETBLREF :rtindex 1}) :quals <>} :targetList ({TARGETENTRY :expr {VAR :varno 1 :varattno 1 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnosyn 1 :varattnosyn 1 :location 58} :resno 1 :resname foo :ressortgroupref 0 :resorigtbl 18288 :resorigcol 1 :resjunk false}) :override 0 :onConflict <> :returningList <> :groupClause <> :groupDistinct false :groupingSets <> :havingQual <> :windowClause <> :distinctClause <> :sortClause <> :limitOffset <> :limitCount <> :limitOption 0 :rowMarks <> :setOperations <> :constraintDeps <> :withCheckOptions <> :stmt_location 0 :stmt_len 0} :search_clause <> :cycle_clause <> :location 39 :cterecursive false :cterefcount 1 :ctecolnames ("foo") :ctecoltypes (o 23) :ctecoltypmods (i -1) :ctecolcollations (o 0)}) :rtable ({RTE :alias {ALIAS :aliasname old :colnames <>} :eref {ALIAS :aliasname old :colnames ("foo" "bar" "baz")} :rtekind 0 :relid 18402 :relkind v :rellockmode 1 :tablesample <> :lateral false :inh false :inFromCl false :requiredPerms 0 :checkAsUser 0 :selectedCols (b) :insertedCols (b) :updatedCols (b) :extraUpdatedCols (b) :securityQuals <>} {RTE :alias {ALIAS :aliasname new :colnames <>} :eref {ALIAS :aliasname new :colnames ("foo" "bar" "baz")} :rtekind 0 :relid 18402 :relkind v :rellockmode 1 :tablesample <> :lateral false :inh false :inFromCl false :requiredPerms 0 :checkAsUser 0 :selectedCols (b) :insertedCols (b) :updatedCols (b) :extraUpdatedCols (b) :securityQuals <>} {RTE :alias {ALIAS :aliasname a :colnames <>} :eref {ALIAS :aliasname a :colnames ("foo")} :rtekind 6 :ctename tt_test :ctelevelsup 0 :self_reference false :coltypes (o 23) :coltypmods (i -1) :colcollations (o 0) :lateral false :inh false :inFromCl true :requiredPerms 0 :checkAsUser 0 :selectedCols (b) :insertedCols (b) :updatedCols (b) :extraUpdatedCols (b) :securityQuals <>} {RTE :alias {ALIAS :aliasname b :colnames <>} :eref {ALIAS :aliasname b :colnames ("foo" "bar" "baz")} :rtekind 0 :relid 18293 :relkind r :rellockmode 1 :tablesample <> :lateral false :inh true :inFromCl true :requiredPerms 2 :checkAsUser 0 :selectedCols (b 8 10) :insertedCols (b) :updatedCols (b) :extraUpdatedCols (b) :securityQuals <>} {RTE :alias <> :eref {ALIAS :aliasname unnamed_join :colnames ("foo" "foo" "bar" "baz")} :rtekind 2 :jointype 0 :joinmergedcols 0 :joinaliasvars ({VAR :varno 3 :varattno 1 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnosyn 3 :varattnosyn 1 :location -1} {VAR :varno 4 :varattno 1 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnosyn 4 :varattnosyn 1 :location -1} {VAR :varno 4 :varattno 2 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnosyn 4 :varattnosyn 2 :location -1} {VAR :varno 4 :varattno 3 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnosyn 4 :varattnosyn 3 :location -1}) :joinleftcols (i 1) :joinrightcols (i 1 2 3) :join_using_alias <> :lateral false :inh false :inFromCl true :requiredPerms 0 :checkAsUser 0 :selectedCols (b) :insertedCols (b) :updatedCols (b) :extraUpdatedCols (b) :securityQuals <>}) :jointree {FROMEXPR :fromlist ({JOINEXPR :jointype 0 :isNatural false :larg {RANGETBLREF :rtindex 3} :rarg {RANGETBLREF :rtindex 4} :usingClause <> :join_using_alias <> :quals {OPEXPR :opno 96 :opfuncid 65 :opresulttype 16 :opretset false :opcollid 0 :inputcollid 0 :args ({VAR :varno 4 :varattno 1 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnosyn 4 :varattnosyn 1 :location 153} {VAR :varno 3 :varattno 1 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnosyn 3 :varattnosyn 1 :location 161}) :location 159} :alias <> :rtindex 5}) :quals <>} :targetList ({TARGETENTRY :expr {VAR :varno 3 :varattno 1 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnosyn 3 :varattnosyn 1 :location 85} :resno 1 :resname foo :ressortgroupref 0 :resorigtbl 18288 :resorigcol 1 :resjunk false} {TARGETENTRY :expr {FUNCEXPR :funcid 18318 :funcresulttype 23 :funcretset false :funcvariadic false :funcformat 0 :funccollid 0 :inputcollid 0 :args ({VAR :varno 3 :varattno 1 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnosyn 3 :varattnosyn 1 :location 101}) :location 92} :resno 2 :resname bar :ressortgroupref 0 :resorigtbl 0 :resorigcol 0 :resjunk false} {TARGETENTRY :expr {VAR :varno 4 :varattno 3 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnosyn 4 :varattnosyn 3 :location 117} :resno 3 :resname baz :ressortgroupref 0 :resorigtbl 18293 :resorigcol 3 :resjunk false}) :override 0 :onConflict <> :returningList <> :groupClause <> :groupDistinct false :groupingSets <> :havingQual <> :windowClause <> :distinctClause <> :sortClause <> :limitOffset <> :limitCount <> :limitOption 0 :rowMarks <> :setOperations <> :constraintDeps <> :withCheckOptions <> :stmt_location 0 :stmt_len 168})'; 
get_parse_tree_obj( $test_string );
