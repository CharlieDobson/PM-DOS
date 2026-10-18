/* ==========================================================================
 * ECP256.H - NIST P-256 (secp256r1) ECDH for NETSPLOR's ECDHE key exchange.
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89).  Built on BIGNUM.C.
 * ==========================================================================
 * Just enough for ephemeral ECDH: generate a keypair, and compute the shared
 * secret's X-coordinate (the TLS premaster for ECDHE suites).  Points are the
 * uncompressed SEC1 form 0x04 || X(32) || Y(32).
 * ========================================================================== */
#ifndef NETSPLOR_ECP256_H
#define NETSPLOR_ECP256_H

#include "rng.h"

#define ECP256_PUB_LEN   65     /* 0x04 || X || Y            */
#define ECP256_PRIV_LEN  32
#define ECP256_COORD_LEN 32

/* Force the one-off curve-constant setup that every other entry point does
   lazily.  Optional; lets a host pay that cost at a chosen moment (DLL load)
   rather than inside the first handshake.  Idempotent. */
void ecp256_init(void);

/* Generate an ephemeral keypair.  priv <- [1, n-1], pub <- priv*G (SEC1).
   Returns 0 on success, -1 on failure. */
int ecp256_keygen(nst_rng *rng, unsigned char priv[ECP256_PRIV_LEN],
                  unsigned char pub[ECP256_PUB_LEN]);

/* ECDH: out <- X-coordinate of priv * peerPub (32 bytes, big-endian).
   Validates that peerPub is a well-formed point on the curve.
   Returns 0 on success, -1 on bad input / point not on curve. */
int ecp256_ecdh(const unsigned char priv[ECP256_PRIV_LEN],
                const unsigned char peer[ECP256_PUB_LEN],
                unsigned char out[ECP256_COORD_LEN]);

/* Scalar-multiply the base point: pub <- k*G (SEC1).  Exposed for testing. */
int ecp256_mul_base(const unsigned char k[ECP256_PRIV_LEN],
                    unsigned char pub[ECP256_PUB_LEN]);

/* Verify an ECDSA signature (r,s) over the 32-byte hash 'hash' with the SEC1
   public key 'pub' (65 bytes).  r/s are the raw INTEGER magnitudes (any length
   up to 33 bytes, leading zero allowed).  Returns 0 if valid, -1 otherwise. */
int ecdsa_verify_p256(const unsigned char pub[ECP256_PUB_LEN],
                      const unsigned char hash[32],
                      const unsigned char *r, int rlen,
                      const unsigned char *s, int slen);

#endif /* NETSPLOR_ECP256_H */
