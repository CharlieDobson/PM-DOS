/* ==========================================================================
 * SHA512.H - SHA-384 (SHA-512 core) for NETSPLOR.  C89, no __int64
 * (64-bit words emulated as two 32-bit halves, matching BIGNUM's style so it
 * builds clean on MSVC 2.2 / Win32s as well as Open Watcom 1.9).
 * ==========================================================================
 * Needed to hash tbsCertificate for ecdsa-with-SHA384 / P-384 certificate
 * chains (common for real-world ECDSA CAs).  Only the one-shot is exposed.
 * ========================================================================== */
#ifndef NETSPLOR_SHA512_H
#define NETSPLOR_SHA512_H

#define SHA384_DIGEST_LEN 48
#define SHA384_BLOCK_LEN  128

/* One-shot SHA-384: out receives 48 bytes. */
void sha384(const void *data, unsigned int len, unsigned char out[48]);

/* Streaming SHA-384 (needed for the TLS 1.2 handshake transcript hash of the
 * AES_256_GCM_SHA384 suite, which is fed message-by-message).  The 64-bit
 * state is stored as 8 (hi,lo) 32-bit pairs so the struct is __int64-free. */
typedef struct {
    unsigned int  h[16];        /* 8 x 64-bit state, hi then lo               */
    unsigned char buf[128];     /* partial block                             */
    unsigned int  nbuf;         /* bytes buffered                            */
    unsigned int  lenlo, lenhi; /* total message length in bytes             */
} SHA384_CTX;

void sha384_init(SHA384_CTX *c);
void sha384_update(SHA384_CTX *c, const void *data, unsigned int len);
void sha384_final(SHA384_CTX *c, unsigned char out[48]);

/* HMAC-SHA384 (RFC 2104), block size 128.  out receives 48 bytes. */
void hmac_sha384(const unsigned char *key, int keylen,
                 const unsigned char *msg, int msglen, unsigned char out[48]);

/* SHA-512: same core as SHA-384, full 64-byte output (SMB 3.1.1 pre-auth).
   Streaming reuses the SHA-384 context and sha384_update. */
void sha512_init(SHA384_CTX *c);
void sha512_final(SHA384_CTX *c, unsigned char out[64]);
void sha512(const void *data, unsigned int len, unsigned char out[64]);

#endif /* NETSPLOR_SHA512_H */
