/* ==========================================================================
 * RSA.C - RSA public operation + PKCS#1 v1.5 (type 2) encryption.  C89.
 * ========================================================================== */
#include "rsa.h"
#include "sha.h"
#include "sha512.h"
#include <string.h>

void rsa_pubkey_init(rsa_pubkey *k,
                     const uint8_t *n, int nlen,
                     const uint8_t *e, int elen)
{
    bn_from_bytes(&k->n, n, nlen);
    bn_from_bytes(&k->e, e, elen);
    k->klen = (bn_bitlen(&k->n) + 7) / 8;       /* true modulus byte length */
}

void rsa_public_raw(const rsa_pubkey *k,
                    const uint8_t *in, uint8_t *out)
{
    bignum m, c;
    bn_from_bytes(&m, in, k->klen);
    bn_modexp(&c, &m, &k->e, &k->n);
    bn_to_bytes(&c, out, k->klen);
}

int rsa_pkcs1_encrypt(const rsa_pubkey *k,
                      const uint8_t *msg, int mlen,
                      const uint8_t *rnd, uint8_t *out)
{
    uint8_t em[BN_WORDS * 4];
    int klen = k->klen, pslen, i;

    /* EM = 00 || 02 || PS || 00 || M ; PS must be >= 8 bytes. */
    pslen = klen - 3 - mlen;
    if (mlen < 0 || pslen < 8 || klen > (int)sizeof(em))
        return -1;

    em[0] = 0x00;
    em[1] = 0x02;
    for (i = 0; i < pslen; i++) {
        uint8_t b = rnd[i];
        if (b == 0x00)
            b = 0x01;                           /* PS bytes must be nonzero */
        em[2 + i] = b;
    }
    em[2 + pslen] = 0x00;
    for (i = 0; i < mlen; i++)
        em[3 + pslen + i] = msg[i];

    rsa_public_raw(k, em, out);
    return 0;
}

/* DER DigestInfo prefixes (RFC 8017 / PKCS#1 v2.2), the same 19 bytes for
   each SHA-2 hash but for the OID's last byte and the digest length. */
static const uint8_t SHA256_DIGESTINFO[19] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
};
static const uint8_t SHA384_DIGESTINFO[19] = {
    0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30
};
static const uint8_t SHA512_DIGESTINFO[19] = {
    0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40
};

/* RSASSA-PKCS1-v1_5 verify for a SHA-2 digest of 32, 48 or 64 bytes:
   EM = 00 01 FF..FF 00 || DigestInfo || hash.  0 if valid, -1 otherwise. */
int rsa_pkcs1_verify( const rsa_pubkey *k, int hlen,
                      const uint8_t *hash,
                      const uint8_t *sig, int siglen )
{
    uint8_t em[BN_WORDS * 4];
    const uint8_t *info;
    int klen = k->klen, tlen, pslen, i;

    if ( hlen == 32 ) {
        info = SHA256_DIGESTINFO;
    } else if ( hlen == 48 ) {
        info = SHA384_DIGESTINFO;
    } else if ( hlen == 64 ) {
        info = SHA512_DIGESTINFO;
    } else {
        return -1;
    }
    tlen = 19 + hlen;
    if ( siglen != klen || klen > (int)sizeof( em ) || klen < tlen + 11 ) return -1;

    rsa_public_raw( k, sig, em );               /* EM = s^e mod n */

    if ( em[0] != 0x00 || em[1] != 0x01 ) return -1;     /* block type 1 */
    pslen = klen - 3 - tlen;
    if ( pslen < 8 ) return -1;
    for ( i = 0; i < pslen; i++ ) {
        if ( em[2 + i] != 0xFF ) return -1;     /* PS = 0xFF... */
    }
    if ( em[2 + pslen] != 0x00 ) return -1;
    if ( memcmp( em + 3 + pslen, info, 19 ) != 0 ) return -1;
    if ( memcmp( em + 3 + pslen + 19, hash, hlen ) != 0 ) return -1;
    return 0;                                   /* valid */
}

int rsa_pkcs1_verify_sha256(const rsa_pubkey *k,
                            const uint8_t hash[32],
                            const uint8_t *sig, int siglen)
{
    return rsa_pkcs1_verify( k, 32, hash, sig, siglen );
}

