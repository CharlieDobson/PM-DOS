/*
 * X87NUM.C - the emulator's arithmetic: the working number (wide), its
 *            four operations and square root, rounding, and the formats
 *            the 387 reads and writes
 *
 * A wide is a 128-bit significand and an exponent.  The significand is
 * four 32-bit words because wcc386 calls helpers for a 64-bit integer
 * type, and PMIO has none to give it.  The carries are done by hand.
 *
 * WHAT IS EXACT.  An operand the 387 can be given has at most 64
 * significant bits, so a product of two fits in 128 exactly.  A sum is
 * exact when the exponents are within 64 of each other, and otherwise
 * the smaller one shifts out into a sticky bit, which is all rounding
 * needs of it.  A quotient and a root are developed bit by bit to as
 * many bits as the caller asks for, and what is left over becomes the
 * sticky bit.  Rounding a result once, at the end, therefore gives the
 * IEEE answer.  See IO\X87EMU.H.
 */
#include "x87emu.h"

void w_zero( wide *w )
{
    w->m[0] = 0;
    w->m[1] = 0;
    w->m[2] = 0;
    w->m[3] = 0;
    w->e = 0;
    w->sign = 0;
    w->kind = K_ZERO;
    w->denorm = 0;
    w->pad = 0;
}

void w_one( wide *w )
{
    w_zero( w );
    w->m[3] = 0x80000000;
    w->kind = K_FIN;
}

/* A table constant: four significand words, least first, and e */
void w_set( wide *w, const uint32_t *k )
{
    w_zero( w );
    w->m[0] = k[0];
    w->m[1] = k[1];
    w->m[2] = k[2];
    w->m[3] = k[3];
    w->e = (int32_t)k[4];
    w->kind = K_FIN;
}

int w_mzero( const wide *w )
{
    return ( w->m[0] | w->m[1] | w->m[2] | w->m[3] ) == 0;
}

static int clz32( uint32_t v )
{
    int count = 0;

    if ( !( v & 0xFFFF0000 ) ) {
        count += 16;
        v <<= 16;
    }
    if ( !( v & 0xFF000000 ) ) {
        count += 8;
        v <<= 8;
    }
    if ( !( v & 0xF0000000 ) ) {
        count += 4;
        v <<= 4;
    }
    if ( !( v & 0xC0000000 ) ) {
        count += 2;
        v <<= 2;
    }
    if ( !( v & 0x80000000 ) ) {
        count += 1;
    }
    return count;
}

/* m <<= count, 0 <= count < 128 */
static void m_shl( uint32_t *m, int count )
{
    int words = count >> 5;
    int bits = count & 31;
    int i;

    if ( words ) {
        for ( i = 3; i >= 0; i-- ) {
            m[i] = i >= words ? m[i - words] : 0;
        }
    }
    if ( bits ) {
        for ( i = 3; i > 0; i-- ) {
            m[i] = ( m[i] << bits ) | ( m[i - 1] >> ( 32 - bits ) );
        }
        m[0] <<= bits;
    }
}

/* m <<= 1, and the bit that falls off the top */
static uint32_t m_shl1( uint32_t *m )
{
    uint32_t out = m[3] >> 31;

    m[3] = ( m[3] << 1 ) | ( m[2] >> 31 );
    m[2] = ( m[2] << 1 ) | ( m[1] >> 31 );
    m[1] = ( m[1] << 1 ) | ( m[0] >> 31 );
    m[0] <<= 1;
    return out;
}

static int m_cmp( const uint32_t *a, const uint32_t *b )
{
    int i;

    for ( i = 3; i >= 0; i-- ) {
        if ( a[i] != b[i] ) {
            return a[i] > b[i] ? 1 : -1;
        }
    }
    return 0;
}

/* a += b, and the carry out */
static uint32_t m_add( uint32_t *a, const uint32_t *b )
{
    uint32_t carry = 0;
    uint32_t sum, next;
    int i;

    for ( i = 0; i < 4; i++ ) {
        sum = a[i] + carry;
        next = sum < carry;
        sum += b[i];
        next += sum < b[i];
        a[i] = sum;
        carry = next;
    }
    return carry;
}

