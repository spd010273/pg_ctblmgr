package ConfigManager;

use strict;
use warnings;

use POSIX qw( setsid );
use Perl6::Export::Attrs;
use Readonly;
use IO::Interactive qw( is_interactive );
use Params::Validate qw( :all );
use English qw( -no_match_vars );

use JSON::XS;
use Storable qw( dclone );
use List::Util qw( any );
use Try::Tiny;

use lib "$FindBin::Bin";
use Util;

Readonly::Scalar my $T_INTEGER           => 1;
Readonly::Scalar my $T_FLOAT             => 2;
Readonly::Scalar my $T_STRING            => 3;
Readonly::Scalar my $T_BOOLEAN           => 4;
Readonly::Scalar my $T_ARRAY_OF_INTEGERS => 5;

# load_only_on_start is a flag that indicates that a configuration can only be set on start and not reloadable
Readonly::Hash our %CONFIG_CATALOG :Export( :MANDATORY ) => (
    xid_service_port => {
        value       => 5522,
        type        => $T_INTEGER,
        description => 'Port used when accessing the XID reservation service',
    },
    xid_service_host => {
        value       => 'localhost',
        type        => $T_STRING,
        description => 'Host running the XID service',
    },
    xid_max_reservation_time => {
        value       => 10,
        type        => $T_INTEGER,
        description => 'How long an XID reservation is held when requested by pg_ctblmgr'
    },
    debug => {
        value       => 0,
        type        => $T_BOOLEAN,
        description => 'Enable verbose messaging related to process state',
    },
    timing => {
        value       => 0,
        type        => $T_INTEGER,
        description => 'Enable timing information',
    },
    refresh_on_start => {
        value       => 0,
        type        => $T_BOOLEAN,
        description => 'Refresh cache table on startup',
    },
    sleep_timer => {
        value       => 0.25,
        type        => $T_FLOAT,
        description => 'Sleep time between main loop iterations',
    },
    conservative_fast_delete => {
        value       => 0,
        type        => $T_BOOLEAN,
        description => 'In situations where we can normally perform a fast delete but the aged data set contains no rows, we will fall back to a slow delete in cases where this flag is set',
    },
    max_query_retries => {
        value       => 3,
        type        => $T_INTEGER,
        description => 'Number of times to retry a query prior to giving up and going into an error condition',
    },
    enable_blowout_approximate => {
        value       => 1,
        type        => $T_BOOLEAN,
        description => "Enables logic that correlates changes to base tables to changes in cache table output on a row-count basis. This helps pg_ctblmgr determine whether it should use temp tables or common table expressions when performing its DML updates to cache tables. If this is disabled, pg_ctblmgr will use the raw change count to determine if temp tables should be used. This can result no costly count queries, but also inefficient DML if a highly normalized table receives an update, resulting in large changes in the cache table outputs. Because of this if the blowout approximation logic is disabled, it's recommended to set temp_table_cutoff to a smaller value",
    },
    clear_stats_on_sighup => {
        value       => 0,
        type        => $T_BOOLEAN,
        description => 'Clears the stats on SIGHUP',
    },
    temp_table_cutoff => {
        value       => 100,
        type        => $T_INTEGER,
        description => 'Above this number of changes in a given worker, the worker will use temp tables in the following situations: 1) Storing rows fast-forwarded from a historic timeline 2) Storing the changed rows of a dataset when doing UPDATE / INSERT ops. Below this threshold, workers will opt for CTE (Common Table Expressions) and VALUES() statements. This will reduce the number of MATERIALIZE operations a server experiences',
    },
    changes_between_reavg => {
        value       => 5,
        type        => $T_INTEGER,
        description => 'Number of changes that occur between updates to the blowout_factors of varios base tables as they relate to cache table output',
    },
    enable_fast_delete => {
        value              => 1,
        type               => $T_BOOLEAN,
        description        => 'Enable fast delete functionality - 1 is enable, 0 is disable',
        load_only_on_start => 1,
    },
    xid_bucket_times => {
        value              => [3, 300],
        type               => $T_ARRAY_OF_INTEGERS,
        load_only_on_start => 1,
    },
    batched_create => {
        value       => 0,
        type        => $T_BOOLEAN,
        description => 'When creating cache tables, use LIMIT / OFFSET to populate the table rather than one insert',
    },
    batch_size => {
        value       => 1000000,
        type        => $T_INTEGER,
        description => 'Batch size for the above batched create mode',
    },
    bulk_action_cutoff => {
        value       => 100000,
        type        => $T_INTEGER,
        description => 'Number of temp table tuple we toggle into a bulk update mode ( delete + insert ) rather than doing update.',
    },
    outer_grouped_rels_only => {
        value       => 1,
        type        => $T_BOOLEAN,
        description => 'In cases where multiple relations are involved in a single outer join, the filter for the outer relation only goes to the grouped relation, if present',
    },
    outer_fallback_to_largest => {
        value       => 1,
        type        => $T_BOOLEAN,
        description => 'In cases where multiple relations are involved in a single outer join, and no group by is present we will only filter the largest relation',
    },
    conservative_table_filtering => {
        value       => 1,
        type        => $T_BOOLEAN,
        description => 'In cases where bulk changes would apply filters to different sections of the query, do not attempt to combine those filters into the same query to avoid accidentally logically ANDing them',
    },
    db_conn_tcp_keepalives_idle => {
        value       => 60,
        type        => $T_INTEGER,
        description => 'tcp_keepalives_idle setting for database connections',
    },
    db_conn_tcp_keepalives_interval => {
        value  => 5,
        type   => $T_INTEGER,
        description => 'tcp_keepalives_interval setting for database connections',
    },
    db_conn_tcp_user_timeout => {
        value       => 1000 * 60 * 5,
        type        => $T_INTEGER,
        description => 'tcp_user_timeout setting for database connections',
    },
    explicit_materialize_ops => {
        value       => 1,
        type        => $T_BOOLEAN,
        description => 'Force addition of MATERIALIZE operand in CTEs used to generate tuple updates by pg_ctblmgr. This can improve or stabilize performance',
    },
);

