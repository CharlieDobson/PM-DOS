/*
 * RT.C - minimal runtime for LABEL/32 (no C library is linked).
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

/* ------------------------------------------------------------------ */
/* heap: first-fit free list on top of blocks from sys_mem_alloc()     */
/* ------------------------------------------------------------------ */

typedef struct HBLK {
    u32          size;          /* whole block including this header */
    struct HBLK *next;          /* free list link (free blocks only) */
} HBLK;

#define HDR         8u
#define MIN_SPLIT   64u
#define CHUNK       (64u * 1024u)

static HBLK *free_list;

static void heap_insert( HBLK *blk )
{
    HBLK **pp = &free_list;
    HBLK  *prev = NULL;

    while ( *pp && *pp < blk ) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    blk->next = *pp;
    *pp = blk;
    /* coalesce with the following block */
    if ( blk->next && (u8 *)blk + blk->size == (u8 *)blk->next ) {
        blk->size += blk->next->size;
        blk->next = blk->next->next;
    }
    /* coalesce with the preceding block */
    if ( prev && (u8 *)prev + prev->size == (u8 *)blk ) {
        prev->size += blk->size;
        prev->next = blk->next;
    }
}

void *try_alloc( u32 size )
{
    HBLK **pp, *blk, *rest;
    u32 need, chunk;
    void *mem;
    int pass;

    if ( size > 0xFFFFFF00UL - HDR ) {
        return NULL;
    }
    need = (size + HDR + 7u) & ~7u;
    for ( pass = 0; pass < 2; pass++ ) {
        for ( pp = &free_list; (blk = *pp) != NULL; pp = &blk->next ) {
            if ( blk->size < need ) {
                continue;
            }
            if ( blk->size - need >= MIN_SPLIT ) {
                rest = (HBLK *)((u8 *)blk + need);
                rest->size = blk->size - need;
                rest->next = blk->next;
                *pp = rest;
                blk->size = need;
            } else {
                *pp = blk->next;
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

void *xzalloc( u32 size )
{
    void *ptr = xalloc( size );
    mem_set( ptr, 0, size );
    return ptr;
}

void xfree( void *ptr )
{
    if ( ptr != NULL ) {
        heap_insert( (HBLK *)((u8 *)ptr - HDR) );
    }
}

void list_add( U32LIST *list, u32 val )
{
    u32 *nv;

    if ( list->count == list->cap ) {
        list->cap = list->cap ? list->cap * 2 : 16;
        nv = (u32 *)xalloc( list->cap * sizeof( u32 ) );
        if ( list->count ) {
            mem_cpy( nv, list->items, list->count * sizeof( u32 ) );
        }
        xfree( list->items );
        list->items = nv;
    }
    list->items[list->count++] = val;
}

void list_free( U32LIST *list )
{
    xfree( list->items );
    list->items = NULL;
    list->count = list->cap = 0;
}

/* ------------------------------------------------------------------ */
/* 64-bit helpers                                                      */
/* ------------------------------------------------------------------ */

u64 mul32( u32 lhs, u32 rhs )
{
    U64 prod;
    u32 al = lhs & 0xFFFFu, ah = lhs >> 16;
    u32 bl = rhs & 0xFFFFu, bh = rhs >> 16;
    u32 p0 = al * bl, p1 = al * bh, p2 = ah * bl, p3 = ah * bh;
    u32 mid = p1 + p2;
    u32 cmid = (mid < p1) ? 0x10000u : 0;
    u32 lo = p0 + (mid << 16);
    u32 clo = (lo < p0) ? 1u : 0;

    prod.parts.lo = lo;
    prod.parts.hi = p3 + (mid >> 16) + cmid + clo;
    return prod.whole;
}

u32 lo32( u64 val )
{
    U64 split;
    split.whole = val;
    return split.parts.lo;
}

u32 hi32( u64 val )
{
    U64 split;
    split.whole = val;
    return split.parts.hi;
}