/* a -= b, and the borrow out */
static uint32_t m_sub( uint32_t *a, const uint32_t *b )
{
    uint32_t borrow = 0;
    uint32_t diff, next;
    int i;

    for ( i = 0; i < 4; i++ ) {
        next = a[i] < b[i];
        diff = a[i] - b[i];
        next |= diff < borrow;
        a[i] = diff - borrow;
        borrow = next;
    }
    return borrow;
}

static uint32_t m_bit( const uint32_t *m, int n )
{
    return ( m[n >> 5] >> ( n & 31 ) ) & 1;
}

/* Any bit below bit n set?  n is 0 to 128. */
static uint32_t m_below( const uint32_t *m, int n )
{
    uint32_t any = 0;
    int i;

    for ( i = 0; i < ( n >> 5 ); i++ ) {
        any |= m[i];
    }
    if ( ( n & 31 ) && n < 128 ) {
        any |= m[n >> 5] & ( ( 1u << ( n & 31 ) ) - 1 );
    }
    return any;
}

/* Clear every bit below bit n, 0 to 128 */
static void m_clear( uint32_t *m, int n )
{
    int i;

    for ( i = 0; i < ( n >> 5 ); i++ ) {
        m[i] = 0;
    }
    if ( ( n & 31 ) && n < 128 ) {
        m[n >> 5] &= ~( ( 1u << ( n & 31 ) ) - 1 );
    }
}

/* Make bit 127 the top one.  A zero significand makes a zero. */
void w_norm( wide *w )
{
    int shift;

    if ( w_mzero( w ) ) {
        w->kind = K_ZERO;
        w->e = 0;
        return;
    }
    w->kind = K_FIN;
    if ( w->m[3] ) {
        shift = clz32( w->m[3] );
    } else if ( w->m[2] ) {
        shift = 32 + clz32( w->m[2] );
    } else if ( w->m[1] ) {
        shift = 64 + clz32( w->m[1] );
    } else {
        shift = 96 + clz32( w->m[0] );
    }
    if ( shift ) {
        m_shl( w->m, shift );
        w->e -= shift;
    }
}

/* Shift right, keeping the value: what falls off sets bit 0, the sticky
   bit, so that rounding still knows something was there. */
void w_shr( wide *w, int32_t count )
{
    uint32_t lost = 0;
    int words, bits, i;

    if ( count <= 0 ) {
        return;
    }
    w->e += count;
    if ( count >= 128 ) {
        lost = w->m[0] | w->m[1] | w->m[2] | w->m[3];
        w->m[0] = lost ? 1 : 0;
        w->m[1] = 0;
        w->m[2] = 0;
        w->m[3] = 0;
        return;
    }
    words = count >> 5;
    bits = count & 31;
    for ( i = 0; i < words; i++ ) {
        lost |= w->m[i];
    }
    for ( i = 0; i < 4; i++ ) {
        w->m[i] = i + words < 4 ? w->m[i + words] : 0;
    }
    if ( bits ) {
        lost |= w->m[0] << ( 32 - bits );
        for ( i = 0; i < 3; i++ ) {
            w->m[i] = ( w->m[i] >> bits ) | ( w->m[i + 1] << ( 32 - bits ) );
        }
        w->m[3] >>= bits;
    }
    if ( lost ) {
        w->m[0] |= 1;
    }
}

/* |a| against |b|, both finite: -1, 0 or 1 */
int w_cmpmag( const wide *a, const wide *b )
{
    if ( a->kind == K_ZERO || b->kind == K_ZERO ) {
        if ( a->kind == b->kind ) {
            return 0;
        }
        return a->kind == K_ZERO ? -1 : 1;
    }
    if ( a->e != b->e ) {
        return a->e > b->e ? 1 : -1;
    }
    return m_cmp( a->m, b->m );
}

/* a + b, both finite.  An exact zero comes out positive; the caller
   knows the rounding mode, which decides that sign. */
