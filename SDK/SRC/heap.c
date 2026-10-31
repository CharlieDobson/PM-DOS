/*
 * HEAP.C - malloc() and its family
 *
 * The memory comes from 21h/48h, which on PM-DOS takes a size in BYTES
 * in EBX and returns the block's offset in EAX, growing the slot when
 * it has to.  The heap asks for 64K at a time, or more for a larger
 * request, and hands it out first-fit from a list of free blocks kept
 * in address order, so that a freed block joins its free neighbours.
 *
 * Every block starts with a 16-byte header and the address handed out
 * is 16-aligned, which is what any type needs.  A block in use has
 * IN_USE where a free one has its link: free() and realloc() of a
 * pointer that is not one of ours, or of one already freed, end the
 * program rather than go on with a damaged heap.
 */
#include "pmlib.h"
#include <stdlib.h>
#include <string.h>

typedef struct block {
    size_t        size;                 /* the whole block, header too */
    struct block *next;                 /* free: the next free block */
    size_t        spare[2];             /* to 16 bytes */
} BLOCK;

#define HDR         sizeof( BLOCK )
#define ALIGN       16
#define MIN_SPLIT   (HDR + ALIGN)       /* a remainder worth keeping */
#define CHUNK       0x10000UL
#define IN_USE      ((BLOCK *)0xA110CA7EUL)
#define LARGEST     (0x7FFFFFFFUL - HDR - ALIGN)

static BLOCK *free_list;

static void heap_damaged( void )
{
    static const char msg[] = "The heap is damaged: free() of a bad pointer\r\n";
    pm_regs regs = { 0 };

    regs.ebx = 2;
    regs.ecx = sizeof( msg ) - 1;
    regs.edx = (uint32_t)msg;
    __pm_dos( &regs, 0x4000 );
    _exit( 3 );
}

/* put a block on the free list, in address order, joining it to the
   blocks either side when they touch it */
static void insert_free( BLOCK *blk )
{
    BLOCK *prev = NULL;
    BLOCK *cur = free_list;

    while ( cur != NULL && cur < blk ) {
        prev = cur;
        cur = cur->next;
    }
    if ( cur != NULL && (char *)blk + blk->size == (char *)cur ) {
        blk->size += cur->size;
        blk->next = cur->next;
    } else {
        blk->next = cur;
    }
    if ( prev != NULL && (char *)prev + prev->size == (char *)blk ) {
        prev->size += blk->size;
        prev->next = blk->next;
    } else if ( prev != NULL ) {
        prev->next = blk;
    } else {
        free_list = blk;
    }
}

/* more memory from the kernel: at least need bytes of block */
static int grow( size_t need )
{
    pm_regs regs = { 0 };
    size_t ask = need < CHUNK ? CHUNK : need;
    uintptr_t base;
    BLOCK *blk;

    ask += ALIGN;                       /* room to align the start */
    regs.ebx = ask;
    if ( __pm_dos( &regs, 0x4800 ) ) {
        if ( ask == need + ALIGN ) {
            return 0;
        }
        ask = need + ALIGN;             /* the 64K would not fit; this might */
        regs.ebx = ask;
        if ( __pm_dos( &regs, 0x4800 ) ) {
            return 0;
        }
    }
    base = (regs.eax + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
    blk = (BLOCK *)base;
    blk->size = (ask - (base - regs.eax)) & ~(size_t)(ALIGN - 1);
    insert_free( blk );
    return 1;
}

void *malloc( size_t size )
{
    BLOCK *prev;
    BLOCK *cur;
    BLOCK *rest;
    size_t need;

    if ( size > LARGEST ) {
        errno = ENOMEM;
        return NULL;
    }
    need = (size + HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    for ( ;; ) {
        prev = NULL;
        for ( cur = free_list; cur != NULL; prev = cur, cur = cur->next ) {
            if ( cur->size >= need ) {
                break;
            }
        }
        if ( cur != NULL ) {
            break;
        }
        if ( !grow( need ) ) {
            errno = ENOMEM;
            return NULL;
        }
    }
    if ( cur->size - need >= MIN_SPLIT ) {
        rest = (BLOCK *)((char *)cur + need);
        rest->size = cur->size - need;
        rest->next = cur->next;
        cur->size = need;
        cur->next = rest;
    }
    if ( prev != NULL ) {
        prev->next = cur->next;
    } else {
        free_list = cur->next;
    }
    cur->next = IN_USE;
    return (char *)cur + HDR;
}

static BLOCK *block_of( void *ptr )
{
    BLOCK *blk = (BLOCK *)((char *)ptr - HDR);

    if ( ((uintptr_t)ptr & (ALIGN - 1)) != 0 || blk->next != IN_USE ) {
        heap_damaged();
    }
    return blk;
}

void free( void *ptr )
{
    if ( ptr != NULL ) {
        insert_free( block_of( ptr ) );
    }
}

void *calloc( size_t count, size_t size )
{
    void *ptr;

    if ( size != 0 && count > LARGEST / size ) {
        errno = ENOMEM;
        return NULL;
    }
    ptr = malloc( count * size );
    if ( ptr != NULL ) {
        memset( ptr, 0, count * size );
    }
    return ptr;
}

void *realloc( void *ptr, size_t size )
{
    BLOCK *blk;
    BLOCK *rest;
    void *moved;
    size_t have;
    size_t need;

    if ( ptr == NULL ) {
        return malloc( size );
    }
    if ( size == 0 ) {
        free( ptr );
        return NULL;
    }
    blk = block_of( ptr );
    if ( size > LARGEST ) {
        errno = ENOMEM;
        return NULL;
    }
    need = (size + HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    if ( need <= blk->size ) {
        if ( blk->size - need >= MIN_SPLIT ) {
            rest = (BLOCK *)((char *)blk + need);
            rest->size = blk->size - need;
            blk->size = need;
            insert_free( rest );
        }
        return ptr;
    }
    moved = malloc( size );
    if ( moved == NULL ) {
        return NULL;                    /* the old block is still good */
    }
    have = blk->size - HDR;
    memcpy( moved, ptr, have < size ? have : size );
    free( ptr );
    return moved;
}
