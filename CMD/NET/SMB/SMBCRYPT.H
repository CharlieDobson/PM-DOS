/* ==========================================================================
 * SMBCRYPT.H - the crypto SMB2 + NTLMv2 need that the TLS port does not
 * already carry.  Portable C89, no C runtime beyond <string.h>.
 * ==========================================================================
 * The vendored NSTLS core (CMD\NET\TLS) brings SHA-256 and HMAC-SHA256,
 * which is SMB2 2.0.2's packet signing.  NTLMv2 authentication needs the
 * older primitives it does not: MD4 (the NT hash), MD5 and HMAC-MD5 (the
 * NTLMv2 response), and RC4 (sealing the exported session key).  They are
 * here so both a client program and, later, the kernel redirector can link
 * them without pulling in TLS.
 * ========================================================================== */
#ifndef PMDOS_SMBCRYPT_H
#define PMDOS_SMBCRYPT_H

/* ---- MD4 (RFC 1320) ---- */
typedef struct {
    unsigned long  state[4];
    unsigned long  count[2];        /* bit count, low then high */
    unsigned char  buffer[64];
} MD4_CTX;

void md4_init( MD4_CTX *ctx );
void md4_update( MD4_CTX *ctx, const unsigned char *data, unsigned int len );
void md4_final( MD4_CTX *ctx, unsigned char digest[16] );
void md4( const unsigned char *data, unsigned int len, unsigned char digest[16] );

/* ---- MD5 (RFC 1321) ---- */
typedef struct {
    unsigned long  state[4];
    unsigned long  count[2];
    unsigned char  buffer[64];
} MD5_CTX;

void md5_init( MD5_CTX *ctx );
void md5_update( MD5_CTX *ctx, const unsigned char *data, unsigned int len );
void md5_final( MD5_CTX *ctx, unsigned char digest[16] );
void md5( const unsigned char *data, unsigned int len, unsigned char digest[16] );

/* ---- HMAC-MD5 (RFC 2104) ---- */
void hmac_md5( const unsigned char *key, int keylen,
               const unsigned char *data, int datalen,
               unsigned char out[16] );

/* ---- RC4 (for the NTLMSSP exported-session-key exchange) ---- */
typedef struct {
    unsigned char  s[256];
    int            i, j;
} RC4_CTX;

void rc4_init( RC4_CTX *ctx, const unsigned char *key, int keylen );
void rc4_crypt( RC4_CTX *ctx, const unsigned char *in, unsigned char *out, int len );

/* ---- NTLMv2 helpers ----
 * nt_hash      : the NT one-way function, MD4 of the password in UTF-16LE.
 * ntowf_v2     : HMAC-MD5( nt_hash, UTF-16LE( upper(user) + domain ) ) - the
 *                NTLMv2 hash the response and the session base key derive from.
 * utf16le      : ASCII -> UTF-16LE into 'out' (2*len bytes); returns the byte
 *                count.  SMB names are case-folded by the caller where needed.
 */
int  utf16le( const char *ascii, unsigned char *out, int outcap );
void nt_hash( const char *password, unsigned char out[16] );
void ntowf_v2( const char *password, const char *user, const char *domain,
               unsigned char out[16] );

/* ---- DES and the down-level LM / NTLMv1 responses (for SMB1) ----
 * SMB1's down-level peers (WfW 3.11, Win9x, NT) authenticate with the
 * old LM/NTLM challenge-response, which SMB2's NTLMv2 path does not use:
 * DES of an 8-byte server challenge under keys made from the password
 * hashes.  des_encrypt_block is one raw DES-ECB block; lm_hash is the LM
 * one-way function; desl spreads a 16-byte hash over three DES keys and
 * encrypts the challenge into the 24-byte response (MS-NLMP's DESL).
 * nt_hash (above) is NTOWFv1, so the NTLMv1 response is desl( nt_hash,
 * challenge ) and the LM response is desl( lm_hash, challenge ).
 */
void des_encrypt_block( const unsigned char key[8], const unsigned char in[8],
                        unsigned char out[8] );
void lm_hash( const char *password, unsigned char out[16] );
void desl( const unsigned char hash[16], const unsigned char challenge[8],
           unsigned char out[24] );

#endif /* PMDOS_SMBCRYPT_H */