void w_add( wide *res, const wide *a, const wide *b )
{
    wide big, small;

    if ( a->kind == K_ZERO ) {
        *res = *b;
        res->denorm = 0;
        return;
    }
    if ( b->kind == K_ZERO ) {
        *res = *a;
        res->denorm = 0;
        return;
    }
    if ( w_cmpmag( a, b ) >= 0 ) {
        big = *a;
        small = *b;
    } else {
        big = *b;
        small = *a;
    }
    w_shr( &small, big.e - small.e );
    if ( big.sign == small.sign ) {
        if ( m_add( big.m, small.m ) ) {
            w_shr( &big, 1 );
            big.m[3] |= 0x80000000;
        }
    } else {
        m_sub( big.m, small.m );
        w_norm( &big );
        if ( big.kind == K_ZERO ) {
            big.sign = 0;
        }
    }
    big.denorm = 0;
    *res = big;
}

void w_sub( wide *res, const wide *a, const wide *b )
{
    wide neg = *b;

    neg.sign ^= 1;
    w_add( res, a, &neg );
}

/* a * b.  The 256-bit product's top 128 bits, and the rest as sticky. */
void w_mul( wide *res, const wide *a, const wide *b )
{
    uint32_t prod[8];
    uint32_t lo, hi, carry;
    int i, j;
    unsigned char sign = (unsigned char)( a->sign ^ b->sign );

    if ( a->kind == K_ZERO || b->kind == K_ZERO ) {
        w_zero( res );
        res->sign = sign;
        return;
    }
    for ( i = 0; i < 8; i++ ) {
        prod[i] = 0;
    }
    for ( i = 0; i < 4; i++ ) {
        if ( a->m[i] == 0 ) {
            continue;
        }
        carry = 0;
        for ( j = 0; j < 4; j++ ) {
            lo = mul32( a->m[i], b->m[j], &hi );
            lo += carry;
            if ( lo < carry ) {
                hi++;
            }
            prod[i + j] += lo;
            if ( prod[i + j] < lo ) {
                hi++;
            }
            carry = hi;
        }
        prod[i + 4] = carry;
    }
    res->sign = sign;
    res->kind = K_FIN;
    res->denorm = 0;
    res->pad = 0;
    res->e = a->e + b->e + 1;
    if ( !( prod[7] & 0x80000000 ) ) {
        /* the product is under 2^255: one bit up, from below */
        for ( i = 7; i > 0; i-- ) {
            prod[i] = ( prod[i] << 1 ) | ( prod[i - 1] >> 31 );
        }
        prod[0] <<= 1;
        res->e--;
    }
    res->m[0] = prod[4];
    res->m[1] = prod[5];
    res->m[2] = prod[6];
    res->m[3] = prod[7];
    if ( prod[0] | prod[1] | prod[2] | prod[3] ) {
        res->m[0] |= 1;
    }
}

/* a / b, b not zero, to `bits` significant bits (at most 127) and the
   sticky bit below them */
void w_div( wide *res, const wide *a, const wide *b, int bits )
{
    uint32_t numer[4], quot[4];
    uint32_t extra = 0;
    int i;
    unsigned char sign = (unsigned char)( a->sign ^ b->sign );

    if ( a->kind == K_ZERO ) {
        w_zero( res );
        res->sign = sign;
        return;
    }
    for ( i = 0; i < 4; i++ ) {
        numer[i] = a->m[i];
        quot[i] = 0;
    }
    res->e = a->e - b->e;
    if ( m_cmp( numer, b->m ) < 0 ) {
        extra = m_shl1( numer );
        res->e--;
    }
    for ( i = 0; i < bits; i++ ) {
        if ( extra || m_cmp( numer, b->m ) >= 0 ) {
            m_sub( numer, b->m );
            quot[( 127 - i ) >> 5] |= 1u << ( ( 127 - i ) & 31 );
        }
        extra = m_shl1( numer );
    }
    if ( extra || numer[0] | numer[1] | numer[2] | numer[3] ) {
        quot[0] |= 1;
    }
    for ( i = 0; i < 4; i++ ) {
        res->m[i] = quot[i];
    }
    res->sign = sign;
    res->kind = K_FIN;
    res->denorm = 0;
    res->pad = 0;
}

/* The bits of the radicand, from the top.  With an odd exponent the
   significand is used as it stands; with an even one, halved, so that
   the root's exponent is the exponent halved. */
static uint32_t sq_bit( const wide *a, int odd, int pos )
{
    if ( !odd ) {
        if ( pos == 0 ) {
            return 0;
        }
        pos--;
    }
    return pos < 128 ? m_bit( a->m, 127 - pos ) : 0;
}

