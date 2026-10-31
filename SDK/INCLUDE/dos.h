/*
 * DOS.H - the DOS calls other DOS compilers give C names to
 *
 * The _dos_ functions return 0, or the DOS error code (and set errno
 * as well).  _dos_findfirst() and _dos_findnext() see 8.3 names; use
 * DIRENT.H for long ones.  For any call that has no function here,
 * PMDOS.H's pm_int21() issues it directly.
 */
#ifndef _DOS_H
#define _DOS_H

#include <_pmdefs.h>
#include <pmdos.h>

#define _A_NORMAL       0x00
#define _A_RDONLY       0x01
#define _A_HIDDEN       0x02
#define _A_SYSTEM       0x04
#define _A_VOLID        0x08
#define _A_SUBDIR       0x10
#define _A_ARCH         0x20

/* the DTA 4Eh fills: the layout is DOS's */
struct find_t {
    char            reserved[21];
    char            attrib;
    unsigned short  wr_time;
    unsigned short  wr_date;
    unsigned long   size;
    char            name[13];
};

struct dosdate_t {
    unsigned char   day;                /* 1-31 */
    unsigned char   month;              /* 1-12 */
    unsigned short  year;
    unsigned char   dayofweek;          /* 0 = Sunday */
};

struct dostime_t {
    unsigned char   hour;
    unsigned char   minute;
    unsigned char   second;
    unsigned char   hsecond;            /* hundredths */
};

struct diskfree_t {
    unsigned long   total_clusters;
    unsigned long   avail_clusters;
    unsigned long   sectors_per_cluster;
    unsigned long   bytes_per_sector;
};

_PM_BEGIN_C

unsigned _dos_findfirst( const char *path, unsigned attrib, struct find_t *f );
unsigned _dos_findnext( struct find_t *f );
unsigned _dos_findclose( struct find_t *f );

void     _dos_getdate( struct dosdate_t *date );
unsigned _dos_setdate( const struct dosdate_t *date );
void     _dos_gettime( struct dostime_t *time );
unsigned _dos_settime( const struct dostime_t *time );

/* drive 0 is the current one, 1 is A: */
unsigned _dos_getdiskfree( unsigned drive, struct diskfree_t *df );

unsigned _dos_getfileattr( const char *path, unsigned *attrib );
unsigned _dos_setfileattr( const char *path, unsigned attrib );
unsigned _dos_getftime( int handle, unsigned *date, unsigned *time );
unsigned _dos_setftime( int handle, unsigned date, unsigned time );

/* wait this many milliseconds, to the timer's 55 */
void     delay( unsigned ms );

_PM_END_C

#endif
