/*
 * DOS2.C - DOS.H's functions
 */
#include "pmlib.h"
#include <dos.h>
#include <string.h>

/* 0, or the DOS error code with errno set */
static unsigned dos_result( pm_regs *regs, int carry )
{
    if ( !carry ) {
        return 0;
    }
    __pm_fail( regs );
    return (unsigned)_doserrno;
}

unsigned _dos_findfirst( const char *path, unsigned attrib, struct find_t *f )
{
    pm_regs regs = { 0 };

    regs.edx = (uint32_t)f;
    __pm_dos( &regs, 0x1A00 );
    memset( &regs, 0, sizeof( regs ) );
    regs.edx = (uint32_t)path;
    regs.ecx = attrib;
    if ( __pm_dos( &regs, 0x4E00 ) && PM_AX( regs ) == 0x02 ) {
        regs.eax = 0x12;                /* PM-DOS up to 0.69 says "not */
    }                                   /* found" for "no more files" */
    return dos_result( &regs, PM_CF( regs ) );
}

unsigned _dos_findnext( struct find_t *f )
{
    pm_regs regs = { 0 };

    regs.edx = (uint32_t)f;
    __pm_dos( &regs, 0x1A00 );          /* in case another find moved it */
    memset( &regs, 0, sizeof( regs ) );
    if ( __pm_dos( &regs, 0x4F00 ) && PM_AX( regs ) == 0x02 ) {
        regs.eax = 0x12;                /* PM-DOS up to 0.69 says "not */
    }                                   /* found" for "no more files" */
    return dos_result( &regs, PM_CF( regs ) );
}

unsigned _dos_findclose( struct find_t *f )
{
    (void)f;
    return 0;                           /* 4Eh keeps nothing open */
}

void _dos_getdate( struct dosdate_t *date )
{
    pm_regs regs = { 0 };

    __pm_dos( &regs, 0x2A00 );
    date->year = (unsigned short)regs.ecx;
    date->month = (unsigned char)(regs.edx >> 8);
    date->day = (unsigned char)regs.edx;
    date->dayofweek = (unsigned char)regs.eax;
}

unsigned _dos_setdate( const struct dosdate_t *date )
{
    pm_regs regs = { 0 };

    regs.ecx = date->year;
    regs.edx = ((uint32_t)date->month << 8) | date->day;
    __pm_dos( &regs, 0x2B00 );
    if ( (regs.eax & 0xFF) != 0 ) {
        regs.eax = 0x0D;                /* AL = FFh: not a valid date */
        return dos_result( &regs, 1 );
    }
    return 0;
}

void _dos_gettime( struct dostime_t *time )
{
    pm_regs regs = { 0 };

    __pm_dos( &regs, 0x2C00 );
    time->hour = (unsigned char)(regs.ecx >> 8);
    time->minute = (unsigned char)regs.ecx;
    time->second = (unsigned char)(regs.edx >> 8);
    time->hsecond = (unsigned char)regs.edx;
}

unsigned _dos_settime( const struct dostime_t *time )
{
    pm_regs regs = { 0 };

    regs.ecx = ((uint32_t)time->hour << 8) | time->minute;
    regs.edx = ((uint32_t)time->second << 8) | time->hsecond;
    __pm_dos( &regs, 0x2D00 );
    if ( (regs.eax & 0xFF) != 0 ) {
        regs.eax = 0x0D;
        return dos_result( &regs, 1 );
    }
    return 0;
}

