/*
 * BIGDEC.C - a DOUBLE to decimal digits and decimal digits to a DOUBLE,
 * exactly, in whole-number arithmetic.
 *
 * WHY NOT THE COPROCESSOR: scaling by a power of ten in the x87's 64-bit
 * precision gets the last digit right on a real 486, but an emulator
 * that keeps only 53 bits (DOSBox-X does) loses it - 1D300 came out as
 * 9.999999999999998D+299 there.  Here every digit is exact on any
 * machine: the binary value is a whole number times a power of two, and
 * that is written out in full before it is rounded.
 *
 * THE NUMBERS are kept in 16-bit pieces, lowest first, so that a product
 * of two pieces and a quotient of a piece fit in 32 bits: this program
 * has no 64-bit arithmetic library.  The largest needed is 5^1074 times
 * a 53-bit mantissa, about 2,550 bits.
 */
#include "qb.h"

#define LIMBS   170             /* 2,720 bits */

typedef struct {
    int n;                      /* pieces in use; 0 is zero */
    u16 d[LIMBS];
} BIG;

static void big_trim( BIG *b )
{
    while ( b->n > 0 && b->d[b->n - 1] == 0 ) {
        b->n--;
    }
}

static void big_set( BIG *b, u32 lo, u32 hi )
{
    b->d[0] = (u16)lo;
    b->d[1] = (u16)(lo >> 16);
    b->d[2] = (u16)hi;
    b->d[3] = (u16)(hi >> 16);
    b->n = 4;
    big_trim( b );
}

/* b *= m, m below 65536 */
static void big_mul( BIG *b, u32 m )
{
    u32 carry = 0, t;
    int i;

    for ( i = 0; i < b->n; i++ ) {
        t = (u32)b->d[i] * m + carry;
        b->d[i] = (u16)t;
        carry = t >> 16;
    }
    if ( carry && b->n < LIMBS ) {
        b->d[b->n++] = (u16)carry;
    }
}

/* b /= div, div below 65536: -> the remainder */
static u32 big_div( BIG *b, u32 div )
{
    u32 rem = 0, t;
    int i;

    for ( i = b->n - 1; i >= 0; i-- ) {
        t = (rem << 16) | b->d[i];
        b->d[i] = (u16)(t / div);
        rem = t % div;
    }
    big_trim( b );
    return rem;
}

static void big_mul_pow( BIG *b, u32 base, int count )
{
    /* 5^6 and 10^4 are the largest powers below 65536 */
    u32 step = base == 5 ? 15625UL : 10000UL;
    int per = base == 5 ? 6 : 4;

    while ( count >= per ) {
        big_mul( b, step );
        count -= per;
    }
    while ( count-- > 0 ) {
        big_mul( b, base );
    }
}

static int big_bits( const BIG *b )
{
    int bits;
    u16 top;

    if ( b->n == 0 ) {
        return 0;
    }
    top = b->d[b->n - 1];
    for ( bits = 0; top; bits++ ) {
        top >>= 1;
    }
    return (b->n - 1) * 16 + bits;
}

/* bit "at" of b */
static int big_bit( const BIG *b, int at )
{
    if ( at < 0 || at / 16 >= b->n ) {
        return 0;
    }
    return (b->d[at / 16] >> (at % 16)) & 1;
}

/* any bit below "at" set? */
static int big_any_below( const BIG *b, int at )
{
    int i;

    for ( i = 0; i < at / 16 && i < b->n; i++ ) {
        if ( b->d[i] ) {
            return 1;
        }
    }
    if ( at / 16 < b->n && (b->d[at / 16] & ((1u << (at % 16)) - 1)) ) {
        return 1;
    }
    return 0;
}

static void big_shl( BIG *b, int bits )
{
    int whole = bits / 16, part = bits % 16, i;

    if ( b->n == 0 || bits == 0 ) {
        return;
    }
    if ( b->n + whole + 1 > LIMBS ) {
        whole = LIMBS - b->n - 1;           /* cannot happen with the sizes used */
    }
    for ( i = b->n - 1; i >= 0; i-- ) {
        b->d[i + whole] = b->d[i];
    }
    for ( i = 0; i < whole; i++ ) {
        b->d[i] = 0;
    }
    b->n += whole;
    if ( part ) {
        u32 carry = 0, t;
        for ( i = whole; i < b->n; i++ ) {
            t = ((u32)b->d[i] << part) | carry;
            b->d[i] = (u16)t;
            carry = t >> 16;
        }
        if ( carry ) {
            b->d[b->n++] = (u16)carry;
        }
    }
}

