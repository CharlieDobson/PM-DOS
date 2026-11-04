/* ==========================================================================
 * PRF.H - TLS 1.2 pseudo-random function (RFC 5246 sec. 5) for NETSPLOR.
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89).
 * ==========================================================================
 * TLS 1.2 defines  PRF(secret, label, seed) = P_SHA256(secret, label + seed)
 * where
 *     P_hash(secret, seed) = HMAC(secret, A(1)+seed) | HMAC(secret, A(2)+seed) | ...
 *     A(0) = seed,   A(i) = HMAC(secret, A(i-1))
 *
 * Used to derive master_secret from the premaster secret, and to expand that
 * into the per-connection key block (MAC keys, AES keys, IVs).
 * ========================================================================== */
#ifndef NETSPLOR_PRF_H
#define NETSPLOR_PRF_H

/* Fill 'out' (outlen bytes) with PRF output.
 *   secret/secretlen : the secret (e.g. master secret)
 *   label            : ASCII label, e.g. "master secret" / "key expansion"
 *   seed/seedlen     : the seed (typically client_random + server_random)
 */
void tls12_prf(const unsigned char *secret, int secretlen,
               const char *label,
               const unsigned char *seed, int seedlen,
               unsigned char *out, int outlen);

/* Same PRF built on P_SHA384 - used by the AES_256_GCM_SHA384 suite, whose
 * key schedule and Finished verify_data use SHA-384 rather than SHA-256. */
void tls12_prf_sha384(const unsigned char *secret, int secretlen,
                      const char *label,
                      const unsigned char *seed, int seedlen,
                      unsigned char *out, int outlen);

/* TLS 1.3 (RFC 8446 7.1).  hlen is the suite's hash size: 32 for SHA-256,
 * 48 for SHA-384.
 *   tls13_hkdf_extract : out(hlen) = HMAC(salt, ikm); a NULL or empty salt or
 *                        ikm means hlen zero bytes, as the schedule wants.
 *   tls13_expand_label : out(outlen) = HKDF-Expand-Label(secret, label, ctx),
 *                        the "tls13 " prefix added here.  secret is hlen bytes.
 */
#define TLS13_LABEL_MAX  32
#define TLS13_CTX_MAX    64
void tls13_hkdf_extract( int hlen, const unsigned char *salt, int saltlen,
                         const unsigned char *ikm, int ikmlen, unsigned char *out );
void tls13_expand_label( int hlen, const unsigned char *secret,
                         const char *label,
                         const unsigned char *ctx, int ctxlen,
                         unsigned char *out, int outlen );

#endif /* NETSPLOR_PRF_H */
