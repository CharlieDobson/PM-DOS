/* ==========================================================================
 * ECP384.C - NIST P-384 (secp384r1) ECDSA verify + ECDH.  C89; MSVC 2.2 + OW 1.9.
 * ==========================================================================
 * Same construction as ECP256.C: coordinates are 12 words in Montgomery form
 * (R = 2^384) multiplied by bn_w_mont, Jacobian points (one inversion per
 * scalar-mult), a = -3 doubling, and 4-bit windows.  ecdsa_verify_p384
 * checks certificate and TLS signatures; ecp384_keygen and ecp384_ecdh
 * serve a server that will do ECDHE only on P-384.
 * ========================================================================== */
#include "ecp384.h"
#include "bignum.h"
#include "rng.h"
#include <string.h>

#define NW 12                               /* words in a coordinate */

/* secp384r1 domain parameters (big-endian, 48 bytes each). */
static const unsigned char P_BYTES[48] = {
 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
 0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF };
static const unsigned char B_BYTES[48] = {
 0xB3,0x31,0x2F,0xA7,0xE2,0x3E,0xE7,0xE4,0x98,0x8E,0x05,0x6B,0xE3,0xF8,0x2D,0x19,
 0x18,0x1D,0x9C,0x6E,0xFE,0x81,0x41,0x12,0x03,0x14,0x08,0x8F,0x50,0x13,0x87,0x5A,
 0xC6,0x56,0x39,0x8D,0x8A,0x2E,0xD1,0x9D,0x2A,0x85,0xC8,0xED,0xD3,0xEC,0x2A,0xEF };
static const unsigned char GX_BYTES[48] = {
 0xAA,0x87,0xCA,0x22,0xBE,0x8B,0x05,0x37,0x8E,0xB1,0xC7,0x1E,0xF3,0x20,0xAD,0x74,
 0x6E,0x1D,0x3B,0x62,0x8B,0xA7,0x9B,0x98,0x59,0xF7,0x41,0xE0,0x82,0x54,0x2A,0x38,
 0x55,0x02,0xF2,0x5D,0xBF,0x55,0x29,0x6C,0x3A,0x54,0x5E,0x38,0x72,0x76,0x0A,0xB7 };
static const unsigned char GY_BYTES[48] = {
 0x36,0x17,0xDE,0x4A,0x96,0x26,0x2C,0x6F,0x5D,0x9E,0x98,0xBF,0x92,0x92,0xDC,0x29,
 0xF8,0xF4,0x1D,0xBD,0x28,0x9A,0x14,0x7C,0xE9,0xDA,0x31,0x13,0xB5,0xF0,0xB8,0xC0,
 0x0A,0x60,0xB1,0xCE,0x1D,0x7E,0x81,0x9D,0x7A,0x43,0x1D,0x7C,0x90,0xEA,0x0E,0x5F };
static const unsigned char N_BYTES[48] = {
 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xC7,0x63,0x4D,0x81,0xF4,0x37,0x2D,0xDF,
 0x58,0x1A,0x0D,0xB2,0x48,0xB0,0xA7,0x7A,0xEC,0xEC,0x19,0x6A,0xCC,0xC5,0x29,0x73 };

static u32    MODP[NW], MODP2[NW], PINV;    /* p, p-2 (the inverse's exponent), -p^-1 mod 2^32 */
static u32    RR[NW], RONE[NW], UNIT[NW];   /* R^2 and R mod p, plain 1 */
static u32    BM[NW], GXM[NW], GYM[NW];     /* b and G in Montgomery form */
static bignum N, N2;                        /* the order n, and n-2 (inverse mod n) */
static int    g_init = 0;

