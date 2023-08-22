package DataPopulation::TestCase;

#### STRICTURES ####
use strict;
use warnings;
use feature qw( say switch );

#### NEADWERX MODULES ####
use Carp;
use Readonly;
use Perl6::Export::Attrs;
use English qw( -no_match_vars );
use Fatal qw( open );
use Params::Validate qw( :all );

#### CPAN MODULES ####
use JSON;
use List::Util;

use FindBin;
use lib "$FindBin::Bin";

sub new ## class method
{
    my $class = shift;

    my ( $db_handle, $cache_tables ) = validate_pos(
        @_,
        { type => OBJECT  },
        { type => HASHREF },
    );

    my $class_spec = {
        db_handle => $db_handle,
    };

    my $new_object = bless( $class_spec, $class );

    my $cache_table_name = $new_object->CACHE_TABLE_NAME;
    if( not exists $cache_tables->{$cache_table_name} )
    {
        croak 'Definition for cache table ' . $cache_table_name . ' not provided';
    }

    $new_object->{definition_query} = $cache_tables->{$cache_table_name}->{definition};

    return $new_object;
}

sub TEST_CASE_NAME ## class method
{
    croak ref( shift ) . ': TEST_CASE_NAME not implemented';
}

sub CACHE_TABLE_NAME ## class method
{
    croak ref( shift ) . ': CACHE_TABLE_NAME not implemented';
}

# The output of this function will be passed to apply_base_table_change
# and verify_data_population.
#
# Returns:
#   scalar - error message
#   undef - no data
#   reference - data found
sub get_data_for_change ## class method
{
    croak ref( shift ) . ': get_data_for_change() not implemented';
}

# Returns:
#   scalar - error message
#   undef - success
sub apply_base_table_change ## class method
{
    croak ref( shift ) . ': apply_base_table_change() not implemented';
}

# Returns:
#   scalar - error message
#   undef - success
sub verify_data_population ## class method
{
    croak ref( shift ) . ': verify_data_population() not implemented';
}

sub _validate_cached_rows ## no critics
{
    my $self = shift;

    my ( $cached_rows, $validation_rows ) = validate_pos( @_, { type => ARRAYREF }, { type => ARRAYREF } );

    my $db_handle = $self->{db_handle};

    if( scalar( @$cached_rows ) != scalar( @$validation_rows ) )
    {
        return "Number of records with key doesn't match\n\nCache Table: " . scalar( @$cached_rows ). " row(s) \nDefinition Query: " . scalar( @$validation_rows ) . ' row(s)';
    }

    unless( @$cached_rows )
    {
        return;
    }

    my @cached_columns = sort( keys %{$cached_rows->[0]} );
    my @validation_columns = sort( keys %{$validation_rows->[0]} );

    my @missing_columns;
    my @no_target_columns;

    if( encode_json( \@cached_columns ) ne encode_json( \@validation_columns ) )
    {
        foreach my $cached_column ( @cached_columns )
        {
            unless( List::Util::any { $cached_column eq $_ } @validation_columns )
            {
                push @missing_columns, $cached_column;
            }
        }

        foreach my $validation_column ( @validation_columns )
        {
            unless( List::Util::any { $validation_column eq $_ } @cached_columns )
            {
                push @no_target_columns, $validation_column;
            }
        }

        if( @missing_columns )
        {
            return 'Cache table is missing column(s) named ' . join( ', ', @missing_columns ) . ' while cache table query has column(s) with no target named ' . join( ', ', @no_target_columns );
        }
        elsif( @missing_columns )
        {
            return 'Cache table definition is missing column(s) named ' . join( ', ', @missing_columns );
        }
        else
        {
            return 'Cache table query has column(s) with no target named ' . join( ', ', @missing_columns );
        }
    }

    foreach my $cached_row( @$cached_rows )
    {
        foreach my $cached_column ( @cached_columns )
        {
            if( not defined $cached_row->{$cached_column} )
            {
                $cached_row->{$cached_column} = '[VALIDATION_NULL]';
            }
        }
    }

    my @sorted_cached_rows = @$cached_rows;

    foreach my $cached_column( @cached_columns )
    {
        @sorted_cached_rows = sort { $a->{$cached_column} cmp $b->{$cached_column} } @sorted_cached_rows;
    }

    foreach my $validation_row ( @$validation_rows )
    {
        foreach my $validation_column ( @validation_columns )
        {
            if( not defined $validation_row->{$validation_column} )
            {
                $validation_row->{$validation_column} = '[VALIDATION_NULL]';
            }
        }
    }

    my @sorted_validation_rows = @$validation_rows;

    foreach my $validation_column ( @validation_columns )
    {
        @sorted_validation_rows = sort { $a->{$validation_column} cmp $b->{$validation_column} } @sorted_validation_rows;
    }

    my $i = 0;

    while( $i < scalar( @sorted_cached_rows ) )
    {
        my $cached_row     = $sorted_cached_rows[$i];
        my $validation_row = $sorted_validation_rows[$i];

        my $mistmatch_str = '';

        foreach my $cached_column ( @cached_columns )
        {
            if( $cached_row->{$cached_column} ne $validation_row->{$cached_column} )
            {
                $mistmatch_str .= "\n\n$cached_column\nCache Table: '$cached_row->{$cached_column}'\nDefinition Query: '$validation_row->{$cached_column}'";
            }
        }

        if( $mistmatch_str ne '' )
        {
            return "Value mismatches$mistmatch_str";
        }

        $i += 1;
    }

    return;
}

1;
