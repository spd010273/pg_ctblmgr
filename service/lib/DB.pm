package DB;

use strict;
use warnings;
use utf8;

use DBI;
use Perl6::Export::Attrs;
use FindBin;
use English qw( -no_match_vars );
use Params::Validate qw( :all );

use lib "$FindBin::Bin";
use Util;

our $CONNECTION_MAP :Export( :MANDATORY );

sub try_query($$;$) :Export( :MANDATORY )
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

1;
