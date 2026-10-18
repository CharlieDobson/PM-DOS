/* ==========================================================================
 * GCM.H - AES-128-GCM AEAD for NETSPLOR (RFC 5288 GCM cipher suites).
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89).  Built on AES.C.
 * ==========================================================================
 * 96-bit (12-byte) IV only, as used by TLS 1.2.  rk is the expanded AES-128
 * key (aes128_expand_key); H is the GHASH subkey = AES_enc(key, 0^128).
 * ========================================================================== */
#ifndef NETSPLOR_GCM_H
#define NETSPLOR_GCM_H

/* 'nr' is the AES round count (10 = AES-128, 14 = AES-256); 'rk' is the
 * matching expanded schedule, so GCM works with either key size. */

/* Precompute the GHASH subkey H = AES-encrypt(rk, all-zero block). */
void gcm_calc_h(const unsigned char *rk, int nr, unsigned char H[16]);

/* Authenticated encryption.  cipher[] gets plen bytes; tag[] gets 16. */
void gcm_encrypt(const unsigned char *rk, int nr, const unsigned char H[16],
                 const unsigned char iv[12],
                 const unsigned char *aad, int aadlen,
                 const unsigned char *plain, int plen,
                 unsigned char *cipher, unsigned char tag[16]);

/* Authenticated decryption.  Returns 0 and writes plain[] if the tag verifies,
   -1 (and leaves plain[] undefined) if authentication fails. */
int  gcm_decrypt(const unsigned char *rk, int nr, const unsigned char H[16],
                 const unsigned char iv[12],
                 const unsigned char *aad, int aadlen,
                 const unsigned char *cipher, int clen,
                 const unsigned char tag[16], unsigned char *plain);

#endif /* NETSPLOR_GCM_H */
