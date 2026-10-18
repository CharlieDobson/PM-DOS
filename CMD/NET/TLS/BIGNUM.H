/* ==========================================================================
 * BIGNUM.H - Fixed-width unsigned big integers for NETSPLOR's RSA.
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89).
 * ==========================================================================
 * Just enough to do the RSA *public* operation (c = m^e mod n) needed by
 * TLS: encrypt the 48-byte premaster with the server's key, and verify a
 * certificate signature.  Both use small public exponents, so a simple
 * bit-by-bit modular reduction (no Montgomery) is fast enough - we do only
 * ~17 modmuls per operation.  The schoolbook multiply inner loop is the
 * eventual hand-asm target (after AES); this C version is the KAT reference.
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

/* Convenience + modular arithmetic (used by the P-256 curve, ECP256.C). */
int  bn_is_zero(const bignum *a);
int  bn_bit(const bignum *a, int i);             /* bit i (0 = lsb)         */
void bn_copy(bignum *dst, const bignum *src);
void bn_addmod(bignum *out, const bignum *a, const bignum *b, const bignum *m);
void bn_submod(bignum *out, const bignum *a, const bignum *b, const bignum *m);
void bn_mulmod(bignum *out, const bignum *a, const bignum *b, const bignum *m);

#endif /* NETSPLOR_BIGNUM_H */
