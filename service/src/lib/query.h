#ifndef QUERY_H
#define QUERY_H

#include "libpq-fe.h"
#include "util.h"
#include <stdbool.h>

#define MAX_CONN_RETRIES 5

#define SQL_STATE_TERMINATED_BY_ADMINISTRATOR "57P01"
#define SQL_STATE_CANCELED_BY_ADMINISTRATOR "57014"
#define SQL_STATE_CONNECTION_FAILURE "08006"
#define SQL_STATE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION "08001"
#define SQL_STATE_CONNECTION_DOES_NOT_EXIST "08003"
#define SQL_STATE_CONNECTION_EXCEPTION "08000"

extern PGresult * execute_query( struct worker *, char *, char **, unsigned int );
extern bool db_connect( struct worker * );

extern bool begin_transaction( struct worker * );
extern bool commit_transaction( struct worker * );
extern bool rollback_transaction( struct worker * );

extern char * get_column_value( int, PGresult *, char * );
extern bool is_column_null( int, PGresult *, char * );
#endif // QUERY_H
