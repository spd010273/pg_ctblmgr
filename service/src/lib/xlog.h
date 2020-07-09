#ifndef XLOG_H
#define XLOG_H

#include "util.h"

#define LSN_SEGMENT_LENGTH 8
#define LSN_TOTAL_LENGTH 18

extern uint64_t lsn_to_offset( char * );
extern char * offset_to_lsn( uint64_t );
extern uint32_t xid_in( char * );
extern char * xid_out( uint32_t );

#endif // XLOG_H
