/*
 * RT.C - minimal runtime for LABEL/32 (no C library is linked).
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

    for ( ; len; len--, lcur++, rcur++ ) {
        if ( *lcur != *rcur ) {
            return (int)*lcur - (int)*rcur;
        }
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
    unsigned int size;          /* whole block including this header */
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
    if ( blk->next && (uint8_t *)blk + blk->size == (uint8_t *)blk->next ) {
        blk->size += blk->next->size;
        blk->next = blk->next->next;
    }
    /* coalesce with the preceding block */
    if ( prev && (uint8_t *)prev + prev->size == (uint8_t *)blk ) {
        prev->size += blk->size;
        prev->next = blk->next;
    }
}

void *try_alloc( unsigned int size )
{
    HBLK **pp, *blk, *rest;
    unsigned int need, chunk;
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
                rest = (HBLK *)((uint8_t *)blk + need);
                rest->size = blk->size - need;
                rest->next = blk->next;
                *pp = rest;
                blk->size = need;
            } else {
                *pp = blk->next;
            }
            return (uint8_t *)blk + HDR;
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

void *xalloc( unsigned int size )
{
    void *ptr = try_alloc( size );
    if ( ptr == NULL ) {
        out_of_memory();
    }
    return ptr;
}

void *xzalloc( unsigned int size )
{
    void *ptr = xalloc( size );
    mem_set( ptr, 0, size );
    return ptr;
}

void xfree( void *ptr )
{
    if ( ptr != NULL ) {
        heap_insert( (HBLK *)((uint8_t *)ptr - HDR) );
    }
}

void list_add( U32LIST *list, uint32_t val )
{
    uint32_t *nv;

    if ( list->count == list->cap ) {
        list->cap = list->cap ? list->cap * 2 : 16;
        nv = (uint32_t *)xalloc( list->cap * sizeof( uint32_t ) );
        if ( list->count ) {
            mem_cpy( nv, list->items, list->count * sizeof( uint32_t ) );
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

uint64_t mul32( uint32_t lhs, uint32_t rhs )
{
    U64 prod;
    uint32_t al = lhs & 0xFFFFu, ah = lhs >> 16;
    uint32_t bl = rhs & 0xFFFFu, bh = rhs >> 16;
    uint32_t p0 = al * bl, p1 = al * bh, p2 = ah * bl, p3 = ah * bh;
    uint32_t mid = p1 + p2;
    uint32_t cmid = (mid < p1) ? 0x10000u : 0;
    uint32_t lo = p0 + (mid << 16);
    uint32_t clo = (lo < p0) ? 1u : 0;

    prod.parts.lo = lo;
    prod.parts.hi = p3 + (mid >> 16) + cmid + clo;
    return prod.whole;
}

uint32_t lo32( uint64_t val )
{
    U64 split;
    split.whole = val;
    return split.parts.lo;
}

uint32_t hi32( uint64_t val )
{
    U64 split;
    split.whole = val;
    return split.parts.hi;
}
