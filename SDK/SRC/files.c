/*
 * FILES.C - remove(), rename(), tmpnam(), tmpfile() and perror()
 */
#include "pmlib.h"
#include "stream.h"
#include <stdlib.h>
#include <string.h>
#include <io.h>

int remove( const char *name )
{
    return unlink( name );
}

/* 7156h / 56h: EDX -> the old name, EDI -> the new one */
int rename( const char *from, const char *to )
{
    pm_regs regs;

    for ( ;; ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.edx = (uint32_t)from;
        regs.edi = (uint32_t)to;
        if ( !__pm_dos( &regs, __pm_lfn ? 0x7156 : 0x5600 ) ) {
            return 0;
        }
        if ( !__pm_lfn_missing( &regs ) ) {
            return __pm_fail( &regs );
        }
    }
}

/* A name that is not in use, in the directory TMP or TEMP names, or
   the current one: TMPnnnnn.$$$ */
char *tmpnam( char *buf )
{
    static char own[L_tmpnam];
    static unsigned serial;
    const char *dir = getenv( "TMP" );
    size_t len;
    unsigned tries;
    unsigned n;
    int i;

    if ( buf == NULL ) {
        buf = own;
    }
    if ( dir == NULL ) {
        dir = getenv( "TEMP" );
    }
    if ( dir == NULL ) {
        dir = "";
    }
    len = strlen( dir );
    if ( len + 14 > L_tmpnam ) {
        dir = "";
        len = 0;
    }
    memcpy( buf, dir, len );
    if ( len && dir[len - 1] != '\\' && dir[len - 1] != '/' && dir[len - 1] != ':' ) {
        buf[len++] = '\\';
    }
    for ( tries = 0; tries < TMP_MAX; tries++ ) {
        n = ++serial % 100000U;
        memcpy( buf + len, "TMP00000.$$$", 13 );
        for ( i = 7; i >= 3; i-- ) {
            buf[len + i] = (char)('0' + n % 10);
            n /= 10;
        }
        if ( access( buf, F_OK ) < 0 && errno == ENOENT ) {
            return buf;
        }
    }
    return NULL;
}

FILE *tmpfile( void )
{
    char name[L_tmpnam];
    FILE *fp;
    char *keep;

    if ( tmpnam( name ) == NULL ) {
        return NULL;
    }
    keep = strdup( name );
    if ( keep == NULL ) {
        return NULL;
    }
    fp = fopen( name, "wb+x" );
    if ( fp == NULL ) {
        free( keep );
        return NULL;
    }
    fp->tmpname = keep;
    return fp;
}

void perror( const char *s )
{
    const char *text = strerror( errno );

    if ( s != NULL && *s ) {
        fputs( s, stderr );
        fputs( ": ", stderr );
    }
    fputs( text, stderr );
    fputs( "\n", stderr );
}
