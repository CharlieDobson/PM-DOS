/*
 * FMTFLT.C - printf's %f %e %g %a
 *
 * EXACT.  A binary floating-point number is M * 2^E for integers M and
 * E, and every one has a finite decimal expansion, so the digits
 * printed are the true ones, rounded once at the precision asked for
 * (to even on an exact tie, as the x87 rounds by default).
 *
 * THE INTEGER PART, when E >= 0, is M shifted left: a big integer,
 * divided down by 10^9 to give nine digits at a time.  When E < 0 the
 * integer part is M >> -E, which fits in 64 bits, and the fraction is
 * F / 2^s with F < 2^s: multiplying F by 10^9 lifts the next nine
 * digits above bit s.  Digits are made only as far as the precision
 * needs, plus one to round on; "sticky" says whether anything nonzero
 * is left after them.
 *
 * The arithmetic is on 32-bit words with 64-bit intermediate products,
 * sized for the 80-bit long double of the GNU compilers (an exponent of
 * 16384 is 512 words).  Under Open Watcom long double is double, and
 * the arrays are smaller.
 *
 * %a is always normalised, 0x1.xxxp+N, with as many hex digits as the
 * value needs when no precision is given.
 */
#include "pmlib.h"
#include "fmt.h"
#include <stdlib.h>
#include <string.h>

#if defined( __SIZEOF_LONG_DOUBLE__ ) && __SIZEOF_LONG_DOUBLE__ > 8
#define X87_LONG_DOUBLE 1
#define WORDS           520             /* 16384 + 64 bits, and spare */
#else
#define X87_LONG_DOUBLE 0
#define WORDS           40              /* 1024 + 64 bits */
#endif

#define CHUNK           1000000000UL
#define LOCAL_DIGITS    600

typedef unsigned long long u64;

/* the number, taken apart: zero, infinite or NaN, or mant * 2^exp */
typedef struct {
    int neg;
    int kind;                           /* 0 zero, 1 finite, 2 inf, 3 nan */
    u64 mant;
    int exp;
} PARTS;

static void take_apart( long double value, PARTS *p )
{
#if X87_LONG_DOUBLE
    union {
        long double v;
        struct { uint32_t lo, hi; uint16_t se; } w;
    } u;
    int e;

    u.v = value;
    p->neg = (u.w.se >> 15) & 1;
    e = u.w.se & 0x7FFF;
    p->mant = ((u64)u.w.hi << 32) | u.w.lo;
    if ( e == 0x7FFF ) {
        p->kind = (p->mant << 1) ? 3 : 2;
        return;
    }
    p->exp = (e ? e : 1) - 16383 - 63;
#else
    union {
        double v;
        u64 bits;
    } u;
    int e;

    u.v = (double)value;
    p->neg = (int)(u.bits >> 63);
    e = (int)((u.bits >> 52) & 0x7FF);
    p->mant = u.bits & 0xFFFFFFFFFFFFFULL;
    if ( e == 0x7FF ) {
        p->kind = p->mant ? 3 : 2;
        return;
    }
    if ( e ) {
        p->mant |= 1ULL << 52;
    }
    p->exp = (e ? e : 1) - 1023 - 52;
#endif
    p->kind = p->mant ? 1 : 0;
}

/* The decimal digits of a finite nonzero number: d[0..n-1] with no
   leading zero, the decimal point after `point' of them (which may be
   0 or less, or past n), and sticky set if digits that are not zero
   follow the last one made. */
typedef struct {
    char *d;
    int   n;
    int   point;
    int   sticky;
    char *heap;                         /* d, when it had to be malloc()ed */
} DIGITS;

/* nine digits of chunk at out, leading zeroes and all */
static void nine( char *out, uint32_t chunk )
{
    int i;

    for ( i = 8; i >= 0; i-- ) {
        out[i] = (char)('0' + chunk % 10);
        chunk /= 10;
    }
}

/* Make the digits.  frac_need: digits wanted after the point (the %f
   kind), or -1; sig_need: significant digits wanted (the %e kind), or
   -1.  local is a buffer of LOCAL_DIGITS.  Returns 0, or -1 when the
   memory for a very long result could not be had. */
