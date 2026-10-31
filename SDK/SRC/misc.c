/*
 * MISC.C - abs(), div(), qsort(), bsearch(), rand(), and the multibyte
 * functions of the "C" locale, where every character is one byte
 */
#include <stdlib.h>
#include <string.h>

int abs( int n )                { return n < 0 ? -n : n; }
long labs( long n )             { return n < 0 ? -n : n; }
long long llabs( long long n )  { return n < 0 ? -n : n; }

div_t div( int num, int den )
{
    div_t r;

    r.quot = num / den;
    r.rem = num % den;
    return r;
}

ldiv_t ldiv( long num, long den )
{
    ldiv_t r;

    r.quot = num / den;
    r.rem = num % den;
    return r;
}

lldiv_t lldiv( long long num, long long den )
{
    lldiv_t r;

    r.quot = num / den;
    r.rem = num % den;
    return r;
}

/* swap two elements of size bytes */
static void swap( char *a, char *b, size_t size )
{
    char t;

    while ( size-- ) {
        t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* Quicksort on the middle element, recursing into the smaller side
   and looping on the larger, so the stack stays under 32 levels; runs
   of fewer than eight go to an insertion sort. */
void qsort( void *base, size_t count, size_t size,
            int (*compare)( const void *, const void * ) )
{
    char *lo = (char *)base;
    char *hi;
    char *i;
    char *j;
    size_t left;

    while ( count >= 8 ) {
        hi = lo + (count - 1) * size;
        swap( lo, lo + (count / 2) * size, size );      /* pivot to lo */
        i = lo;
        j = hi + size;
        for ( ;; ) {
            do {
                i += size;
            } while ( i <= hi && compare( i, lo ) < 0 );
            do {
                j -= size;
            } while ( compare( j, lo ) > 0 );
            if ( i >= j ) {
                break;
            }
            swap( i, j, size );
        }
        swap( lo, j, size );
        left = (size_t)(j - lo) / size;
        if ( left < count - left - 1 ) {
            qsort( lo, left, size, compare );
            lo = j + size;
            count = count - left - 1;
        } else {
            qsort( j + size, count - left - 1, size, compare );
            count = left;
        }
    }
    for ( i = lo + size; count > 1 && i < lo + count * size; i += size ) {
        for ( j = i; j > lo && compare( j - size, j ) > 0; j -= size ) {
            swap( j - size, j, size );
        }
    }
}

void *bsearch( const void *key, const void *base, size_t count, size_t size,
               int (*compare)( const void *, const void * ) )
{
    const char *lo = (const char *)base;
    const char *mid;
    int c;

    while ( count > 0 ) {
        mid = lo + (count / 2) * size;
        c = compare( key, mid );
        if ( c == 0 ) {
            return (void *)mid;
        }
        if ( c > 0 ) {
            lo = mid + size;
            count = count - count / 2 - 1;
        } else {
            count /= 2;
        }
    }
    return NULL;
}

static unsigned long seed = 1;

int rand( void )
{
    seed = seed * 1103515245UL + 12345;
    return (int)((seed >> 16) & RAND_MAX);
}

void srand( unsigned s )
{
    seed = s;
}

int mblen( const char *s, size_t n )
{
    if ( s == NULL ) {
        return 0;                       /* no shift states */
    }
    if ( n == 0 ) {
        return -1;
    }
    return *s ? 1 : 0;
}

int mbtowc( wchar_t *wc, const char *s, size_t n )
{
    if ( s == NULL ) {
        return 0;
    }
    if ( n == 0 ) {
        return -1;
    }
    if ( wc != NULL ) {
        *wc = (wchar_t)(unsigned char)*s;
    }
    return *s ? 1 : 0;
}

int wctomb( char *s, wchar_t wc )
{
    if ( s == NULL ) {
        return 0;
    }
    if ( (unsigned long)wc > 255 ) {
        return -1;
    }
    *s = (char)wc;
    return 1;
}

size_t mbstowcs( wchar_t *dst, const char *src, size_t n )
{
    size_t i = 0;

    if ( dst == NULL ) {
        return strlen( src );
    }
    for ( ; i < n; i++ ) {
        dst[i] = (wchar_t)(unsigned char)src[i];
        if ( src[i] == '\0' ) {
            break;
        }
    }
    return i;
}

size_t wcstombs( char *dst, const wchar_t *src, size_t n )
{
    size_t i = 0;

    for ( ; dst == NULL || i < n; i++ ) {
        if ( (unsigned long)src[i] > 255 ) {
            return (size_t)-1;
        }
        if ( dst != NULL ) {
            dst[i] = (char)src[i];
        }
        if ( src[i] == 0 ) {
            break;
        }
    }
    return i;
}
