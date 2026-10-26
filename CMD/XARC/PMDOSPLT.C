/*
 * PMDOSPLT.C - the XArchive backend's platform, PM-DOS native.
 *
 * Compiled INSTEAD OF PLATFORM.C (Win32), DOSPLAT.C (DOS/32A) and
 * OS2PLAT.C, and it carries the DOS build's COMPAT.C as well: the few
 * Win32 calls the backend makes through WINDOWS.H, and three that are
 * PM-DOS's own, which ARCFILE.C and NUMFMT.C call where the other
 * builds ask their own systems:
 *
 *   PmMemFree        INT 21h/48h for the impossible, which answers with
 *                    the largest block it could have given
 *   PmNamesNeed83    71A0h on the destination's root: does it hold long
 *                    names?  It does whenever the 71h calls are there
 *   PmThousandsSep   21h/38h with the buffer in the program's memory -
 *                    no extender, so no detour through real memory
 *
 * A FOLDER IS READ WITH 714Eh, so the archive writer gets long names,
 * and with the classic 4Eh/4Fh when CONFIG.SYS has LFN=OFF.  The DOS
 * date and time come out of the search itself; the FILETIME made from
 * them is that stamp read as UTC, which is what the extractor assumes
 * on the way back out - DOS keeps no time zone (see DOSPLAT.C).
 *
 * The FILETIME arithmetic is 64-bit, done in 32-bit pieces (XSYS.H):
 * XARC links none of Watcom's run-time helpers.
 */
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "platform.h"
#include "xsys.h"

/* ------------------------------------------------------------------ */
/* the Win32 calls (the DOS build's COMPAT.C)                          */
/* ------------------------------------------------------------------ */

char *xa_lstrcpyn( char *dst, const char *src, int count )
{
    int at;

    if ( count <= 0 ) {
        return dst;
    }
    for ( at = 0; at < count - 1 && src[at]; at++ ) {
        dst[at] = src[at];
    }
    dst[at] = 0;
    return dst;
}

int WideCharToMultiByte( unsigned cp, DWORD flags, const WCHAR *wstr, int wlen,
                         char *dst, int dstBytes, const char *defChar, int *usedDef )
{
    int at = 0;

    (void)cp; (void)flags; (void)wlen; (void)defChar; (void)usedDef;
    while ( wstr[at] && at < dstBytes - 1 ) {
        dst[at] = wstr[at] < 0x100 ? (char)wstr[at] : '_';
        at++;
    }
    dst[at] = 0;
    return at + 1;
}

/* srcLen -1 means "up to and including the NUL", and the count returned
   includes it, as Win32's does */
int MultiByteToWideChar( unsigned cp, DWORD flags, const char *src, int srcLen,
                         WCHAR *wdst, int wdstChars )
{
    int at, len = srcLen < 0 ? (int)strlen( src ) + 1 : srcLen;

    (void)cp; (void)flags;
    for ( at = 0; at < len && at < wdstChars; at++ ) {
        wdst[at] = (WCHAR)(u8)src[at];
    }
    return at;
}

#define DAYS_1601_TO_1970   134774UL
#define TICKS_PER_SEC       10000000UL

BOOL SystemTimeToFileTime( const SYSTEMTIME *st, FILETIME *ft )
{
    long days = xa_days_from_civil( st->wYear, st->wMonth, st->wDay );
    u32 tod = (u32)st->wHour * 3600UL + (u32)st->wMinute * 60UL + st->wSecond;
    u64 secs, ticks, upper;

    if ( days + (long)DAYS_1601_TO_1970 < 0 ) {
        return FALSE;                       /* before 1601 */
    }
    secs = xa_mul32( (u32)(days + (long)DAYS_1601_TO_1970), 86400UL ) + tod;
    /* seconds times ten million, the high half and the low half apart */
    ticks = xa_mul32( xa_lo32( secs ), TICKS_PER_SEC );
    upper = xa_mul32( xa_hi32( secs ), TICKS_PER_SEC );
    ticks = ticks + xa_make64( xa_lo32( upper ), 0 );
    ft->dwLowDateTime = xa_lo32( ticks );
    ft->dwHighDateTime = xa_hi32( ticks );
    return TRUE;
}