/* 7303h first, whose counts are 32 bits; 36h's stop at 65535 */
unsigned _dos_getdiskfree( unsigned drive, struct diskfree_t *df )
{
    unsigned char rec[48];
    char root[4];
    pm_regs regs;

    if ( drive == 0 ) {
        memset( &regs, 0, sizeof( regs ) );
        __pm_dos( &regs, 0x1900 );
        drive = (regs.eax & 0xFF) + 1;
    }
    root[0] = (char)('A' + drive - 1);
    root[1] = ':';
    root[2] = '\\';
    root[3] = '\0';
    memset( rec, 0, sizeof( rec ) );
    memset( &regs, 0, sizeof( regs ) );
    regs.edx = (uint32_t)root;
    regs.edi = (uint32_t)rec;
    regs.ecx = sizeof( rec );
    if ( !__pm_dos( &regs, 0x7303 ) ) {
        /* Windows 95's record: the size at 0, the version at 2, then
           sectors per cluster, bytes per sector, free clusters and total
           clusters, dwords from 4.  PM-DOS 0.69's has no size word, so
           its fields start at 2; the size word (2Ch) tells them apart */
        int at = (rec[0] == 0x2C && rec[1] == 0) ? 4 : 2;

        df->sectors_per_cluster = *(uint32_t *)(rec + at);
        df->bytes_per_sector = *(uint32_t *)(rec + at + 4);
        df->avail_clusters = *(uint32_t *)(rec + at + 8);
        df->total_clusters = *(uint32_t *)(rec + at + 12);
        return 0;
    }
    memset( &regs, 0, sizeof( regs ) );
    regs.edx = drive;
    __pm_dos( &regs, 0x3600 );
    if ( (regs.eax & 0xFFFF) == 0xFFFF ) {
        regs.eax = 0x0F;                /* no such drive */
        return dos_result( &regs, 1 );
    }
    df->sectors_per_cluster = regs.eax & 0xFFFF;
    df->avail_clusters = regs.ebx & 0xFFFF;
    df->bytes_per_sector = regs.ecx & 0xFFFF;
    df->total_clusters = regs.edx & 0xFFFF;
    return 0;
}

unsigned _dos_getfileattr( const char *path, unsigned *attrib )
{
    pm_regs regs;

    for ( ;; ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.edx = (uint32_t)path;
        if ( !__pm_dos( &regs, __pm_lfn ? 0x7143 : 0x4300 ) ) {
            *attrib = regs.ecx & 0xFFFF;
            return 0;
        }
        if ( !__pm_lfn_missing( &regs ) ) {
            return dos_result( &regs, 1 );
        }
    }
}

unsigned _dos_setfileattr( const char *path, unsigned attrib )
{
    pm_regs regs;

    for ( ;; ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.edx = (uint32_t)path;
        regs.ebx = 1;
        regs.ecx = attrib;
        if ( !__pm_dos( &regs, __pm_lfn ? 0x7143 : 0x4301 ) ) {
            return 0;
        }
        if ( !__pm_lfn_missing( &regs ) ) {
            return dos_result( &regs, 1 );
        }
    }
}

unsigned _dos_getftime( int handle, unsigned *date, unsigned *time )
{
    pm_regs regs = { 0 };

    regs.ebx = (uint32_t)handle;
    if ( __pm_dos( &regs, 0x5700 ) ) {
        return dos_result( &regs, 1 );
    }
    *date = regs.edx & 0xFFFF;
    *time = regs.ecx & 0xFFFF;
    return 0;
}

unsigned _dos_setftime( int handle, unsigned date, unsigned time )
{
    pm_regs regs = { 0 };

    regs.ebx = (uint32_t)handle;
    regs.ecx = time;
    regs.edx = date;
    return dos_result( &regs, __pm_dos( &regs, 0x5701 ) );
}

/* hundredths since midnight */
static long now_hs( void )
{
    pm_regs regs = { 0 };

    __pm_dos( &regs, 0x2C00 );
    return (long)((regs.ecx >> 8) & 0xFF) * 360000L + (long)(regs.ecx & 0xFF) * 6000L
         + (long)((regs.edx >> 8) & 0xFF) * 100L + (long)(regs.edx & 0xFF);
}

void delay( unsigned ms )
{
    long start = now_hs();
    long want = (long)(ms / 10);
    long gone;

    for ( ;; ) {
        gone = now_hs() - start;
        if ( gone < 0 ) {
            gone += 8640000L;           /* past midnight */
        }
        if ( gone >= want ) {
            break;
        }
    }
}

unsigned sleep( unsigned seconds )
{
    while ( seconds > 60 ) {
        delay( 60000U );
        seconds -= 60;
    }
    delay( seconds * 1000U );
    return 0;
}

int usleep( unsigned long microseconds )
{
    delay( (unsigned)(microseconds / 1000UL) );
    return 0;
}
