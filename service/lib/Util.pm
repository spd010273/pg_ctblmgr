package Util;

use Perl6::Export::Attrs;
use Readonly;
use IO::Interactive qw( is_interactive );
use Params::Validate qw( :all );
use English qw( -no_match_vars );

$OUTPUT_AUTOFLUSH = 1;

Readonly::Scalar our $DEBUG                      :Export( :MANDATORY ) => 1;
Readonly::Scalar our $CLEAN_UP                   :Export( :MANDATORY ) => 0; # Emergency shm cleanup
Readonly::Scalar our $LOG_LEVEL_FATAL            :Export( :MANDATORY ) => 5;
Readonly::Scalar our $LOG_LEVEL_ERROR            :Export( :MANDATORY ) => 4;
Readonly::Scalar our $LOG_LEVEL_WARNING          :Export( :MANDATORY ) => 3;
Readonly::Scalar our $LOG_LEVEL_INFO             :Export( :MANDATORY ) => 2;
Readonly::Scalar our $LOG_LEVEL_DEBUG            :Export( :MANDATORY ) => 1;
Readonly::Scalar our $WORKER_STATUS_STARTUP      :Export( :MANDATORY ) => 1;
Readonly::Scalar our $WORKER_STATUS_RUNNING      :Export( :MANDATORY ) => 2; #not used?
Readonly::Scalar our $WORKER_STATUS_IDLE         :Export( :MANDATORY ) => 3;
Readonly::Scalar our $WORKER_STATUS_EXITED       :Export( :MANDATORY ) => 4;
Readonly::Scalar our $WORKER_STATUS_INSERT       :Export( :MANDATORY ) => 5;
Readonly::Scalar our $WORKER_STATUS_SLOW_DELETE  :Export( :MANDATORY ) => 6;
Readonly::Scalar our $WORKER_STATUS_FAST_DELETE  :Export( :MANDATORY ) => 7;
Readonly::Scalar our $WORKER_STATUS_UPDATE       :Export( :MANDATORY ) => 8;
Readonly::Scalar our $WORKER_STATUS_TEMP_TABLE   :Export( :MANDATORY ) => 9;
Readonly::Scalar our $WORKER_STATUS_QUERY_PARSE  :Export( :MANDATORY ) => 10;
Readonly::Scalar our $WORKER_STATUS_REPLACE      :Export( :MANDATORY ) => 11;
Readonly::Scalar our $EXTENSION_NAME             :Export( :MANDATORY ) => 'pg_ctblmgr';
Readonly::Scalar our $SCHEMA_NAME                :Export( :MANDATORY ) => 'pgctblmgr';
Readonly::Scalar our $SQL_STATE_ADMIN_TERM       :Export( :MANDATORY ) => '57P01';
Readonly::Scalar our $SQL_STATE_ADMIN_CANC       :Export( :MANDATORY ) => '57014';

# User globals
# Number of times to retry a query prior to giving up and going into an error condition
Readonly::Scalar our $MAX_QUERY_RETRIES          :Export( :MANDATORY ) => 5;

# Enable fast delete functionality - 1 is enable, 0 is disable
Readonly::Scalar our $ENABLE_FAST_DELETE         :Export( :MANDATORY ) => 1;

# Entry length for historic transaction storage. This allows fast delete to work
Readonly::Scalar our $MAX_XID_LENGTH             :Export( :MANDATORY ) => 10;

# Soread of XID map - the larger the number, the larger the spacing between XID map entries.
# This increases the likelyhood that long running tx's result in a fast delete
Readonly::Scalar our $XID_MAP_SPREAD             :Export( :MANDATORY ) => 25;

# When creating cache tables, use LIMIT / OFFSET to populate the table rather than one insert
Readonly::Scalar our $BATCHED_CREATE             :Export( :MANDATORY ) => 1;

# Batch size for the above batched create mode
Readonly::Scalar our $BATCH_SIZE                 :Export( :MANDATORY ) => 1000000;

# Number of temp table tuple we toggle into a bulk update mode ( delete + insert ) rather than doing update
Readonly::Scalar our $BULK_ACTION_CUTOFF         :Export( :MANDATORY ) => 100000;

# In cases where multiple relations are involved in a single outer join, the filter for the outer
# relation only goes to the grouped relation, if present.
Readonly::Scalar our $OUTER_GROUPED_RELS_ONLY    :Export( :MANDATORY ) => 1;

# In cases where multiple relations are involved in a single outer join, and no group by is present
# we will only filter the largest relation
Readonly::Scalar our $OUTER_FALLBACK_TO_LARGEST  :Export( :MANDATORY ) => 1;