/* --- field ops mod p (Montgomery form) ----------------------------------- */
#define fp_mul( r, a, b )   bn_w_mont( (r), (a), (b), MODP, NW, PINV )
#define fp_add( r, a, b )   bn_w_addmod( (r), (a), (b), MODP, NW )
#define fp_sub( r, a, b )   bn_w_submod( (r), (a), (b), MODP, NW )
#define fp_copy( r, a )     memcpy( (r), (a), NW * sizeof( u32 ) )
static void fp_sqr( u32 *r, const u32 *a ) { fp_mul( r, a, a ); }
static void fp_inv( u32 *r, const u32 *a ) { bn_w_mont_pow( r, a, MODP2, 32 * NW, MODP, NW, PINV, RONE ); }
static void fp_mul2( u32 *r, const u32 *a ) { fp_add( r, a, a ); }
static void fp_mul3( u32 *r, const u32 *a ) { u32 t[NW]; fp_add( t, a, a ); fp_add( r, t, a ); }
static void fp_mul4( u32 *r, const u32 *a ) { u32 t[NW]; fp_mul2( t, a ); fp_mul2( r, t ); }
static void fp_mul8( u32 *r, const u32 *a ) { u32 t[NW]; fp_mul4( t, a ); fp_mul2( r, t ); }
static void to_mont( u32 *r, const u32 *a ) { fp_mul( r, a, RR ); }
static void from_mont( u32 *r, const u32 *a ) { fp_mul( r, a, UNIT ); }

static void ec_init( void )
{
    bignum t, p;
    u32 plain[NW];
    if ( g_init ) return;
    bn_w_from_bytes( MODP, NW, P_BYTES, 48 );
    fp_copy( MODP2, MODP );
    MODP2[0] -= 2;                          /* p low word is ...FFFFFFFF, no borrow */
    PINV = bn_w_minv( MODP[0] );
    bn_from_bytes( &p, P_BYTES, 48 );
    bn_zero( &t );
    t.w[2 * NW] = 1;                        /* R^2 = 2^768 */
    bn_mod( &t, &t, &p );
    memcpy( RR, t.w, sizeof( RR ) );
    memset( UNIT, 0, sizeof( UNIT ) );
    UNIT[0] = 1;
    from_mont( RONE, RR );                  /* R^2 / R = R */
    bn_w_from_bytes( plain, NW, B_BYTES, 48 );  to_mont( BM, plain );
    bn_w_from_bytes( plain, NW, GX_BYTES, 48 ); to_mont( GXM, plain );
    bn_w_from_bytes( plain, NW, GY_BYTES, 48 ); to_mont( GYM, plain );
    bn_from_bytes( &N, N_BYTES, 48 );
    bn_copy( &N2, &N );
    N2.w[0] -= 2;                           /* n low word is ...CCC52973, no borrow */
    g_init = 1;
}

/* --- Jacobian point (Z == 0 means point at infinity) --------------------- */
typedef struct { u32 X[NW], Y[NW], Z[NW]; } jpoint;

static void jp_infinity( jpoint *r ) { memset( r->X, 0, sizeof( r->X ) ); fp_copy( r->Y, RONE ); memset( r->Z, 0, sizeof( r->Z ) ); }
static int  jp_is_inf( const jpoint *p ) { return bn_w_is_zero( p->Z, NW ); }

