/* ==========================================================================
 * ECP384.C - NIST P-384 (secp384r1) ECDSA verify.  C89; MSVC 2.2 + OW 1.9.
 * ==========================================================================
 * Same construction as ECP256.C: field arithmetic via BIGNUM.C (bn_*mod) with
 * modulus p, Jacobian points (one inversion per scalar-mult), a = -3 doubling.
 * Only ecdsa_verify_p384 is exposed (cert-chain signature verification).
 * ========================================================================== */
#include "ecp384.h"
#include "bignum.h"
#include <string.h>

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

static bignum MODP, B, GX, GY, N, MODP2, N2, ONE;
static int    g_init = 0;

static void ec_init(void)
{
    if (g_init) return;
    bn_from_bytes(&MODP, P_BYTES,  48);
    bn_from_bytes(&B,    B_BYTES,  48);
    bn_from_bytes(&GX,   GX_BYTES, 48);
    bn_from_bytes(&GY,   GY_BYTES, 48);
    bn_from_bytes(&N,    N_BYTES,  48);
    bn_copy(&MODP2, &MODP);  MODP2.w[0] -= 2;   /* p low word ...FFFFFFFF, no borrow */
    bn_copy(&N2, &N);        N2.w[0]    -= 2;   /* n low word ...CCC52973, no borrow */
    bn_set_word(&ONE, 1);
    g_init = 1;
}

/* --- field ops mod p --- */
#define fp_add(r,a,b) bn_addmod((r),(a),(b),&MODP)
#define fp_sub(r,a,b) bn_submod((r),(a),(b),&MODP)
#define fp_mul(r,a,b) bn_mulmod((r),(a),(b),&MODP)
static void fp_sqr(bignum *r, const bignum *a) { bn_mulmod(r, a, a, &MODP); }
static void fp_inv(bignum *r, const bignum *a) { bn_modexp(r, a, &MODP2, &MODP); }
static void fp_mul2(bignum *r, const bignum *a) { fp_add(r, a, a); }
static void fp_mul3(bignum *r, const bignum *a) { bignum t; fp_add(&t, a, a); fp_add(r, &t, a); }
static void fp_mul4(bignum *r, const bignum *a) { bignum t; fp_mul2(&t, a); fp_mul2(r, &t); }
static void fp_mul8(bignum *r, const bignum *a) { bignum t; fp_mul4(&t, a); fp_mul2(r, &t); }

typedef struct { bignum X, Y, Z; } jpoint;

static void jp_infinity(jpoint *r) { bn_zero(&r->X); bn_set_word(&r->Y, 1); bn_zero(&r->Z); }
static int  jp_is_inf(const jpoint *p) { return bn_is_zero(&p->Z); }

static void jp_double(jpoint *R, const jpoint *P)
{
    bignum delta, gamma, beta, alpha, t, u, x3, y3, z3;
    if (jp_is_inf(P)) { *R = *P; return; }
    fp_sqr(&delta, &P->Z);
    fp_sqr(&gamma, &P->Y);
    fp_mul(&beta, &P->X, &gamma);
    fp_sub(&t, &P->X, &delta);
    fp_add(&u, &P->X, &delta);
    fp_mul(&alpha, &t, &u);
    fp_mul3(&alpha, &alpha);
    fp_sqr(&x3, &alpha);
    fp_mul8(&t, &beta);
    fp_sub(&x3, &x3, &t);
    fp_add(&z3, &P->Y, &P->Z);
    fp_sqr(&z3, &z3);
    fp_sub(&z3, &z3, &gamma);
    fp_sub(&z3, &z3, &delta);
    fp_mul4(&t, &beta);
    fp_sub(&t, &t, &x3);
    fp_mul(&y3, &alpha, &t);
    fp_sqr(&t, &gamma);
    fp_mul8(&t, &t);
    fp_sub(&y3, &y3, &t);
    bn_copy(&R->X, &x3); bn_copy(&R->Y, &y3); bn_copy(&R->Z, &z3);
}

