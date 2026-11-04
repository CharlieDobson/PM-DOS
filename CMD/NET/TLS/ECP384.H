/* ==========================================================================
 * ECP384.H - NIST P-384 (secp384r1) ECDSA signature verification.
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89).
 * ==========================================================================
 * Only the verify path is needed: real-world ECDSA certificate chains use
 * P-384 roots/intermediates (ecdsa-with-SHA384).  Field/point arithmetic is
 * delegated to BIGNUM.C, mirroring ECP256.C.
 * ========================================================================== */
#ifndef NETSPLOR_ECP384_H
#define NETSPLOR_ECP384_H

#include "rng.h"

/* Force the one-off curve-constant setup.  See ecp256_init(). */
void ecp384_init(void);

/* Verify an ECDSA/P-384 signature.
 *   pub  : 0x04 || X(48) || Y(48)   (97-byte SEC1 uncompressed point)
 *   hash : message digest (SHA-384 = 48 bytes; SHA-256 = 32 also accepted)
 *   r,s  : signature integers (big-endian, DER INTEGER contents)
 * Returns 0 if valid, -1 otherwise.
 */
int ecdsa_verify_p384(const unsigned char pub[97],
                      const unsigned char *hash, int hashlen,
                      const unsigned char *r, int rlen,
                      const unsigned char *s, int slen);

/* ECDH on P-384, for a server that offers no other group.
 *   ecp384_keygen : priv(48) random in [1, n-1], pub(97) = priv*G (SEC1).
 *   ecp384_ecdh   : out(48) = X of priv * peer; peer must be on the curve.
 * Both return 0, or -1 on failure.
 */
int ecp384_keygen( nst_rng *rng, unsigned char priv[48], unsigned char pub[97] );
int ecp384_ecdh( const unsigned char priv[48], const unsigned char peer[97],
                 unsigned char out[48] );

#endif /* NETSPLOR_ECP384_H */
