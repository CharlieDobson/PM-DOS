/*
 * FX.C - arithmetic without a coprocessor: 16.16 fixed point, the REAL
 * a number from the file is read into, and matrices of each.
 *
 * A REAL is a 30-bit mantissa and a binary exponent.  It is used where
 * precision matters and speed does not: reading numbers, and joining
 * one matrix to another.  A scale of 0.001 followed by one of 1000 has
 * to come out as 1, and in 16.16 it comes out as 1.007.  What a matrix
 * is finally used for - turning the points of a path into pixels - is
 * done in fx, where a coefficient's error of 2^-17 times a coordinate
 * below 32768 is under a quarter of a pixel.
 */
#include "pdf.h"

/* ------------------------------------------------------------------ */
/* integers                                                            */
/* ------------------------------------------------------------------ */

s32 mul_div( s32 lhs, s32 rhs, s32 den )
{
    u32 ula, urh, ude, hi, lo, quo;
    int neg = 0;

    if ( lhs < 0 ) {
        neg ^= 1;
        ula = (u32)-lhs;
    } else {
        ula = (u32)lhs;
    }
    if ( rhs < 0 ) {
        neg ^= 1;
        urh = (u32)-rhs;
    } else {
        urh = (u32)rhs;
    }
    if ( den < 0 ) {
        neg ^= 1;
        ude = (u32)-den;
    } else {
        ude = (u32)den;
    }
    hi = umul_hi( ula, urh );
    lo = ula * urh;
    if ( hi >= ude ) {
        return neg ? -FX_MAX : FX_MAX;
    }
    quo = udiv64( hi, lo, ude );
    if ( quo > 0x7FFFFFFFUL ) {
        quo = 0x7FFFFFFFUL;
    }
    return neg ? -(s32)quo : (s32)quo;
}

fx fx_div( fx num, fx den )
{
    return mul_div( num, FX_ONE, den );
}

