/*
 * MEM.C - memcpy() and the other block functions
 *
 * The GNU compilers call memcpy, memset, memmove and memcmp for
 * structure copies and clears of their own, so these must exist in
 * every program and must not call themselves: the library is built
 * with -fno-builtin and GCC's -fno-tree-loop-distribute-patterns, or
 * the loops below would be compiled into calls to the functions they
 * are.
 */
#include <string.h>
#include <stdint.h>

void *memcpy( void *dst, const void *src, size_t n )
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if ( (((uintptr_t)d | (uintptr_t)s) & 3) == 0 ) {
        while ( n >= 4 ) {
            *(uint32_t *)d = *(const uint32_t *)s;
            d += 4;
            s += 4;
            n -= 4;
        }
    }
    while ( n-- ) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove( void *dst, const void *src, size_t n )
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if ( d <= s || d >= s + n ) {
        return memcpy( dst, src, n );
    }
    d += n;                             /* overlapping, dst above: backwards */
    s += n;
    while ( n-- ) {
        *--d = *--s;
    }
    return dst;
}

void *memset( void *dst, int c, size_t n )
{
    unsigned char *d = (unsigned char *)dst;
    uint32_t four = (unsigned char)c * 0x01010101UL;

    while ( n && ((uintptr_t)d & 3) ) {
        *d++ = (unsigned char)c;
        n--;
    }
    while ( n >= 4 ) {
        *(uint32_t *)d = four;
        d += 4;
        n -= 4;
    }
    while ( n-- ) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

int memcmp( const void *a, const void *b, size_t n )
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;

    for ( ; n; n--, p++, q++ ) {
        if ( *p != *q ) {
            return *p - *q;
        }
    }
    return 0;
}

void *memchr( const void *s, int c, size_t n )
{
    const unsigned char *p = (const unsigned char *)s;

    for ( ; n; n--, p++ ) {
        if ( *p == (unsigned char)c ) {
            return (void *)p;
        }
    }
    return NULL;
}
