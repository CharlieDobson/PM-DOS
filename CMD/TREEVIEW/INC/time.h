/*
 * TIME.H - XARC's C library: the calendar (LIBIO.C).
 *
 * A time_t is seconds since 1 January 1970, and it is LOCAL time: DOS
 * keeps no time zone, so localtime is the exact inverse of how a DOS
 * date and time become a time_t, and nothing moves on the way through.
 */
#ifndef _XA_TIME_H
#define _XA_TIME_H

#include <stddef.h>

#ifndef _XA_TIME_T
#define _XA_TIME_T
typedef unsigned long time_t;      /* to 2106; DOS dates end in 2107 */
#endif

struct tm {
    int tm_sec;         /* 0-59 */
    int tm_min;         /* 0-59 */
    int tm_hour;        /* 0-23 */
    int tm_mday;        /* 1-31 */
    int tm_mon;         /* 0-11 */
    int tm_year;        /* since 1900 */
    int tm_wday;        /* 0 = Sunday */
    int tm_yday;        /* 0-365 */
    int tm_isdst;
};

time_t     time( time_t *now );
struct tm *localtime( const time_t *when );
time_t     mktime( struct tm *when );

#endif