static int make_digits( const PARTS *p, int frac_need, int sig_need, char *local, DIGITS *dg )
{
    uint32_t big[WORDS];
    u64 mant = p->mant;
    int exp = p->exp;
    int nw, i, s, top, size, start;
    u64 acc;
    uint32_t chunk;
    char *d;
    int n = 0;
    int point = 0;
    int have_int = 0;

    while ( !(mant & 1) ) {             /* the fewer bits, the less work */
        mant >>= 1;
        exp++;
    }

    /* how much room the digits may need */
    if ( exp >= 0 ) {
        size = (int)(((long)(64 + exp) * 30103L) / 100000L) + 20;
    } else {
        s = -exp;
        size = 40 + (frac_need >= 0 ? frac_need : 0) + (sig_need >= 0 ? sig_need : 0);
        if ( size > s + 40 ) {
            size = s + 40;              /* a fraction has at most s digits */
        }
    }
    dg->heap = NULL;
    if ( size <= LOCAL_DIGITS ) {
        d = local;
    } else {
        d = dg->heap = (char *)malloc( (size_t)size );
        if ( d == NULL ) {
            return -1;
        }
    }

    if ( exp >= 0 ) {
        /* an integer: mant << exp, in words, then by 10^9 down to 0 */
        nw = (64 + exp) / 32 + 1;
        memset( big, 0, (size_t)nw * sizeof( uint32_t ) );
        i = exp / 32;
        s = exp % 32;
        big[i] = (uint32_t)(mant << s);
        big[i + 1] = (uint32_t)((s ? mant >> (32 - s) : mant >> 32) & 0xFFFFFFFFUL);
        if ( s ) {
            big[i + 2] = (uint32_t)(mant >> (64 - s));
        }
        while ( nw > 0 && big[nw - 1] == 0 ) {
            nw--;
        }
        /* the chunks come out lowest first: fill from the end */
        start = size;
        while ( nw > 0 ) {
            acc = 0;
            for ( i = nw - 1; i >= 0; i-- ) {
                acc = (acc << 32) | big[i];
                big[i] = (uint32_t)(acc / CHUNK);
                acc %= CHUNK;
            }
            start -= 9;
            nine( d + start, (uint32_t)acc );
            while ( nw > 0 && big[nw - 1] == 0 ) {
                nw--;
            }
        }
        while ( d[start] == '0' ) {
            start++;
        }
        n = size - start;
        memmove( d, d + start, (size_t)n );
        dg->d = d;
        dg->n = n;
        dg->point = n;
        dg->sticky = 0;
        return 0;
    }

    /* the integer part, 64 bits at most */
    s = -exp;
    if ( s < 64 && (mant >> s) != 0 ) {
        char tmp[20];
        u64 ip = mant >> s;
        int k = 0;

        while ( ip ) {
            tmp[k++] = (char)('0' + (int)(ip % 10));
            ip /= 10;
        }
        while ( k ) {
            d[n++] = tmp[--k];
        }
        point = n;
        have_int = 1;
    }

    /* the fraction, F / 2^s */
    nw = (s + 31) / 32;
    memset( big, 0, (size_t)nw * sizeof( uint32_t ) );
    if ( s >= 64 ) {
        big[0] = (uint32_t)mant;
        big[1] = (uint32_t)(mant >> 32);
    } else {
        u64 f = mant & ((1ULL << s) - 1);

        big[0] = (uint32_t)f;
        if ( nw > 1 ) {
            big[1] = (uint32_t)(f >> 32);
        }
    }
    top = s % 32;                       /* bits used in the top word; 0 = 32 */
    for ( ;; ) {
        /* stop when enough digits are made, or the fraction is gone */
        if ( frac_need >= 0 && n - point >= frac_need ) {
            break;
        }
        if ( sig_need >= 0 && (have_int || n > 0) && n >= sig_need ) {
            break;
        }
        for ( i = 0; i < nw && big[i] == 0; i++ ) {
        }
        if ( i == nw ) {
            break;
        }
        if ( n + 9 > size ) {
            break;                      /* cannot happen: size allows for it */
        }
        acc = 0;
        for ( i = 0; i < nw; i++ ) {
            acc += (u64)big[i] * CHUNK;
            big[i] = (uint32_t)acc;
            acc >>= 32;
        }
        if ( top ) {
            chunk = (uint32_t)((acc << (32 - top)) | (big[nw - 1] >> top));
            big[nw - 1] &= (1UL << top) - 1;
        } else {
            chunk = (uint32_t)acc;
        }
        if ( n == 0 && sig_need >= 0 && frac_need < 0 ) {
            /* no digit yet: leading zeroes only move the point */
            if ( chunk == 0 ) {
                point -= 9;
                continue;
            }
            nine( d, chunk );
            for ( i = 0; d[i] == '0'; i++ ) {
            }
            point -= i;
            memmove( d, d + i, (size_t)(9 - i) );
            n = 9 - i;
            continue;
        }
        nine( d + n, chunk );
        n += 9;
    }
    dg->sticky = 0;
    for ( i = 0; i < nw; i++ ) {
        if ( big[i] ) {
            dg->sticky = 1;
            break;
        }
    }
    /* a %f fraction keeps its leading zeroes; strip them for the
       "no leading zero" rule, moving the point */
    start = 0;
    while ( start < n && d[start] == '0' ) {
        start++;
    }
    if ( start ) {
        memmove( d, d + start, (size_t)(n - start) );
        n -= start;
        point -= start;
    }
    dg->d = d;
    dg->n = n;
    dg->point = point;
    return 0;
}

