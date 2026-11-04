/* ==========================================================================
 * AES.H - AES-128/256 block cipher, CBC and counter mode (FIPS-197 /
 * SP800-38A).  Portable C89, no OS dependency.
 * ==========================================================================
 * The rounds are table-driven (one 1K table each way; see AES.C).  The
 * multi-block calls - CBC and counter mode - turn the schedule into words
 * once per call, so a whole record costs that once.
 * ========================================================================== */
#ifndef NETSPLOR_AES_H
#define NETSPLOR_AES_H

#define AES128_KEY_BYTES    16
#define AES128_BLOCK_BYTES  16
#define AES128_RK_BYTES     176     /* 11 round keys * 16 bytes */
#define AES256_KEY_BYTES    32
#define AES256_RK_BYTES     240     /* 15 round keys * 16 bytes */

/* Expand a 16-byte key into the 176-byte round-key schedule. */
void aes128_expand_key(const unsigned char key[16], unsigned char rk[176]);
/* Expand a 32-byte key into the 240-byte round-key schedule. */
void aes256_expand_key(const unsigned char key[32], unsigned char rk[240]);

/* Single-block ECB primitives (in/out are 16 bytes; may alias). */
void aes128_encrypt(const unsigned char rk[176],
                    const unsigned char in[16], unsigned char out[16]);
void aes128_decrypt(const unsigned char rk[176],
                    const unsigned char in[16], unsigned char out[16]);
void aes256_encrypt(const unsigned char rk[240],
                    const unsigned char in[16], unsigned char out[16]);
void aes256_decrypt(const unsigned char rk[240],
                    const unsigned char in[16], unsigned char out[16]);

/* Key-size-generic block/CBC primitives: 'nr' is the round count (10 = AES-128,
 * 14 = AES-256); 'rk' is the matching expanded schedule.  Used by the record
 * layer and GCM so they work with either key size. */
void aes_encrypt(const unsigned char *rk, int nr,
                 const unsigned char in[16], unsigned char out[16]);
void aes_decrypt(const unsigned char *rk, int nr,
                 const unsigned char in[16], unsigned char out[16]);
int  aes_cbc_encrypt(const unsigned char *rk, int nr, const unsigned char iv[16],
                     const unsigned char *in, unsigned char *out, int len);
int  aes_cbc_decrypt(const unsigned char *rk, int nr, const unsigned char iv[16],
                     const unsigned char *in, unsigned char *out, int len);

/* CBC mode.  'len' must be a positive multiple of 16.  'in' and 'out' may be
 * the same buffer.  Returns 0 on success, -1 on a bad length. */
int  aes128_cbc_encrypt(const unsigned char rk[176], const unsigned char iv[16],
                        const unsigned char *in, unsigned char *out, int len);
int  aes128_cbc_decrypt(const unsigned char rk[176], const unsigned char iv[16],
                        const unsigned char *in, unsigned char *out, int len);

/* Counter mode from the counter block icb, adding 1 to its last 32 bits for
 * each block (GCM's inc32).  len need not be a multiple of 16.  'in' and
 * 'out' may be the same buffer. */
void aes_ctr32(const unsigned char *rk, int nr, const unsigned char icb[16],
               const unsigned char *in, int len, unsigned char *out);

#endif /* NETSPLOR_AES_H */