# Globals initialized at runtime start
our $PARENT_PID :Export( :MANDATORY ) = 0;
our $SLOT_NAME  :Export( :MANDATORY ) = '';
our $DAEMONIZE  :Export( :MANDATORY ) = 0;
our $LOG_FH     :Export( :MANDATORY );
our $LOG_FILE   :Export( :MANDATORY ) = '';

Readonly::Scalar my $USAGE => <<"USAGE";
    Usage:
    $0
        -U <database user>
        -h <database host name>
        -d <database name>
        [ -p <database port> ]
        [ -D ( do not daemonize ) ]
USAGE

sub _log($$) :Export( :MANDATORY )
{
    my( $log_level, $message ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => SCALAR },
    );

    my $message_prefix = '';
    my $log_level_name = '';

    if( !is_interactive() && $DAEMONIZE )
    {
        unless( defined( $LOG_FH ) )
        {
            #attempt to open log file
        }
    }

    return if( $log_level == $LOG_LEVEL_DEBUG && !$DEBUG );

    if( $log_level == $LOG_LEVEL_DEBUG )
    {
        $log_level_name = 'DEBUG';
    }
    elsif( $log_level == $LOG_LEVEL_INFO )
    {
        $log_level_name = 'INFO';
    }
    elsif( $log_level == $LOG_LEVEL_WARNING )
    {
        $log_level_name = 'WARNING';
    }
    elsif( $log_level == $LOG_LEVEL_ERROR )
    {
        $log_level_name = 'ERROR';
    }
    elsif( $log_level == $LOG_LEVEL_FATAL )
    {
        $log_level_name = 'FATAL';
    }
    else
    {
        return;
    }

    my $pid  = $PROCESS_ID;
    my(
        $sec,
        $min,
        $hour,
        $mday,
        $mon,
        $year,
        $wday,
        $yday,
        $is_dst
      ) = localtime( time );

    my $time_stamp = sprintf(
        '%04d-%02d-%02d %02d:%02d:%02d',
        $year + 1900,
        $mon + 1,
        $mday,
        $hour,
        $min,
        $sec
    );

    my $log_message = "$time_stamp [$pid] $log_level_name: $message\n";

    if( is_interactive() || !$DAEMONIZE )
    {
        if( $log_level >= $LOG_LEVEL_ERROR )
        {
            print( STDERR $log_message );
        }
        else
        {
            print( STDOUT $log_message );
        }
    }
    else
    {
        print( $LOG_FH $log_message );
    }

    if( $log_level == $LOG_LEVEL_FATAL )
    {
        if( $SIG{__INT__} )
        {
            $SIG{__INT__}();
        }
        else
        {
            exit 1;
        }
    }

    return;
}

sub usage($) :Export( :MANDATORY )
{
    my( $message ) = validate_pos(
        @_,
        { type => SCALAR },
    );

    print "$message\n" if( $message );
    print $USAGE;

    exit( 1 );
}

# lsn_cmp( A, B ):
#  Compares two LSNs (Log Sequence Number)
#  and determines which is greater
#  Returns:
#   - -1 if( A < B )
#   - 0 if( A == B )
#   - 1 if( A > B )
#  LSNs are a 64-bit integer represented as two
#  32-bit values (expressed in hex),
#  separated by a slash IE:
#  XXXXXXXX/YYYYYYYY
sub lsn_cmp($$) :Export( :MANDATORY )
{
    my( $a_lsn, $b_lsn ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => SCALAR },
    );

    my $a_ms = $a_lsn;
    $a_ms =~ s/\/.*$//;
    $a_ms = 0 unless( $a_ms );
    my $b_ms = $b_lsn;
    $b_ms =~ s/\/.*$//;
    $b_ms = 0 unless( $b_ms );
    my $a_ls = $a_lsn;
    $a_ls =~ s/^[0-9a-fA-F]*\///;
    $a_ls = 0 unless( $a_ls );
    my $b_ls = $b_lsn;
    $b_ls =~ s/^[0-9a-fA-F]*\///;
    $b_ls = 0 unless( $b_ls );
    my $a_ms_int = hex( $a_ms );
    my $a_ls_int = hex( $a_ls );
    my $b_ms_int = hex( $b_ms );
    my $b_ls_int = hex( $b_ls );

    if( $a_ms_int == $b_ms_int )
    {
        return 1 if( $a_ls_int > $b_ls_int );
        return 0 if( $a_ls_int == $b_ls_int );
        return -1;
    }
    elsif( $a_ms_int > $b_ms_int )
    {
        return 1;
    }

    return -1;
}

sub set_program_name($$) :Export( :MANDATORY )
{
    my( $handle, $program_name ) = validate_pos(
        @_,
        { type => OBJECT | UNDEF },
        { type => SCALAR },
    );

    if( $handle )
    {
        $handle->do( "SET application_name = '$program_name'" );
    }

    $PROGRAM_NAME = $EXTENSION_NAME . ' ' . $program_name;
    return;
}

1;