/* b >>= bits */
static void big_shr( BIG *b, int bits )
{
    int whole = bits / 16, part = bits % 16, i;

    if ( whole >= b->n ) {
        b->n = 0;
        return;
    }
    for ( i = 0; i + whole < b->n; i++ ) {
        b->d[i] = b->d[i + whole];
    }
    b->n -= whole;
    if ( part ) {
        for ( i = 0; i < b->n; i++ ) {
            b->d[i] = (u16)((b->d[i] >> part) | (i + 1 < b->n ? (u16)(b->d[i + 1] << (16 - part)) : 0));
        }
    }
    big_trim( b );
}

static void big_shr1( BIG *b )
{
    int i;

    for ( i = 0; i < b->n; i++ ) {
        b->d[i] = (u16)((b->d[i] >> 1) | (i + 1 < b->n ? (u16)(b->d[i + 1] << 15) : 0));
    }
    big_trim( b );
}

static int big_cmp( const BIG *a, const BIG *b )
{
    int i;

    if ( a->n != b->n ) {
        return a->n < b->n ? -1 : 1;
    }
    for ( i = a->n - 1; i >= 0; i-- ) {
        if ( a->d[i] != b->d[i] ) {
            return a->d[i] < b->d[i] ? -1 : 1;
        }
    }
    return 0;
}

/* a -= b, a not below b */
static void big_sub( BIG *a, const BIG *b )
{
    u32 borrow = 0, t;
    int i;

    for ( i = 0; i < a->n; i++ ) {
        t = (u32)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        a->d[i] = (u16)t;
        borrow = (t >> 16) & 1;
    }
    big_trim( a );
}

/* ------------------------------------------------------------------ */
/* a DOUBLE to digits                                                  */
/* ------------------------------------------------------------------ */

typedef union {
    double v;
    u32    w[2];
} DBITS;

static BIG work, other;
static char text[800];          /* 5^1074's digits are about 750 */

/* the digits of |v| (not zero), rounded to "prec" significant ones -
   a half to the even - with trailing zeros off; *exp10 is the power of
   ten of the first.  -> how many digits */
