/* ==========================================================================
 * GCM.C - AES-128-GCM AEAD (see GCM.H).  C89; MSVC 2.2 + OW 1.9 clean.
 * ==========================================================================
 * GHASH uses the straightforward bitwise GF(2^128) multiply (no tables) with
 * the reduction polynomial R = 0xE1 || 0^120.  Counter mode reuses AES.C's
 * single-block encrypt.  Lengths are TLS-record sized, so 32-bit bit-lengths
 * are sufficient (no 64-bit type needed).
 * ========================================================================== */
#include "gcm.h"
#include "aes.h"
#include <string.h>

/* out = X . H  in GF(2^128) (GCM bit convention: x_0 is the MSB of byte 0). */
static void gf_mult(const unsigned char X[16], const unsigned char H[16],
                    unsigned char out[16])
{
    unsigned char Z[16], V[16];
    int i, j;
    memset(Z, 0, 16);
    memcpy(V, H, 16);
    for (i = 0; i < 128; i++) {
        if (X[i >> 3] & (0x80 >> (i & 7))) {
            for (j = 0; j < 16; j++) Z[j] ^= V[j];
        }
        {
            unsigned char lsb = (unsigned char)(V[15] & 1);
            for (j = 15; j > 0; j--)
                V[j] = (unsigned char)((V[j] >> 1) | (V[j-1] << 7));
            V[0] >>= 1;
            if (lsb) V[0] ^= 0xE1;
        }
    }
    memcpy(out, Z, 16);
}

/* Y = GHASH_H(Y || data), zero-padding a final partial block. */
static void ghash_data(const unsigned char H[16], const unsigned char *data,
                       int len, unsigned char Y[16])
{
    unsigned char blk[16], t[16];
    int i, j, n;
    for (i = 0; i < len; i += 16) {
        n = (len - i >= 16) ? 16 : (len - i);
        memset(blk, 0, 16);
        memcpy(blk, data + i, n);
        for (j = 0; j < 16; j++) Y[j] ^= blk[j];
        gf_mult(Y, H, t);
        memcpy(Y, t, 16);
    }
}

/* CTR mode from an initial counter block icb (incrementing the low 32 bits). */
static void gctr(const unsigned char *rk, int nr, const unsigned char icb[16],
                 const unsigned char *in, int len, unsigned char *out)
{
    unsigned char cb[16], ks[16];
    int i, j, n;
    memcpy(cb, icb, 16);
    for (i = 0; i < len; i += 16) {
        aes_encrypt(rk, nr, cb, ks);
        n = (len - i >= 16) ? 16 : (len - i);
        for (j = 0; j < n; j++) out[i + j] = (unsigned char)(in[i + j] ^ ks[j]);
        for (j = 15; j >= 12; j--) { if (++cb[j] != 0) break; }   /* inc32 */
    }
}

void gcm_calc_h(const unsigned char *rk, int nr, unsigned char H[16])
{
    unsigned char zero[16];
    memset(zero, 0, 16);
    aes_encrypt(rk, nr, zero, H);
}

/* Compute S = GHASH over aad || cipher || len-block, and the tag pad E(J0). */
static void gcm_core(const unsigned char rk[176], const unsigned char H[16],
                     const unsigned char iv[12],
                     const unsigned char *aad, int aadlen,
                     const unsigned char *cipher, int clen,
                     unsigned char S[16], unsigned char J0[16])
{
    unsigned char lenblk[16];
    unsigned long abits = (unsigned long)aadlen * 8;
    unsigned long cbits = (unsigned long)clen * 8;
    int j;

    memcpy(J0, iv, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    memset(S, 0, 16);
    ghash_data(H, aad, aadlen, S);
    ghash_data(H, cipher, clen, S);

    memset(lenblk, 0, 16);                       /* [len(AAD)]64 || [len(C)]64 */
    lenblk[4] = (unsigned char)(abits >> 24); lenblk[5] = (unsigned char)(abits >> 16);
    lenblk[6] = (unsigned char)(abits >> 8);  lenblk[7] = (unsigned char)abits;
    lenblk[12] = (unsigned char)(cbits >> 24); lenblk[13] = (unsigned char)(cbits >> 16);
    lenblk[14] = (unsigned char)(cbits >> 8);  lenblk[15] = (unsigned char)cbits;
    for (j = 0; j < 16; j++) S[j] ^= lenblk[j];
    {
        unsigned char t[16];
        gf_mult(S, H, t);
        memcpy(S, t, 16);
    }
}

void gcm_encrypt(const unsigned char *rk, int nr, const unsigned char H[16],
                 const unsigned char iv[12],
                 const unsigned char *aad, int aadlen,
                 const unsigned char *plain, int plen,
                 unsigned char *cipher, unsigned char tag[16])
{
    unsigned char J0[16], icb[16], S[16], ek[16];
    int j;

    memcpy(J0, iv, 12); J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=1;
    memcpy(icb, J0, 16);
    for (j = 15; j >= 12; j--) { if (++icb[j] != 0) break; }     /* inc32(J0) */
    gctr(rk, nr, icb, plain, plen, cipher);

    gcm_core(rk, H, iv, aad, aadlen, cipher, plen, S, J0);
    aes_encrypt(rk, nr, J0, ek);
    for (j = 0; j < 16; j++) tag[j] = (unsigned char)(S[j] ^ ek[j]);
}

int gcm_decrypt(const unsigned char *rk, int nr, const unsigned char H[16],
                const unsigned char iv[12],
                const unsigned char *aad, int aadlen,
                const unsigned char *cipher, int clen,
                const unsigned char tag[16], unsigned char *plain)
{
    unsigned char J0[16], icb[16], S[16], ek[16], want[16];
    int j, diff = 0;

    gcm_core(rk, H, iv, aad, aadlen, cipher, clen, S, J0);
    aes_encrypt(rk, nr, J0, ek);
    for (j = 0; j < 16; j++) want[j] = (unsigned char)(S[j] ^ ek[j]);

    for (j = 0; j < 16; j++) diff |= (want[j] ^ tag[j]);         /* constant-ish */
    if (diff != 0) return -1;

    memcpy(icb, J0, 16);
    for (j = 15; j >= 12; j--) { if (++icb[j] != 0) break; }
    gctr(rk, nr, icb, cipher, clen, plain);
    return 0;
}