/* R = 2*P  (a = -3 doubling; R may alias P) */
static void jp_double( jpoint *R, const jpoint *P )
{
    u32 delta[NW], gamma[NW], beta[NW], alpha[NW], t[NW], u[NW], x3[NW], y3[NW], z3[NW];

    if ( jp_is_inf( P ) ) { *R = *P; return; }

    fp_sqr( delta, P->Z );                  /* delta = Z^2       */
    fp_sqr( gamma, P->Y );                  /* gamma = Y^2       */
    fp_mul( beta, P->X, gamma );            /* beta  = X*gamma   */

    fp_sub( t, P->X, delta );               /* X - delta         */
    fp_add( u, P->X, delta );               /* X + delta         */
    fp_mul( alpha, t, u );                  /* (X-d)(X+d)        */
    fp_mul3( alpha, alpha );                /* alpha = 3*(...)   */

    fp_sqr( x3, alpha );                    /* alpha^2           */
    fp_mul8( t, beta );                     /* 8*beta            */
    fp_sub( x3, x3, t );                    /* X3 = alpha^2-8beta*/

    fp_add( z3, P->Y, P->Z );               /* Y+Z               */
    fp_sqr( z3, z3 );                       /* (Y+Z)^2           */
    fp_sub( z3, z3, gamma );
    fp_sub( z3, z3, delta );                /* Z3                */

    fp_mul4( t, beta );                     /* 4*beta            */
    fp_sub( t, t, x3 );                     /* 4beta - X3        */
    fp_mul( y3, alpha, t );                 /* alpha*(4beta-X3)  */
    fp_sqr( t, gamma );                     /* gamma^2           */
    fp_mul8( t, t );                        /* 8*gamma^2         */
    fp_sub( y3, y3, t );                    /* Y3                */

    fp_copy( R->X, x3 ); fp_copy( R->Y, y3 ); fp_copy( R->Z, z3 );
}

/* R = P + Q  (Jacobian; R must not alias P or Q) */
static void jp_add( jpoint *R, const jpoint *P, const jpoint *Q )
{
    u32 z1z1[NW], z2z2[NW], u1[NW], u2[NW], s1[NW], s2[NW], h[NW], i[NW], j[NW];
    u32 r[NW], v[NW], t[NW], t2[NW], x3[NW], y3[NW], z3[NW];

    if ( jp_is_inf( P ) ) { *R = *Q; return; }
    if ( jp_is_inf( Q ) ) { *R = *P; return; }

    fp_sqr( z1z1, P->Z );
    fp_sqr( z2z2, Q->Z );
    fp_mul( u1, P->X, z2z2 );               /* U1 = X1*Z2Z2 */
    fp_mul( u2, Q->X, z1z1 );               /* U2 = X2*Z1Z1 */
    fp_mul( s1, P->Y, Q->Z ); fp_mul( s1, s1, z2z2 );   /* S1=Y1*Z2*Z2Z2 */
    fp_mul( s2, Q->Y, P->Z ); fp_mul( s2, s2, z1z1 );   /* S2=Y2*Z1*Z1Z1 */

    fp_sub( h, u2, u1 );                    /* H = U2-U1 */
    fp_sub( r, s2, s1 );                    /* r0 = S2-S1 (doubled below) */

    if ( bn_w_is_zero( h, NW ) ) {          /* same x */
        if ( bn_w_is_zero( r, NW ) ) { jp_double( R, P ); return; }    /* P == Q */
        jp_infinity( R ); return;                                       /* P == -Q */
    }

    fp_mul2( t, h ); fp_sqr( i, t );        /* I = (2H)^2 */
    fp_mul( j, h, i );                      /* J = H*I    */
    fp_mul2( r, r );                        /* r = 2*(S2-S1) */
    fp_mul( v, u1, i );                     /* V = U1*I   */

    fp_sqr( x3, r );                        /* r^2 */
    fp_sub( x3, x3, j );
    fp_mul2( t, v );
    fp_sub( x3, x3, t );                    /* X3 = r^2 - J - 2V */

    fp_sub( t, v, x3 );
    fp_mul( y3, r, t );                     /* r*(V-X3) */
    fp_mul( t2, s1, j ); fp_mul2( t2, t2 ); /* 2*S1*J */
    fp_sub( y3, y3, t2 );                   /* Y3 */

    fp_add( z3, P->Z, Q->Z );
    fp_sqr( z3, z3 );
    fp_sub( z3, z3, z1z1 );
    fp_sub( z3, z3, z2z2 );
    fp_mul( z3, z3, h );                    /* Z3 = ((Z1+Z2)^2-Z1Z1-Z2Z2)*H */

    fp_copy( R->X, x3 ); fp_copy( R->Y, y3 ); fp_copy( R->Z, z3 );
}