int dec_digits( double v, int prec, char *digits, int *exp10 )
{
    DBITS bits;
    u32 hi, lo;
    int field, e2, len, i, up, count;
    u32 rem;

    bits.v = v;
    hi = bits.w[1] & 0x7FFFFFFFUL;
    lo = bits.w[0];
    field = (int)(hi >> 20);
    hi &= 0x000FFFFFUL;
    if ( field ) {
        hi |= 0x00100000UL;
    }
    e2 = (field ? field : 1) - 1075;
    big_set( &work, lo, hi );
    if ( e2 >= 0 ) {
        big_shl( &work, e2 );
        e2 = 0;
    } else {
        big_mul_pow( &work, 5, -e2 );       /* m / 2^k = m * 5^k / 10^k */
    }
    /* the whole number's digits, four at a time from the bottom */
    len = 0;
    while ( work.n > 0 && len < (int)sizeof( text ) - 4 ) {
        rem = big_div( &work, 10000UL );
        for ( i = 0; i < 4; i++ ) {
            text[len++] = (char)('0' + rem % 10);
            rem /= 10;
        }
    }
    while ( len > 1 && text[len - 1] == '0' ) {
        len--;                              /* the zeros at the top */
    }
    /* turned round: the first digit first */
    for ( i = 0; i < len / 2; i++ ) {
        char swap = text[i];
        text[i] = text[len - 1 - i];
        text[len - 1 - i] = swap;
    }
    *exp10 = len - 1 + e2;
    for ( i = 0; i < prec; i++ ) {
        digits[i] = i < len ? text[i] : '0';
    }
    if ( len > prec ) {
        /* rounded at "prec": past the half up, at the half to the even */
        up = 0;
        if ( text[prec] > '5' ) {
            up = 1;
        } else if ( text[prec] == '5' ) {
            for ( i = prec + 1; i < len && text[i] == '0'; i++ ) {
            }
            up = i < len ? 1 : ((digits[prec - 1] - '0') & 1);
        }
        if ( up ) {
            for ( i = prec - 1; i >= 0; i-- ) {
                if ( digits[i] == '9' ) {
                    digits[i] = '0';
                } else {
                    digits[i]++;
                    break;
                }
            }
            if ( i < 0 ) {                  /* 9.99 to 10.0 */
                digits[0] = '1';
                for ( i = 1; i < prec; i++ ) {
                    digits[i] = '0';
                }
                (*exp10)++;
            }
        }
    }
    count = prec;
    while ( count > 1 && digits[count - 1] == '0' ) {
        count--;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* digits to a DOUBLE                                                  */
/* ------------------------------------------------------------------ */

/* q (a whole number) times 2^shift, to the nearest DOUBLE: the bits
   below the 53 kept, and "sticky" for any below those, round it */
static double make_double( BIG *q, int shift, int sticky )
{
    DBITS out;
    int bits = big_bits( q ), drop, e, top_bit, half, below, i;
    u32 mant_hi, mant_lo;

    if ( bits == 0 ) {
        return 0.0;
    }
    e = bits - 1 + shift;                   /* the value is in [2^e, 2^(e+1)) */
    drop = bits - 53;                       /* bits of q that do not fit */
    if ( e < -1022 ) {
        drop += -1022 - e;                  /* a subnormal keeps fewer */
    }
    if ( drop > 0 ) {
        half = big_bit( q, drop - 1 );
        below = sticky || big_any_below( q, drop - 1 );
        big_shr( q, drop );
        shift += drop;
        if ( half && (below || (q->n && (q->d[0] & 1))) ) {
            /* q + 1 */
            for ( i = 0; i < q->n; i++ ) {
                if ( ++q->d[i] != 0 ) {
                    break;
                }
            }
            if ( i == q->n && q->n < LIMBS ) {
                q->d[q->n++] = 1;
            }
        }
    } else if ( drop < 0 ) {
        big_shl( q, -drop );
        shift += drop;
    }
    bits = big_bits( q );
    if ( bits == 0 ) {
        return 0.0;
    }
    e = bits - 1 + shift;
    if ( e > 1023 ) {
        out.w[1] = 0x7FF00000UL;            /* too big: the caller says Overflow */
        out.w[0] = 0;
        return out.v;
    }
    /* q is 53 bits now, or fewer for a subnormal */
    mant_lo = (u32)q->d[0] | ((u32)(q->n > 1 ? q->d[1] : 0) << 16);
    mant_hi = (u32)(q->n > 2 ? q->d[2] : 0) | ((u32)(q->n > 3 ? q->d[3] : 0) << 16);
    if ( e < -1022 || bits < 53 ) {
        out.w[1] = mant_hi & 0x000FFFFFUL;  /* the exponent field 0 */
        out.w[0] = mant_lo;
        return out.v;
    }
    top_bit = e + 1023;
    out.w[1] = ((u32)top_bit << 20) | (mant_hi & 0x000FFFFFUL);
    out.w[0] = mant_lo;
    return out.v;
}

/* the whole number hi:lo (below 2^63) times 10^scale, to the nearest
   DOUBLE; too big for one is the infinity DBL_BAD looks for */
double dec_build( u32 lo, u32 hi, int scale )
{
    int digits, shift, qbits, bit;
    u32 t;
    BIG *n = &work, *d = &other;
    static BIG q;

    if ( lo == 0 && hi == 0 ) {
        return 0.0;
    }
    /* far out of range either way: no need to work it out */
    for ( digits = 0, t = hi ? hi : lo; t; t /= 10 ) {
        digits++;
    }
    if ( hi ) {
        digits += 9;                        /* hi is at least 2^32 */
    }
    if ( digits + scale > 330 ) {
        DBITS inf;
        inf.w[1] = 0x7FF00000UL;            /* too big: the caller says Overflow */
        inf.w[0] = 0;
        return inf.v;
    }
    if ( digits + scale < -345 ) {
        return 0.0;
    }
    big_set( n, lo, hi );
    if ( scale >= 0 ) {
        big_mul_pow( n, 10, scale );
        return make_double( n, 0, 0 );
    }
    /* n / 10^-scale: n moved up so that the quotient has 56 bits or
       more, divided a bit at a time, the remainder kept as "sticky" */
    big_set( d, 1, 0 );
    big_mul_pow( d, 10, -scale );
    shift = big_bits( d ) - big_bits( n ) + 56;
    if ( shift < 0 ) {
        shift = 0;
    }
    big_shl( n, shift );
    qbits = big_bits( n ) - big_bits( d ) + 1;
    q.n = 0;
    if ( qbits > 0 ) {
        big_shl( d, qbits - 1 );
        for ( bit = qbits - 1; bit >= 0; bit-- ) {
            if ( big_cmp( n, d ) >= 0 ) {
                big_sub( n, d );
                while ( q.n <= bit / 16 ) {
                    q.d[q.n++] = 0;
                }
                q.d[bit / 16] |= (u16)(1u << (bit % 16));
            }
            big_shr1( d );
        }
        big_trim( &q );
    }
    return make_double( &q, -shift, n->n != 0 );
}
