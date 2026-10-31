/*
 * STRTOD.C - strings to floating point: strtod(), strtof(), strtold(),
 * atof()
 *
 * CORRECTLY ROUNDED.  The decimal number is D * 10^E for an integer D;
 * the answer is the binary number m * 2^k, m of P bits, nearest to it
 * (to even on a tie).  An estimate in long double gets within a few
 * units of m; then each neighbour is tested exactly, by comparing D *
 * 10^E with the halfway point between two candidates, all of it in
 * big integers, until neither neighbour is nearer.
 *
 * Up to NDIG significant digits are kept.  A number with more has the
 * rest replaced by one digit 1 - "something not zero follows" - which
 * is exact for float and double (no halfway point between two doubles
 * needs more than 767 digits) and right for long double unless the
 * input sits within 10^-800 of a halfway point.
 *
 * Hexadecimal input (0x1.8p3) is exact binary and is rounded directly.
 * "inf", "infinity" and "nan" are read in any case, "nan(...)" too.
 */
#include "pmlib.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>

#if defined( __SIZEOF_LONG_DOUBLE__ ) && __SIZEOF_LONG_DOUBLE__ > 8
#define X87_LONG_DOUBLE 1
#define BIGW            720             /* 23,000 bits */
#define NPOW            13
#else
#define X87_LONG_DOUBLE 0
#define BIGW            200             /* 6,400 bits */
#define NPOW            9
#endif

#define NDIG            800

typedef unsigned long long u64;

/* the three formats: mantissa bits, and the smallest and largest k */
typedef struct {
    int p;
    int kmin;
    int kmax;
} FORMAT;

static const FORMAT fmt_float = { 24, -149, 104 };
static const FORMAT fmt_double = { 53, -1074, 971 };
#if X87_LONG_DOUBLE
static const FORMAT fmt_ldouble = { 64, -16445, 16320 };
#endif

/*--- big integers ---------------------------------------------------*/

typedef struct {
    int      n;                         /* words in use */
    uint32_t w[BIGW];
} BIG;

static void big_set( BIG *b, u64 v )
{
    b->w[0] = (uint32_t)v;
    b->w[1] = (uint32_t)(v >> 32);
    b->n = b->w[1] ? 2 : (b->w[0] ? 1 : 0);
}

/* b = b * m + add */
static void big_muladd( BIG *b, uint32_t m, uint32_t add )
{
    u64 acc = add;
    int i;

    for ( i = 0; i < b->n; i++ ) {
        acc += (u64)b->w[i] * m;
        b->w[i] = (uint32_t)acc;
        acc >>= 32;
    }
    if ( acc && b->n < BIGW ) {
        b->w[b->n++] = (uint32_t)acc;
    }
}

static void big_pow10( BIG *b, int e )
{
    static const uint32_t small[9] = { 1, 10, 100, 1000, 10000, 100000, 1000000,
                                       10000000, 100000000 };

    while ( e >= 9 ) {
        big_muladd( b, 1000000000UL, 0 );
        e -= 9;
    }
    if ( e > 0 ) {
        big_muladd( b, small[e], 0 );
    }
}

static void big_shl( BIG *b, int bits )
{
    int words = bits / 32;
    int s = bits % 32;
    int i;

    if ( b->n == 0 || bits == 0 ) {
        return;
    }
    if ( b->n + words + 1 > BIGW ) {
        words = BIGW - b->n - 1;        /* cannot happen with the sizes above */
    }
    if ( s ) {
        b->w[b->n] = 0;
        for ( i = b->n; i > 0; i-- ) {
            b->w[i] = (b->w[i] << s) | (b->w[i - 1] >> (32 - s));
        }
        b->w[0] <<= s;
        b->n++;
    }
    if ( words ) {
        memmove( b->w + words, b->w, (size_t)b->n * sizeof( uint32_t ) );
        memset( b->w, 0, (size_t)words * sizeof( uint32_t ) );
        b->n += words;
    }
    while ( b->n > 0 && b->w[b->n - 1] == 0 ) {
        b->n--;
    }
}

