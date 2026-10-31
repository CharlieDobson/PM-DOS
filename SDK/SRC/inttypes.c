/*
 * INTTYPES.C - the intmax_t functions
 */
#include <inttypes.h>
#include <stdlib.h>

intmax_t imaxabs( intmax_t n )
{
    return n < 0 ? -n : n;
}

imaxdiv_t imaxdiv( intmax_t num, intmax_t den )
{
    imaxdiv_t r;

    r.quot = num / den;
    r.rem = num % den;
    return r;
}

intmax_t strtoimax( const char *s, char **end, int base )
{
    return strtoll( s, end, base );
}

uintmax_t strtoumax( const char *s, char **end, int base )
{
    return strtoull( s, end, base );
}
