/*
 * STR.C - the C standard's string functions
 */
#include <string.h>

size_t strlen( const char *s )
{
    const char *p = s;

    while ( *p ) {
        p++;
    }
    return (size_t)(p - s);
}

size_t strnlen( const char *s, size_t max )
{
    size_t n = 0;

    while ( n < max && s[n] ) {
        n++;
    }
    return n;
}

char *strcpy( char *dst, const char *src )
{
    char *d = dst;

    while ( (*d++ = *src++) != '\0' ) {
    }
    return dst;
}

char *strncpy( char *dst, const char *src, size_t n )
{
    size_t i = 0;

    for ( ; i < n && src[i]; i++ ) {
        dst[i] = src[i];
    }
    for ( ; i < n; i++ ) {
        dst[i] = '\0';
    }
    return dst;
}

char *strcat( char *dst, const char *src )
{
    strcpy( dst + strlen( dst ), src );
    return dst;
}

char *strncat( char *dst, const char *src, size_t n )
{
    char *d = dst + strlen( dst );

    while ( n-- && *src ) {
        *d++ = *src++;
    }
    *d = '\0';
    return dst;
}

int strcmp( const char *a, const char *b )
{
    while ( *a && *a == *b ) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp( const char *a, const char *b, size_t n )
{
    for ( ; n; n--, a++, b++ ) {
        if ( *a != *b || *a == '\0' ) {
            return (unsigned char)*a - (unsigned char)*b;
        }
    }
    return 0;
}

int strcoll( const char *a, const char *b )
{
    return strcmp( a, b );              /* the "C" locale */
}

size_t strxfrm( char *dst, const char *src, size_t n )
{
    size_t len = strlen( src );

    if ( n > 0 ) {
        if ( len < n ) {
            memcpy( dst, src, len + 1 );
        } else {
            memcpy( dst, src, n - 1 );
            dst[n - 1] = '\0';
        }
    }
    return len;
}

char *strchr( const char *s, int c )
{
    for ( ;; s++ ) {
        if ( *s == (char)c ) {
            return (char *)s;
        }
        if ( *s == '\0' ) {
            return NULL;
        }
    }
}

char *strrchr( const char *s, int c )
{
    const char *found = NULL;

    for ( ;; s++ ) {
        if ( *s == (char)c ) {
            found = s;
        }
        if ( *s == '\0' ) {
            return (char *)found;
        }
    }
}

char *strstr( const char *s, const char *find )
{
    size_t len = strlen( find );

    if ( len == 0 ) {
        return (char *)s;
    }
    for ( ; *s; s++ ) {
        if ( *s == *find && strncmp( s, find, len ) == 0 ) {
            return (char *)s;
        }
    }
    return NULL;
}

size_t strspn( const char *s, const char *accept )
{
    size_t n = 0;

    while ( s[n] && strchr( accept, s[n] ) != NULL ) {
        n++;
    }
    return n;
}

size_t strcspn( const char *s, const char *reject )
{
    size_t n = 0;

    while ( s[n] && strchr( reject, s[n] ) == NULL ) {
        n++;
    }
    return n;
}

char *strpbrk( const char *s, const char *accept )
{
    s += strcspn( s, accept );
    return *s ? (char *)s : NULL;
}

char *strtok( char *s, const char *delim )
{
    static char *next;
    char *start;

    if ( s == NULL ) {
        s = next;
        if ( s == NULL ) {
            return NULL;
        }
    }
    s += strspn( s, delim );
    if ( *s == '\0' ) {
        next = NULL;
        return NULL;
    }
    start = s;
    s += strcspn( s, delim );
    if ( *s ) {
        *s = '\0';
        next = s + 1;
    } else {
        next = NULL;
    }
    return start;
}