/* Round to keep the first r digits (r may be 0 or less, or n or more).
   Afterwards n <= r, and a carry out of the top digit becomes a 1 one
   place up.  Returns 1 if the result is zero. */
static int round_at( DIGITS *dg, int r )
{
    int up, i;

    if ( r >= dg->n ) {
        goto strip;                     /* nothing past r: exact */
    }
    if ( r < 0 ) {
        dg->n = 0;
        return 1;
    }
    if ( dg->d[r] > '5' ) {
        up = 1;
    } else if ( dg->d[r] < '5' ) {
        up = 0;
    } else {
        up = dg->sticky;
        for ( i = r + 1; !up && i < dg->n; i++ ) {
            up = dg->d[i] != '0';
        }
        if ( !up ) {                    /* a tie: to even */
            up = r > 0 && ((dg->d[r - 1] - '0') & 1);
        }
    }
    dg->n = r;
    if ( up ) {
        for ( i = r - 1; i >= 0; i-- ) {
            if ( dg->d[i] != '9' ) {
                dg->d[i]++;
                break;
            }
            dg->d[i] = '0';
        }
        if ( i < 0 ) {                  /* 999 became 1000 */
            dg->d[0] = '1';
            dg->n = r > 0 ? r : 1;
            for ( i = 1; i < dg->n; i++ ) {
                dg->d[i] = '0';
            }
            dg->point++;
        }
    }
strip:
    while ( dg->n > 0 && dg->d[dg->n - 1] == '0' ) {
        dg->n--;                        /* trailing zeroes are implied */
    }
    return dg->n == 0;
}

/* the digit at position i of the (rounded) number: '0' outside d */
#define DIGIT( dg, i )  (((i) >= 0 && (i) < (dg)->n) ? (dg)->d[(i)] : '0')

/* write the digits of dg from position from, count of them */
static int put_digits( __pm_out out, void *ctx, const DIGITS *dg, int from, int count )
{
    int lo, hi;

    while ( count > 0 && (from < 0 || from >= dg->n) ) {
        lo = from < 0 ? -from : count;  /* a run of zeroes */
        if ( lo > count ) {
            lo = count;
        }
        if ( from >= dg->n ) {
            lo = count;
        }
        if ( __pm_pad( out, ctx, '0', lo ) < 0 ) {
            return -1;
        }
        from += lo;
        count -= lo;
    }
    if ( count > 0 ) {
        hi = dg->n - from < count ? dg->n - from : count;
        if ( out( ctx, dg->d + from, (size_t)hi ) < 0 ) {
            return -1;
        }
        from += hi;
        count -= hi;
        if ( __pm_pad( out, ctx, '0', count ) < 0 ) {
            return -1;
        }
    }
    return 0;
}

/* sign, padding and body: body_len characters that emit() writes */
typedef struct {
    int         style;                  /* 'f' or 'e' */
    int         upper;
    DIGITS     *dg;
    int         zero;                   /* the value is (or rounded to) 0 */
    int         prec;                   /* digits after the point */
    int         dot;                    /* print the point */
    int         intlen;                 /* %f: digits before the point */
    int         xp;                     /* %e: the exponent */
    char        expbuf[12];
    int         explen;
} BODY;

static int emit_body( __pm_out out, void *ctx, BODY *b )
{
    char e[2];

    if ( b->style == 'f' ) {
        if ( b->zero || b->dg->point <= 0 ) {
            if ( out( ctx, "0", 1 ) < 0 ) {
                return -1;
            }
        } else if ( put_digits( out, ctx, b->dg, 0, b->intlen ) < 0 ) {
            return -1;
        }
        if ( b->dot && out( ctx, ".", 1 ) < 0 ) {
            return -1;
        }
        if ( b->zero ) {
            return __pm_pad( out, ctx, '0', b->prec );
        }
        return put_digits( out, ctx, b->dg, b->dg->point, b->prec );
    }
    if ( b->zero ) {
        if ( out( ctx, "0", 1 ) < 0 ) {
            return -1;
        }
    } else if ( put_digits( out, ctx, b->dg, 0, 1 ) < 0 ) {
        return -1;
    }
    if ( b->dot && out( ctx, ".", 1 ) < 0 ) {
        return -1;
    }
    if ( b->zero ) {
        if ( __pm_pad( out, ctx, '0', b->prec ) < 0 ) {
            return -1;
        }
    } else if ( put_digits( out, ctx, b->dg, 1, b->prec ) < 0 ) {
        return -1;
    }
    e[0] = b->upper ? 'E' : 'e';
    if ( out( ctx, e, 1 ) < 0 ) {
        return -1;
    }
    return out( ctx, b->expbuf, (size_t)b->explen );
}

