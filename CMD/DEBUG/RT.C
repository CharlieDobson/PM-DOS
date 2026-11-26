/*
 * RT.C - minimal runtime for DEBUG (no C library is linked): memory,
 * strings, the heap and the output line.
 */
#include "rt.h"
#include "sys.h"

/* ------------------------------------------------------------------ */
/* memory and strings                                                  */
/* ------------------------------------------------------------------ */

void mem_set( void *dst, int val, unsigned int len )
{
    uint8_t *cur = (uint8_t *)dst;

    while ( len-- ) {
        *cur++ = (uint8_t)val;
    }
}

/* safe for overlapping blocks, in either direction */
void mem_cpy( void *dst, const void *src, unsigned int len )
{
    uint8_t *dcur = (uint8_t *)dst;
    const uint8_t *scur = (const uint8_t *)src;

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

int mem_cmp( const void *lhs, const void *rhs, unsigned int len )
{
    const uint8_t *lcur = (const uint8_t *)lhs;
    const uint8_t *rcur = (const uint8_t *)rhs;

    for ( ; len; len-- ) {
        if ( *lcur != *rcur ) {
            return (int)*lcur - (int)*rcur;
        }
        lcur++;
        rcur++;
    }
    return 0;
}

unsigned int str_len( const char *str )
{
    unsigned int len = 0;

    while ( str[len] ) {
        len++;
    }
    return len;
}

void str_cpy( char *dst, const char *src )
{
    while ( (*dst++ = *src++) != 0 ) {
    }
}

int ch_upper( int ch )
{
    ch &= 0xFF;
    if ( ch >= 'a' && ch <= 'z' ) {
        ch -= 'a' - 'A';
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

int str_nicmp( const char *lhs, const char *rhs, unsigned int len )
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

/* ------------------------------------------------------------------ */
/* the heap: DEBUG buys its buffers once, at the start, and keeps them */
/* ------------------------------------------------------------------ */

void *try_alloc( unsigned int size )
{
    return sys_mem_alloc( size );
}

/* A command that needs a buffer for itself alone (M) takes this one: it
   is kept between commands and only bought again when a command needs
   more, so a command ended early by an error leaves nothing behind. */
static void        *scratch;
static unsigned int scratch_len;

void *scratch_get( unsigned int size )
{
    if ( size > scratch_len ) {
        if ( scratch ) {
            sys_mem_free( scratch );
        }
        scratch = sys_mem_alloc( size );
        scratch_len = scratch ? size : 0;
    }
    return scratch;
}

/* ------------------------------------------------------------------ */
/* the output line                                                     */
/* ------------------------------------------------------------------ */

#define OUT_MAX 256

static char         out_buf[OUT_MAX];
static unsigned int out_len;
static int          out_column;

void out_flush( void )
{
    if ( out_len ) {
        sys_write( H_OUT, out_buf, out_len );
        out_len = 0;
    }
}

void out_ch( int ch )
{
    if ( out_len >= OUT_MAX ) {
        out_flush();
    }
    out_buf[out_len++] = (char)ch;
    if ( ch == '\n' || ch == '\r' ) {
        out_column = 0;
    } else if ( ch == '\b' ) {
        if ( out_column > 0 ) {
            out_column--;
        }
    } else {
        out_column++;
    }
}

void out_str( const char *str )
{
    while ( *str ) {
        out_ch( *str++ );
    }
}

/* VAL in DIGITS hex digits, capitals, leading zeros */
void out_hex( uint32_t val, int digits )
{
    int shift;

    for ( shift = ( digits - 1 ) * 4; shift >= 0; shift -= 4 ) {
        out_ch( "0123456789ABCDEF"[( val >> shift ) & 15] );
    }
}

void out_spaces( int count )
{
    while ( count-- > 0 ) {
        out_ch( ' ' );
    }
}

void out_pad( int column )
{
    out_spaces( column - out_column );
}

int out_col( void )
{
    return out_column;
}

void out_nl( void )
{
    out_ch( '\r' );
    out_ch( '\n' );
    out_flush();
}

/* "File not found" and the out-of-memory lines go to standard error,
   as 6.22's did, so a redirected session does not carry them */
void out_err( const char *msg )
{
    out_flush();
    sys_write( H_ERR, msg, str_len( msg ) );
    sys_write( H_ERR, "\r\n", 2 );
    out_column = 0;
}