static int big_cmp( const BIG *a, const BIG *b )
{
    int i;

    if ( a->n != b->n ) {
        return a->n < b->n ? -1 : 1;
    }
    for ( i = a->n - 1; i >= 0; i-- ) {
        if ( a->w[i] != b->w[i] ) {
            return a->w[i] < b->w[i] ? -1 : 1;
        }
    }
    return 0;
}

/*--- the decimal number ---------------------------------------------*/

/* the value is 0.digits * 10^exp, which is the integer digits times
   10^(exp - nd) */
typedef struct {
    char digits[NDIG + 2];              /* significant, no leading zero */
    int  nd;
    int  exp;
} DECIMAL;

/* The sign of digits * 10^(exp - nd) - h * 2^j, where h = base * mul
   + add: 2m + 1 and 4m - 1 do not fit in 64 bits when m has 64, so
   the caller gives them as (m, 2, 1) and (m - 1, 4, 3). */
static int compare( const DECIMAL *dec, u64 base, uint32_t mul, uint32_t add, int j )
{
    static BIG l, r;                    /* 6K: not on the stack */
    int e = dec->exp - dec->nd;
    int i, k, len;
    uint32_t chunk;

    big_set( &l, 0 );
    for ( i = 0; i < dec->nd; i += 9 ) {
        len = dec->nd - i < 9 ? dec->nd - i : 9;
        chunk = 0;
        for ( k = 0; k < len; k++ ) {
            chunk = chunk * 10 + (uint32_t)(dec->digits[i + k] - '0');
        }
        if ( l.n == 0 ) {
            big_set( &l, chunk );
        } else {
            big_pow10( &l, len );
            big_muladd( &l, 1, chunk );
        }
    }
    big_set( &r, base );
    big_muladd( &r, mul, add );
    if ( e >= 0 ) {
        big_pow10( &l, e );
    } else {
        big_pow10( &r, -e );
    }
    if ( j >= 0 ) {
        big_shl( &r, j );
    } else {
        big_shl( &l, -j );
    }
    return big_cmp( &l, &r );
}

/* 10^e in long double, for the estimate: a product of a few powers */
static long double pow10l( int e )
{
    static const long double powers[NPOW] = {
        1e1L, 1e2L, 1e4L, 1e8L, 1e16L, 1e32L, 1e64L, 1e128L, 1e256L
#if X87_LONG_DOUBLE
        , 1e512L, 1e1024L, 1e2048L, 1e4096L
#endif
    };
    long double r = 1.0L;
    int neg = e < 0;
    int i;

    if ( neg ) {
        e = -e;
    }
    for ( i = 0; e && i < NPOW; i++, e >>= 1 ) {
        if ( e & 1 ) {
            r *= powers[i];
        }
    }
    return neg ? 1.0L / r : r;
}

/* the long double estimate, taken apart into m * 2^k with m of P bits */
static void estimate( const DECIMAL *dec, const FORMAT *f, u64 *m, int *k )
{
    u64 lead = 0;
    int i, n = dec->nd < 19 ? dec->nd : 19;
    int e = dec->exp - n;
    long double x;
    int exp2 = 0;

    for ( i = 0; i < n; i++ ) {
        lead = lead * 10 + (u64)(dec->digits[i] - '0');
    }
    /* in two halves, so that neither step leaves the range */
    x = (long double)lead * pow10l( e / 2 );
    x *= pow10l( e - e / 2 );
    /* not "x - x != 0": Open Watcom keeps x in an x87 register at 64
       bits and subtracts the copy it stored as a double, which differs */
    if ( !(x <= LDBL_MAX) ) {
        *m = 1ULL << (f->p - 1);        /* past the largest long double */
        *k = f->kmax + 1;
        return;
    }
    if ( x == 0.0L ) {
        *m = 1;
        *k = f->kmin;
        return;
    }
    /* frexp by hand: x = frac * 2^exp2, frac in [0.5, 1) */
    while ( x >= 1.0L ) {
        if ( x >= 18446744073709551616.0L ) {
            x /= 18446744073709551616.0L;
            exp2 += 64;
        } else {
            x /= 2.0L;
            exp2++;
        }
    }
    while ( x < 0.5L ) {
        if ( x < 1.0L / 18446744073709551616.0L ) {
            x *= 18446744073709551616.0L;
            exp2 -= 64;
        } else {
            x *= 2.0L;
            exp2--;
        }
    }
    /* m = frac * 2^P, k = exp2 - P */
    for ( i = 0; i < f->p; i++ ) {
        x *= 2.0L;
    }
    *m = (u64)x;
    *k = exp2 - f->p;
    while ( *k < f->kmin ) {            /* subnormal: fewer bits */
        *m >>= 1;
        (*k)++;
    }
}