sub _derive_xid_bucket_count
{
    my $xid_bucket_times = shift;

    return scalar( @$xid_bucket_times );
}

sub _derive_xid_idle_timeout
{
    my $xid_bucket_times = shift;

    my $xid_bucket_count = _derive_xid_bucket_count( $xid_bucket_times );

    return ( 1000 * $xid_bucket_times->[$xid_bucket_count - 1] + ( 1000 * 30 ) );
}

Readonly::Hash my %DERIVED_CONFIG_CATALOG => (
    xid_bucket_count => {
        derive_func => \&_derive_xid_bucket_count,
        derive_from => [ 'xid_bucket_times' ],
    },
    xid_idle_timeout => {
        derive_func => \&_derive_xid_idle_timeout,
        derive_from => [ 'xid_bucket_times' ],
    },
);

sub new
{
    my $class = shift;

    my %params = @_;

    my $config_file = $params{config_file};

    my $self = {
        _config_file => $config_file,
        _configs     => undef,
    };

    bless( $self, $class );

    $self->_load_default_configs();

    $self->load_configs();

    return $self;
}

sub get_config_file
{
    my $self = shift;

    return $self->{_config_file};
}

sub get_configs
{
    my $self = shift;

    return $self->{_configs};
}

sub set_configs
{
    my $self = shift;

    my $new_configs = shift;

    $self->{_configs} = $new_configs;
}

sub get_config_value
{
    my $self = shift;

    my $config_name = shift;

    if( !exists( $self->{_configs}->{$config_name} ) )
    {
        _log( $LOG_LEVEL_ERROR, "ConfigManager::get_config_value: unrecognized config: $config_name" );
        return;
    }

    my $config_value = $self->{_configs}->{$config_name};

    return $config_value;
}