/* The root of a positive finite a, to `bits` bits (at most 127), the
   way it is done by hand: two bits of the radicand at a time. */
void w_sqrt( wide *res, const wide *a, int bits )
{
    uint32_t rem[5], trial[5], root[4];
    uint32_t carry;
    int32_t rexp;
    int odd = a->e & 1;
    int i, j;

    if ( a->kind == K_ZERO ) {
        *res = *a;
        return;
    }
    for ( i = 0; i < 5; i++ ) {
        rem[i] = 0;
    }
    for ( i = 0; i < 4; i++ ) {
        root[i] = 0;
    }
    for ( i = 0; i < bits; i++ ) {
        /* rem = rem * 4 + the next two bits */
        for ( j = 4; j > 0; j-- ) {
            rem[j] = ( rem[j] << 2 ) | ( rem[j - 1] >> 30 );
        }
        rem[0] = ( rem[0] << 2 ) | ( sq_bit( a, odd, 2 * i ) << 1 )
                 | sq_bit( a, odd, 2 * i + 1 );
        /* trial = root * 4 + 1 */
        trial[4] = root[3] >> 30;
        for ( j = 3; j > 0; j-- ) {
            trial[j] = ( root[j] << 2 ) | ( root[j - 1] >> 30 );
        }
        trial[0] = ( root[0] << 2 ) | 1;
        for ( j = 4; j >= 0; j-- ) {
            if ( rem[j] != trial[j] ) {
                break;
            }
        }
        m_shl1( root );
        if ( j < 0 || rem[j] > trial[j] ) {
            carry = 0;
            for ( j = 0; j < 5; j++ ) {
                uint32_t next = rem[j] < trial[j] || ( rem[j] - trial[j] ) < carry;

                rem[j] = rem[j] - trial[j] - carry;
                carry = next;
            }
            root[0] |= 1;
        }
    }
    /* what is left, and floor( e / 2 ) for a negative e too - both from
       a before res is written, which may be the same number */
    carry = rem[0] | rem[1] | rem[2] | rem[3] | rem[4];
    for ( i = 2 * bits; i < 130; i++ ) {
        carry |= sq_bit( a, odd, i );
    }
    rexp = a->e >= 0 ? a->e / 2 : -( ( 1 - a->e ) / 2 );
    /* root has `bits` bits; move them to the top */
    for ( i = 0; i < 4; i++ ) {
        res->m[i] = root[i];
    }
    m_shl( res->m, 128 - bits );
    if ( carry ) {
        res->m[0] |= 1;
    }
    res->e = rexp;
    res->sign = 0;
    res->kind = K_FIN;
    res->denorm = 0;
    res->pad = 0;
}

/* The partial remainder FPREM and FPREM1 are built on.  x and y finite
   and not zero, n 0 to 63: q = floor( mx 2^n / my ) of the two
   significands, as though y had been scaled to x's exponent less n, and
   r = what is left, which is exact.  -> q's low 32 bits; r has x's
   sign. */
uint32_t w_prem( wide *r, const wide *x, const wide *y, int n )
{
    uint32_t numer[4];
    uint32_t extra = 0, quot = 0;
    int32_t ex = x->e;
    unsigned char sign = x->sign;
    int i;

    for ( i = 0; i < 4; i++ ) {
        numer[i] = x->m[i];
    }
    for ( i = 0; i <= n; i++ ) {
        if ( i ) {
            extra = m_shl1( numer );
        }
        quot <<= 1;
        if ( extra || m_cmp( numer, y->m ) >= 0 ) {
            m_sub( numer, y->m );
            quot |= 1;
        }
    }
    w_zero( r );
    for ( i = 0; i < 4; i++ ) {
        r->m[i] = numer[i];
    }
    r->e = ex - n;
    r->sign = sign;
    w_norm( r );
    return quot;
}

/* Bit n of the significand, 0 the lowest */
uint32_t w_bit( const wide *w, int n )
{
    return n >= 0 && n < 128 ? m_bit( w->m, n ) : 0;
}

/* w / divisor, divisor small and not zero; nothing is kept of the
   remainder, so this is for the series, not for results */