/* Round dec to the nearest m * 2^k of format f.  m == 0 is zero;
   k == kmax + 1 with m = 2^(P-1) is infinity. */
static void nearest( const DECIMAL *dec, const FORMAT *f, u64 *mp, int *kp )
{
    u64 top = 1ULL << (f->p - 1);       /* the smallest normal m */
    u64 m;
    int k, c, steps;

    estimate( dec, f, &m, &k );
    if ( k > f->kmax ) {
        m = (top << 1) - 1;             /* start from the largest; the */
        k = f->kmax;                    /* loop may still go on up */
    }
    for ( steps = 0; steps < 2000; steps++ ) {
        if ( k > f->kmax ) {
            break;                      /* infinity */
        }
        /* above the halfway point to the next one up? */
        c = compare( dec, m, 2, 1, k - 1 );
        if ( c > 0 || (c == 0 && (m & 1)) ) {
            m++;
            if ( m == top << 1 ) {
                m = top;
                k++;
            }
            continue;
        }
        if ( m == 0 ) {
            break;
        }
        /* below the halfway point to the next one down? */
        if ( m == top && k > f->kmin ) {
            c = compare( dec, m - 1, 4, 3, k - 2 );
        } else {
            c = compare( dec, m - 1, 2, 1, k - 1 );
        }
        if ( c < 0 || (c == 0 && (m & 1)) ) {
            if ( m == top && k > f->kmin ) {
                m = (top << 1) - 1;
                k--;
            } else {
                m--;
            }
            continue;
        }
        break;
    }
    *mp = m;
    *kp = k;
}

/*--- parsing ---------------------------------------------------------*/

static int match( const char *s, const char *word )
{
    int n = 0;

    while ( word[n] && tolower( (unsigned char)s[n] ) == word[n] ) {
        n++;
    }
    return word[n] ? 0 : n;
}

/* the kinds of thing parse() finds */
enum { P_NONE, P_ZERO, P_DEC, P_HEX, P_INF, P_NAN };

typedef struct {
    int     kind;
    int     neg;
    DECIMAL dec;
    u64     hmant;                      /* hex: the first 64 bits */
    int     hexp;                       /* hex: value = hmant * 2^hexp */
    int     hsticky;                    /* hex: bits that did not fit */
} PARSED;

/* read an exponent after e or p: digits with a sign; s if there are no
   digits, which leaves the e or p unread */
static const char *exponent( const char *s, long *value )
{
    const char *mark = s;
    int sign = 1;
    long e = 0;

    s++;
    if ( *s == '-' || *s == '+' ) {
        sign = (*s == '-') ? -1 : 1;
        s++;
    }
    if ( !isdigit( (unsigned char)*s ) ) {
        *value = 0;
        return mark;
    }
    while ( isdigit( (unsigned char)*s ) ) {
        if ( e < 100000 ) {
            e = e * 10 + (*s - '0');
        }
        s++;
    }
    *value = sign * e;
    return s;
}

static const char *parse_hex( const char *s, PARSED *p )
{
    int seen_point = 0;
    int bits = 0;                       /* bits in hmant */
    long e;
    int d;

    p->hmant = 0;
    p->hexp = 0;
    p->hsticky = 0;
    for ( ;; s++ ) {
        if ( *s == '.' && !seen_point ) {
            seen_point = 1;
            continue;
        }
        if ( !isxdigit( (unsigned char)*s ) ) {
            break;
        }
        d = isdigit( (unsigned char)*s ) ? *s - '0' : tolower( (unsigned char)*s ) - 'a' + 10;
        if ( bits == 0 && d == 0 ) {
            if ( seen_point ) {
                p->hexp -= 4;           /* a leading zero after the point */
            }
            continue;
        }
        if ( bits <= 60 ) {
            p->hmant = (p->hmant << 4) | (u64)d;
            bits += 4;
            if ( seen_point ) {
                p->hexp -= 4;
            }
        } else {
            if ( d ) {
                p->hsticky = 1;
            }
            if ( !seen_point ) {
                p->hexp += 4;           /* a digit that did not fit */
            }
        }
    }
    if ( *s == 'p' || *s == 'P' ) {
        s = exponent( s, &e );
        p->hexp += (int)e;
    }
    p->kind = p->hmant ? P_HEX : P_ZERO;
    return s;
}