sub load_configs
{
    my $self = shift;

    my ( $reload ) = validate_pos( @_, { type => SCALAR, default => 0 } );

    my $fallback_behavior = 'Using default configs';
    my $fallback_behavior_single_entry = 'Using the default value';

    if( $reload )
    {
        $fallback_behavior = 'Keeping current configs';
        $fallback_behavior_single_entry = 'Keeping the current value';
    }

    if( not defined( $self->{_config_file} ) )
    {
       _log( $LOG_LEVEL_ERROR, "ConfigManager::load_configs: config file not specified - $fallback_behavior." );

        return;
    }

    if( !-e $self->{_config_file} )
    {
        _log( $LOG_LEVEL_ERROR, "ConfigManager::load_configs: config file does not exist: $self->{_config_file} - $fallback_behavior." );

        return;
    }

    my $fh;

    if( !open( $fh, '<', $self->{_config_file} ) )
    {
        _log( $LOG_LEVEL_ERROR, "ConfigManager::load_configs: failed to open config file: $self->{_config_file} - $fallback_behavior." );

        return;
    }

    my $json = '';

    while( my $line = <$fh> )
    {
        chomp( $line );
        $json .= $line;
    }

    close( $fh );

    my $loaded_configs;

    # Or the error couldn't be caught correctly
    local $SIG{__DIE__} = \&Carp::croak;

    try
    {
        $loaded_configs = JSON::XS::decode_json( $json );
    }
    catch
    {
        _log( $LOG_LEVEL_ERROR, "ConfigManager::load_configs: failed to decode config JSON: $_ - $fallback_behavior." );
    };

    unless( defined( $loaded_configs ) )
    {
        return;
    }

    if( ref( $loaded_configs ) ne 'HASH' )
    {
        _log( $LOG_LEVEL_ERROR, "ConfigManager::load_configs: config JSON is not an object - $fallback_behavior." );
        return;
    }

    my $new_configs = {};

    foreach my $config_name ( keys( %CONFIG_CATALOG ) )
    {
        $new_configs->{$config_name} = $CONFIG_CATALOG{$config_name}->{value};
    }

    my @accepted_configs = keys( %CONFIG_CATALOG );
    my @loaded_configs = keys( %$loaded_configs );

    my $existing_configs = $self->get_configs();

    # Only accept configs that are in the default config list
    # and report any unrecognized configs
    foreach my $loaded_config ( @loaded_configs )
    {
        if( any { $_ eq $loaded_config } @accepted_configs )
        {
            my $validate_error = $self->_validate_config( $loaded_config, $loaded_configs->{$loaded_config} );

            if( $validate_error )
            {
                _log( $LOG_LEVEL_ERROR, "ConfigManager::load_configs: invalid value for configuration '$loaded_config': $validate_error - $fallback_behavior_single_entry" );
                next;
            }

            if( $reload and $CONFIG_CATALOG{$loaded_config}->{load_only_on_start} )
            {
                my $existing_config_value = $existing_configs->{$loaded_config};
                my $new_config_value = $self->_transform_loaded_value( $loaded_config, $loaded_configs->{$loaded_config} );

                my $existing_config_value_json = encode_json( $existing_config_value );

                if( $existing_config_value_json ne encode_json( $new_config_value ) )
                {
                    _log( $LOG_LEVEL_INFO, "ConfigManager::load_configs: configuration '$loaded_config' value changes but it can only take effect on start - original value '$existing_config_value_json' will be kept" );
                    next;
                }

                $new_configs->{$loaded_config} = $existing_config_value;
            }
            else
            {
                $new_configs->{$loaded_config} = $self->_transform_loaded_value( $loaded_config, $loaded_configs->{$loaded_config} );
            }
        }
        else
        {
            _log( $LOG_LEVEL_WARNING, "ConfigManager::load_configs: unrecognized config: $loaded_config" );
        }
    }

    # Report any missing configs
    foreach my $accepted_config ( @accepted_configs )
    {
        if( !exists( $loaded_configs->{$accepted_config} ) )
        {
            _log( $LOG_LEVEL_INFO, "ConfigManager::load_configs: '$accepted_config' not found in config file - $fallback_behavior_single_entry" );
        }
    }

    $self->_derive_configs( $new_configs );

    $self->set_configs( $new_configs );

    if( $reload )
    {
        _log( $LOG_LEVEL_INFO, "ConfigManager::load_configs: reloaded configs from $self->{_config_file}" );
    }
    else
    {
        _log( $LOG_LEVEL_INFO, "ConfigManager::load_configs: loaded configs from $self->{_config_file}" );
    }

    return 1;
}

