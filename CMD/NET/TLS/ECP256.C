/* ==========================================================================
 * ECP256.C - NIST P-256 ECDH (see ECP256.H).  C89; MSVC 2.2 + OW 1.9 clean.
 * ==========================================================================
 * Field arithmetic is delegated to BIGNUM.C (bn_*mod) with modulus p.  Points
 * use Jacobian coordinates so a scalar multiply needs only ONE field inversion
 * (at the final affine conversion); doubling uses the a = -3 shortcut.
 * ========================================================================== */
#include "ecp256.h"
#include "bignum.h"
#include <string.h>

/* Curve constants (big-endian), converted to bignum on first use. */
static const unsigned char P_BYTES[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const unsigned char B_BYTES[32] = {
    0x5A,0xC6,0x35,0xD8,0xAA,0x3A,0x93,0xE7,0xB3,0xEB,0xBD,0x55,0x76,0x98,0x86,0xBC,
    0x65,0x1D,0x06,0xB0,0xCC,0x53,0xB0,0xF6,0x3B,0xCE,0x3C,0x3E,0x27,0xD2,0x60,0x4B };
static const unsigned char GX_BYTES[32] = {
    0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,
    0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96 };
static const unsigned char GY_BYTES[32] = {
    0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,
    0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5 };
static const unsigned char N_BYTES[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x51 };

static bignum MODP, B, GX, GY, N, MODP2; /* MODP2 = p-2 (for the field inverse) */
static bignum N2;                        /* n-2 (for inverse mod the order)     */
static bignum ONE;
static int    g_init = 0;

static void ec_init(void)
{
    if (g_init) return;
    bn_from_bytes(&MODP, P_BYTES,  32);
    bn_from_bytes(&B,    B_BYTES,  32);
    bn_from_bytes(&GX,   GX_BYTES, 32);
    bn_from_bytes(&GY,   GY_BYTES, 32);
    bn_from_bytes(&N,    N_BYTES,  32);
    bn_copy(&MODP2, &MODP);
    MODP2.w[0] -= 2;                     /* p low word is ...FFFFFFFF, no borrow */
    bn_copy(&N2, &N);
    N2.w[0] -= 2;                        /* n low word is ...FC632551, no borrow */
    bn_set_word(&ONE, 1);
    g_init = 1;
}

/* --- field ops mod p ----------------------------------------------------- */
#define fp_add(r,a,b) bn_addmod((r),(a),(b),&MODP)
#define fp_sub(r,a,b) bn_submod((r),(a),(b),&MODP)
#define fp_mul(r,a,b) bn_mulmod((r),(a),(b),&MODP)
static void fp_sqr(bignum *r, const bignum *a) { bn_mulmod(r, a, a, &MODP); }
static void fp_inv(bignum *r, const bignum *a) { bn_modexp(r, a, &MODP2, &MODP); }
static void fp_mul2(bignum *r, const bignum *a) { fp_add(r, a, a); }
static void fp_mul3(bignum *r, const bignum *a) { bignum t; fp_add(&t, a, a); fp_add(r, &t, a); }
static void fp_mul4(bignum *r, const bignum *a) { bignum t; fp_mul2(&t, a); fp_mul2(r, &t); }
static void fp_mul8(bignum *r, const bignum *a) { bignum t; fp_mul4(&t, a); fp_mul2(r, &t); }

/* --- Jacobian point (Z == 0 means point at infinity) --------------------- */
typedef struct { bignum X, Y, Z; } jpoint;

static void jp_infinity(jpoint *r) { bn_zero(&r->X); bn_set_word(&r->Y, 1); bn_zero(&r->Z); }
static int  jp_is_inf(const jpoint *p) { return bn_is_zero(&p->Z); }

/* R = 2*P  (a = -3 doubling; R may alias P) */
static void jp_double(jpoint *R, const jpoint *P)
{
    bignum delta, gamma, beta, alpha, t, u, x3, y3, z3;

    if (jp_is_inf(P)) { *R = *P; return; }

    fp_sqr(&delta, &P->Z);              /* delta = Z^2       */
    fp_sqr(&gamma, &P->Y);              /* gamma = Y^2       */
    fp_mul(&beta, &P->X, &gamma);       /* beta  = X*gamma   */

    fp_sub(&t, &P->X, &delta);          /* X - delta         */
    fp_add(&u, &P->X, &delta);          /* X + delta         */
    fp_mul(&alpha, &t, &u);             /* (X-d)(X+d)        */
    fp_mul3(&alpha, &alpha);            /* alpha = 3*(...)   */

    fp_sqr(&x3, &alpha);                /* alpha^2           */
    fp_mul8(&t, &beta);                 /* 8*beta            */
    fp_sub(&x3, &x3, &t);               /* X3 = alpha^2-8beta*/

    fp_add(&z3, &P->Y, &P->Z);          /* Y+Z               */
    fp_sqr(&z3, &z3);                   /* (Y+Z)^2           */
    fp_sub(&z3, &z3, &gamma);
    fp_sub(&z3, &z3, &delta);           /* Z3                */

    fp_mul4(&t, &beta);                 /* 4*beta            */
    fp_sub(&t, &t, &x3);                /* 4beta - X3        */
    fp_mul(&y3, &alpha, &t);            /* alpha*(4beta-X3)  */
    fp_sqr(&t, &gamma);                 /* gamma^2           */
    fp_mul8(&t, &t);                    /* 8*gamma^2         */
    fp_sub(&y3, &y3, &t);               /* Y3                */

    bn_copy(&R->X, &x3); bn_copy(&R->Y, &y3); bn_copy(&R->Z, &z3);
}

/* R = P + Q  (Jacobian; R must not alias P or Q) */
static void jp_add(jpoint *R, const jpoint *P, const jpoint *Q)
{
    bignum z1z1, z2z2, u1, u2, s1, s2, h, i, j, r, v, t, t2, x3, y3, z3;

    if (jp_is_inf(P)) { *R = *Q; return; }
    if (jp_is_inf(Q)) { *R = *P; return; }

    fp_sqr(&z1z1, &P->Z);
    fp_sqr(&z2z2, &Q->Z);
    fp_mul(&u1, &P->X, &z2z2);          /* U1 = X1*Z2Z2 */
    fp_mul(&u2, &Q->X, &z1z1);          /* U2 = X2*Z1Z1 */
    fp_mul(&s1, &P->Y, &Q->Z); fp_mul(&s1, &s1, &z2z2);  /* S1=Y1*Z2*Z2Z2 */
    fp_mul(&s2, &Q->Y, &P->Z); fp_mul(&s2, &s2, &z1z1);  /* S2=Y2*Z1*Z1Z1 */

    fp_sub(&h, &u2, &u1);               /* H = U2-U1 */
    fp_sub(&r, &s2, &s1);               /* r0 = S2-S1 (doubled below) */

    if (bn_is_zero(&h)) {               /* same x */
        if (bn_is_zero(&r)) { jp_double(R, P); return; }   /* P == Q */
        jp_infinity(R); return;                            /* P == -Q */
    }

    fp_mul2(&t, &h); fp_sqr(&i, &t);    /* I = (2H)^2 */
    fp_mul(&j, &h, &i);                 /* J = H*I    */
    fp_mul2(&r, &r);                    /* r = 2*(S2-S1) */
    fp_mul(&v, &u1, &i);                /* V = U1*I   */

    fp_sqr(&x3, &r);                    /* r^2 */
    fp_sub(&x3, &x3, &j);
    fp_mul2(&t, &v);
    fp_sub(&x3, &x3, &t);              /* X3 = r^2 - J - 2V */

    fp_sub(&t, &v, &x3);
    fp_mul(&y3, &r, &t);              /* r*(V-X3) */
    fp_mul(&t2, &s1, &j); fp_mul2(&t2, &t2);   /* 2*S1*J */
    fp_sub(&y3, &y3, &t2);            /* Y3 */

    fp_add(&z3, &P->Z, &Q->Z);
    fp_sqr(&z3, &z3);
    fp_sub(&z3, &z3, &z1z1);
    fp_sub(&z3, &z3, &z2z2);
    fp_mul(&z3, &z3, &h);            /* Z3 = ((Z1+Z2)^2-Z1Z1-Z2Z2)*H */

    bn_copy(&R->X, &x3); bn_copy(&R->Y, &y3); bn_copy(&R->Z, &z3);
}

/* R = k*P  (double-and-add over the bits of k, MSB first) */
static void jp_mul(jpoint *R, const bignum *k, const jpoint *P)
{
    jpoint acc, tmp;
    int bits = bn_bitlen(k), i;
    jp_infinity(&acc);
    for (i = bits - 1; i >= 0; i--) {
        jp_double(&tmp, &acc); acc = tmp;
        if (bn_bit(k, i)) { jp_add(&tmp, &acc, P); acc = tmp; }
    }
    *R = acc;
}

/* Convert Jacobian R to affine (x,y).  Returns -1 if R is the infinity. */
static int jp_affine(const jpoint *R, bignum *x, bignum *y)
{
    bignum zi, zi2, zi3;
    if (jp_is_inf(R)) return -1;
    fp_inv(&zi, &R->Z);
    fp_sqr(&zi2, &zi);
    fp_mul(&zi3, &zi2, &zi);
    fp_mul(x, &R->X, &zi2);
    fp_mul(y, &R->Y, &zi3);
    return 0;
}

/* --- public API ---------------------------------------------------------- */

/* pub <- k*G, written SEC1 uncompressed.  Returns 0 / -1. */
int ecp256_mul_base(const unsigned char k[ECP256_PRIV_LEN],
                    unsigned char pub[ECP256_PUB_LEN])
{
    jpoint G, R;
    bignum d, x, y;
    ec_init();

    bn_from_bytes(&d, k, 32);
    bn_mulmod(&d, &d, &ONE, &N);        /* d mod n */
    if (bn_is_zero(&d)) return -1;

    bn_copy(&G.X, &GX); bn_copy(&G.Y, &GY); bn_set_word(&G.Z, 1);
    jp_mul(&R, &d, &G);
    if (jp_affine(&R, &x, &y) != 0) return -1;

    pub[0] = 0x04;
    bn_to_bytes(&x, pub + 1, 32);
    bn_to_bytes(&y, pub + 33, 32);
    return 0;
}

int ecp256_keygen(nst_rng *rng, unsigned char priv[ECP256_PRIV_LEN],
                  unsigned char pub[ECP256_PUB_LEN])
{
    bignum d;
    int tries;
    ec_init();

    for (tries = 0; tries < 16; tries++) {
        rng_bytes(rng, priv, ECP256_PRIV_LEN);
        bn_from_bytes(&d, priv, 32);
        bn_mulmod(&d, &d, &ONE, &N);    /* d mod n */
        if (!bn_is_zero(&d)) break;
    }
    if (tries >= 16) return -1;
    bn_to_bytes(&d, priv, 32);          /* canonical, reduced private scalar */
    return ecp256_mul_base(priv, pub);
}

/* Is (x,y) on the curve y^2 = x^3 - 3x + b (mod p)? */
static int on_curve(const bignum *x, const bignum *y)
{
    bignum lhs, rhs, t;
    fp_sqr(&lhs, y);                    /* y^2 */
    fp_sqr(&t, x);                      /* x^2 */
    fp_mul(&rhs, &t, x);               /* x^3 */
    fp_mul3(&t, x);                    /* 3x  */
    fp_sub(&rhs, &rhs, &t);            /* x^3 - 3x */
    fp_add(&rhs, &rhs, &B);           /* + b */
    return (bn_cmp(&lhs, &rhs) == 0);
}

int ecp256_ecdh(const unsigned char priv[ECP256_PRIV_LEN],
                const unsigned char peer[ECP256_PUB_LEN],
                unsigned char out[ECP256_COORD_LEN])
{
    jpoint Q, R;
    bignum d, x, y, sx, sy;
    ec_init();

    if (peer[0] != 0x04) return -1;
    bn_from_bytes(&x, peer + 1, 32);
    bn_from_bytes(&y, peer + 33, 32);
    if (bn_cmp(&x, &MODP) >= 0 || bn_cmp(&y, &MODP) >= 0) return -1;
    if (bn_is_zero(&x) && bn_is_zero(&y)) return -1;
    if (!on_curve(&x, &y)) return -1;

    bn_from_bytes(&d, priv, 32);
    bn_mulmod(&d, &d, &ONE, &N);
    if (bn_is_zero(&d)) return -1;

    bn_copy(&Q.X, &x); bn_copy(&Q.Y, &y); bn_set_word(&Q.Z, 1);
    jp_mul(&R, &d, &Q);
    if (jp_affine(&R, &sx, &sy) != 0) return -1;

    bn_to_bytes(&sx, out, 32);
    return 0;
}

int ecdsa_verify_p256(const unsigned char pub[65], const unsigned char hash[32],
                      const unsigned char *r, int rlen,
                      const unsigned char *s, int slen)
{
    bignum R, S, E, w, u1, u2, qx, qy, rx, ry, v;
    jpoint G, Q, P1, P2, Rp;
    ec_init();

    if (pub[0] != 0x04) return -1;
    if (rlen <= 0 || rlen > 33 || slen <= 0 || slen > 33) return -1;

    bn_from_bytes(&R, r, rlen);
    bn_from_bytes(&S, s, slen);
    if (bn_is_zero(&R) || bn_cmp(&R, &N) >= 0) return -1;      /* 1 <= r < n */
    if (bn_is_zero(&S) || bn_cmp(&S, &N) >= 0) return -1;      /* 1 <= s < n */

    bn_from_bytes(&E, hash, 32);        /* z = leftmost 256 bits of the hash */

    bn_modexp(&w, &S, &N2, &N);         /* w = s^-1 mod n */
    bn_mulmod(&u1, &E, &w, &N);         /* u1 = z*w mod n */
    bn_mulmod(&u2, &R, &w, &N);         /* u2 = r*w mod n */

    bn_from_bytes(&qx, pub + 1, 32);
    bn_from_bytes(&qy, pub + 33, 32);
    if (bn_cmp(&qx, &MODP) >= 0 || bn_cmp(&qy, &MODP) >= 0) return -1;
    if (!on_curve(&qx, &qy)) return -1;

    bn_copy(&G.X, &GX); bn_copy(&G.Y, &GY); bn_set_word(&G.Z, 1);
    bn_copy(&Q.X, &qx); bn_copy(&Q.Y, &qy); bn_set_word(&Q.Z, 1);

    jp_mul(&P1, &u1, &G);               /* u1*G */
    jp_mul(&P2, &u2, &Q);               /* u2*Q */
    jp_add(&Rp, &P1, &P2);              /* R = u1*G + u2*Q */
    if (jp_is_inf(&Rp)) return -1;
    if (jp_affine(&Rp, &rx, &ry) != 0) return -1;

    bn_mulmod(&v, &rx, &ONE, &N);       /* v = R.x mod n */
    return (bn_cmp(&v, &R) == 0) ? 0 : -1;
}

/* Force the one-off curve-constant setup.  Exposed so a host (e.g. NSTLS32.DLL
   under Win32s, where DLL data is process-shared) can make initialisation
   happen deterministically at load time instead of inside the first
   handshake.  Idempotent, and cheap after the first call. */
void ecp256_init(void) { ec_init(); }