BOOL FileTimeToSystemTime( const FILETIME *ft, SYSTEMTIME *st )
{
    u64 ticks = xa_make64( ft->dwHighDateTime, ft->dwLowDateTime );
    u32 tod;
    long days;
    int year, month, day;

    xa_div64( &ticks, TICKS_PER_SEC );      /* seconds since 1601 */
    tod = xa_div64( &ticks, 86400UL );      /* and days */
    days = (long)xa_lo32( ticks ) - (long)DAYS_1601_TO_1970;
    xa_civil_from_days( days, &year, &month, &day );
    st->wYear = (WORD)year;
    st->wMonth = (WORD)month;
    st->wDay = (WORD)day;
    st->wHour = (WORD)(tod / 3600);
    st->wMinute = (WORD)(tod % 3600 / 60);
    st->wSecond = (WORD)(tod % 60);
    st->wMilliseconds = 0;
    st->wDayOfWeek = (WORD)((days % 7 + 11) % 7);     /* 1970-01-01: Thursday */
    return TRUE;
}

/* DOS has no time zone: see the header */
BOOL FileTimeToLocalFileTime( const FILETIME *ft, FILETIME *lft )
{
    *lft = *ft;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* stamps and attributes                                               */
/* ------------------------------------------------------------------ */

/* 716Ch or 3Dh, for reading - which is enough for 5701h, and works on a
   file that is already read-only; a folder cannot be opened and keeps
   the time it has, as under DOS */
static int open_read( const char *path, u32 *handle )
{
    REGS regs;
    int rc = -1;

    if ( xa_lfn ) {
        xa_clear( &regs );
        regs.esi = (u32)path;
        regs.edx = 0x01;
        regs.flags = 1;
        rc = xa_dos( &regs, 0x716C );
        if ( rc == 0x7100 ) {
            rc = -1;
        }
    }
    if ( rc < 0 ) {
        xa_clear( &regs );
        regs.edx = (u32)path;
        rc = xa_dos( &regs, 0x3D00 );
    }
    *handle = AX( regs );
    return rc;
}

static void apply_stamp( const char *path, unsigned date, unsigned clock )
{
    REGS regs;
    u32 handle;

    if ( open_read( path, &handle ) != 0 ) {
        return;
    }
    xa_clear( &regs );
    regs.ebx = handle;
    regs.ecx = clock;
    regs.edx = date;
    xa_dos( &regs, 0x5701 );
    xa_clear( &regs );
    regs.ebx = handle;
    xa_dos( &regs, 0x3E00 );
}

void SetFileDosMTime( const char *path, WORD dosDate, WORD dosTime )
{
    if ( dosDate == 0 && dosTime == 0 ) {
        return;
    }
    apply_stamp( path, dosDate, dosTime );
}

void SetFileMTime( const char *path, const FILETIME *ft )
{
    SYSTEMTIME st;

    if ( !FileTimeToSystemTime( ft, &st ) || st.wYear < 1980 || st.wYear > 2107 ) {
        return;                             /* outside the DOS epoch */
    }
    apply_stamp( path, (unsigned)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay),
                 (unsigned)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond >> 1)) );
}

/* 7143h, or 43h: BL / AL 0 to read into CX, 1 to set from CX */
static int attr_call( const char *path, int set, unsigned *attr )
{
    REGS regs;
    int rc = -1;

    if ( xa_lfn ) {
        xa_clear( &regs );
        regs.edx = (u32)path;
        regs.ebx = (u32)set;
        regs.ecx = *attr;
        regs.flags = 1;
        rc = xa_dos( &regs, 0x7143 );
        if ( rc == 0x7100 ) {
            rc = -1;
        }
    }
    if ( rc < 0 ) {
        xa_clear( &regs );
        regs.edx = (u32)path;
        regs.ecx = *attr;
        rc = xa_dos( &regs, 0x4300 | (u32)set );
    }
    if ( rc == 0 && !set ) {
        *attr = (unsigned)(regs.ecx & 0xFFFF);
    }
    return rc;
}

/* R, H, S and A as the archive had them; a folder takes only H and S,
   and one with neither is left alone (see PLATFORM.H) */