sub _derive_configs
{
    my $self = shift;

    my ( $raw_configs ) = validate_pos( @_, { type => HASHREF } );

    my @accepted_configs = keys( %CONFIG_CATALOG );

    local $SIG{__DIE__} = \&Carp::croak;

    # Derive any configs that cannot be explicitly set
    foreach my $derived_config ( keys( %DERIVED_CONFIG_CATALOG ) )
    {
        my $derive_func = $DERIVED_CONFIG_CATALOG{$derived_config}->{derive_func};
        my $derive_from = $DERIVED_CONFIG_CATALOG{$derived_config}->{derive_from};

        foreach my $derive_from_config ( @$derive_from )
        {
            unless( any { $_ eq $derive_from_config } @accepted_configs )
            {
                _log( $LOG_LEVEL_FATAL, "ConfigManager::load_configs: missing config for derivation: $derive_from_config" );

                # unreachable unless log level changes to non-fatal
                next;
            }
        }

        my @derive_values = map { $raw_configs->{$_} } @$derive_from;

        my $derived_value = eval { $derive_func->( @derive_values ) };

        if( $EVAL_ERROR )
        {
            _log( $LOG_LEVEL_FATAL, "ConfigManager::load_configs: failed to derive config: $derived_config: $EVAL_ERROR" );

            # unreachable unless log level changes to non-fatal
            next;
        }
        else
        {
            $raw_configs->{$derived_config} = $derive_func->( @derive_values );
        }
    }

    return 1;
}

sub _load_default_configs
{
    my $self = shift;

    my $default_configs = {};

    foreach my $config_name ( keys( %CONFIG_CATALOG ) )
    {
        $default_configs->{$config_name} = $CONFIG_CATALOG{$config_name}->{value};
    }

    $self->_derive_configs( $default_configs );

    $self->set_configs( $default_configs );

    return 1;
}

# Transform config value loaded from JSON to the correct form
sub _transform_loaded_value
{
    my $self = shift;

    my ( $config_name, $config_value ) = @_;

    my $config_datatype = $CONFIG_CATALOG{$config_name}->{type};

    # Or otherwise the boolean value will be an Object
    if( $config_datatype == $T_BOOLEAN )
    {
        if( $config_value )
        {
            return 1;
        }
        else
        {
            return 0;
        }
    }

    return $config_value;
}

sub _validate_config
{
    my $self = shift;

    my ( $config_name, $config_value ) = @_;

    my $expected_config_type = $CONFIG_CATALOG{$config_name}->{type};

    my $validate_error;

    if( $expected_config_type == $T_INTEGER )
    {
        if( $config_value !~ m/^\d+$/ )
        {
            $validate_error = 'expected integer';
        }
    }
    elsif( $expected_config_type == $T_FLOAT )
    {
        if( $config_value !~ m/^\d+(\.\d+)?$/ )
        {
            $validate_error = 'expected float';
        }
    }
    elsif( $expected_config_type == $T_STRING )
    {
        if( ref( $config_value ) )
        {
            $validate_error = 'expected string';
        }
    }
    elsif( $expected_config_type == $T_BOOLEAN )
    {
        if( $config_value ne '0' and $config_value ne '1' )
        {
            $validate_error = 'expected boolean';
        }
    }
    elsif( $expected_config_type == $T_ARRAY_OF_INTEGERS )
    {
        if( ref( $config_value ) ne 'ARRAY' )
        {
            $validate_error = 'expected array';
        }
        else
        {
            foreach my $element ( @$config_value )
            {
                if( $element !~ m/^\d+$/ )
                {
                    $validate_error = 'expected array of integers';
                    last;
                }
            }
        }
    }

    return $validate_error;
}
