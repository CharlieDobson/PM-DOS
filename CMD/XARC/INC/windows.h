/*
 * WINDOWS.H - the sliver of the Win32 API the XArchive backend touches,
 * for PM-DOS.  NOT the real windows.h: the backend (ARC\*.C) is shared
 * with the Win32s program, which includes the genuine one, and with the
 * DOS and OS/2 builds, which have shims like this one.  The DOS build's
 * is DU32\XARCHIVE\DOSBUILD\WINDOWS.H; this is that one with the C
 * library under it swapped for XARC's own.
 *
 * A few integer and struct types, the lstr* and wsprintf string helpers,
 * one code-page conversion each way, and the three FILETIME/SYSTEMTIME
 * calendar calls.  The functions are in PMDOSPLT.C.
 */
#ifndef XA_PMDOS_WINDOWS_SHIM_H
#define XA_PMDOS_WINDOWS_SHIM_H

#include <stdio.h>      /* sprintf */
#include <string.h>     /* strcpy, strlen */

typedef unsigned long   DWORD;
typedef unsigned short  WORD;
typedef unsigned short  WCHAR;
typedef int             BOOL;
typedef char           *LPSTR;
typedef const char     *LPCSTR;
typedef void           *HINSTANCE;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define CP_ACP 0

typedef struct {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay;
    WORD wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

char *xa_lstrcpyn( char *dst, const char *src, int count );

#define lstrcpyn        xa_lstrcpyn
#define lstrcpy( d, s ) strcpy( (d), (s) )
#define lstrcat( d, s ) strcat( (d), (s) )
#define lstrlen( s )    ((int)strlen( s ))
#define wsprintf        sprintf

/* A name in a 7z or a RAR5 is Unicode; DOS's is a byte a character.  Each
   code point below 256 becomes the byte of the same value and anything
   above it '_', and MultiByteToWideChar is the exact inverse, so a name
   the writer stores comes back unchanged. */
int  WideCharToMultiByte( unsigned cp, DWORD flags,
                          const WCHAR *wstr, int wlen,
                          char *dst, int dstBytes,
                          const char *defChar, int *usedDef );
int  MultiByteToWideChar( unsigned cp, DWORD flags,
                          const char *src, int srcLen,
                          WCHAR *wdst, int wdstChars );

BOOL SystemTimeToFileTime( const SYSTEMTIME *st, FILETIME *ft );
BOOL FileTimeToSystemTime( const FILETIME *ft, SYSTEMTIME *st );
BOOL FileTimeToLocalFileTime( const FILETIME *ft, FILETIME *lft );

#endif