static const char *parse( const char *s, PARSED *p )
{
    const char *start = s;
    const char *mark;
    int any = 0;
    int dropped = 0;
    long exp10 = 0;
    long e;
    int d;

    p->kind = P_NONE;
    p->neg = 0;
    p->dec.nd = 0;
    p->dec.exp = 0;
    while ( isspace( (unsigned char)*s ) ) {
        s++;
    }
    if ( *s == '-' || *s == '+' ) {
        p->neg = (*s == '-');
        s++;
    }
    if ( (d = match( s, "infinity" )) != 0 || (d = match( s, "inf" )) != 0 ) {
        p->kind = P_INF;
        return s + d;
    }
    if ( (d = match( s, "nan" )) != 0 ) {
        p->kind = P_NAN;
        s += d;
        if ( *s == '(' ) {
            mark = s + 1;
            while ( isalnum( (unsigned char)*mark ) || *mark == '_' ) {
                mark++;
            }
            if ( *mark == ')' ) {
                s = mark + 1;
            }
        }
        return s;
    }
    if ( s[0] == '0' && (s[1] == 'x' || s[1] == 'X')
         && (isxdigit( (unsigned char)s[2] ) || (s[2] == '.' && isxdigit( (unsigned char)s[3] ))) ) {
        return parse_hex( s + 2, p );
    }

    for ( ; isdigit( (unsigned char)*s ); s++ ) {
        any = 1;
        if ( p->dec.nd == 0 && *s == '0' ) {
            continue;
        }
        if ( p->dec.nd < NDIG ) {
            p->dec.digits[p->dec.nd++] = *s;
        } else if ( *s != '0' ) {
            dropped = 1;
        }
        exp10++;
    }
    if ( *s == '.' ) {
        for ( s++; isdigit( (unsigned char)*s ); s++ ) {
            any = 1;
            if ( p->dec.nd == 0 && *s == '0' ) {
                exp10--;
                continue;
            }
            if ( p->dec.nd < NDIG ) {
                p->dec.digits[p->dec.nd++] = *s;
            } else if ( *s != '0' ) {
                dropped = 1;
            }
        }
    }
    if ( !any ) {
        return start;                   /* no number here */
    }
    if ( *s == 'e' || *s == 'E' ) {
        s = exponent( s, &e );          /* "1e" is 1 followed by e */
        exp10 += e;
    }
    if ( p->dec.nd == 0 ) {
        p->kind = P_ZERO;
        return s;
    }
    while ( p->dec.nd > 1 && p->dec.digits[p->dec.nd - 1] == '0' ) {
        p->dec.nd--;
    }
    if ( dropped ) {
        p->dec.digits[p->dec.nd++] = '1';   /* something not zero follows */
    }
    p->dec.exp = (int)(exp10 > 100000 ? 100000 : exp10 < -100000 ? -100000 : exp10);
    p->kind = P_DEC;
    return s;
}

/* a hex mantissa, exact, rounded to P bits */
static void hex_nearest( const PARSED *p, const FORMAT *f, u64 *mp, int *kp )
{
    u64 m = p->hmant;
    int k = p->hexp;
    int sticky = p->hsticky;
    int lost = 0;                       /* the last bit shifted out */
    u64 limit = 1ULL << f->p;

    while ( m >= limit || k < f->kmin ) {
        if ( lost ) {
            sticky = 1;
        }
        lost = (int)(m & 1);
        m >>= 1;
        k++;
    }
    while ( m < (limit >> 1) && k > f->kmin ) {
        m <<= 1;                        /* normalise up, if there is room */
        k--;
    }
    if ( lost && (sticky || (m & 1)) ) {
        m++;
        if ( m == limit ) {
            m >>= 1;
            k++;
        }
    }
    *mp = m;
    *kp = k;
}