void w_divs( wide *w, uint32_t divisor )
{
    uint32_t rem = 0;
    int i;

    if ( w->kind != K_FIN ) {
        return;
    }
    for ( i = 3; i >= 0; i-- ) {
        w->m[i] = div32( w->m[i], rem, divisor, &rem );
    }
    w_norm( w );
}

/* w * factor, factor not zero */
void w_muls( wide *w, uint32_t factor )
{
    uint32_t prod[5];
    uint32_t lo, hi, carry = 0;
    int i, shift;

    if ( w->kind != K_FIN ) {
        return;
    }
    for ( i = 0; i < 4; i++ ) {
        lo = mul32( w->m[i], factor, &hi );
        lo += carry;
        if ( lo < carry ) {
            hi++;
        }
        prod[i] = lo;
        carry = hi;
    }
    prod[4] = carry;
    shift = prod[4] ? 32 - clz32( prod[4] ) : 0;
    for ( i = 0; i < 4; i++ ) {
        w->m[i] = prod[i];
    }
    if ( shift ) {
        /* the low 128 bits move down, and the fifth word into the room
           that leaves at the top */
        w_shr( w, shift );
        w->m[3] |= prod[4] << ( 32 - shift );
    }
    w_norm( w );
}

/* w = value */
void w_int( wide *w, uint32_t value )
{
    w_zero( w );
    w->m[3] = value;
    w->e = 31;
    w_norm( w );
}

/* Keep the top `keep` bits of the significand (any number up to 128;
   none, or fewer than none, when the value is below the unit) and round
   what is dropped by rc.  -> R_INEXACT, R_UP. */
uint32_t w_round( wide *w, int keep, int rc )
{
    uint32_t half, sticky, lsb, up = 0;
    int drop;
    int i;

    if ( w->kind != K_FIN || keep >= 128 ) {
        return 0;
    }
    if ( keep < 0 ) {
        w_shr( w, -keep );
        keep = 0;
    }
    drop = 128 - keep;
    half = m_bit( w->m, drop - 1 );
    sticky = m_below( w->m, drop - 1 );
    lsb = drop < 128 ? m_bit( w->m, drop ) : 0;
    m_clear( w->m, drop );
    if ( !half && !sticky ) {
        if ( w_mzero( w ) ) {
            w->kind = K_ZERO;
        }
        return 0;
    }
    switch ( rc ) {
    case RC_NEAR:
        up = half && ( sticky || lsb );
        break;
    case RC_DOWN:
        up = w->sign;
        break;
    case RC_UP:
        up = !w->sign;
        break;
    }
    if ( up ) {
        if ( drop == 128 ) {
            w->m[3] = 0x80000000;
            w->e++;
        } else {
            uint32_t add = 1u << ( drop & 31 );

            for ( i = drop >> 5; i < 4; i++ ) {
                w->m[i] += add;
                if ( w->m[i] >= add ) {
                    break;
                }
                add = 1;
            }
            if ( i == 4 ) {
                w->m[3] = 0x80000000;
                w->e++;
            }
        }
    }
    if ( w_mzero( w ) ) {
        w->kind = K_ZERO;
    }
    return R_INEXACT | ( up ? R_UP : 0 );
}

/* Round to a whole number by rc */
uint32_t w_rint( wide *w, int rc )
{
    if ( w->kind != K_FIN || w->e >= 127 ) {
        return 0;
    }
    return w_round( w, w->e + 1, rc );
}

/* Fit a finite result to a format: `prec` significant bits, normal
   exponents emin to emax.  -> SW_PE, SW_UE, SW_OE, SW_C1 (rounded up)
   and F_TINY, for the masked responses; w is the delivered result.
   Underflow is tininess after rounding and a loss of accuracy, as the
   387 has it. */