void SetFileDosAttr( const char *path, DWORD attr, int isDir )
{
    unsigned now = 0, want;

    if ( isDir ) {
        attr &= 0x02 | 0x04;
        if ( attr == 0 || attr_call( path, 0, &now ) != 0 ) {
            return;
        }
        attr |= now & (0x01 | 0x20);
    } else {
        attr &= 0x01 | 0x02 | 0x04 | 0x20;
    }
    want = (unsigned)attr;
    attr_call( path, 1, &want );
}

void ClearFileAttr( const char *path )
{
    unsigned attr = 0;

    if ( attr_call( path, 0, &attr ) != 0 || !(attr & (0x01 | 0x02 | 0x04)) ) {
        return;
    }
    attr &= 0x20;
    attr_call( path, 1, &attr );
}

void ClearArchiveBit( const char *path )
{
    unsigned attr = 0;

    if ( attr_call( path, 0, &attr ) != 0 || !(attr & 0x20) ) {
        return;
    }
    attr &= 0x01 | 0x02 | 0x04;
    attr_call( path, 1, &attr );
}

/* Win32-only shell helpers: nothing here calls them */
int  IsModernShell( void )            { return 0; }
void InitCtl3d( HINSTANCE hInst )     { (void)hInst; }
void CleanupCtl3d( HINSTANCE hInst )  { (void)hInst; }

/* No paging: what malloc hands over is RAM (see PLATFORM.H) */
DWORD PlatPhysFree( void ) { return 0; }

/* ------------------------------------------------------------------ */
/* reading a folder                                                    */
/* ------------------------------------------------------------------ */

#define FIND_ALL    0x37        /* all but the volume label */

typedef struct {
    int lfn;                    /* 714Eh's handle in "handle", or the DTA's */
    u32 handle;
    u8  rec[320];               /* 714Eh's record, or the DTA */
} PLATSEARCH;

static void plat_fill( const PLATSEARCH *search, PlatFind *find )
{
    SYSTEMTIME st;
    const char *name;
    u32 stamp;

    if ( search->lfn ) {
        name = (const char *)search->rec + 0x2C;
        find->attr = RD32( search->rec ) & 0x3F;
        stamp = RD32( search->rec + 0x14 );         /* date high, time low */
        find->size = RD32( search->rec + 0x20 );
    } else {
        name = (const char *)search->rec + 0x1E;
        find->attr = search->rec[0x15] & 0x3F;
        stamp = ((u32)RD16( search->rec + 0x18 ) << 16) | RD16( search->rec + 0x16 );
        find->size = RD32( search->rec + 0x1A );
    }
    strncpy( find->name, name, PLAT_NAME_MAX - 1 );
    find->name[PLAT_NAME_MAX - 1] = 0;
    if ( find->attr & 0x10 ) {
        find->size = 0;
    }
    find->tooBig = find->size >= 0x80000000UL;
    find->dosDate = (WORD)(stamp >> 16);
    find->dosTime = (WORD)(stamp & 0xFFFF);

    st.wYear = (WORD)(((find->dosDate >> 9) & 0x7F) + 1980);
    st.wMonth = (WORD)((find->dosDate >> 5) & 0x0F);
    st.wDay = (WORD)(find->dosDate & 0x1F);
    st.wHour = (WORD)((find->dosTime >> 11) & 0x1F);
    st.wMinute = (WORD)((find->dosTime >> 5) & 0x3F);
    st.wSecond = (WORD)((find->dosTime & 0x1F) * 2);
    st.wMilliseconds = 0;
    st.wDayOfWeek = 0;
    if ( st.wMonth < 1 ) {
        st.wMonth = 1;
    }
    if ( st.wDay < 1 ) {
        st.wDay = 1;
    }
    SystemTimeToFileTime( &st, &find->mtime );
}

/* the classic search keeps its place in the DTA, so the DTA is pointed
   at this search's own before every call */
static int classic_find( PLATSEARCH *search, const char *pattern )
{
    REGS regs;

    xa_clear( &regs );
    regs.edx = (u32)search->rec;
    xa_dos( &regs, 0x1A00 );
    xa_clear( &regs );
    if ( pattern ) {
        regs.edx = (u32)pattern;
        regs.ecx = FIND_ALL;
        return xa_dos( &regs, 0x4E00 );
    }
    return xa_dos( &regs, 0x4F00 );
}