static void make_exp( BODY *b, int xp, int min_digits )
{
    char tmp[8];
    int k = 0;
    unsigned u = xp < 0 ? (unsigned)-xp : (unsigned)xp;

    b->explen = 0;
    b->expbuf[b->explen++] = xp < 0 ? '-' : '+';
    do {
        tmp[k++] = (char)('0' + u % 10);
        u /= 10;
    } while ( u );
    while ( k < min_digits ) {
        tmp[k++] = '0';
    }
    while ( k ) {
        b->expbuf[b->explen++] = tmp[--k];
    }
}

/* the sign, the width and the 0 flag around a body of len characters */
static int frame( __pm_out out, void *ctx, const char *sign, int len, int flags, int width,
                  int zero_ok, int (*body)( __pm_out, void *, void * ), void *arg )
{
    int total = (int)strlen( sign ) + len;
    int fill = width > total ? width - total : 0;

    if ( !(flags & FF_MINUS) && !((flags & FF_ZERO) && zero_ok) && __pm_pad( out, ctx, ' ', fill ) < 0 ) {
        return -1;
    }
    if ( *sign && out( ctx, sign, strlen( sign ) ) < 0 ) {
        return -1;
    }
    if ( !(flags & FF_MINUS) && (flags & FF_ZERO) && zero_ok && __pm_pad( out, ctx, '0', fill ) < 0 ) {
        return -1;
    }
    if ( body( out, ctx, arg ) < 0 ) {
        return -1;
    }
    if ( (flags & FF_MINUS) && __pm_pad( out, ctx, ' ', fill ) < 0 ) {
        return -1;
    }
    return total + fill;
}

static int body_thunk( __pm_out out, void *ctx, void *arg )
{
    return emit_body( out, ctx, (BODY *)arg );
}

static int text_thunk( __pm_out out, void *ctx, void *arg )
{
    const char *s = (const char *)arg;

    return out( ctx, s, strlen( s ) );
}

/* %a and %A: "0x1." and up to 16 digits, then zeroes for a precision
   past 16, then the exponent */
typedef struct {
    char text[24];
    int  len;
    int  zeros;
    BODY exp;
} HEXBODY;

static int hex_thunk( __pm_out out, void *ctx, void *arg )
{
    HEXBODY *h = (HEXBODY *)arg;

    if ( out( ctx, h->text, (size_t)h->len ) < 0 ) {
        return -1;
    }
    if ( __pm_pad( out, ctx, '0', h->zeros ) < 0 ) {
        return -1;
    }
    return out( ctx, h->exp.expbuf, (size_t)h->exp.explen );
}

static int fmt_hex( __pm_out out, void *ctx, const PARTS *p, const char *sign,
                    int flags, int width, int prec, int upper )
{
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    HEXBODY h;
    u64 frac = 0;
    u64 kept, rest;
    u64 m = p->mant;
    int xp = 0;
    int lead = 0;
    int nd, i;

    if ( p->kind == 1 ) {
        lead = 1;
        xp = p->exp + 63;
        while ( !(m & 0x8000000000000000ULL) ) {
            m <<= 1;
            xp--;
        }
        frac = m << 1;                  /* the 63 bits after the leading 1 */
    }
    if ( prec < 0 ) {
        nd = 16;                        /* as many as the value needs */
        while ( nd > 0 && ((frac >> (64 - 4 * nd)) & 0xF) == 0 ) {
            nd--;
        }
    } else {
        nd = prec;
        if ( nd < 16 ) {
            kept = nd ? frac >> (64 - 4 * nd) : 0;
            rest = nd ? frac << (4 * nd) : frac;
            if ( rest > 0x8000000000000000ULL
                 || (rest == 0x8000000000000000ULL && ((nd ? kept : (u64)lead) & 1)) ) {
                kept++;
                if ( nd == 0 || (kept >> (4 * nd)) != 0 ) {
                    kept = 0;           /* 1.fff rounded up is 2.000: 1.000 * 2 */
                    xp++;
                }
            }
            frac = nd ? kept << (64 - 4 * nd) : 0;
        }
    }
    h.len = 0;
    h.text[h.len++] = '0';
    h.text[h.len++] = upper ? 'X' : 'x';
    h.text[h.len++] = (char)('0' + lead);
    if ( nd > 0 || (flags & FF_HASH) ) {
        h.text[h.len++] = '.';
    }
    for ( i = 0; i < nd && i < 16; i++ ) {
        h.text[h.len++] = set[(frac >> (60 - 4 * i)) & 0xF];
    }
    h.zeros = nd > 16 ? nd - 16 : 0;
    make_exp( &h.exp, xp, 1 );
    h.exp.expbuf[h.exp.explen] = '\0';
    {
        /* the exponent letter goes in front of its sign */
        memmove( h.exp.expbuf + 1, h.exp.expbuf, (size_t)h.exp.explen + 1 );
        h.exp.expbuf[0] = upper ? 'P' : 'p';
        h.exp.explen++;
    }
    return frame( out, ctx, sign, h.len + h.zeros + h.exp.explen, flags, width, 1, hex_thunk, &h );
}