/* One-shot hash of two pieces, by digest length: SHA-256, -384 or -512. */
static void hash2( int hlen, const uint8_t *a, int alen,
                   const uint8_t *b, int blen, uint8_t *out )
{
    if ( hlen == 32 ) {
        SHA256_CTX c;
        sha256_init( &c );
        sha256_update( &c, a, (unsigned int)alen );
        sha256_update( &c, b, (unsigned int)blen );
        sha256_final( &c, out );
    } else {
        SHA384_CTX c;
        if ( hlen == 48 ) {
            sha384_init( &c );
        } else {
            sha512_init( &c );
        }
        sha384_update( &c, a, (unsigned int)alen );
        sha384_update( &c, b, (unsigned int)blen );
        if ( hlen == 48 ) {
            sha384_final( &c, out );
        } else {
            sha512_final( &c, out );
        }
    }
}

/* MGF1 mask generation (RFC 8017 App. B.2.1) over the same hash. */
static void mgf1( int hlen, const uint8_t *seed, int seedlen,
                  uint8_t *mask, int masklen )
{
    uint8_t cnt[4], md[64];
    uint32_t counter = 0;
    int off = 0, n;
    while ( off < masklen ) {
        cnt[0] = (uint8_t)( counter >> 24 ); cnt[1] = (uint8_t)( counter >> 16 );
        cnt[2] = (uint8_t)( counter >> 8 );  cnt[3] = (uint8_t)counter;
        hash2( hlen, seed, seedlen, cnt, 4, md );
        n = masklen - off;
        if ( n > hlen ) n = hlen;
        memcpy( mask + off, md, n );
        off += n; counter++;
    }
}

/* RSASSA-PSS verify with MGF1 over the same hash and salt length == hLen,
   the only parameters TLS uses (rsa_pss_rsae_sha256/384/512).
   EMSA-PSS-VERIFY per RFC 8017 9.1.2.  Returns 0 if valid, -1 otherwise. */
int rsa_pss_verify( const rsa_pubkey *k, int hlen,
                    const uint8_t *hash,
                    const uint8_t *sig, int siglen )
{
    uint8_t full[BN_WORDS * 4];
    uint8_t db[BN_WORDS * 4], mask[BN_WORDS * 4], zeros[8], hp[64];
    const uint8_t *em, *H, *salt;
    int klen = k->klen, modbits, embits, emlen, maskbits;
    int sLen = hlen, dblen, i;

    if ( hlen != 32 && hlen != 48 && hlen != 64 ) return -1;
    if ( siglen != klen || klen > (int)sizeof( full ) ) return -1;

    modbits = bn_bitlen( &k->n );
    embits  = modbits - 1;
    emlen   = ( embits + 7 ) / 8;
    if ( emlen < hlen + sLen + 2 || emlen > klen ) return -1;

    rsa_public_raw( k, sig, full );             /* EM = s^e mod n (klen bytes) */
    /* EM < 2^embits, so the top (klen-emlen) bytes are zero. */
    for ( i = 0; i < klen - emlen; i++ ) {
        if ( full[i] != 0x00 ) return -1;
    }
    em = full + ( klen - emlen );

    if ( em[emlen - 1] != 0xBC ) return -1;     /* trailer field */

    dblen = emlen - hlen - 1;
    H = em + dblen;                             /* maskedDB=[0..dblen), H=[dblen..) */

    /* leftmost (8*emlen - embits) bits of the leading maskedDB byte must be 0 */
    maskbits = 8 * emlen - embits;
    if ( maskbits > 0 && ( em[0] & ( 0xFF << ( 8 - maskbits ) ) ) != 0 ) return -1;

    mgf1( hlen, H, hlen, mask, dblen );
    for ( i = 0; i < dblen; i++ ) {
        db[i] = (uint8_t)( em[i] ^ mask[i] );
    }
    if ( maskbits > 0 ) db[0] = (uint8_t)( db[0] & ( 0xFF >> maskbits ) );

    /* DB = PS(0x00..) || 0x01 || salt */
    for ( i = 0; i < dblen - sLen - 1; i++ ) {
        if ( db[i] != 0x00 ) return -1;
    }
    if ( db[dblen - sLen - 1] != 0x01 ) return -1;
    salt = db + dblen - sLen;

    /* H' = Hash( 00*8 || mHash || salt ): mHash and salt are hlen each */
    memset( zeros, 0, 8 );
    {
        uint8_t ms[128];
        memcpy( ms, hash, hlen );
        memcpy( ms + hlen, salt, sLen );
        hash2( hlen, zeros, 8, ms, hlen + sLen, hp );
    }
    return memcmp( hp, H, hlen ) == 0 ? 0 : -1;
}

int rsa_pss_verify_sha256(const rsa_pubkey *k,
                          const uint8_t hash[32],
                          const uint8_t *sig, int siglen)
{
    return rsa_pss_verify( k, 32, hash, sig, siglen );
}
