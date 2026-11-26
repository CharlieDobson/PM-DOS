/* ==========================================================================
 * SHA.H - SHA-1, SHA-256 and HMAC for NETSPLOR's TLS 1.2 crypto stack.
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89, no OS dependency).
 * ==========================================================================
 * SHA-1  : record MAC (HMAC-SHA1) for TLS_RSA_WITH_AES_128_CBC_SHA, and the
 *          handshake hash where TLS 1.0/1.1 style is needed.
 * SHA-256: the TLS 1.2 PRF (P_SHA256) and the Finished handshake hash.
 * HMAC   : keyed MAC over either hash.
 * ========================================================================== */
#ifndef NETSPLOR_SHA_H
#define NETSPLOR_SHA_H

#define SHA1_DIGEST_LEN     20
#define SHA256_DIGEST_LEN   32
#define SHA_BLOCK_LEN       64      /* both use a 64-byte block */

#include <stdint.h>

typedef struct {
    uint32_t      h[5];
    uint32_t      count[2];         /* 64-bit message length in bits */
    uint8_t       buf[64];
    unsigned int  nbuf;
} SHA1_CTX;

typedef struct {
    uint32_t      h[8];
    uint32_t      count[2];
    uint8_t       buf[64];
    unsigned int  nbuf;
} SHA256_CTX;

/* --- SHA-1 --- */
void sha1_init(SHA1_CTX *c);
void sha1_update(SHA1_CTX *c, const void *data, unsigned int len);
void sha1_final(SHA1_CTX *c, uint8_t out[20]);
void sha1(const void *data, unsigned int len, uint8_t out[20]);

/* --- SHA-256 --- */
void sha256_init(SHA256_CTX *c);
void sha256_update(SHA256_CTX *c, const void *data, unsigned int len);
void sha256_final(SHA256_CTX *c, uint8_t out[32]);
void sha256(const void *data, unsigned int len, uint8_t out[32]);

/* --- HMAC (one-shot) --- */
void hmac_sha1(const uint8_t *key, int keylen,
               const uint8_t *msg, int msglen, uint8_t out[20]);
void hmac_sha256(const uint8_t *key, int keylen,
                 const uint8_t *msg, int msglen, uint8_t out[32]);

#endif /* NETSPLOR_SHA_H */