/* R = k*P, k plain (not Montgomery), NW words: four doublings, then one
   add from the table, per 4 bits of k, most significant first. */
static void jp_mul( jpoint *R, const u32 *k, const jpoint *P )
{
    jpoint tab[16], acc, tmp;
    int i, digit, started = 0;

    jp_infinity( &tab[0] );
    tab[1] = *P;
    jp_double( &tab[2], P );
    for ( i = 3; i < 16; i++ ) jp_add( &tab[i], &tab[i - 1], P );
    jp_infinity( &acc );
    for ( i = NW * 8 - 1; i >= 0; i-- ) {
        if ( started ) {
            jp_double( &acc, &acc ); jp_double( &acc, &acc );
            jp_double( &acc, &acc ); jp_double( &acc, &acc );
        }
        digit = (int)( ( k[i >> 3] >> ( 4 * ( i & 7 ) ) ) & 0xf );
        if ( digit ) {
            jp_add( &tmp, &acc, &tab[digit] );
            acc = tmp;
            started = 1;
        }
    }
    *R = acc;
}

/* Convert Jacobian R to affine (x,y), plain (out of Montgomery form).
   Returns -1 if R is the infinity. */
static int jp_affine( const jpoint *R, u32 *x, u32 *y )
{
    u32 zi[NW], zi2[NW], zi3[NW];
    if ( jp_is_inf( R ) ) return -1;
    fp_inv( zi, R->Z );
    fp_sqr( zi2, zi );
    fp_mul( zi3, zi2, zi );
    fp_mul( x, R->X, zi2 );
    fp_mul( y, R->Y, zi3 );
    from_mont( x, x );
    from_mont( y, y );
    return 0;
}

/* Is (x,y), in Montgomery form, on the curve y^2 = x^3 - 3x + b (mod p)? */
static int on_curve( const u32 *x, const u32 *y )
{
    u32 lhs[NW], rhs[NW], t[NW];
    fp_sqr( lhs, y );                       /* y^2 */
    fp_sqr( t, x );                         /* x^2 */
    fp_mul( rhs, t, x );                    /* x^3 */
    fp_mul3( t, x );                        /* 3x  */
    fp_sub( rhs, rhs, t );                  /* x^3 - 3x */
    fp_add( rhs, rhs, BM );                 /* + b */
    return bn_w_cmp( lhs, rhs, NW ) == 0;
}

/* A SEC1 uncompressed point as a Jacobian point (Z = 1), after checking that
   the coordinates are < p and the point is on the curve.  0 / -1. */
static int load_point( jpoint *Q, const unsigned char pub[97] )
{
    u32 x[NW], y[NW];
    if ( pub[0] != 0x04 ) return -1;
    bn_w_from_bytes( x, NW, pub + 1, 48 );
    bn_w_from_bytes( y, NW, pub + 49, 48 );
    if ( bn_w_cmp( x, MODP, NW ) >= 0 || bn_w_cmp( y, MODP, NW ) >= 0 ) return -1;
    if ( bn_w_is_zero( x, NW ) && bn_w_is_zero( y, NW ) ) return -1;
    to_mont( Q->X, x );
    to_mont( Q->Y, y );
    fp_copy( Q->Z, RONE );
    return on_curve( Q->X, Q->Y ) ? 0 : -1;
}

static void base_point( jpoint *G )
{
    fp_copy( G->X, GXM ); fp_copy( G->Y, GYM ); fp_copy( G->Z, RONE );
}

/* --- public API ---------------------------------------------------------- */

