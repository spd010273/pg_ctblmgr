package Util;

use strict;
use warnings;

use POSIX qw( setsid );
use Perl6::Export::Attrs;
use Readonly;
use IO::Interactive qw( is_interactive );
use Params::Validate qw( :all );
use English qw( -no_match_vars );

$OUTPUT_AUTOFLUSH = 1;

# Constants
Readonly::Scalar our $CLEAN_UP                  :Export( :MANDATORY ) => 0;
Readonly::Scalar our $LOG_LEVEL_FATAL           :Export( :MANDATORY ) => 5;
Readonly::Scalar our $LOG_LEVEL_ERROR           :Export( :MANDATORY ) => 4;
Readonly::Scalar our $LOG_LEVEL_WARNING         :Export( :MANDATORY ) => 3;
Readonly::Scalar our $LOG_LEVEL_INFO            :Export( :MANDATORY ) => 2;
Readonly::Scalar our $LOG_LEVEL_DEBUG           :Export( :MANDATORY ) => 1;
Readonly::Scalar our $WORKER_STATUS_STARTUP     :Export( :MANDATORY ) => 1;
Readonly::Scalar our $WORKER_STATUS_RUNNING     :Export( :MANDATORY ) => 2;
Readonly::Scalar our $WORKER_STATUS_IDLE        :Export( :MANDATORY ) => 3;
Readonly::Scalar our $WORKER_STATUS_EXITED      :Export( :MANDATORY ) => 4;
Readonly::Scalar our $WORKER_STATUS_INSERT      :Export( :MANDATORY ) => 5;
Readonly::Scalar our $WORKER_STATUS_SLOW_DELETE :Export( :MANDATORY ) => 6;
Readonly::Scalar our $WORKER_STATUS_FAST_DELETE :Export( :MANDATORY ) => 7;
Readonly::Scalar our $WORKER_STATUS_UPDATE      :Export( :MANDATORY ) => 8;
Readonly::Scalar our $WORKER_STATUS_TEMP_TABLE  :Export( :MANDATORY ) => 9;
Readonly::Scalar our $WORKER_STATUS_QUERY_PARSE :Export( :MANDATORY ) => 10;
Readonly::Scalar our $WORKER_STATUS_REPLACE     :Export( :MANDATORY ) => 11;
Readonly::Scalar our $EXTENSION_NAME            :Export( :MANDATORY ) => 'pg_ctblmgr';
Readonly::Scalar our $SCHEMA_NAME               :Export( :MANDATORY ) => 'pgctblmgr';
Readonly::Scalar our $SQL_STATE_ADMIN_TERM      :Export( :MANDATORY ) => '57P01';
Readonly::Scalar our $SQL_STATE_ADMIN_CANC      :Export( :MANDATORY ) => '57014';

###############################################################################
################################ USER GLOBALS #################################

# Enable verbose messaging related to process state
Readonly::Scalar our $DEBUG                        :Export( :MANDATORY ) => 0;

# In situations where we can normally perform a fast delete but the aged data set
# contains no rows, we will fall back to a slow delete in cases where this flag
# is set.
Readonly::Scalar our $CONSERVATIVE_FAST_DELETE     :Export( :MANDATORY ) => 0;

# Number of times to retry a query prior to giving up and going into an error
# condition.
Readonly::Scalar our $MAX_QUERY_RETRIES            :Export( :MANDATORY ) => 3;

###############################################################################
############################## QUERY PERFORMANCE ##############################

# Enables logic that correlates changes to base tables to changes in cache table
# output on a row-count basis. This helps pg_ctblmgr determine whether it should
# use temp tables or common table expressions when performing its DML updates
# to cache tables. If this is disabled, pg_ctblmgr will use the raw change count
# to determine if temp tables should be used. This can result no costly count
# queries, but also inefficient DML if a highly normalized table receives an
# update, resulting in large changes in the cache table outputs. Because of this
# if the blowout approximation logic is disabled, it's recommended to set
# temp_table_cutoff to a smaller value
Readonly::Scalar our $ENABLE_BLOWOUT_APPROXIMATE   :Export( :MANDATORY ) => 1;

Readonly::Scalar our $CLEAR_STATS_ON_SIGHUP        :Export( :MANDATORY ) => 0;

