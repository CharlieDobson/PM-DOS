/*
 * RT.C - minimal runtime for MODE (no C library is linked).
 */
#include "rt.h"

void mem_set( void *dst, int val, unsigned int len )
{
    uint8_t *cur = (uint8_t *)dst;

    while ( len-- ) {
        *cur++ = (uint8_t)val;
    }
}

void mem_cpy( void *dst, const void *src, unsigned int len )
{
    uint8_t *dcur = (uint8_t *)dst;
    const uint8_t *scur = (const uint8_t *)src;

    while ( len-- ) {
        *dcur++ = *scur++;
    }
}

unsigned int str_len( const char *str )
{
    unsigned int len = 0;

    while ( str[len] ) {
        len++;
    }
    return len;
}

/* a copy that stops short of "max" bytes including the NUL */
int str_cpyn( char *dst, const char *src, unsigned int max )
{
    unsigned int len = str_len( src );

    if ( len + 1 > max ) {
        return 0;
    }
    mem_cpy( dst, src, len + 1 );
    return 1;
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

unsigned int num_str( char *buf, unsigned int val )
{
    char rev[10];
    unsigned int len = 0, pos = 0;

    do {
        rev[len++] = (char)('0' + val % 10);
        val /= 10;
    } while ( val );
    while ( len ) {
        buf[pos++] = rev[--len];
    }
    buf[pos] = 0;
    return pos;
}
