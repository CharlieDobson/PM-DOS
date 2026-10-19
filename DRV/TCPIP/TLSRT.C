/*
 * TLSRT.C - the small C runtime the vendored NSTLS core (CMD\NET\TLS)
 * needs when it is linked into the TCP/IP driver for kernel-side TLS
 * sockets (SOCK_OPEN's SOCK_TLS flag).
 *
 * A 32-bit DEVICE= driver has no C runtime linked (-zl), and - unlike a
 * program - it must not call DOS, so PMCLIB's INT 21h heap is no use in
 * a driver.  These are the same functions PMCLIB gives a TLS program,
 * but the heap is one arena block grabbed ONCE, at driver load, through
 * tlsrt_init (NDS_ALLOC).  That matters: a TLS connection wants ~200K,
 * and by the time the WfW bridge drives the parked stack, WfW has taken
 * most of the arena - so the memory is reserved while it is still
 * plentiful, before WfW parks the kernel, and sub-allocated first fit.
 */
#define ND_DRIVER
#include "ndrv.h"

typedef unsigned u_size;

/* ---- memory and string primitives (as PMCLIB, for the TLS core) ---- */

void *memcpy( void *to, const void *from, u_size len )
{
    u8 *dst = (u8 *)to;
    const u8 *src = (const u8 *)from;
    while ( len-- ) *dst++ = *src++;
    return to;
}

void *memmove( void *to, const void *from, u_size len )
{
    u8 *dst = (u8 *)to;
    const u8 *src = (const u8 *)from;
    if ( dst < src ) {
        while ( len-- ) *dst++ = *src++;
    } else {
        dst += len; src += len;
        while ( len-- ) *--dst = *--src;
    }
    return to;
}

void *memset( void *to, int value, u_size len )
{
    u8 *dst = (u8 *)to;
    while ( len-- ) *dst++ = (u8)value;
    return to;
}

int memcmp( const void *aptr, const void *bptr, u_size len )
{
    const u8 *aa = (const u8 *)aptr, *bb = (const u8 *)bptr;
    while ( len-- ) {
        if ( *aa != *bb ) return (int)*aa - (int)*bb;
        aa++; bb++;
    }
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
    return ( ch == 0 ) ? (char *)s : (char *)0;
}

/* ---- the heap: one arena block, grabbed once, carved first fit ---- */

typedef struct hblk {
    u_size size;                /* payload bytes */
    u_size used;
    struct hblk *next;
} HBLK;

#define HDR ( ( u_size )sizeof( HBLK ) )

static u8   *pool;
static HBLK *head;

/* Grab the pool from the kernel arena.  Called at nd_init, before WfW
   loads, so the (large) block is there; tries a few sizes down.  Returns
   the bytes grabbed, or 0 if even the smallest would not fit. */
u32 tlsrt_init( u32 want )
{
    static const u32 fallbacks[] = { 0x40000UL, 0x30000UL, 0x20000UL, 0 };
    u32 bytes = want;
    int i = 0;

    if ( pool ) return (u32)( head->size + HDR );
    for ( ;; ) {
        pool = (u8 *)nds_alloc( bytes );
        if ( pool ) break;
        bytes = fallbacks[i++];
        if ( bytes == 0 ) return 0;
    }
    head = (HBLK *)pool;
    head->size = bytes - HDR;
    head->used = 0;
    head->next = (HBLK *)0;
    return bytes;
}

void *malloc( u_size want )
{
    HBLK *b;

    if ( head == (HBLK *)0 ) return (void *)0;      /* tlsrt_init never ran */
    want = ( want + 7 ) & ~7U;                      /* 8-byte align */

    for ( b = head; b; b = b->next ) {
        if ( b->used || b->size < want ) continue;
        if ( b->size >= want + HDR + 8 ) {          /* split the tail off */
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
    if ( old >= want ) return ptr;      /* the block already holds enough */
    nu = malloc( want );
    if ( nu == (void *)0 ) return (void *)0;
    memcpy( nu, ptr, old );
    free( ptr );
    return nu;
}
