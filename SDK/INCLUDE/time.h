/*
 * TIME.H - dates and times
 *
 * The machine's clock keeps local time.  time_t counts seconds since
 * 1970 in UTC, worked out from the TZ variable as POSIX writes it:
 * TZ=EST5EDT is five hours west of UTC with the US's daylight saving
 * rules, TZ=CET-1CEST,M3.5.0,M10.5.0/3 is Central Europe.  With no TZ
 * the clock is taken to be UTC, and localtime() is gmtime().
 *
 * clock() counts in hundredths of a second from the start of the
 * program, as fine as the 18.2-a-second timer allows.
 */
#ifndef _TIME_H
#define _TIME_H

#include <_pmdefs.h>

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __pm_size_t size_t;
#endif

#ifndef _TIME_T_DEFINED
#define _TIME_T_DEFINED
typedef __pm_time_t time_t;
#endif
typedef long clock_t;

#define CLOCKS_PER_SEC  100
#define TIME_UTC        1

struct tm {
    int tm_sec;                 /* 0-60 */
    int tm_min;                 /* 0-59 */
    int tm_hour;                /* 0-23 */
    int tm_mday;                /* 1-31 */
    int tm_mon;                 /* 0-11 */
    int tm_year;                /* since 1900 */
    int tm_wday;                /* 0-6, Sunday 0 */
    int tm_yday;                /* 0-365 */
    int tm_isdst;               /* >0 daylight saving, 0 not, <0 not known */
};

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

_PM_BEGIN_C

time_t  time( time_t *t );
clock_t clock( void );
double  difftime( time_t later, time_t earlier );
time_t  mktime( struct tm *tm );
struct tm *gmtime( const time_t *t );
struct tm *localtime( const time_t *t );
char   *asctime( const struct tm *tm );
char   *ctime( const time_t *t );
size_t  strftime( char *buf, size_t size, const char *fmt, const struct tm *tm );
int     timespec_get( struct timespec *ts, int base );

/* TZ, as last read: seconds west of UTC, whether there is daylight
   saving, and the two names */
void    tzset( void );
extern long  timezone;
extern int   daylight;
extern char *tzname[2];

_PM_END_C

#endif