uint32_t w_fit( wide *w, int prec, int32_t emin, int32_t emax, int rc )
{
    wide probe;
    uint32_t res, flags = 0;
    int i;

    if ( w->kind != K_FIN ) {
        return 0;
    }
    probe = *w;
    res = w_round( &probe, prec, rc );
    if ( probe.e > emax ) {
        flags = SW_OE | SW_PE;
        if ( rc == RC_NEAR || ( rc == RC_UP && !w->sign )
          || ( rc == RC_DOWN && w->sign ) ) {
            w->kind = K_INF;
            w->m[3] = 0x80000000;
            w->m[2] = 0;
            w->m[1] = 0;
            w->m[0] = 0;
            flags |= SW_C1;
        } else {
            for ( i = 0; i < 4; i++ ) {
                w->m[i] = 0xFFFFFFFF;
            }
            m_clear( w->m, 128 - prec );
            w->e = emax;
        }
        return flags;
    }
    if ( probe.kind == K_ZERO || probe.e >= emin ) {
        *w = probe;
        if ( res & R_INEXACT ) {
            flags |= SW_PE;
        }
        if ( res & R_UP ) {
            flags |= SW_C1;
        }
        return flags;
    }
    flags = F_TINY;
    res = w_round( w, prec - ( emin - w->e ), rc );
    if ( res & R_INEXACT ) {
        flags |= SW_PE | SW_UE;
    }
    if ( res & R_UP ) {
        flags |= SW_C1;
    }
    return flags;
}

/*---------------------------------------------------------------------
 * The formats
 *-------------------------------------------------------------------*/

/* 80 bits: a register, or a ten-byte operand */
void x_unpack( wide *w, const xreg *x )
{
    uint32_t bexp = x->se & 0x7FFF;

    w_zero( w );
    w->sign = (unsigned char)( x->se >> 15 );
    w->m[3] = x->hi;
    w->m[2] = x->lo;
    if ( bexp == 0x7FFF ) {
        if ( !( x->hi & 0x80000000 ) ) {
            w->kind = K_BAD;
        } else if ( ( x->hi & 0x7FFFFFFF ) == 0 && x->lo == 0 ) {
            w->kind = K_INF;
        } else {
            w->kind = ( x->hi & 0x40000000 ) ? K_QNAN : K_SNAN;
        }
        return;
    }
    if ( bexp == 0 ) {
        if ( x->hi == 0 && x->lo == 0 ) {
            return;
        }
        /* a denormal, or the pseudo-denormal a 387 reads as one */
        w->e = EXT_EMIN;
        w->denorm = 1;
        w_norm( w );
        return;
    }
    if ( !( x->hi & 0x80000000 ) ) {
        w->kind = K_BAD;                /* an unnormal */
        return;
    }
    w->kind = K_FIN;
    w->e = (int32_t)bexp - EXT_BIAS;
}

/* A result w_fit has fitted to EXT_EMIN..EXT_EMAX, or a special */
void x_pack( xreg *x, const wide *w )
{
    wide t;

    x->se = (uint16_t)( w->sign << 15 );
    x->pad = 0;
    switch ( w->kind ) {
    case K_ZERO:
        x->hi = 0;
        x->lo = 0;
        return;
    case K_INF:
        x->se |= 0x7FFF;
        x->hi = 0x80000000;
        x->lo = 0;
        return;
    case K_QNAN:
    case K_SNAN:
    case K_BAD:
        x->se |= 0x7FFF;
        x->hi = w->m[3];
        x->lo = w->m[2];
        return;
    }
    if ( w->e >= EXT_EMIN ) {
        x->se |= (uint16_t)( w->e + EXT_BIAS );
        x->hi = w->m[3];
        x->lo = w->m[2];
        return;
    }
    t = *w;
    w_shr( &t, EXT_EMIN - w->e );
    x->hi = t.m[3];
    x->lo = t.m[2];
}

/* 64 bits */
void d_unpack( wide *w, uint32_t lo, uint32_t hi )
{
    uint32_t bexp = ( hi >> 20 ) & 0x7FF;
    uint32_t frac = hi & 0xFFFFF;

    w_zero( w );
    w->sign = (unsigned char)( hi >> 31 );
    w->m[3] = ( frac << 11 ) | ( lo >> 21 );
    w->m[2] = lo << 11;
    if ( bexp == 0x7FF ) {
        w->m[3] |= 0x80000000;
        if ( frac == 0 && lo == 0 ) {
            w->kind = K_INF;
        } else {
            w->kind = ( frac & 0x80000 ) ? K_QNAN : K_SNAN;
        }
        return;
    }
    if ( bexp == 0 ) {
        if ( frac == 0 && lo == 0 ) {
            return;
        }
        w->e = DBL_EMIN;
        w->denorm = 1;
        w_norm( w );
        return;
    }
    w->m[3] |= 0x80000000;
    w->kind = K_FIN;
    w->e = (int32_t)bexp - DBL_BIAS;
}

