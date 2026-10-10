/*
 * RT.C - minimal runtime for PMD (no C library is linked).  EDIT's,
 * and the heap is LABEL's: first fit over blocks bought from 48h.
 * sfmt at the bottom is new here.
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

/* ------------------------------------------------------------------ */
/* heap: first-fit free list on top of blocks from sys_mem_alloc()     */
/* ------------------------------------------------------------------ */

typedef struct HBLK {
    u32          size;          /* whole block including this header */
    struct HBLK *next;          /* free list link (free blocks only) */
} HBLK;

#define HDR         8u
#define MIN_SPLIT   64u
#define CHUNK       (8u * 1024u)    /* small: a big block goes to DOS as it is, and
                                       a chunk mostly empty is memory no file gets */

static HBLK *free_list;

static void heap_insert( HBLK *blk )
{
    HBLK **link = &free_list;
    HBLK  *prev = NULL;

    while ( *link && *link < blk ) {
        prev = *link;
        link = &(*link)->next;
    }
    blk->next = *link;
    *link = blk;
    if ( blk->next && (u8 *)blk + blk->size == (u8 *)blk->next ) {
        blk->size += blk->next->size;
        blk->next = blk->next->next;
    }
    if ( prev && (u8 *)prev + prev->size == (u8 *)blk ) {
        prev->size += blk->size;
        prev->next = blk->next;
    }
}

void *try_alloc( u32 size )
{
    HBLK **link, *blk, *rest;
    u32 need, chunk;
    void *mem;
    int pass;

    if ( size > 0xFFFFFF00UL - HDR ) {
        return NULL;
    }
    need = (size + HDR + 7u) & ~7u;
    for ( pass = 0; pass < 2; pass++ ) {
        for ( link = &free_list; (blk = *link) != NULL; link = &blk->next ) {
            if ( blk->size < need ) {
                continue;
            }
            if ( blk->size - need >= MIN_SPLIT ) {
                rest = (HBLK *)((u8 *)blk + need);
                rest->size = blk->size - need;
                rest->next = blk->next;
                *link = rest;
                blk->size = need;
            } else {
                *link = blk->next;
            }
            return (u8 *)blk + HDR;
        }
        if ( pass ) {
            break;
        }
        chunk = need > CHUNK ? need : CHUNK;
        mem = sys_mem_alloc( chunk );
        if ( mem == NULL && chunk > need ) {
            chunk = need;
            mem = sys_mem_alloc( chunk );
        }
        if ( mem == NULL ) {
            return NULL;
        }
        blk = (HBLK *)mem;
        blk->size = chunk & ~7u;
        heap_insert( blk );
    }
    return NULL;
}

/* Only for what the program cannot start without: anything later uses
   try_alloc and says "no memory" instead, so an unsaved file survives. */
void *xalloc( u32 size )
{
    void *ptr = try_alloc( size );

    if ( ptr == NULL ) {
        out_of_memory();
        sys_exit( 8 );
    }
    return ptr;
}

void xfree( void *ptr )
{
    if ( ptr != NULL ) {
        heap_insert( (HBLK *)((u8 *)ptr - HDR) );
    }
}

/* ------------------------------------------------------------------ */
/* searching memory                                                    */
/* ------------------------------------------------------------------ */