# Above this number of changes in a given worker, the worker will use temp tables
# in the following situations:
#  - Storing rows fast-forwarded from a historic timeline
#  - Storing the changed rows of a dataset when doing UPDATE / INSERT ops
# Below this threshold, workers will opt for CTE (Common Table Expressions) and
# VALUES() statements. This will reduce the number of MATERIALIZE operations a
# server experiences
Readonly::Scalar our $TEMP_TABLE_CUTOFF            :Export( :MANDATORY ) => 100;

# Number of changes that occur between updates to the blowout_factors of
# varios base tables as they relate to cache table output.
Readonly::Scalar our $CHANGES_BETWEEN_REAVG        :Export( :MANDATORY ) => 5;

# Enable fast delete functionality - 1 is enable, 0 is disable
Readonly::Scalar our $ENABLE_FAST_DELETE           :Export( :MANDATORY ) => 1;

# Entry length for historic transaction storage. This allows fast delete to work
Readonly::Scalar our $MAX_XID_LENGTH               :Export( :MANDATORY ) => 2;

# Soread of XID map - This is the spacing between historic snapshots in minutes
# the larger the number, the larger the spacing between XID map entries.
# This increases the likelyhood that long running tx's result in a
# fast delete.
Readonly::Scalar our $XID_MAP_SPREAD               :Export( :MANDATORY ) => 300;

# When creating cache tables, use LIMIT / OFFSET to populate the table rather
# than one insert
Readonly::Scalar our $BATCHED_CREATE               :Export( :MANDATORY ) => 0;

# Batch size for the above batched create mode.
Readonly::Scalar our $BATCH_SIZE                   :Export( :MANDATORY ) => 1000000;

# Number of temp table tuple we toggle into a bulk update mode ( delete +
# insert ) rather than doing update.
Readonly::Scalar our $BULK_ACTION_CUTOFF           :Export( :MANDATORY ) => 100000;

# In cases where multiple relations are involved in a single outer join, the
# filter for the outer relation only goes to the grouped relation, if present.
Readonly::Scalar our $OUTER_GROUPED_RELS_ONLY      :Export( :MANDATORY ) => 1;

# In cases where multiple relations are involved in a single outer join, and no
# group by is present we will only filter the largest relation.
Readonly::Scalar our $OUTER_FALLBACK_TO_LARGEST    :Export( :MANDATORY ) => 1;

# in cases where bulk changes would apply filters to different sections of the
# query, do not attempt to combine those filters into the same query to avoid
# accidentally logically ANDing them.
Readonly::Scalar our $CONSERVATIVE_TABLE_FILTERING :Export( :MANDATORY ) => 1;

# Globals initialized at runtime start
our $PARENT_PID    :Export( :MANDATORY ) = 0;
our $DAEMONIZE     :Export( :MANDATORY ) = 0;
our $LOG_FH        :Export( :MANDATORY ) = undef;
our $LOG_FILE      :Export( :MANDATORY ) = '';
our $PID_LOCKSTATE :Export( :MANDATORY ) = '';

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

    if( is_interactive() || !$DAEMONIZE || !defined( $LOG_FH ) )
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

sub daemonize() :Export( :MANDATORY )
{
    unless( open STDIN, '/dev/null' )
    {
        die( "Can't read /dev/null: $!" );
    }

    unless( open STDOUT, '>/dev/null' )
    {
        die( "Can't write to /dev/null: $!" );
    }

    my $pid = fork();

    unless( defined( $pid ) )
    {
        die( "Can't fork: $!" );
    }

    if( $pid )
    {
        exit 0;
    }

    unless( setsid() )
    {
        die( "Can't start a new session: $!" );
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
        { type => SCALAR | UNDEF },
        { type => SCALAR | UNDEF },
    );

    if( defined( $a_lsn ) != defined( $b_lsn ) )
    {
        if( defined( $a_lsn ) && !defined( $b_lsn ) )
        {
            return 1;
        }
        elsif( !defined( $a_lsn ) && defined( $b_lsn ) )
        {
            return -1;
        }
    }
    elsif( !defined( $a_lsn ) && !defined( $b_lsn ) )
    {
        return 0;
    }

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

    $PROGRAM_NAME = $EXTENSION_NAME . ' ' . $program_name . ' ' . $PID_LOCKSTATE;
    return;
}

1;