u32 isqrt( u32 val )
{
    u32 root = 0, bit = 0x40000000UL;

    while ( bit > val ) {
        bit >>= 2;
    }
    while ( bit ) {
        if ( val >= root + bit ) {
            val -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

fx fx_sqrt( fx val )
{
    fx root;

    if ( val <= 0 ) {
        return 0;
    }
    root = (fx)(isqrt( (u32)val ) << 8);
    if ( root == 0 ) {
        return 0;
    }
    return (root + fx_div( val, root )) >> 1;       /* one step of Newton's */
}

fx fx_hypot( fx dx, fx dy )
{
    u32 ax = (u32)(dx < 0 ? -dx : dx), ay = (u32)(dy < 0 ? -dy : dy);
    int shift = 0;

    while ( (ax | ay) > 0x7FFFUL ) {
        ax >>= 1;
        ay >>= 1;
        shift++;
    }
    return (fx)(isqrt( ax * ax + ay * ay ) << shift);
}

/* Sines of 0 to 90 degrees, made at the first call by turning a unit
   vector one degree at a time: there is no library to ask. */
static fx  sine[91];
static int sine_made;

static void sine_make( void )
{
    s32 cs = 0x40000000L, sn = 0, next;     /* 2.30 */
    int deg;

    for ( deg = 0; deg <= 90; deg++ ) {
        sine[deg] = (sn + 0x2000) >> 14;
        next = mul_shr( cs, 1073578288L, 30 ) - mul_shr( sn, 18739379L, 30 );
        sn = mul_shr( sn, 1073578288L, 30 ) + mul_shr( cs, 18739379L, 30 );
        cs = next;
    }
    sine[90] = FX_ONE;
    sine_made = 1;
}

fx fx_sin( int deg )
{
    if ( !sine_made ) {
        sine_make();
    }
    deg %= 360;
    if ( deg < 0 ) {
        deg += 360;
    }
    if ( deg <= 90 ) {
        return sine[deg];
    }
    if ( deg <= 180 ) {
        return sine[180 - deg];
    }
    if ( deg <= 270 ) {
        return -sine[deg - 180];
    }
    return -sine[360 - deg];
}

fx fx_cos( int deg )
{
    return fx_sin( deg + 90 );
}

/* base^expo for a base of 0 or more: 2^(expo * log2 base).  The
   logarithm is a bit at a time by squaring; the power of two is a
   polynomial.  Good to three or four digits, which is a colour. */
fx fx_pow( fx base, fx expo )
{
    s32 whole, frac, mant, prod, result;
    int bit, top;

    if ( expo == FX_ONE ) {
        return base;
    }
    if ( expo == 0 ) {
        return FX_ONE;
    }
    if ( base <= 0 ) {
        return 0;
    }
    top = bit_top( (u32)base );
    whole = top - 16;
    mant = top > 16 ? base >> (top - 16) : base << (16 - top);     /* 1.0 to 2.0 */
    frac = 0;
    for ( bit = 15; bit >= 0; bit-- ) {
        mant = fx_mul( mant, mant );
        if ( mant >= 2 * FX_ONE ) {
            mant >>= 1;
            frac |= 1L << bit;
        }
    }
    /* log2 = whole + frac/65536; the product in 16.16 */
    prod = mul_shr( whole * 65536L + frac, expo, 16 );
    whole = prod >> 16;
    frac = prod & 0xFFFF;
    /* 2^frac = 1 + f(0.69315 + f(0.24023 + f(0.05550 + f*0.00962))) */
    result = 630;
    result = 3637 + fx_mul( result, frac );
    result = 15744 + fx_mul( result, frac );
    result = 45426 + fx_mul( result, frac );
    result = FX_ONE + fx_mul( result, frac );
    if ( whole >= 15 ) {
        return FX_MAX;
    }
    if ( whole >= 0 ) {
        return result << whole;
    }
    if ( whole < -31 ) {
        return 0;
    }
    return result >> -whole;
}

/* ------------------------------------------------------------------ */
/* REAL                                                                */
/* ------------------------------------------------------------------ */

static REAL real_norm( s32 man, s32 exp )
{
    REAL out;
    int top;

    if ( man == 0 ) {
        out.man = 0;
        out.exp = 0;
        return out;
    }
    top = bit_top( man < 0 ? (u32)-man : (u32)man );
    if ( top > 29 ) {
        man >>= top - 29;
        exp += top - 29;
    } else if ( top < 29 ) {
        man <<= 29 - top;
        exp -= 29 - top;
    }
    out.man = man;
    out.exp = exp;
    return out;
}

REAL real_int( s32 val )
{
    return real_norm( val, 0 );
}

REAL real_fx( fx val )
{
    return real_norm( val, -16 );
}

REAL real_neg( REAL val )
{
    val.man = -val.man;
    return val;
}

REAL real_mul( REAL lhs, REAL rhs )
{
    if ( lhs.man == 0 || rhs.man == 0 ) {
        return real_norm( 0, 0 );
    }
    return real_norm( mul_shr( lhs.man, rhs.man, 29 ), lhs.exp + rhs.exp + 29 );
}

/* (num << 30) / den: the quotient of two normalised mantissas is
   between a half and two, so it fits */
static s32 shl30_div( s32 num, s32 den );
#pragma aux shl30_div =     \
    "mov  edx,eax"          \
    "sar  edx,2"            \
    "shl  eax,30"           \
    "idiv ebx"              \
    parm [eax] [ebx] value [eax] modify [edx];

REAL real_div( REAL lhs, REAL rhs )
{
    if ( lhs.man == 0 || rhs.man == 0 ) {
        return real_norm( 0, 0 );
    }
    return real_norm( shl30_div( lhs.man, rhs.man ), lhs.exp - rhs.exp - 30 );
}

REAL real_add( REAL lhs, REAL rhs )
{
    REAL swap;
    s32 diff;

    if ( lhs.man == 0 ) {
        return rhs;
    }
    if ( rhs.man == 0 ) {
        return lhs;
    }
    if ( lhs.exp < rhs.exp ) {
        swap = lhs;
        lhs = rhs;
        rhs = swap;
    }
    diff = lhs.exp - rhs.exp;
    if ( diff > 31 ) {
        return lhs;
    }
    return real_norm( lhs.man + (rhs.man >> diff), lhs.exp );
}

fx real_to_fx( REAL val )
{
    s32 shift = val.exp + 16;

    if ( val.man == 0 || shift < -31 ) {
        return 0;
    }
    if ( shift > 1 ) {
        return val.man < 0 ? -FX_MAX : FX_MAX;
    }
    if ( shift >= 0 ) {
        return val.man << shift;
    }
    return (val.man + (1L << (-shift - 1))) >> -shift;
}

s32 real_to_int( REAL val )
{
    if ( val.man == 0 || val.exp < -31 ) {
        return 0;
    }
    if ( val.exp > 1 ) {
        return val.man < 0 ? -FX_MAX : FX_MAX;
    }
    if ( val.exp >= 0 ) {
        return val.man << val.exp;
    }
    if ( val.man < 0 ) {
        return -((-val.man) >> -val.exp);
    }
    return val.man >> -val.exp;
}

REAL real_dec( u32 digits, int scale )
{
    REAL val = real_int( (s32)digits ), ten = real_int( 10 );

    for ( ; scale > 0; scale-- ) {
        val = real_div( val, ten );
    }
    for ( ; scale < 0; scale++ ) {
        val = real_mul( val, ten );
    }
    return val;
}

/* ------------------------------------------------------------------ */
/* matrices                                                            */
/* ------------------------------------------------------------------ */

void rmat_identity( RMAT *mat )
{
    REAL one = real_int( 1 ), zero = real_int( 0 );

    rmat_set( mat, one, zero, zero, one, zero, zero );
}

void rmat_set( RMAT *mat, REAL ma, REAL mb, REAL mc, REAL md, REAL me, REAL mf )
{
    mat->m[0] = ma;
    mat->m[1] = mb;
    mat->m[2] = mc;
    mat->m[3] = md;
    mat->m[4] = me;
    mat->m[5] = mf;
}

#define RM( lhs, rhs )  real_mul( (lhs), (rhs) )

void rmat_mul( RMAT *out, const RMAT *first, const RMAT *then )
{
    RMAT res;
    const REAL *fm = first->m, *tm = then->m;

    res.m[0] = real_add( RM( fm[0], tm[0] ), RM( fm[1], tm[2] ) );
    res.m[1] = real_add( RM( fm[0], tm[1] ), RM( fm[1], tm[3] ) );
    res.m[2] = real_add( RM( fm[2], tm[0] ), RM( fm[3], tm[2] ) );
    res.m[3] = real_add( RM( fm[2], tm[1] ), RM( fm[3], tm[3] ) );
    res.m[4] = real_add( real_add( RM( fm[4], tm[0] ), RM( fm[5], tm[2] ) ), tm[4] );
    res.m[5] = real_add( real_add( RM( fm[4], tm[1] ), RM( fm[5], tm[3] ) ), tm[5] );
    *out = res;
}

int rmat_invert( RMAT *out, const RMAT *mat )
{
    RMAT res;
    const REAL *mm = mat->m;
    REAL det = real_add( RM( mm[0], mm[3] ), real_neg( RM( mm[1], mm[2] ) ) );

    if ( det.man == 0 ) {
        return 0;
    }
    res.m[0] = real_div( mm[3], det );
    res.m[1] = real_neg( real_div( mm[1], det ) );
    res.m[2] = real_neg( real_div( mm[2], det ) );
    res.m[3] = real_div( mm[0], det );
    res.m[4] = real_div( real_add( RM( mm[2], mm[5] ), real_neg( RM( mm[3], mm[4] ) ) ), det );
    res.m[5] = real_div( real_add( RM( mm[1], mm[4] ), real_neg( RM( mm[0], mm[5] ) ) ), det );
    *out = res;
    return 1;
}

void rmat_to_fmat( FMAT *out, const RMAT *mat )
{
    out->a = real_to_fx( mat->m[0] );
    out->b = real_to_fx( mat->m[1] );
    out->c = real_to_fx( mat->m[2] );
    out->d = real_to_fx( mat->m[3] );
    out->e = real_to_fx( mat->m[4] );
    out->f = real_to_fx( mat->m[5] );
}

void rmat_point( const RMAT *mat, REAL px, REAL py, fx *ox, fx *oy )
{
    const REAL *mm = mat->m;

    *ox = real_to_fx( real_add( real_add( RM( mm[0], px ), RM( mm[2], py ) ), mm[4] ) );
    *oy = real_to_fx( real_add( real_add( RM( mm[1], px ), RM( mm[3], py ) ), mm[5] ) );
}

void fmat_point( const FMAT *mat, fx px, fx py, fx *ox, fx *oy )
{
    *ox = fx_mul( mat->a, px ) + fx_mul( mat->c, py ) + mat->e;
    *oy = fx_mul( mat->b, px ) + fx_mul( mat->d, py ) + mat->f;
}

/* ------------------------------------------------------------------ */
/* for the calculator functions: logarithms and an arctangent          */
/* ------------------------------------------------------------------ */

/* log2 of a positive number */
fx fx_log2( fx val )
{
    s32 mant, frac = 0;
    int bit, top;

    if ( val <= 0 ) {
        return -FX_MAX;
    }
    top = bit_top( (u32)val );
    mant = top > 16 ? val >> (top - 16) : val << (16 - top);
    for ( bit = 15; bit >= 0; bit-- ) {
        mant = fx_mul( mant, mant );
        if ( mant >= 2 * FX_ONE ) {
            mant >>= 1;
            frac |= 1L << bit;
        }
    }
    return (top - 16) * 65536L + frac;
}

/* the angle of (dx, dy) in degrees, 0 up to 360 */
fx fx_atan2( fx dy, fx dx )
{
    fx ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, ratio, mag, angle;

    if ( ax == 0 && ay == 0 ) {
        return 0;
    }
    ratio = ax >= ay ? fx_div( ay, ax ) : fx_div( ax, ay );
    /* atan z = (pi/4)z - z(z - 1)(0.2447 + 0.0663z), in radians, for 0..1 */
    mag = fx_mul( 51472L, ratio ) -
          fx_mul( fx_mul( ratio, ratio - FX_ONE ), 16037L + fx_mul( 4345L, ratio ) );
    angle = fx_mul( mag, 3754936L );                /* to degrees */
    if ( ay > ax ) {
        angle = 90 * FX_ONE - angle;
    }
    if ( dx < 0 ) {
        angle = 180 * FX_ONE - angle;
    }
    if ( dy < 0 ) {
        angle = 360 * FX_ONE - angle;
    }
    return angle;
}