int __pm_fmt_float( __pm_out out, void *ctx, long double value,
                    int flags, int width, int prec, int conv )
{
    PARTS p;
    DIGITS dg;
    BODY b;
    char local[LOCAL_DIGITS];
    const char *sign;
    int upper = (conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A');
    int style, sig, xp, result;
    int strip = 0;

    take_apart( value, &p );
    sign = p.neg ? "-" : (flags & FF_PLUS) ? "+" : (flags & FF_SPACE) ? " " : "";
    if ( p.kind >= 2 ) {
        const char *word = (p.kind == 2) ? (upper ? "INF" : "inf") : (upper ? "NAN" : "nan");

        return frame( out, ctx, sign, 3, flags, width, 0, text_thunk, (void *)word );
    }
    if ( conv == 'a' || conv == 'A' ) {
        return fmt_hex( out, ctx, &p, sign, flags, width, prec, upper );
    }
    if ( prec < 0 ) {
        prec = 6;
    }

    memset( &b, 0, sizeof( b ) );
    b.upper = upper;
    b.dg = &dg;
    dg.heap = NULL;
    dg.n = 0;
    dg.point = 1;
    dg.d = local;
    dg.sticky = 0;

    style = conv | 0x20;                /* lower case */
    if ( style == 'g' ) {
        sig = prec == 0 ? 1 : prec;
        if ( p.kind == 0 ) {
            xp = 0;
            b.zero = 1;
        } else {
            if ( make_digits( &p, -1, sig + 1, local, &dg ) < 0 ) {
                return -1;
            }
            b.zero = round_at( &dg, sig );
            xp = dg.point - 1;
        }
        if ( xp < sig && xp >= -4 ) {
            style = 'f';
            prec = sig - 1 - xp;
        } else {
            style = 'e';
            prec = sig - 1;
        }
        strip = !(flags & FF_HASH);
    } else if ( p.kind == 0 ) {
        b.zero = 1;
        xp = 0;
    } else if ( style == 'f' ) {
        if ( make_digits( &p, prec + 1, -1, local, &dg ) < 0 ) {
            return -1;
        }
        b.zero = round_at( &dg, dg.point + prec );
        xp = 0;
    } else {
        if ( make_digits( &p, -1, prec + 2, local, &dg ) < 0 ) {
            return -1;
        }
        b.zero = round_at( &dg, prec + 1 );
        xp = dg.point - 1;
    }
    if ( b.zero ) {
        xp = 0;
    }

    if ( strip ) {
        /* %g without #: no trailing zeroes after the point */
        int last;

        if ( b.zero ) {
            prec = 0;
        } else if ( style == 'f' ) {
            last = dg.n - dg.point;     /* digits that matter after the point */
            prec = last < 0 ? 0 : (last < prec ? last : prec);
        } else {
            last = dg.n - 1;
            prec = last < prec ? last : prec;
        }
    }

    b.style = style;
    b.prec = prec;
    b.dot = prec > 0 || (flags & FF_HASH);
    if ( style == 'f' ) {
        b.intlen = (b.zero || dg.point <= 0) ? 1 : dg.point;
        result = frame( out, ctx, sign, b.intlen + b.dot + prec, flags, width, 1, body_thunk, &b );
    } else {
        b.xp = xp;
        make_exp( &b, xp, 2 );
        result = frame( out, ctx, sign, 1 + b.dot + prec + 1 + b.explen, flags, width, 1,
                        body_thunk, &b );
    }
    if ( dg.heap != NULL ) {
        free( dg.heap );
    }
    return result;
}
