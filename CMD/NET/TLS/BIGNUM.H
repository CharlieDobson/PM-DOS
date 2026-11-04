/* ==========================================================================
 * BIGNUM.H - Fixed-width unsigned big integers for NETSPLOR's RSA.
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89).
 * ==========================================================================
 * Just enough to do the RSA *public* operation (c = m^e mod n) needed by
 * TLS: encrypt the 48-byte premaster with the server's key, and verify a
 * certificate signature - plus the word-array Montgomery arithmetic that
 * the elliptic curves (ECP256.C, ECP384.C) and odd-modulus bn_modexp run on.
 *
 * Numbers are stored little-endian in 32-bit limbs, fixed BN_WORDS long,
 * supporting a modulus up to BN_WORDS*32 bits (4096 by default).
 * ========================================================================== */
#ifndef NETSPLOR_BIGNUM_H
#define NETSPLOR_BIGNUM_H

#ifndef NST_U32_DEFINED
#define NST_U32_DEFINED
typedef unsigned int u32;           /* 32-bit on all Win32 targets */
#endif

#define BN_WORDS  128               /* up to 4096-bit modulus */

typedef struct {
    u32 w[BN_WORDS];                /* w[0] = least-significant limb */
} bignum;

void bn_zero(bignum *a);
void bn_set_word(bignum *a, u32 v);
int  bn_cmp(const bignum *a, const bignum *b);   /* -1 / 0 / 1 */
int  bn_bitlen(const bignum *a);                 /* index of highest set bit +1 */

/* Big-endian byte <-> bignum (len bytes; len <= BN_WORDS*4). */
void bn_from_bytes(bignum *a, const unsigned char *bytes, int len);
void bn_to_bytes(const bignum *a, unsigned char *bytes, int len);

/* out = base^exp mod mod   (mod must be nonzero). */
void bn_modexp(bignum *out, const bignum *base,
               const bignum *exp, const bignum *mod);

/* Convenience + modular arithmetic (used by the curves, ECP256.C / ECP384.C,
   for their scalars mod the group order). */
int  bn_is_zero(const bignum *a);
int  bn_bit(const bignum *a, int i);             /* bit i (0 = lsb)         */
void bn_copy(bignum *dst, const bignum *src);
void bn_addmod(bignum *out, const bignum *a, const bignum *b, const bignum *m);
void bn_submod(bignum *out, const bignum *a, const bignum *b, const bignum *m);
void bn_mulmod(bignum *out, const bignum *a, const bignum *b, const bignum *m);
void bn_mod(bignum *out, const bignum *a, const bignum *m);   /* out = a mod m */

/* Word arrays: nw 32-bit words (up to BN_WORDS), w[0] least significant.
   The curves keep their coordinates this way, in Montgomery form (x R mod p,
   R = 2^(32 nw)), and bn_modexp uses the same multiply for odd moduli. */
void bn_w_from_bytes(u32 *w, int nw, const unsigned char *bytes, int len);
void bn_w_to_bytes(const u32 *w, int nw, unsigned char *bytes, int len);
int  bn_w_cmp(const u32 *a, const u32 *b, int nw);           /* -1 / 0 / 1 */
int  bn_w_is_zero(const u32 *a, int nw);
/* r = a + b or a - b mod m, for a, b < m; r may be a or b. */
void bn_w_addmod(u32 *r, const u32 *a, const u32 *b, const u32 *m, int nw);
void bn_w_submod(u32 *r, const u32 *a, const u32 *b, const u32 *m, int nw);
/* minv = -m^-1 mod 2^32 (m odd): bn_w_minv(m[0]).  bn_w_mont: r = a b / R
   mod m, for a, b < m; r may be a or b.  bn_w_mont_pow: r = a^e over e's
   low ebits bits, a and one (R mod m) in Montgomery form; r may be a. */
u32  bn_w_minv(u32 m0);
void bn_w_mont(u32 *r, const u32 *a, const u32 *b, const u32 *m, int nw, u32 minv);
void bn_w_mont_pow(u32 *r, const u32 *a, const u32 *e, int ebits,
                   const u32 *m, int nw, u32 minv, const u32 *one);

#endif /* NETSPLOR_BIGNUM_H */
