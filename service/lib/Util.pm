package Util;

use Perl6::Export::Attrs;

Readonly::Scalar our $DEBUG                  :Export( :MANDATORY ) => 1;
Readonly::Scalar our $CLEAN_UP               :Export( :MANDATORY ) => 0; # Emergency shm cleanup
Readonly::Scalar our $LOG_LEVEL_FATAL        :Export( :MANDATORY ) => 5;
Readonly::Scalar our $LOG_LEVEL_ERROR        :Export( :MANDATORY ) => 4;
Readonly::Scalar our $LOG_LEVEL_WARNING      :Export( :MANDATORY ) => 3;
Readonly::Scalar our $LOG_LEVEL_INFO         :Export( :MANDATORY ) => 2;
Readonly::Scalar our $LOG_LEVEL_DEBUG        :Export( :MANDATORY ) => 1;
Readonly::Scalar our $WORKER_STATUS_STARTUP  :Export( :MANDATORY ) => 1;
Readonly::Scalar our $WORKER_STATUS_RUNNING  :Export( :MANDATORY ) => 2;
Readonly::Scalar our $WORKER_STATUS_UPDATING :Export( :MANDATORY ) => 3;
Readonly::Scalar our $WORKER_STATUS_EXITED   :Export( :MANDATORY ) => 4;
Readonly::Scalar our $EXTENSION_NAME         :Export( :MANDATORY ) => 'pg_ctblmgr';
Readonly::Scalar our $SCHEMA_NAME            :Export( :MANDATORY ) => 'pgctblmgr';
Readonly::Scalar our $SQL_STATE_ADMIN_TERM   :Export( :MANDATORY ) => '57P01';
Readonly::Scalar our $SQL_STATE_ADMIN_CANC   :Export( :MANDATORY ) => '57014';
Readonly::Scalar our $MAX_QUERY_RETRIES      :Export( :MANDATORY ) => 5;

our $PARENT_PID :Export( :MANDATORY ) = 0;

1;