void d_pack( const wide *w, uint32_t *lo, uint32_t *hi )
{
    wide t = *w;
    uint32_t top = (uint32_t)w->sign << 31;

    switch ( w->kind ) {
    case K_ZERO:
        *hi = top;
        *lo = 0;
        return;
    case K_INF:
        *hi = top | 0x7FF00000;
        *lo = 0;
        return;
    case K_QNAN:
    case K_SNAN:
    case K_BAD:
        *hi = top | 0x7FF00000 | ( ( w->m[3] >> 11 ) & 0xFFFFF );
        *lo = ( w->m[3] << 21 ) | ( w->m[2] >> 11 );
        return;
    }
    if ( w->e >= DBL_EMIN ) {
        top |= (uint32_t)( w->e + DBL_BIAS ) << 20;
    } else {
        w_shr( &t, DBL_EMIN - w->e );
    }
    *hi = top | ( ( t.m[3] >> 11 ) & 0xFFFFF );
    *lo = ( t.m[3] << 21 ) | ( t.m[2] >> 11 );
}

/* 32 bits */
void f_unpack( wide *w, uint32_t bits )
{
    uint32_t bexp = ( bits >> 23 ) & 0xFF;
    uint32_t frac = bits & 0x7FFFFF;

    w_zero( w );
    w->sign = (unsigned char)( bits >> 31 );
    w->m[3] = frac << 8;
    if ( bexp == 0xFF ) {
        w->m[3] |= 0x80000000;
        if ( frac == 0 ) {
            w->kind = K_INF;
        } else {
            w->kind = ( frac & 0x400000 ) ? K_QNAN : K_SNAN;
        }
        return;
    }
    if ( bexp == 0 ) {
        if ( frac == 0 ) {
            return;
        }
        w->e = FLT_EMIN;
        w->denorm = 1;
        w_norm( w );
        return;
    }
    w->m[3] |= 0x80000000;
    w->kind = K_FIN;
    w->e = (int32_t)bexp - FLT_BIAS;
}

uint32_t f_pack( const wide *w )
{
    wide t = *w;
    uint32_t top = (uint32_t)w->sign << 31;

    switch ( w->kind ) {
    case K_ZERO:
        return top;
    case K_INF:
        return top | 0x7F800000;
    case K_QNAN:
    case K_SNAN:
    case K_BAD:
        return top | 0x7F800000 | ( ( w->m[3] >> 8 ) & 0x7FFFFF );
    }
    if ( w->e >= FLT_EMIN ) {
        top |= (uint32_t)( w->e + FLT_BIAS ) << 23;
    } else {
        w_shr( &t, FLT_EMIN - w->e );
    }
    return top | ( ( t.m[3] >> 8 ) & 0x7FFFFF );
}

/* A signed 64-bit integer, as two words */
void i_unpack( wide *w, uint32_t lo, uint32_t hi )
{
    w_zero( w );
    if ( hi & 0x80000000 ) {
        w->sign = 1;
        lo = ~lo + 1;
        hi = ~hi + ( lo == 0 );
    }
    w->m[3] = hi;
    w->m[2] = lo;
    w->e = 63;
    w_norm( w );
}

/* A whole number (w_rint has been) as a signed 64-bit integer.  0 if it
   does not fit. */
int i_pack( const wide *w, uint32_t *lo, uint32_t *hi )
{
    wide t = *w;

    *lo = 0;
    *hi = 0;
    if ( w->kind == K_ZERO ) {
        return 1;
    }
    if ( w->kind != K_FIN || w->e > 63 ) {
        return 0;
    }
    if ( w->e == 63 ) {
        /* 2^63 and over: only -2^63 itself */
        if ( !w->sign || w->m[3] != 0x80000000 || w->m[2] | w->m[1] | w->m[0] ) {
            return 0;
        }
        *hi = 0x80000000;
        return 1;
    }
    w_shr( &t, 127 - w->e );
    *lo = t.m[0];
    *hi = t.m[1];
    if ( w->sign ) {
        *lo = ~*lo + 1;
        *hi = ~*hi + ( *lo == 0 );
    }
    return 1;
}
