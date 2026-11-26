/* ==========================================================================
 * GCM.H - AES-GCM AEAD (the TLS 1.2 GCM suites of RFC 5288, and TLS 1.3's).
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89).  Built on AES.C.
 * ==========================================================================
 * 96-bit (12-byte) IV only, as TLS uses.  rk is the expanded AES key
 * (aes128_expand_key or aes256_expand_key); H is the GHASH subkey =
 * AES_enc(key, 0^128).  'plain' and 'cipher' may be the same buffer.
 * ========================================================================== */
#ifndef NETSPLOR_GCM_H
#define NETSPLOR_GCM_H

#include <stdint.h>

/* 'nr' is the AES round count (10 = AES-128, 14 = AES-256); 'rk' is the
 * matching expanded schedule, so GCM works with either key size. */

/* Precompute the GHASH subkey H = AES-encrypt(rk, all-zero block). */
void gcm_calc_h(const uint8_t *rk, int nr, uint8_t H[16]);

/* Authenticated encryption.  cipher[] gets plen bytes; tag[] gets 16. */
void gcm_encrypt(const uint8_t *rk, int nr, const uint8_t H[16],
                 const uint8_t iv[12],
                 const uint8_t *aad, int aadlen,
                 const uint8_t *plain, int plen,
                 uint8_t *cipher, uint8_t tag[16]);

/* Authenticated decryption.  Returns 0 and writes plain[] if the tag verifies,
   -1 (and leaves plain[] undefined) if authentication fails. */
int  gcm_decrypt(const uint8_t *rk, int nr, const uint8_t H[16],
                 const uint8_t iv[12],
                 const uint8_t *aad, int aadlen,
                 const uint8_t *cipher, int clen,
                 const uint8_t tag[16], uint8_t *plain);

#endif /* NETSPLOR_GCM_H */