void *PlatFindFirst( const char *pattern, PlatFind *f )
{
    PLATSEARCH *search = (PLATSEARCH *)malloc( sizeof( PLATSEARCH ) );
    REGS regs;
    int rc = -1;

    if ( search == NULL ) {
        return NULL;
    }
    search->lfn = 0;
    if ( xa_lfn ) {
        xa_clear( &regs );
        regs.edx = (u32)pattern;
        regs.edi = (u32)search->rec;
        regs.ecx = FIND_ALL;
        regs.esi = 1;                       /* DOS date and time */
        regs.flags = 1;
        rc = xa_dos( &regs, 0x714E );
        if ( rc == 0x7100 ) {
            xa_lfn = 0;
            rc = -1;
        } else if ( rc == 0 ) {
            search->lfn = 1;
            search->handle = AX( regs );
        }
    }
    if ( rc < 0 ) {
        rc = classic_find( search, pattern );
        /* the classic search can still hand back the volume label */
        while ( rc == 0 && (search->rec[0x15] & 0x08) ) {
            rc = classic_find( search, NULL );
        }
    }
    if ( rc != 0 ) {
        free( search );
        return NULL;
    }
    plat_fill( search, f );
    return search;
}

int PlatFindNext( void *h, PlatFind *f )
{
    PLATSEARCH *search = (PLATSEARCH *)h;
    REGS regs;
    int rc;

    if ( search->lfn ) {
        xa_clear( &regs );
        regs.ebx = search->handle;
        regs.edi = (u32)search->rec;
        regs.esi = 1;
        rc = xa_dos( &regs, 0x714F );
    } else {
        do {
            rc = classic_find( search, NULL );
        } while ( rc == 0 && (search->rec[0x15] & 0x08) );
    }
    if ( rc != 0 ) {
        return 0;
    }
    plat_fill( search, f );
    return 1;
}

void PlatFindClose( void *h )
{
    PLATSEARCH *search = (PLATSEARCH *)h;
    REGS regs;

    if ( search == NULL ) {
        return;
    }
    if ( search->lfn ) {
        xa_clear( &regs );
        regs.ebx = search->handle;
        xa_dos( &regs, 0x71A1 );
    }
    free( search );
}

/* ------------------------------------------------------------------ */
/* PM-DOS's own: see the header                                        */
/* ------------------------------------------------------------------ */

unsigned int PmMemFree( void )
{
    REGS regs;

    xa_clear( &regs );
    regs.ebx = 0xFFFFFFF0UL;
    if ( xa_dos( &regs, 0x4800 ) == 0 ) {
        /* it cannot have given that much; if it did, it has it back */
        regs.edx = regs.eax;
        xa_dos( &regs, 0x4900 );
        return 0;
    }
    return (unsigned int)regs.ebx;
}

int PmNamesNeed83( const char *path )
{
    REGS regs;
    char root[4], fs[32];
    int rc;

    if ( !xa_lfn ) {
        return 1;
    }
    if ( path && path[0] && path[1] == ':' ) {
        root[0] = path[0];
        root[1] = ':';
        root[2] = '\\';
        root[3] = 0;
    } else {
        root[0] = '\\';
        root[1] = 0;
    }
    xa_clear( &regs );
    regs.edx = (u32)root;
    regs.edi = (u32)fs;
    regs.ecx = sizeof( fs );
    regs.flags = 1;
    rc = xa_dos( &regs, 0x71A0 );
    if ( rc == 0x7100 ) {
        xa_lfn = 0;
        return 1;
    }
    if ( rc != 0 ) {
        return 0;               /* the calls are there: the volume will be too */
    }
    return (regs.ecx & 0xFFFF) <= 12;
}

int PmThousandsSep( char *buf, int size )
{
    REGS regs;
    u8 block[64];
    int len;

    memset( block, 0, sizeof( block ) );
    xa_clear( &regs );
    regs.edx = (u32)block;
    if ( xa_dos( &regs, 0x3800 ) != 0 ) {
        return 0;
    }
    /* offset 7: a two-byte ASCIIZ field, believed only when it is a
       terminated string that is not empty */
    for ( len = 0; len < 2 && block[7 + len]; len++ ) {
    }
    if ( len == 0 || block[7 + len] != 0 || len >= size ) {
        return 0;
    }
    memcpy( buf, block + 7, (size_t)len );
    buf[len] = 0;
    return 1;
}
