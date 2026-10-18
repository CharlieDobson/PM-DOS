/* ==========================================================================
 * X509.H - Minimal X.509 DER walk to extract the server's RSA public key.
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89).
 * ==========================================================================
 * NOT a validating parser.  v1 extracts {modulus, exponent} from the leaf
 * certificate's SubjectPublicKeyInfo and trusts it (trust-on-first-use); it
 * does NOT verify the signature chain, dates, names or key usage.  Chain
 * validation (RSA signature verify against a bundled CA set) is future work.
 * ========================================================================== */
#ifndef NETSPLOR_X509_H
#define NETSPLOR_X509_H

#define X509_OK              0
#define X509_ERR_PARSE     (-1)
#define X509_ERR_NOTRSA    (-2)
#define X509_ERR_TOOBIG    (-3)
#define X509_ERR_SIG       (-4)     /* signature did not verify            */
#define X509_ERR_EXPIRED   (-5)     /* outside notBefore..notAfter         */
#define X509_ERR_HOST      (-6)     /* identity does not match the host    */

/* Extract the RSA public key from a DER-encoded certificate.
 *   cert/certlen : the DER Certificate
 *   n / nlen     : receives the modulus (big-endian, leading zero stripped);
 *                  caller supplies a buffer, *nlen is its size in / used out
 *   e / elen     : same, for the public exponent
 * Returns X509_OK or an X509_ERR_*.
 */
int x509_extract_rsa_pubkey(const unsigned char *cert, int certlen,
                            unsigned char *n, int *nlen,
                            unsigned char *e, int *elen);

/* Verify that 'cert' was signed with sha256WithRSAEncryption by the issuer
   whose RSA public key is (n,e).  X509_OK if valid, else X509_ERR_*. */
int x509_verify_signed_by(const unsigned char *cert, int certlen,
                          const unsigned char *n, int nlen,
                          const unsigned char *e, int elen);

/* A parsed subject public key: RSA {n,e} or an EC point (SEC1 uncompressed:
 * P-256 = 65 B, P-384 = 97 B). */
#define X509_KEY_RSA    1
#define X509_KEY_EC256  2
#define X509_KEY_EC384  3
typedef struct {
    int           type;             /* X509_KEY_RSA | X509_KEY_EC256 | X509_KEY_EC384 */
    unsigned char n[512]; int nlen; /* RSA modulus */
    unsigned char e[16];  int elen; /* RSA exponent */
    unsigned char point[97];        /* EC uncompressed point (65 or 97 bytes) */
} x509_pubkey;

/* Extract the subject public key (RSA or EC-P256) from a certificate. */
int x509_get_pubkey(const unsigned char *cert, int certlen, x509_pubkey *out);

/* Verify 'cert's signature (sha256WithRSAEncryption OR ecdsa-with-SHA256)
   against the issuer's public key.  X509_OK if valid, else X509_ERR_*. */
int x509_verify_signed(const unsigned char *cert, int certlen,
                       const x509_pubkey *issuer);

/* notBefore/notAfter as normalized "YYYYMMDDHHMMSS" (14 chars + NUL). */
int x509_get_validity(const unsigned char *cert, int certlen,
                      char notBefore[15], char notAfter[15]);

/* Check the cert is currently valid.  'now' is "YYYYMMDDHHMMSS" (UTC). */
int x509_check_validity(const unsigned char *cert, int certlen,
                        const char *now);

/* Match the cert identity (subjectAltName dNSName/iPAddress, else CN)
   against 'host'.  X509_OK or X509_ERR_HOST. */
int x509_check_host(const unsigned char *cert, int certlen, const char *host);

#endif /* NETSPLOR_X509_H */
