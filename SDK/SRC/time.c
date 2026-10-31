/*
 * TIME.C - time(), the broken-down times, TZ and strftime()
 *
 * DATES are counted in days from 1970-01-01 with the proleptic
 * Gregorian calendar, by the days-from-civil method: shift the year to
 * start in March, so February's extra day falls at the end of it.
 *
 * TZ is "std offset [dst [offset] [,start[/time],end[/time]]]", the
 * offset hours[:minutes[:seconds]] WEST of UTC.  The rules are Mm.w.d
 * (month, week 1-5 with 5 the last, weekday), Jn (day 1-365, no
 * February 29th) or n (day 0-365).  A daylight name with no rules
 * gets the US's: the second Sunday of March to the first Sunday of
 * November, at 2:00.
 */
#include "pmlib.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

long  timezone;
int   daylight;
static char std_name[8] = "UTC";
static char dst_name[8] = "";
char *tzname[2] = { std_name, dst_name };

/* when START.C read the clock: hundredths since midnight, and the day */
extern long __pm_clock0;
extern long __pm_day0;

/*--- the calendar ----------------------------------------------------*/

static long days_from_civil( long y, int m, int d )
{
    long era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days( long z, int *y, int *m, int *d )
{
    long era, doe, yoe, doy, mp, year;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(year + (*m <= 2));
}

static int is_leap( long y )
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/*--- reading the clock -----------------------------------------------*/

/* local time now, in seconds since 1970 as if local were UTC, and
   hundredths into *hs */
static long local_now( int *hs )
{
    pm_regs date, tim, again;
    long days;

    do {
        memset( &date, 0, sizeof( date ) );
        __pm_dos( &date, 0x2A00 );
        memset( &tim, 0, sizeof( tim ) );
        __pm_dos( &tim, 0x2C00 );
        memset( &again, 0, sizeof( again ) );
        __pm_dos( &again, 0x2A00 );
    } while ( (date.edx & 0xFFFF) != (again.edx & 0xFFFF) );   /* midnight in between */
    days = days_from_civil( (long)(date.ecx & 0xFFFF), (int)((date.edx >> 8) & 0xFF),
                            (int)(date.edx & 0xFF) );
    if ( hs != NULL ) {
        *hs = (int)(tim.edx & 0xFF);
    }
    return days * 86400L + (long)((tim.ecx >> 8) & 0xFF) * 3600L
           + (long)(tim.ecx & 0xFF) * 60L + (long)((tim.edx >> 8) & 0xFF);
}

/*--- TZ ---------------------------------------------------------------*/

typedef struct {
    char kind;                          /* 'M', 'J' or 'n' */
    int  a, b, c;                       /* month, week, day / the day */
    long at;                            /* seconds after midnight */
} RULE;

static RULE dst_start, dst_end;
static long dst_offset;                 /* seconds west, in daylight saving */
static int  tz_read;

static const char *tz_name( const char *s, char *out )
{
    int n = 0;

    if ( *s == '<' ) {                  /* <+03> quoted names */
        s++;
        while ( *s && *s != '>' ) {
            if ( n < 7 ) {
                out[n++] = *s;
            }
            s++;
        }
        if ( *s == '>' ) {
            s++;
        }
    } else {
        while ( isalpha( (unsigned char)*s ) ) {
            if ( n < 7 ) {
                out[n++] = *s;
            }
            s++;
        }
    }
    out[n] = '\0';
    return s;
}

/* [+-]hh[:mm[:ss]] -> seconds */
static const char *tz_time( const char *s, long *secs )
{
    int sign = 1;
    long h = 0, m = 0, sec = 0;

    if ( *s == '+' || *s == '-' ) {
        sign = (*s == '-') ? -1 : 1;
        s++;
    }
    while ( isdigit( (unsigned char)*s ) ) {
        h = h * 10 + (*s++ - '0');
    }
    if ( *s == ':' ) {
        for ( s++; isdigit( (unsigned char)*s ); s++ ) {
            m = m * 10 + (*s - '0');
        }
        if ( *s == ':' ) {
            for ( s++; isdigit( (unsigned char)*s ); s++ ) {
                sec = sec * 10 + (*s - '0');
            }
        }
    }
    *secs = sign * (h * 3600L + m * 60L + sec);
    return s;
}

static const char *tz_rule( const char *s, RULE *r )
{
    r->at = 7200;
    r->a = r->b = r->c = 0;
    if ( *s == 'M' ) {
        r->kind = 'M';
        r->a = (int)strtol( s + 1, (char **)&s, 10 );
        if ( *s == '.' ) {
            r->b = (int)strtol( s + 1, (char **)&s, 10 );
        }
        if ( *s == '.' ) {
            r->c = (int)strtol( s + 1, (char **)&s, 10 );
        }
    } else if ( *s == 'J' ) {
        r->kind = 'J';
        r->a = (int)strtol( s + 1, (char **)&s, 10 );
    } else {
        r->kind = 'n';
        r->a = (int)strtol( s, (char **)&s, 10 );
    }
    if ( *s == '/' ) {
        s = tz_time( s + 1, &r->at );
    }
    return s;
}

void tzset( void )
{
    const char *tz = getenv( "TZ" );
    const char *s;
    long off;

    tz_read = 1;
    timezone = 0;
    daylight = 0;
    strcpy( std_name, "UTC" );
    dst_name[0] = '\0';
    if ( tz == NULL || *tz == '\0' ) {
        return;
    }
    s = tz_name( tz, std_name );
    s = tz_time( s, &off );
    timezone = off;
    if ( !isalpha( (unsigned char)*s ) && *s != '<' ) {
        return;
    }
    s = tz_name( s, dst_name );
    daylight = 1;
    dst_offset = timezone - 3600;
    if ( *s && *s != ',' ) {
        s = tz_time( s, &dst_offset );
    }
    if ( *s == ',' ) {
        s = tz_rule( s + 1, &dst_start );
        if ( *s == ',' ) {
            tz_rule( s + 1, &dst_end );
        }
    } else {
        dst_start.kind = dst_end.kind = 'M';    /* the US since 2007 */
        dst_start.a = 3;
        dst_start.b = 2;
        dst_end.a = 11;
        dst_end.b = 1;
        dst_start.c = dst_end.c = 0;
        dst_start.at = dst_end.at = 7200;
    }
}

/* the day of year (0-based) on which a rule falls in year y */
static int rule_day( const RULE *r, int y )
{
    static const int before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int leap = is_leap( y );
    int first, wday, day, mdays;

    if ( r->kind == 'J' ) {
        return r->a - 1 + (leap && r->a > 59);
    }
    if ( r->kind == 'n' ) {
        return r->a;
    }
    first = before[r->a - 1] + (leap && r->a > 2);
    wday = (int)((days_from_civil( y, r->a, 1 ) + 4) % 7);      /* 1970-01-01 was a Thursday */
    if ( wday < 0 ) {
        wday += 7;
    }
    day = (r->c - wday + 7) % 7 + (r->b - 1) * 7;
    mdays = (r->a == 2) ? 28 + leap : 30 + ((r->a + (r->a > 7)) & 1);
    while ( day >= mdays ) {
        day -= 7;                       /* week 5: the last one */
    }
    return first + day;
}

/* is UTC time t (whose year is y) in daylight saving? */
static int in_dst( long t, int y )
{
    long year0 = days_from_civil( y, 1, 1 ) * 86400L;
    long start = year0 + rule_day( &dst_start, y ) * 86400L + dst_start.at + timezone;
    long end = year0 + rule_day( &dst_end, y ) * 86400L + dst_end.at + dst_offset;

    if ( !daylight ) {
        return 0;
    }
    if ( start < end ) {
        return t >= start && t < end;
    }
    return t >= start || t < end;       /* the southern hemisphere */
}

/*--- the C functions ------------------------------------------------*/

static struct tm *breakdown( long t, struct tm *tm )
{
    long days = t / 86400L;
    long secs = t % 86400L;
    int y, m, d;

    if ( secs < 0 ) {
        secs += 86400L;
        days--;
    }
    civil_from_days( days, &y, &m, &d );
    tm->tm_year = y - 1900;
    tm->tm_mon = m - 1;
    tm->tm_mday = d;
    tm->tm_hour = (int)(secs / 3600);
    tm->tm_min = (int)(secs / 60 % 60);
    tm->tm_sec = (int)(secs % 60);
    tm->tm_wday = (int)((days + 4) % 7);
    if ( tm->tm_wday < 0 ) {
        tm->tm_wday += 7;
    }
    tm->tm_yday = (int)(days - days_from_civil( y, 1, 1 ));
    tm->tm_isdst = 0;
    return tm;
}

time_t time( time_t *t )
{
    long now;
    long utc;
    struct tm tm;

    if ( !tz_read ) {
        tzset();
    }
    now = local_now( NULL );
    utc = now + timezone;
    if ( daylight ) {
        breakdown( utc, &tm );
        if ( in_dst( now + dst_offset, tm.tm_year + 1900 ) ) {
            utc = now + dst_offset;
        }
    }
    if ( t != NULL ) {
        *t = utc;
    }
    return utc;
}

int timespec_get( struct timespec *ts, int base )
{
    int hs;

    if ( base != TIME_UTC ) {
        return 0;
    }
    local_now( &hs );
    ts->tv_sec = time( NULL );
    ts->tv_nsec = hs * 10000000L;
    return base;
}

clock_t clock( void )
{
    int hs;
    long now = local_now( &hs );
    long day = now / 86400L;
    long hundredths = (now % 86400L) * 100L + hs;
    long day0 = days_from_civil( (__pm_day0 >> 16) & 0xFFFF, (int)((__pm_day0 >> 8) & 0xFF),
                                 (int)(__pm_day0 & 0xFF) );

    return (clock_t)((day - day0) * 8640000L + hundredths - __pm_clock0);
}

double difftime( time_t later, time_t earlier )
{
    return (double)later - (double)earlier;
}

struct tm *gmtime( const time_t *t )
{
    static struct tm tm;

    return breakdown( *t, &tm );
}

struct tm *localtime( const time_t *t )
{
    static struct tm tm;
    int dst;

    if ( !tz_read ) {
        tzset();
    }
    breakdown( *t - timezone, &tm );
    dst = in_dst( *t, tm.tm_year + 1900 );
    if ( dst ) {
        breakdown( *t - dst_offset, &tm );
        tm.tm_isdst = 1;
    }
    return &tm;
}

time_t mktime( struct tm *tm )
{
    long y = tm->tm_year + 1900L;
    long mon = tm->tm_mon;
    long days, local, utc;
    int dst;

    if ( !tz_read ) {
        tzset();
    }
    /* months out of range move the year; the rest just adds up */
    y += mon / 12;
    mon %= 12;
    if ( mon < 0 ) {
        mon += 12;
        y--;
    }
    days = days_from_civil( y, (int)mon + 1, 1 ) + tm->tm_mday - 1;
    local = days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
    utc = local + timezone;
    if ( daylight ) {
        if ( tm->tm_isdst > 0 ) {
            dst = 1;
        } else if ( tm->tm_isdst == 0 ) {
            dst = 0;
        } else {
            dst = in_dst( local + dst_offset, (int)y );
        }
        if ( dst ) {
            utc = local + dst_offset;
        }
    }
    *tm = *localtime( &utc );
    return utc;
}

static const char *const day_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const month_names[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"
};

char *asctime( const struct tm *tm )
{
    static char buf[32];

    strftime( buf, sizeof( buf ), "%a %b %e %H:%M:%S %Y\n", tm );
    return buf;
}

char *ctime( const time_t *t )
{
    return asctime( localtime( t ) );
}

/* ISO 8601 weeks: a year has 53 when it starts on a Thursday, or on a
   Wednesday in a leap year */
static int iso_weeks( int y )
{
    int jan1 = (int)(((days_from_civil( y, 1, 1 ) + 3) % 7 + 7) % 7);  /* Monday 0 */

    return (jan1 == 3 || (jan1 == 2 && is_leap( y ))) ? 53 : 52;
}

/* the ISO 8601 week-based year and week of tm */
static int iso_week( const struct tm *tm, int *year )
{
    int y = tm->tm_year + 1900;
    int wday = (tm->tm_wday + 6) % 7;           /* Monday 0 */
    int week = (tm->tm_yday - wday + 10) / 7;

    if ( week < 1 ) {
        y--;
        week = iso_weeks( y );
    } else if ( week > iso_weeks( y ) ) {
        week = 1;
        y++;
    }
    *year = y;
    return week;
}

size_t strftime( char *buf, size_t size, const char *fmt, const struct tm *tm )
{
    char tmp[64];
    size_t n = 0;
    size_t len;
    const char *text;
    int iy, hour12;
    long off;

    if ( size == 0 ) {
        return 0;
    }
    for ( ; *fmt; fmt++ ) {
        if ( *fmt != '%' ) {
            if ( n + 1 >= size ) {
                return 0;
            }
            buf[n++] = *fmt;
            continue;
        }
        fmt++;
        if ( *fmt == 'E' || *fmt == 'O' ) {
            fmt++;                      /* the C locale has no alternatives */
        }
        text = tmp;
        hour12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
        switch ( *fmt ) {
        case 'a': text = day_names[tm->tm_wday % 7]; strncpy( tmp, text, 3 ); tmp[3] = 0; text = tmp; break;
        case 'A': text = day_names[tm->tm_wday % 7]; break;
        case 'b':
        case 'h': strncpy( tmp, month_names[tm->tm_mon % 12], 3 ); tmp[3] = 0; break;
        case 'B': text = month_names[tm->tm_mon % 12]; break;
        case 'c': strftime( tmp, sizeof( tmp ), "%a %b %e %H:%M:%S %Y", tm ); break;
        case 'C': sprintf( tmp, "%02d", (tm->tm_year + 1900) / 100 ); break;
        case 'd': sprintf( tmp, "%02d", tm->tm_mday ); break;
        case 'D': strftime( tmp, sizeof( tmp ), "%m/%d/%y", tm ); break;
        case 'e': sprintf( tmp, "%2d", tm->tm_mday ); break;
        case 'F': strftime( tmp, sizeof( tmp ), "%Y-%m-%d", tm ); break;
        case 'g': iso_week( tm, &iy ); sprintf( tmp, "%02d", iy % 100 ); break;
        case 'G': iso_week( tm, &iy ); sprintf( tmp, "%d", iy ); break;
        case 'H': sprintf( tmp, "%02d", tm->tm_hour ); break;
        case 'I': sprintf( tmp, "%02d", hour12 ); break;
        case 'j': sprintf( tmp, "%03d", tm->tm_yday + 1 ); break;
        case 'm': sprintf( tmp, "%02d", tm->tm_mon + 1 ); break;
        case 'M': sprintf( tmp, "%02d", tm->tm_min ); break;
        case 'n': text = "\n"; break;
        case 'p': text = tm->tm_hour < 12 ? "AM" : "PM"; break;
        case 'r': strftime( tmp, sizeof( tmp ), "%I:%M:%S %p", tm ); break;
        case 'R': strftime( tmp, sizeof( tmp ), "%H:%M", tm ); break;
        case 'S': sprintf( tmp, "%02d", tm->tm_sec ); break;
        case 't': text = "\t"; break;
        case 'T': strftime( tmp, sizeof( tmp ), "%H:%M:%S", tm ); break;
        case 'u': sprintf( tmp, "%d", tm->tm_wday ? tm->tm_wday : 7 ); break;
        case 'U': sprintf( tmp, "%02d", (tm->tm_yday + 7 - tm->tm_wday) / 7 ); break;
        case 'V': sprintf( tmp, "%02d", iso_week( tm, &iy ) ); break;
        case 'w': sprintf( tmp, "%d", tm->tm_wday ); break;
        case 'W': sprintf( tmp, "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7 ); break;
        case 'x': strftime( tmp, sizeof( tmp ), "%m/%d/%y", tm ); break;
        case 'X': strftime( tmp, sizeof( tmp ), "%H:%M:%S", tm ); break;
        case 'y': sprintf( tmp, "%02d", (tm->tm_year + 1900) % 100 ); break;
        case 'Y': sprintf( tmp, "%d", tm->tm_year + 1900 ); break;
        case 'z':
            if ( !tz_read ) {
                tzset();
            }
            off = -((tm->tm_isdst > 0) ? dst_offset : timezone);
            sprintf( tmp, "%c%02ld%02ld", off < 0 ? '-' : '+', labs( off ) / 3600, labs( off ) / 60 % 60 );
            break;
        case 'Z':
            if ( !tz_read ) {
                tzset();
            }
            text = (tm->tm_isdst > 0) ? tzname[1] : tzname[0];
            break;
        case '%': text = "%"; break;
        case '\0': fmt--; text = ""; break;
        default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0; break;
        }
        len = strlen( text );
        if ( n + len >= size ) {
            return 0;
        }
        memcpy( buf + n, text, len );
        n += len;
    }
    buf[n] = '\0';
    return n;
}
