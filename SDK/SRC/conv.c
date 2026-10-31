/*
 * CONV.C - strings to integers and back: strtol() and its family,
 * atoi(), itoa()
 */
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

/* The one parser: the magnitude as an unsigned long long, the sign in
   *neg, and errno = ERANGE (with the magnitude at limit) when it goes
   past limit.  *end is where it stopped, or s when there was no
   number at all. */
static unsigned long long parse( const char *s, char **end, int base,
                                 unsigned long long limit, int *neg )
{
    const char *p = s;
    const char *digits;
    unsigned long long value = 0;
    int digit;
    int over = 0;

    *neg = 0;
    while ( isspace( (unsigned char)*p ) ) {
        p++;
    }
    if ( *p == '-' || *p == '+' ) {
        *neg = (*p == '-');
        p++;
    }
    if ( base < 0 || base == 1 || base > 36 ) {
        if ( end != NULL ) {
            *end = (char *)s;
        }
        errno = EINVAL;
        return 0;
    }
    if ( (base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
         && isxdigit( (unsigned char)p[2] ) ) {
        p += 2;
        base = 16;
    } else if ( base == 0 ) {
        base = (p[0] == '0') ? 8 : 10;
    }
    digits = p;
    for ( ;; p++ ) {
        if ( isdigit( (unsigned char)*p ) ) {
            digit = *p - '0';
        } else if ( isalpha( (unsigned char)*p ) ) {
            digit = tolower( (unsigned char)*p ) - 'a' + 10;
        } else {
            break;
        }
        if ( digit >= base ) {
            break;
        }
        if ( value > (limit - (unsigned)digit) / (unsigned)base ) {
            over = 1;
        } else {
            value = value * (unsigned)base + (unsigned)digit;
        }
    }
    if ( end != NULL ) {
        *end = (char *)(p == digits ? s : p);
    }
    if ( over ) {
        errno = ERANGE;
        return limit;
    }
    return value;
}

long strtol( const char *s, char **end, int base )
{
    int neg;
    unsigned long long v = parse( s, end, base, (unsigned long long)LONG_MAX + 1, &neg );

    if ( !neg && v > LONG_MAX ) {
        errno = ERANGE;
        return LONG_MAX;
    }
    return neg ? (long)(0 - (unsigned long)v) : (long)v;
}

unsigned long strtoul( const char *s, char **end, int base )
{
    int neg;
    unsigned long v = (unsigned long)parse( s, end, base, ULONG_MAX, &neg );

    return neg ? 0 - v : v;
}

long long strtoll( const char *s, char **end, int base )
{
    int neg;
    unsigned long long v = parse( s, end, base, (unsigned long long)LLONG_MAX + 1, &neg );

    if ( !neg && v > LLONG_MAX ) {
        errno = ERANGE;
        return LLONG_MAX;
    }
    return neg ? (long long)(0 - v) : (long long)v;
}

unsigned long long strtoull( const char *s, char **end, int base )
{
    int neg;
    unsigned long long v = parse( s, end, base, ULLONG_MAX, &neg );

    return neg ? 0 - v : v;
}

int atoi( const char *s )
{
    return (int)strtol( s, NULL, 10 );
}

long atol( const char *s )
{
    return strtol( s, NULL, 10 );
}

long long atoll( const char *s )
{
    return strtoll( s, NULL, 10 );
}

char *ultoa( unsigned long value, char *buf, int radix )
{
    char tmp[33];
    int n = 0;
    int i;

    if ( radix < 2 || radix > 36 ) {
        buf[0] = '\0';
        return buf;
    }
    do {
        tmp[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[value % (unsigned)radix];
        value /= (unsigned)radix;
    } while ( value );
    for ( i = 0; i < n; i++ ) {
        buf[i] = tmp[n - 1 - i];
    }
    buf[n] = '\0';
    return buf;
}

char *ltoa( long value, char *buf, int radix )
{
    if ( radix == 10 && value < 0 ) {
        buf[0] = '-';
        ultoa( 0 - (unsigned long)value, buf + 1, 10 );
        return buf;
    }
    return ultoa( (unsigned long)value, buf, radix );
}

char *itoa( int value, char *buf, int radix )
{
    return ltoa( value, buf, radix );
}
