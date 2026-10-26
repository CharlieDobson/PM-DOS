/*
 * MEM.C - the heap: memory bought from 48h, handed out and taken back.
 *
 * AN INTERPRETER FREES AS OFTEN AS IT ALLOCATES - every string a
 * statement builds and drops - so a free must not walk a list.  Small
 * blocks (up to SMALL_MAX bytes with their header) are kept in lists by
 * size, eight bytes apart, and taking one or giving one back is a push
 * or a pop.  They are cut from chunks and never merged again: a program
 * that has had a thousand 40-byte strings will want them again.
 *
 * Big blocks - arrays, the code, a file's buffer - are EDIT's heap:
 * first fit over a list kept in address order, neighbours merged as
 * they come back.  There are few of them, so the walk costs little.
 *
 * Every block has an 8-byte header: its size, and while it is free the
 * next in its list.  The size alone says which kind it is.
 */
#include "rt.h"
#include "sys.h"

typedef struct HBLK {
    u32          size;          /* the whole block, header included */
    struct HBLK *next;          /* while free */
} HBLK;

#define HDR         8u
#define SMALL_MAX   1024u       /* header included */
#define NCLASS      (SMALL_MAX / 8 + 1)
#define SCHUNK      (64u * 1024u)
#define MIN_SPLIT   64u
#define CHUNK       (64u * 1024u)

static HBLK *small_free[NCLASS];
static u8   *small_cur, *small_end;
static HBLK *free_list;         /* big blocks, in address order */

static void big_insert( HBLK *blk )
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

static HBLK *big_alloc( u32 need )
{
    HBLK **link, *blk, *rest;
    u32 chunk;
    void *mem;
    int pass;

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
            return blk;
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
        big_insert( blk );
    }
    return NULL;
}

void *try_alloc( u32 size )
{
    HBLK *blk;
    u32 need, cls;

    if ( size > 0xFFFFFF00UL - HDR ) {
        return NULL;
    }
    need = (size + HDR + 7u) & ~7u;
    if ( need < 16 ) {
        need = 16;
    }
    if ( need <= SMALL_MAX ) {
        cls = need / 8;
        blk = small_free[cls];
        if ( blk != NULL ) {
            small_free[cls] = blk->next;
            return (u8 *)blk + HDR;
        }
        if ( small_cur == NULL || (u32)(small_end - small_cur) < need ) {
            /* what is left of the chunk goes to the lists by size */
            while ( small_cur != NULL && (u32)(small_end - small_cur) >= 16 ) {
                u32 left = (u32)(small_end - small_cur);
                u32 piece = left > SMALL_MAX ? SMALL_MAX : left & ~7u;
                blk = (HBLK *)small_cur;
                blk->size = piece;
                blk->next = small_free[piece / 8];
                small_free[piece / 8] = blk;
                small_cur += piece;
            }
            blk = big_alloc( SCHUNK );
            if ( blk == NULL ) {
                return NULL;
            }
            small_cur = (u8 *)blk;
            small_end = small_cur + blk->size;
        }
        blk = (HBLK *)small_cur;
        blk->size = need;
        small_cur += need;
        return (u8 *)blk + HDR;
    }
    blk = big_alloc( need );
    return blk ? (u8 *)blk + HDR : NULL;
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
    HBLK *blk;

    if ( ptr == NULL ) {
        return;
    }
    blk = (HBLK *)((u8 *)ptr - HDR);
    if ( blk->size <= SMALL_MAX ) {
        blk->next = small_free[blk->size / 8];
        small_free[blk->size / 8] = blk;
    } else {
        big_insert( blk );
    }
}

u32 mem_size( void *ptr )
{
    return ((HBLK *)((u8 *)ptr - HDR))->size - HDR;
}

void *try_realloc( void *ptr, u32 size )
{
    void *grown;
    u32 have;

    if ( ptr == NULL ) {
        return try_alloc( size );
    }
    have = mem_size( ptr );
    if ( size <= have ) {
        return ptr;
    }
    grown = try_alloc( size );
    if ( grown == NULL ) {
        return NULL;
    }
    mem_cpy( grown, ptr, have );
    xfree( ptr );
    return grown;
}

u32 mem_avail( void )
{
    HBLK *blk;
    u32 best = sys_mem_largest();

    for ( blk = free_list; blk != NULL; blk = blk->next ) {
        if ( blk->size - HDR > best ) {
            best = blk->size - HDR;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* pools                                                               */
/* ------------------------------------------------------------------ */

#define POOL_CHUNK  (16u * 1024u)

typedef struct PCHUNK {
    struct PCHUNK *next;
    u32 used, size;
} PCHUNK;

struct POOL {
    PCHUNK *chunks;
};

POOL *pool_new( void )
{
    POOL *pool = (POOL *)try_alloc( sizeof( POOL ) );

    if ( pool == NULL ) {
        return NULL;
    }
    pool->chunks = NULL;
    return pool;
}

void *pool_alloc( POOL *pool, u32 size )
{
    PCHUNK *chunk = pool->chunks;
    u8 *mem;

    size = (size + 7u) & ~7u;
    if ( chunk == NULL || chunk->size - chunk->used < size ) {
        u32 want = size > POOL_CHUNK / 4 ? size : POOL_CHUNK;
        chunk = (PCHUNK *)try_alloc( sizeof( PCHUNK ) + want );
        if ( chunk == NULL ) {
            return NULL;
        }
        chunk->size = want;
        chunk->used = 0;
        if ( size > POOL_CHUNK / 4 && pool->chunks != NULL ) {
            /* a big one goes behind the current chunk, which goes on */
            chunk->next = pool->chunks->next;
            pool->chunks->next = chunk;
        } else {
            chunk->next = pool->chunks;
            pool->chunks = chunk;
        }
    }
    mem = (u8 *)(chunk + 1) + chunk->used;
    chunk->used += size;
    mem_set( mem, 0, size );
    return mem;
}

char *pool_strn( POOL *pool, const char *text, u32 len )
{
    char *copy = (char *)pool_alloc( pool, len + 1 );

    if ( copy == NULL ) {
        return NULL;
    }
    mem_cpy( copy, text, len );
    copy[len] = 0;
    return copy;
}

void pool_free( POOL *pool )
{
    PCHUNK *chunk, *next;

    if ( pool == NULL ) {
        return;
    }
    for ( chunk = pool->chunks; chunk != NULL; chunk = next ) {
        next = chunk->next;
        xfree( chunk );
    }
    xfree( pool );
}
