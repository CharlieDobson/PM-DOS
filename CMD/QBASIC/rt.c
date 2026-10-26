/*
 * RT.C - memory and strings for QBASIC (no C library is linked).  EDIT's;
 * the heap is MEM.C's.
 */
#include "rt.h"
#include "sys.h"

/* ------------------------------------------------------------------ */
/* memory and strings                                                  */
/* ------------------------------------------------------------------ */

void mem_set( void *dst, int val, u32 len )
{
    u8 *cur = (u8 *)dst;

    while ( len-- ) {
        *cur++ = (u8)val;
    }
}

void mem_cpy( void *dst, const void *src, u32 len )
{
    u8 *dcur = (u8 *)dst;
    const u8 *scur = (const u8 *)src;

    if ( dcur < scur ) {
        while ( len-- ) {
            *dcur++ = *scur++;
        }
    } else if ( dcur > scur ) {
        dcur += len;
        scur += len;
        while ( len-- ) {
            *--dcur = *--scur;
        }
    }
}

u32 str_len( const char *str )
{
    u32 len = 0;

    while ( str[len] ) {
        len++;
    }
    return len;
}

char *str_cpy( char *dst, const char *src )
{
    char *start = dst;

    while ( (*dst++ = *src++) != 0 ) {
    }
    return start;
}

/* a copy that stops short of "max" bytes including the NUL */
int str_cpyn( char *dst, const char *src, u32 max )
{
    u32 len = str_len( src );

    if ( len + 1 > max ) {
        return 0;
    }
    mem_cpy( dst, src, len + 1 );
    return 1;
}

/* as much of src as fits in max bytes, always ended */
void str_cpyt( char *dst, const char *src, u32 max )
{
    u32 len = str_len( src );

    if ( max == 0 ) {
        return;
    }
    if ( len > max - 1 ) {
        len = max - 1;
    }
    mem_cpy( dst, src, len );
    dst[len] = 0;
}

int str_catn( char *dst, const char *src, u32 max )
{
    u32 have = str_len( dst );

    if ( have >= max ) {
        return 0;
    }
    return str_cpyn( dst + have, src, max - have );
}

int ch_upper( int ch )
{
    ch &= 0xFF;
    if ( ch >= 'a' && ch <= 'z' ) {
        ch -= 'a' - 'A';
    }
    return ch;
}

int ch_lower( int ch )
{
    ch &= 0xFF;
    if ( ch >= 'A' && ch <= 'Z' ) {
        ch += 'a' - 'A';
    }
    return ch;
}

int str_icmp( const char *lhs, const char *rhs )
{
    int lch, rch;

    for ( ;; ) {
        lch = ch_upper( *lhs++ );
        rch = ch_upper( *rhs++ );
        if ( lch != rch ) {
            return lch - rch;
        }
        if ( lch == 0 ) {
            return 0;
        }
    }
}

int str_nicmp( const char *lhs, const char *rhs, u32 len )
{
    int lch, rch;

    for ( ; len; len-- ) {
        lch = ch_upper( *lhs++ );
        rch = ch_upper( *rhs++ );
        if ( lch != rch ) {
            return lch - rch;
        }
        if ( lch == 0 ) {
            return 0;
        }
    }
    return 0;
}

char *str_rchr( const char *str, int ch )
{
    const char *found = NULL;

    for ( ; *str; str++ ) {
        if ( *str == (char)ch ) {
            found = str;
        }
    }
    return (char *)found;
}

char *str_chr( const char *str, int ch )
{
    for ( ; *str; str++ ) {
        if ( *str == (char)ch ) {
            return (char *)str;
        }
    }
    return NULL;
}

int str_cmp( const char *lhs, const char *rhs )
{
    while ( *lhs && *lhs == *rhs ) {
        lhs++;
        rhs++;
    }
    return (int)(u8)*lhs - (int)(u8)*rhs;
}

int mem_cmp( const void *lhs, const void *rhs, u32 len )
{
    const u8 *lcur = (const u8 *)lhs;
    const u8 *rcur = (const u8 *)rhs;

    for ( ; len; len--, lcur++, rcur++ ) {
        if ( *lcur != *rcur ) {
            return (int)*lcur - (int)*rcur;
        }
    }
    return 0;
}

u32 dec_parse( const char *str, int *ok )
{
    u32 val = 0;

    *ok = 0;
    while ( *str >= '0' && *str <= '9' ) {
        val = val * 10 + (u32)(*str - '0');
        *ok = 1;
        str++;
    }
    if ( *str ) {
        *ok = 0;
    }
    return val;
}

char *fmt_dec( u32 val, int width, char *buf )
{
    char digits[12];
    int count = 0, pos = 0;

    do {
        digits[count++] = (char)('0' + val % 10);
        val /= 10;
    } while ( val );
    while ( width-- > count ) {
        buf[pos++] = ' ';
    }
    while ( count ) {
        buf[pos++] = digits[--count];
    }
    buf[pos] = 0;
    return buf;
}