/* Parse and round to format f: m * 2^k, or a special.  Returns what
   was found, and sets *end. */
static int convert( const char *s, char **end, const FORMAT *f, u64 *m, int *k, int *neg )
{
    static PARSED p;                    /* 800 digits: not on the stack */
    const char *stop = parse( s, &p );

    if ( end != NULL ) {
        *end = (char *)stop;
    }
    *neg = p.neg;
    *m = 0;
    *k = f->kmin;
    if ( p.kind == P_DEC ) {
        /* clearly out of range either way: no need to look closer */
        if ( p.dec.exp > f->kmax / 3 + 30 ) {
            *m = 1ULL << (f->p - 1);
            *k = f->kmax + 1;
        } else if ( p.dec.exp >= (f->kmin - 64) / 3 - 30 ) {
            nearest( &p.dec, f, m, k );
        }
    } else if ( p.kind == P_HEX ) {
        hex_nearest( &p, f, m, k );
    }
    if ( p.kind == P_DEC || p.kind == P_HEX ) {
        if ( *k > f->kmax ) {
            errno = ERANGE;
            return P_INF;
        }
        if ( *m < (1ULL << (f->p - 1)) ) {
            errno = ERANGE;             /* subnormal, or gone to zero */
        }
    }
    return p.kind;
}

double strtod( const char *s, char **end )
{
    union {
        double v;
        u64 bits;
    } u;
    u64 m;
    int k, neg;
    int kind = convert( s, end, &fmt_double, &m, &k, &neg );

    if ( kind == P_INF ) {
        u.bits = 0x7FF0000000000000ULL;
    } else if ( kind == P_NAN ) {
        u.bits = 0x7FF8000000000000ULL;
    } else if ( m == 0 ) {
        u.bits = 0;
    } else if ( m >= (1ULL << 52) ) {
        u.bits = ((u64)(k + 52 + 1023) << 52) | (m & 0xFFFFFFFFFFFFFULL);
    } else {
        u.bits = m;                     /* subnormal: exponent field 0 */
    }
    if ( neg ) {
        u.bits |= 0x8000000000000000ULL;
    }
    return u.v;
}

float strtof( const char *s, char **end )
{
    union {
        float v;
        uint32_t bits;
    } u;
    u64 m;
    int k, neg;
    int kind = convert( s, end, &fmt_float, &m, &k, &neg );

    if ( kind == P_INF ) {
        u.bits = 0x7F800000UL;
    } else if ( kind == P_NAN ) {
        u.bits = 0x7FC00000UL;
    } else if ( m == 0 ) {
        u.bits = 0;
    } else if ( m >= (1ULL << 23) ) {
        u.bits = ((uint32_t)(k + 23 + 127) << 23) | (uint32_t)(m & 0x7FFFFF);
    } else {
        u.bits = (uint32_t)m;
    }
    if ( neg ) {
        u.bits |= 0x80000000UL;
    }
    return u.v;
}

long double strtold( const char *s, char **end )
{
#if X87_LONG_DOUBLE
    union {
        long double v;
        struct { uint32_t lo, hi; uint16_t se; } w;
    } u;
    u64 m;
    int k, neg;
    int kind = convert( s, end, &fmt_ldouble, &m, &k, &neg );

    memset( &u, 0, sizeof( u ) );
    if ( kind == P_INF ) {
        u.w.se = 0x7FFF;
        u.w.hi = 0x80000000UL;
    } else if ( kind == P_NAN ) {
        u.w.se = 0x7FFF;
        u.w.hi = 0xC0000000UL;
    } else if ( m != 0 ) {
        u.w.lo = (uint32_t)m;
        u.w.hi = (uint32_t)(m >> 32);
        u.w.se = (uint16_t)((m >> 63) ? k + 63 + 16383 : 0);
    }
    if ( neg ) {
        u.w.se |= 0x8000;
    }
    return u.v;
#else
    return strtod( s, end );            /* long double is double */
#endif
}

double atof( const char *s )
{
    return strtod( s, NULL );
}
