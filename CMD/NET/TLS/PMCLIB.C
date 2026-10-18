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

void __cdecl int21( REGS *regs );

typedef unsigned u_size;

/* ---- memory and string primitives ---- */

void *memcpy( void *to, const void *from, u_size len )
{
    u8 *d = (u8 *)to; const u8 *s = (const u8 *)from;
    while ( len-- ) *d++ = *s++;
    return to;
}

void *memmove( void *to, const void *from, u_size len )
{
    u8 *d = (u8 *)to; const u8 *s = (const u8 *)from;
    if ( d < s ) { while ( len-- ) *d++ = *s++; }
    else { d += len; s += len; while ( len-- ) *--d = *--s; }
    return to;
}

void *memset( void *to, int value, u_size len )
{
    u8 *d = (u8 *)to;
    while ( len-- ) *d++ = (u8)value;
    return to;
}

int memcmp( const void *a, const void *b, u_size len )
{
    const u8 *p = (const u8 *)a, *q = (const u8 *)b;
    while ( len-- ) { if ( *p != *q ) return (int)*p - (int)*q; p++; q++; }
    return 0;
}

u_size strlen( const char *s )
{
    u_size n = 0;
    while ( s[n] ) n++;
    return n;
}

int strcmp( const char *a, const char *b )
{
    while ( *a && *a == *b ) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

char *strncpy( char *to, const char *from, u_size max )
{
    u_size i = 0;
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
    u_size size;                /* payload bytes */
    u_size used;
    struct hblk *next;
} HBLK;

#define HDR ( ( u_size )sizeof( HBLK ) )

static u8   *pool;
static HBLK *head;

static int pool_grab( void )
{
    REGS regs;
    static const u32 sizes[] = { 0x180000UL, 0x100000UL, 0x80000UL, 0x40000UL, 0 };
    int i;

    for ( i = 0; sizes[i]; i++ ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.eax = 0x4800;
        regs.ebx = sizes[i];
        int21( &regs );
        if ( !CF( regs ) ) {
            pool = (u8 *)regs.eax;
            head = (HBLK *)pool;
            head->size = sizes[i] - HDR;
            head->used = 0;
            head->next = (HBLK *)0;
            return 1;
        }
    }
    return 0;
}

void *malloc( u_size want )
{
    HBLK *b;

    if ( pool == (u8 *)0 && !pool_grab() ) return (void *)0;
    want = ( want + 7 ) & ~7U;          /* 8-byte align */

    for ( b = head; b; b = b->next ) {
        if ( b->used || b->size < want ) continue;
        if ( b->size >= want + HDR + 8 ) {     /* split */
            HBLK *rest = (HBLK *)( (u8 *)b + HDR + want );
            rest->size = b->size - want - HDR;
            rest->used = 0;
            rest->next = b->next;
            b->size = want;
            b->next = rest;
        }
        b->used = 1;
        return (u8 *)b + HDR;
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
    b = (HBLK *)( (u8 *)ptr - HDR );
    b->used = 0;
    coalesce();
}

void *realloc( void *ptr, u_size want )
{
    HBLK *b;
    void *nu;
    u_size old;

    if ( ptr == (void *)0 ) return malloc( want );
    b = (HBLK *)( (u8 *)ptr - HDR );
    old = b->size;
    if ( old >= want ) return ptr;      /* the block already fits */
    nu = malloc( want );
    if ( nu == (void *)0 ) return (void *)0;
    memcpy( nu, ptr, old );
    free( ptr );
    return nu;
}
