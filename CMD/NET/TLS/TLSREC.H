/* ==========================================================================
 * TLSREC.H - TLS 1.2 record layer (CBC cipher suite) for NETSPLOR.
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89).
 * ==========================================================================
 * Implements the record protection for TLS_RSA_WITH_AES_128_CBC_SHA:
 *   MAC = HMAC-SHA1(mac_key, seq || type || version || len || fragment)
 *   record fragment = explicit_IV(16) || AES-128-CBC(fragment || MAC || pad)
 * plus the 5-byte record header (type, version, length).
 *
 * A tls_keys holds one direction's keys and its 64-bit sequence number; the
 * handshake creates one for read and one for write once keys are derived.
 * ========================================================================== */
#ifndef NETSPLOR_TLSREC_H
#define NETSPLOR_TLSREC_H

#include "rng.h"

/* Content types */
#define TLS_CT_CHANGE_CIPHER_SPEC   20
#define TLS_CT_ALERT                21
#define TLS_CT_HANDSHAKE            22
#define TLS_CT_APPLICATION_DATA     23

/* Protocol version bytes (TLS 1.2) */
#define TLS_VER_HI  3
#define TLS_VER_LO  3

#define TLS_MAC_LEN     20          /* HMAC-SHA1 */
#define TLS_IV_LEN      16          /* AES block / explicit IV */
#define TLS_REC_HDR     5

typedef struct {
    unsigned char mac_key[TLS_MAC_LEN];
    unsigned char enc_key[32];      /* AES-128 (16) or AES-256 (32)        */
    unsigned char rk[240];          /* expanded AES key (176 or 240 used)  */
    int           nr;               /* AES round count: 10 = 128, 14 = 256 */
    unsigned char seq[8];           /* 64-bit big-endian sequence number   */
    int           is_gcm;           /* 1 = AES-GCM (AEAD), 0 = CBC+HMAC     */
    unsigned char implicit_iv[4];   /* GCM salt (client/server write IV)    */
    unsigned char H[16];            /* GCM GHASH subkey                    */
} tls_keys;

/* Initialise one direction for CBC+HMAC: install keys, zero the sequence.
   keylen is 16 (AES-128) or 32 (AES-256). */
void tls_keys_init(tls_keys *k,
                   const unsigned char mac_key[20],
                   const unsigned char *enc_key, int keylen);

/* Initialise one direction for AES-GCM: install key + 4-byte implicit IV.
   keylen is 16 (AES-128) or 32 (AES-256). */
void tls_keys_init_gcm(tls_keys *k,
                       const unsigned char *enc_key, int keylen,
                       const unsigned char iv4[4]);

/* Build a plaintext (unprotected) record: header || data.  Used before the
   cipher is active.  Returns total bytes, or -1 if it won't fit. */
int tls_plain_record(int content_type, const unsigned char *data, int len,
                     unsigned char *out, int outcap);

/* Protect a fragment into a full record using an explicit IV (testable). */
int tls_protect_iv(tls_keys *k, int content_type,
                   const unsigned char *plain, int plen,
                   const unsigned char iv[16],
                   unsigned char *out, int outcap);

/* Protect a fragment, drawing the explicit IV from 'rng'. */
int tls_protect(tls_keys *k, int content_type,
                const unsigned char *plain, int plen,
                nst_rng *rng, unsigned char *out, int outcap);

/* Decrypt+verify a full record (header included).  On success returns the
   plaintext length, writes it to 'out', and sets *content_type.  Returns -1
   on any parse / padding / MAC failure. */
int tls_unprotect(tls_keys *k, const unsigned char *rec, int reclen,
                  int *content_type, unsigned char *out, int outcap);

#endif /* NETSPLOR_TLSREC_H */
