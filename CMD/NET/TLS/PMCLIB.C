/*
 * PMCLIB.C - the little C library the vendored NSTLS core needs
 *
 * The TLS modules call malloc/free/realloc and the usual mem/str
 * functions.  PM-DOS native programs have no C runtime linked (-zl), so
 * these are supplied here: one big block from DOS (21h/48h, bytes on
 * PM-DOS) carved up first-fit, and the string/memory primitives.  Only
 * the TLS clients link this.
 */
#include "ndrv.h"
#include <stddef.h>

void __cdecl int21( REGS *regs );

/* ---- memory and string primitives ---- */

void *memcpy( void *to, const void *from, size_t len )
{
    uint8_t *d = (uint8_t *)to; const uint8_t *s = (const uint8_t *)from;
    while ( len-- ) *d++ = *s++;
    return to;
}

void *memmove( void *to, const void *from, size_t len )
{
    uint8_t *d = (uint8_t *)to; const uint8_t *s = (const uint8_t *)from;
    if ( d < s ) { while ( len-- ) *d++ = *s++; }
    else { d += len; s += len; while ( len-- ) *--d = *--s; }
    return to;
}

void *memset( void *to, int value, size_t len )
{
    uint8_t *d = (uint8_t *)to;
    while ( len-- ) *d++ = (uint8_t)value;
    return to;
}

int memcmp( const void *a, const void *b, size_t len )
{
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    while ( len-- ) { if ( *p != *q ) return (int)*p - (int)*q; p++; q++; }
    return 0;
}

size_t strlen( const char *s )
{
    size_t n = 0;
    while ( s[n] ) n++;
    return n;
}

int strcmp( const char *a, const char *b )
{
    while ( *a && *a == *b ) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strncpy( char *to, const char *from, size_t max )
{
    size_t i = 0;
    while ( i < max && from[i] ) { to[i] = from[i]; i++; }
    while ( i < max ) to[i++] = 0;
    return to;
}

char *strchr( const char *s, int ch )
{
    for ( ; *s; s++ ) if ( *s == (char)ch ) return (char *)s;
    return ( ch == 0 ) ? (char *)s : (void *)0;
}

/* ---- the heap: one DOS block, first fit ---- */

typedef struct hblk {
    size_t size;                /* payload bytes */
    size_t used;
    struct hblk *next;
} HBLK;

#define HDR ( ( size_t )sizeof( HBLK ) )

static uint8_t *pool;
static HBLK    *head;

static int pool_grab( void )
{
    REGS regs;
    static const unsigned int sizes[] = { 0x180000UL, 0x100000UL, 0x80000UL, 0x40000UL, 0 };
    int i;

    for ( i = 0; sizes[i]; i++ ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.eax = 0x4800;
        regs.ebx = sizes[i];
        int21( &regs );
        if ( !CF( regs ) ) {
            pool = (uint8_t *)regs.eax;
            head = (HBLK *)pool;
            head->size = sizes[i] - HDR;
            head->used = 0;
            head->next = (HBLK *)0;
            return 1;
        }
    }
    return 0;
}

void *malloc( size_t want )
{
    HBLK *b;

    if ( pool == (uint8_t *)0 && !pool_grab() ) return (void *)0;
    want = ( want + 7 ) & ~7U;          /* 8-byte align */

    for ( b = head; b; b = b->next ) {
        if ( b->used || b->size < want ) continue;
        if ( b->size >= want + HDR + 8 ) {     /* split */
            HBLK *rest = (HBLK *)( (uint8_t *)b + HDR + want );
            rest->size = b->size - want - HDR;
            rest->used = 0;
            rest->next = b->next;
            b->size = want;
            b->next = rest;
        }
        b->used = 1;
        return (uint8_t *)b + HDR;
    }
    return (void *)0;
}

static void coalesce( void )
{
    HBLK *b;
    for ( b = head; b && b->next; ) {
        if ( !b->used && !b->next->used ) {
            b->size += HDR + b->next->size;
            b->next = b->next->next;
        } else {
            b = b->next;
        }
    }
}

void free( void *ptr )
{
    HBLK *b;
    if ( ptr == (void *)0 ) return;
    b = (HBLK *)( (uint8_t *)ptr - HDR );
    b->used = 0;
    coalesce();
}

void *realloc( void *ptr, size_t want )
{
    HBLK *b;
    void *nu;
    size_t old;

    if ( ptr == (void *)0 ) return malloc( want );
    b = (HBLK *)( (uint8_t *)ptr - HDR );
    old = b->size;
    if ( old >= want ) return ptr;      /* the block already fits */
    nu = malloc( want );
    if ( nu == (void *)0 ) return (void *)0;
    memcpy( nu, ptr, old );
    free( ptr );
    return nu;
}