int ecdsa_verify_p384( const unsigned char pub[97],
                       const unsigned char *hash, int hashlen,
                       const unsigned char *r, int rlen,
                       const unsigned char *s, int slen )
{
    bignum R, S, E, w, u1, u2, rx, v;
    jpoint G, Q, P1, P2, Rp;
    u32 x[NW], y[NW];
    int zlen = hashlen;
    ec_init();

    if ( rlen <= 0 || rlen > 49 || slen <= 0 || slen > 49 ) return -1;
    if ( hashlen <= 0 ) return -1;
    if ( zlen > 48 ) zlen = 48;             /* leftmost 384 bits (n is 384-bit) */

    bn_from_bytes( &R, r, rlen );
    bn_from_bytes( &S, s, slen );
    if ( bn_is_zero( &R ) || bn_cmp( &R, &N ) >= 0 ) return -1;    /* 1 <= r < n */
    if ( bn_is_zero( &S ) || bn_cmp( &S, &N ) >= 0 ) return -1;    /* 1 <= s < n */

    bn_from_bytes( &E, hash, zlen );        /* z = leftmost bits of the digest */

    bn_modexp( &w, &S, &N2, &N );           /* w = s^-1 mod n */
    bn_mulmod( &u1, &E, &w, &N );           /* u1 = z*w mod n */
    bn_mulmod( &u2, &R, &w, &N );           /* u2 = r*w mod n */

    if ( load_point( &Q, pub ) != 0 ) return -1;
    base_point( &G );

    jp_mul( &P1, u1.w, &G );                /* u1*G */
    jp_mul( &P2, u2.w, &Q );                /* u2*Q */
    jp_add( &Rp, &P1, &P2 );                /* R = u1*G + u2*Q */
    if ( jp_affine( &Rp, x, y ) != 0 ) return -1;

    bn_zero( &rx );
    memcpy( rx.w, x, sizeof( x ) );
    bn_mod( &v, &rx, &N );                  /* v = R.x mod n */
    return ( bn_cmp( &v, &R ) == 0 ) ? 0 : -1;
}

/* pub <- d*G for the private scalar d (made by rng, reduced mod n, nonzero),
   both written big-endian / SEC1 uncompressed.  0 / -1. */
int ecp384_keygen( nst_rng *rng, unsigned char priv[48], unsigned char pub[97] )
{
    jpoint G, R;
    bignum d;
    u32 x[NW], y[NW];
    int tries;
    ec_init();

    for ( tries = 0; tries < 16; tries++ ) {
        rng_bytes( rng, priv, 48 );
        bn_from_bytes( &d, priv, 48 );
        bn_mod( &d, &d, &N );               /* d mod n */
        if ( !bn_is_zero( &d ) ) break;
    }
    if ( tries >= 16 ) return -1;
    bn_to_bytes( &d, priv, 48 );            /* canonical, reduced private scalar */

    base_point( &G );
    jp_mul( &R, d.w, &G );
    if ( jp_affine( &R, x, y ) != 0 ) return -1;
    pub[0] = 0x04;
    bn_w_to_bytes( x, NW, pub + 1, 48 );
    bn_w_to_bytes( y, NW, pub + 49, 48 );
    return 0;
}

/* out <- the X coordinate of priv * peer, after checking peer is a point on
   the curve.  0 / -1. */
int ecp384_ecdh( const unsigned char priv[48], const unsigned char peer[97],
                 unsigned char out[48] )
{
    jpoint Q, R;
    bignum d;
    u32 sx[NW], sy[NW];
    ec_init();

    if ( load_point( &Q, peer ) != 0 ) return -1;

    bn_from_bytes( &d, priv, 48 );
    bn_mod( &d, &d, &N );
    if ( bn_is_zero( &d ) ) return -1;

    jp_mul( &R, d.w, &Q );
    if ( jp_affine( &R, sx, sy ) != 0 ) return -1;
    bn_w_to_bytes( sx, NW, out, 48 );
    return 0;
}

/* Force the one-off curve-constant setup.  See ecp256_init(). */
void ecp384_init( void ) { ec_init(); }
