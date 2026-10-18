/* ==========================================================================
 * RSA.H - RSA public-key operation + PKCS#1 v1.5 encryption for NETSPLOR.
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89).
 * ==========================================================================
 * Client-side TLS needs only the RSA *public* op:
 *   - encrypt the 48-byte premaster secret with the server's public key
 *     (RSAES-PKCS1-v1_5, block type 2) for ClientKeyExchange,
 *   - and (later) verify a certificate signature (block type 1).
 * ========================================================================== */
#ifndef NETSPLOR_RSA_H
#define NETSPLOR_RSA_H

#include "bignum.h"

typedef struct {
    bignum n;       /* modulus  */
    bignum e;       /* public exponent */
    int    klen;    /* modulus length in bytes (RSA block size) */
} rsa_pubkey;

/* Build a public key from big-endian modulus and exponent byte strings. */
void rsa_pubkey_init(rsa_pubkey *k,
                     const unsigned char *n, int nlen,
                     const unsigned char *e, int elen);

/* Raw RSAEP: out = in^e mod n.  Both buffers are k->klen bytes, big-endian. */
void rsa_public_raw(const rsa_pubkey *k,
                    const unsigned char *in, unsigned char *out);

/* RSAES-PKCS1-v1_5 encrypt (block type 2):
 *   EM = 0x00 0x02 PS 0x00 M     (PS = k->klen-3-mlen NONZERO pad bytes)
 * 'rnd' must supply at least (klen-3-mlen) nonzero bytes for PS.
 * 'out' receives klen ciphertext bytes.
 * Returns 0, or -1 if the message is too long for the key.
 */
int  rsa_pkcs1_encrypt(const rsa_pubkey *k,
                       const unsigned char *msg, int mlen,
                       const unsigned char *rnd, unsigned char *out);

/* RSASSA-PKCS1-v1_5 verify with SHA-256 (block type 1):
 *   EM = s^e mod n must equal  00 01 FF..FF 00 || DigestInfo(SHA-256) || hash
 * 'hash' is the 32-byte SHA-256 of the signed data; 'sig' is k->klen bytes.
 * Returns 0 if the signature is valid, -1 otherwise.
 */
int  rsa_pkcs1_verify_sha256(const rsa_pubkey *k,
                             const unsigned char hash[32],
                             const unsigned char *sig, int siglen);

/* RSASSA-PSS verify with SHA-256 + MGF1-SHA256 and salt length == 32 (hLen),
 * per RFC 8017 §8.1.2 / §9.1.2.  Modern (TLS 1.2+) certs are frequently signed
 * with rsassaPss instead of PKCS#1 v1.5.
 * 'hash' is SHA-256 of the signed data; 'sig' is k->klen bytes.
 * Returns 0 if the signature is valid, -1 otherwise.
 */
int  rsa_pss_verify_sha256(const rsa_pubkey *k,
                           const unsigned char hash[32],
                           const unsigned char *sig, int siglen);

#endif /* NETSPLOR_RSA_H */
