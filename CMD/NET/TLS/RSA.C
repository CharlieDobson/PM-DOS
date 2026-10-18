/* ==========================================================================
 * RSA.C - RSA public operation + PKCS#1 v1.5 (type 2) encryption.  C89.
 * ========================================================================== */
#include "rsa.h"
#include "sha.h"
#include <string.h>

void rsa_pubkey_init(rsa_pubkey *k,
                     const unsigned char *n, int nlen,
                     const unsigned char *e, int elen)
{
    bn_from_bytes(&k->n, n, nlen);
    bn_from_bytes(&k->e, e, elen);
    k->klen = (bn_bitlen(&k->n) + 7) / 8;       /* true modulus byte length */
}

void rsa_public_raw(const rsa_pubkey *k,
                    const unsigned char *in, unsigned char *out)
{
    bignum m, c;
    bn_from_bytes(&m, in, k->klen);
    bn_modexp(&c, &m, &k->e, &k->n);
    bn_to_bytes(&c, out, k->klen);
}

int rsa_pkcs1_encrypt(const rsa_pubkey *k,
                      const unsigned char *msg, int mlen,
                      const unsigned char *rnd, unsigned char *out)
{
    unsigned char em[BN_WORDS * 4];
    int klen = k->klen, pslen, i;

    /* EM = 00 || 02 || PS || 00 || M ; PS must be >= 8 bytes. */
    pslen = klen - 3 - mlen;
    if (mlen < 0 || pslen < 8 || klen > (int)sizeof(em))
        return -1;

    em[0] = 0x00;
    em[1] = 0x02;
    for (i = 0; i < pslen; i++) {
        unsigned char b = rnd[i];
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

/* DER DigestInfo prefix for SHA-256 (RFC 8017 / PKCS#1 v2.2). */
static const unsigned char SHA256_DIGESTINFO[19] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
};

int rsa_pkcs1_verify_sha256(const rsa_pubkey *k,
                            const unsigned char hash[32],
                            const unsigned char *sig, int siglen)
{
    unsigned char em[BN_WORDS * 4];
    int klen = k->klen, tlen = 19 + 32, pslen, i;

    if (siglen != klen || klen > (int)sizeof(em) || klen < tlen + 11)
        return -1;

    rsa_public_raw(k, sig, em);                  /* EM = s^e mod n */

    if (em[0] != 0x00 || em[1] != 0x01)          /* block type 1 */
        return -1;
    pslen = klen - 3 - tlen;
    if (pslen < 8)
        return -1;
    for (i = 0; i < pslen; i++)
        if (em[2 + i] != 0xFF)                   /* PS = 0xFF... */
            return -1;
    if (em[2 + pslen] != 0x00)
        return -1;
    if (memcmp(em + 3 + pslen, SHA256_DIGESTINFO, 19) != 0)
        return -1;
    if (memcmp(em + 3 + pslen + 19, hash, 32) != 0)
        return -1;
    return 0;                                    /* valid */
}

/* MGF1 mask generation (RFC 8017 App. B.2.1) with SHA-256. */
static void mgf1_sha256(const unsigned char *seed, int seedlen,
                        unsigned char *mask, int masklen)
{
    unsigned char cnt[4], md[32];
    SHA256_CTX c;
    unsigned long counter = 0;
    int off = 0, n;
    while (off < masklen) {
        cnt[0] = (unsigned char)(counter >> 24); cnt[1] = (unsigned char)(counter >> 16);
        cnt[2] = (unsigned char)(counter >> 8);  cnt[3] = (unsigned char)counter;
        sha256_init(&c);
        sha256_update(&c, seed, (unsigned int)seedlen);
        sha256_update(&c, cnt, 4);
        sha256_final(&c, md);
        n = masklen - off; if (n > 32) n = 32;
        memcpy(mask + off, md, n);
        off += n; counter++;
    }
}

/* RSASSA-PSS verify with SHA-256 + MGF1-SHA256, salt length == 32 (hLen).
   EMSA-PSS-VERIFY per RFC 8017 §9.1.2.  Returns 0 if valid, -1 otherwise. */
int rsa_pss_verify_sha256(const rsa_pubkey *k,
                          const unsigned char hash[32],
                          const unsigned char *sig, int siglen)
{
    unsigned char full[BN_WORDS * 4];
    unsigned char db[BN_WORDS * 4], mask[BN_WORDS * 4], mp[8 + 32 + 32], hp[32];
    const unsigned char *em, *H, *salt;
    int klen = k->klen, modbits, embits, emlen, maskbits;
    int hLen = 32, sLen = 32, dblen, i;

    if (siglen != klen || klen > (int)sizeof(full))
        return -1;

    modbits = bn_bitlen(&k->n);
    embits  = modbits - 1;
    emlen   = (embits + 7) / 8;
    if (emlen < hLen + sLen + 2 || emlen > klen)
        return -1;

    rsa_public_raw(k, sig, full);                /* EM = s^e mod n (klen bytes) */
    /* EM < 2^embits, so the top (klen-emlen) bytes are zero. */
    for (i = 0; i < klen - emlen; i++)
        if (full[i] != 0x00) return -1;
    em = full + (klen - emlen);

    if (em[emlen - 1] != 0xBC)                   /* trailer field */
        return -1;

    dblen = emlen - hLen - 1;
    H = em + dblen;                              /* maskedDB=[0..dblen), H=[dblen..) */

    /* leftmost (8*emlen - embits) bits of the leading maskedDB byte must be 0 */
    maskbits = 8 * emlen - embits;
    if (maskbits > 0 && (em[0] & (0xFF << (8 - maskbits))) != 0)
        return -1;

    mgf1_sha256(H, hLen, mask, dblen);
    for (i = 0; i < dblen; i++)
        db[i] = (unsigned char)(em[i] ^ mask[i]);
    if (maskbits > 0)
        db[0] = (unsigned char)(db[0] & (0xFF >> maskbits));

    /* DB = PS(0x00..) || 0x01 || salt */
    for (i = 0; i < dblen - sLen - 1; i++)
        if (db[i] != 0x00) return -1;
    if (db[dblen - sLen - 1] != 0x01)
        return -1;
    salt = db + dblen - sLen;

    /* H' = Hash( 00*8 || mHash || salt ) */
    memset(mp, 0, 8);
    memcpy(mp + 8, hash, hLen);
    memcpy(mp + 8 + hLen, salt, sLen);
    sha256(mp, (unsigned int)(8 + hLen + sLen), hp);

    return memcmp(hp, H, hLen) == 0 ? 0 : -1;
}
