/*
 * RT.C - minimal runtime for XCOPY (no C library is linked).  The heap
 * is LABEL's: first fit over blocks bought from 48h.
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
#define CHUNK       (32u * 1024u)

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

void *xalloc( u32 size )
{
    void *ptr = try_alloc( size );

    if ( ptr == NULL ) {
        out_of_memory();
    }
    return ptr;
}

void xfree( void *ptr )
{
    if ( ptr != NULL ) {
        heap_insert( (HBLK *)((u8 *)ptr - HDR) );
    }
}