int mem_ifind( const u8 *hay, u32 len, const char *needle )
{
    u32 nlen = str_len( needle ), pos, at;
    int first;

    if ( nlen == 0 || nlen > len ) {
        return -1;
    }
    first = ch_upper( needle[0] );
    for ( pos = 0; pos + nlen <= len; pos++ ) {
        if ( ch_upper( hay[pos] ) != first ) {
            continue;
        }
        for ( at = 1; at < nlen && ch_upper( hay[pos + at] ) == ch_upper( needle[at] ); at++ ) {
        }
        if ( at == nlen ) {
            return (int)pos;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* sfmt                                                                */
/* ------------------------------------------------------------------ */

static int group_ch = ',';

void fmt_group_set( int ch )
{
    group_ch = ch;
}

typedef struct {
    char *buf;
    u32   max, len;
} FOUT;

static void out_ch( FOUT *out, int ch )
{
    if ( out->len + 1 < out->max ) {
        out->buf[out->len++] = (char)ch;
    }
}

static void out_pad( FOUT *out, int count, int ch )
{
    while ( count-- > 0 ) {
        out_ch( out, ch );
    }
}

static void out_text( FOUT *out, const char *text, int len, int width, int left, int fill )
{
    int index;

    if ( !left ) {
        out_pad( out, width - len, fill );
    }
    for ( index = 0; index < len; index++ ) {
        out_ch( out, text[index] );
    }
    if ( left ) {
        out_pad( out, width - len, ' ' );
    }
}

/* a number into "digits", backwards; returns how many characters */
static int num_digits( u32 val, u32 base, int upper, int group, char *digits )
{
    static const char lower_set[] = "0123456789abcdef";
    static const char upper_set[] = "0123456789ABCDEF";
    int count = 0, run = 0;

    do {
        if ( group && run == 3 ) {
            digits[count++] = (char)group_ch;
            run = 0;
        }
        digits[count++] = upper ? upper_set[val % base] : lower_set[val % base];
        val /= base;
        run++;
    } while ( val );
    return count;
}

int vfmt( char *buf, u32 max, const char *fmt, char *args )
{
    FOUT out;
    char digits[20], text[20];
    const char *str;
    int left, zero, group, width, count, index, neg;
    u32 val;

    out.buf = buf;
    out.max = max;
    out.len = 0;
    if ( max == 0 ) {
        return 0;
    }
    for ( ; *fmt; fmt++ ) {
        if ( *fmt != '%' ) {
            out_ch( &out, *fmt );
            continue;
        }
        fmt++;
        left = zero = group = width = 0;
        for ( ;; fmt++ ) {
            if ( *fmt == '-' ) {
                left = 1;
            } else if ( *fmt == '0' ) {
                zero = 1;
            } else if ( *fmt == ',' ) {
                group = 1;
            } else {
                break;
            }
        }
        if ( *fmt == '*' ) {
            width = VA_NEXT( args, int );
            fmt++;
            if ( width < 0 ) {
                left = 1;
                width = -width;
            }
        }
        while ( *fmt >= '0' && *fmt <= '9' ) {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if ( *fmt == 'l' ) {
            fmt++;
        }
        switch ( *fmt ) {
        case 's':
            str = VA_NEXT( args, const char * );
            if ( str == NULL ) {
                str = "";
            }
            out_text( &out, str, (int)str_len( str ), width, left, ' ' );
            break;
        case 'c':
            text[0] = (char)VA_NEXT( args, int );
            out_text( &out, text, 1, width, left, ' ' );
            break;
        case 'd':
        case 'u':
        case 'x':
        case 'X':
            val = VA_NEXT( args, u32 );
            neg = 0;
            if ( *fmt == 'd' && (s32)val < 0 ) {
                neg = 1;
                val = (u32)(-(s32)val);
            }
            count = num_digits( val, *fmt == 'x' || *fmt == 'X' ? 16 : 10, *fmt == 'X',
                                group && *fmt != 'x' && *fmt != 'X', digits );
            index = 0;
            if ( neg ) {
                text[index++] = '-';
            }
            if ( zero && !left ) {
                while ( index + count < width && index < 12 ) {
                    text[index++] = '0';
                }
            }
            while ( count && index < (int)sizeof( text ) ) {
                text[index++] = digits[--count];
            }
            out_text( &out, text, index, width, left, ' ' );
            break;
        case 0:
            fmt--;
            break;
        default:
            out_ch( &out, *fmt );
            break;
        }
    }
    out.buf[out.len] = 0;
    return (int)out.len;
}

int __cdecl sfmt( char *buf, u32 max, const char *fmt, ... )
{
    return vfmt( buf, max, fmt, VA_START( fmt ) );
}