static void jp_add(jpoint *R, const jpoint *P, const jpoint *Q)
{
    bignum z1z1, z2z2, u1, u2, s1, s2, h, i, j, r, v, t, t2, x3, y3, z3;
    if (jp_is_inf(P)) { *R = *Q; return; }
    if (jp_is_inf(Q)) { *R = *P; return; }
    fp_sqr(&z1z1, &P->Z);
    fp_sqr(&z2z2, &Q->Z);
    fp_mul(&u1, &P->X, &z2z2);
    fp_mul(&u2, &Q->X, &z1z1);
    fp_mul(&s1, &P->Y, &Q->Z); fp_mul(&s1, &s1, &z2z2);
    fp_mul(&s2, &Q->Y, &P->Z); fp_mul(&s2, &s2, &z1z1);
    fp_sub(&h, &u2, &u1);
    fp_sub(&r, &s2, &s1);
    if (bn_is_zero(&h)) {
        if (bn_is_zero(&r)) { jp_double(R, P); return; }
        jp_infinity(R); return;
    }
    fp_mul2(&t, &h); fp_sqr(&i, &t);
    fp_mul(&j, &h, &i);
    fp_mul2(&r, &r);
    fp_mul(&v, &u1, &i);
    fp_sqr(&x3, &r);
    fp_sub(&x3, &x3, &j);
    fp_mul2(&t, &v);
    fp_sub(&x3, &x3, &t);
    fp_sub(&t, &v, &x3);
    fp_mul(&y3, &r, &t);
    fp_mul(&t2, &s1, &j); fp_mul2(&t2, &t2);
    fp_sub(&y3, &y3, &t2);
    fp_add(&z3, &P->Z, &Q->Z);
    fp_sqr(&z3, &z3);
    fp_sub(&z3, &z3, &z1z1);
    fp_sub(&z3, &z3, &z2z2);
    fp_mul(&z3, &z3, &h);
    bn_copy(&R->X, &x3); bn_copy(&R->Y, &y3); bn_copy(&R->Z, &z3);
}

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

static int on_curve(const bignum *x, const bignum *y)
{
    bignum lhs, rhs, t;
    fp_sqr(&lhs, y);
    fp_sqr(&t, x);
    fp_mul(&rhs, &t, x);
    fp_mul3(&t, x);
    fp_sub(&rhs, &rhs, &t);
    fp_add(&rhs, &rhs, &B);
    return (bn_cmp(&lhs, &rhs) == 0);
}

int ecdsa_verify_p384(const unsigned char pub[97],
                      const unsigned char *hash, int hashlen,
                      const unsigned char *r, int rlen,
                      const unsigned char *s, int slen)
{
    bignum R, S, E, w, u1, u2, qx, qy, rx, ry, v;
    jpoint G, Q, P1, P2, Rp;
    int zlen = hashlen;
    ec_init();

    if (pub[0] != 0x04) return -1;
    if (rlen <= 0 || rlen > 49 || slen <= 0 || slen > 49) return -1;
    if (hashlen <= 0) return -1;
    if (zlen > 48) zlen = 48;                 /* leftmost 384 bits (n is 384-bit) */

    bn_from_bytes(&R, r, rlen);
    bn_from_bytes(&S, s, slen);
    if (bn_is_zero(&R) || bn_cmp(&R, &N) >= 0) return -1;
    if (bn_is_zero(&S) || bn_cmp(&S, &N) >= 0) return -1;

    bn_from_bytes(&E, hash, zlen);            /* z = leftmost bits of the digest */

    bn_modexp(&w, &S, &N2, &N);               /* w = s^-1 mod n */
    bn_mulmod(&u1, &E, &w, &N);
    bn_mulmod(&u2, &R, &w, &N);

    bn_from_bytes(&qx, pub + 1,  48);
    bn_from_bytes(&qy, pub + 49, 48);
    if (bn_cmp(&qx, &MODP) >= 0 || bn_cmp(&qy, &MODP) >= 0) return -1;
    if (!on_curve(&qx, &qy)) return -1;

    bn_copy(&G.X, &GX); bn_copy(&G.Y, &GY); bn_set_word(&G.Z, 1);
    bn_copy(&Q.X, &qx); bn_copy(&Q.Y, &qy); bn_set_word(&Q.Z, 1);

    jp_mul(&P1, &u1, &G);
    jp_mul(&P2, &u2, &Q);
    jp_add(&Rp, &P1, &P2);
    if (jp_is_inf(&Rp)) return -1;
    if (jp_affine(&Rp, &rx, &ry) != 0) return -1;

    bn_mulmod(&v, &rx, &ONE, &N);             /* v = R.x mod n */
    return (bn_cmp(&v, &R) == 0) ? 0 : -1;
}

/* Force the one-off curve-constant setup.  See ecp256_init(). */
void ecp384_init(void) { ec_init(); }
