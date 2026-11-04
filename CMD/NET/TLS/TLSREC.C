/* ==========================================================================
 * TLSREC.C - TLS 1.2 CBC record protection.  C89.
 * ==========================================================================
 * TX:  MAC = HMAC-SHA1(mac_key, seq(8) || type(1) || ver(2) || len(2) || frag)
 *      padded = frag || MAC || pad          (pad = (p+1) bytes each == p)
 *      record = type || 03 03 || len(2) || IV(16) || CBC(enc_key, IV, padded)
 * RX:  reverse, checking padding then MAC.
 * ========================================================================== */
#include "tlsrec.h"
#include "aes.h"
#include "sha.h"
#include "gcm.h"
#include <string.h>
#include <stdlib.h>

/* Expand the AES key by size and record the round count. */
static void install_key(tls_keys *k, const unsigned char *enc_key, int keylen)
{
    memcpy(k->enc_key, enc_key, keylen);
    if (keylen == 32) { aes256_expand_key(enc_key, k->rk); k->nr = 14; }
    else              { aes128_expand_key(enc_key, k->rk); k->nr = 10; }
}

void tls_keys_init(tls_keys *k,
                   const unsigned char mac_key[20],
                   const unsigned char *enc_key, int keylen)
{
    memcpy(k->mac_key, mac_key, TLS_MAC_LEN);
    install_key(k, enc_key, keylen);
    memset(k->seq, 0, 8);
    k->is_gcm = 0;
}

void tls_keys_init_gcm(tls_keys *k,
                       const unsigned char *enc_key, int keylen,
                       const unsigned char iv4[4])
{
    install_key(k, enc_key, keylen);
    gcm_calc_h(k->rk, k->nr, k->H);
    memcpy(k->implicit_iv, iv4, 4);
    memset(k->seq, 0, 8);
    k->is_gcm = 1;
}

void tls_keys_init13(tls_keys *k,
                     const unsigned char *enc_key, int keylen,
                     const unsigned char iv12[12])
{
    install_key( k, enc_key, keylen );
    gcm_calc_h( k->rk, k->nr, k->H );
    memcpy( k->iv, iv12, 12 );
    memset( k->seq, 0, 8 );
    k->is_gcm = 2;
}

static void seq_inc(unsigned char seq[8])
{
    int i;
    for (i = 7; i >= 0; i--)
        if (++seq[i] != 0)
            break;                              /* stop when no carry */
}

/* HMAC-SHA1 over seq || type || ver || len(frag) || frag. */
static void record_mac(const tls_keys *k, int type, const unsigned char *frag,
                       int flen, unsigned char mac[20])
{
    unsigned char hdr[13];      /* seq(8) + type(1) + ver(2) + len(2) */
    unsigned char *m;
    int mlen = 13 + flen;

    memcpy(hdr, k->seq, 8);
    hdr[8]  = (unsigned char)type;
    hdr[9]  = TLS_VER_HI;
    hdr[10] = TLS_VER_LO;
    hdr[11] = (unsigned char)((flen >> 8) & 0xFF);
    hdr[12] = (unsigned char)(flen & 0xFF);

    m = (unsigned char *)malloc(mlen);
    if (m == NULL) { memset(mac, 0, 20); return; }
    memcpy(m, hdr, 13);
    if (flen > 0)
        memcpy(m + 13, frag, flen);
    hmac_sha1(k->mac_key, TLS_MAC_LEN, m, mlen, mac);
    free(m);
}

int tls_plain_record(int content_type, const unsigned char *data, int len,
                     unsigned char *out, int outcap)
{
    if (len < 0 || TLS_REC_HDR + len > outcap)
        return -1;
    out[0] = (unsigned char)content_type;
    out[1] = TLS_VER_HI;
    out[2] = TLS_VER_LO;
    out[3] = (unsigned char)((len >> 8) & 0xFF);
    out[4] = (unsigned char)(len & 0xFF);
    if (len > 0)
        memcpy(out + TLS_REC_HDR, data, len);
    return TLS_REC_HDR + len;
}

int tls_protect_iv(tls_keys *k, int content_type,
                   const unsigned char *plain, int plen,
                   const unsigned char iv[16],
                   unsigned char *out, int outcap)
{
    unsigned char mac[20];
    unsigned char *padded;
    int L, padval, plen_pad, reclen, i;

    if (plen < 0)
        return -1;

    record_mac(k, content_type, plain, plen, mac);

    L = plen + TLS_MAC_LEN;                      /* fragment || MAC */
    padval = (16 - ((L + 1) % 16)) % 16;         /* TLS block padding */
    plen_pad = L + padval + 1;                   /* multiple of 16 */
    reclen = TLS_REC_HDR + TLS_IV_LEN + plen_pad;
    if (reclen > outcap)
        return -1;

    padded = (unsigned char *)malloc(plen_pad);
    if (padded == NULL)
        return -1;
    if (plen > 0)
        memcpy(padded, plain, plen);
    memcpy(padded + plen, mac, TLS_MAC_LEN);
    for (i = 0; i <= padval; i++)
        padded[L + i] = (unsigned char)padval;

    /* header */
    out[0] = (unsigned char)content_type;
    out[1] = TLS_VER_HI;
    out[2] = TLS_VER_LO;
    out[3] = (unsigned char)(((TLS_IV_LEN + plen_pad) >> 8) & 0xFF);
    out[4] = (unsigned char)((TLS_IV_LEN + plen_pad) & 0xFF);
    /* explicit IV then CBC ciphertext */
    memcpy(out + TLS_REC_HDR, iv, TLS_IV_LEN);
    aes_cbc_encrypt(k->rk, k->nr, iv, padded, out + TLS_REC_HDR + TLS_IV_LEN, plen_pad);

    free(padded);
    seq_inc(k->seq);
    return reclen;
}

/* AES-128-GCM record: header || explicit_nonce(8=seq) || ciphertext || tag(16).
   nonce = implicit_iv(4) || explicit_nonce(8); AAD = seq||type||ver||plen. */
static int tls_protect_gcm(tls_keys *k, int content_type,
                           const unsigned char *plain, int plen,
                           unsigned char *out, int outcap)
{
    unsigned char nonce[12], aad[13], tag[16];
    int reclen = TLS_REC_HDR + 8 + plen + 16;
    int rl = 8 + plen + 16;

    if (plen < 0 || reclen > outcap) return -1;

    memcpy(nonce, k->implicit_iv, 4);
    memcpy(nonce + 4, k->seq, 8);

    memcpy(aad, k->seq, 8);
    aad[8]  = (unsigned char)content_type;
    aad[9]  = TLS_VER_HI; aad[10] = TLS_VER_LO;
    aad[11] = (unsigned char)((plen >> 8) & 0xFF);
    aad[12] = (unsigned char)(plen & 0xFF);

    out[0] = (unsigned char)content_type;
    out[1] = TLS_VER_HI; out[2] = TLS_VER_LO;
    out[3] = (unsigned char)((rl >> 8) & 0xFF);
    out[4] = (unsigned char)(rl & 0xFF);
    memcpy(out + TLS_REC_HDR, k->seq, 8);           /* explicit nonce */

    gcm_encrypt(k->rk, k->nr, k->H, nonce, aad, 13, plain, plen,
                out + TLS_REC_HDR + 8, tag);
    memcpy(out + TLS_REC_HDR + 8 + plen, tag, 16);

    seq_inc(k->seq);
    return reclen;
}

static int tls_unprotect_gcm(tls_keys *k, const unsigned char *rec, int reclen,
                             int *content_type, unsigned char *out, int outcap)
{
    unsigned char nonce[12], aad[13];
    const unsigned char *expl, *cipher, *tag;
    int type, len, clen;

    if (reclen < TLS_REC_HDR + 8 + 16) return -1;
    type = rec[0];
    len  = (rec[3] << 8) | rec[4];
    if (TLS_REC_HDR + len != reclen) return -1;
    clen = len - 8 - 16;
    if (clen < 0 || clen > outcap) return -1;

    expl   = rec + TLS_REC_HDR;
    cipher = expl + 8;
    tag    = cipher + clen;

    memcpy(nonce, k->implicit_iv, 4);
    memcpy(nonce + 4, expl, 8);                     /* peer's explicit nonce */

    memcpy(aad, k->seq, 8);
    aad[8]  = (unsigned char)type;
    aad[9]  = TLS_VER_HI; aad[10] = TLS_VER_LO;
    aad[11] = (unsigned char)((clen >> 8) & 0xFF);
    aad[12] = (unsigned char)(clen & 0xFF);

    if (gcm_decrypt(k->rk, k->nr, k->H, nonce, aad, 13, cipher, clen, tag, out) != 0)
        return -1;

    seq_inc(k->seq);
    *content_type = type;
    return clen;
}

/* TLS 1.3 record (RFC 8446 5.2): the content type goes inside, after the
   data, and the record goes out as application_data:
     record = 17 03 03 len(2) || AES-GCM(data || type) || tag(16)
   nonce = iv XOR (0000 || seq), AAD = the 5-byte record header.  No
   padding is sent. */
static void nonce13(const tls_keys *k, unsigned char nonce[12])
{
    int i;
    memcpy( nonce, k->iv, 12 );
    for ( i = 0; i < 8; i++ ) {
        nonce[4 + i] ^= k->seq[i];
    }
}

static int tls_protect13(tls_keys *k, int content_type,
                         const unsigned char *plain, int plen,
                         unsigned char *out, int outcap)
{
    unsigned char nonce[12], tag[16];
    int rl = plen + 1 + 16;                     /* inner plaintext + tag */
    int reclen = TLS_REC_HDR + rl;

    if ( plen < 0 || reclen > outcap ) return -1;

    out[0] = TLS_CT_APPLICATION_DATA;
    out[1] = TLS_VER_HI; out[2] = TLS_VER_LO;
    out[3] = (unsigned char)( ( rl >> 8 ) & 0xFF );
    out[4] = (unsigned char)( rl & 0xFF );
    if ( plen > 0 ) memmove( out + TLS_REC_HDR, plain, plen );
    out[TLS_REC_HDR + plen] = (unsigned char)content_type;

    nonce13( k, nonce );
    /* in place: GCM counter mode reads each byte before writing it */
    gcm_encrypt( k->rk, k->nr, k->H, nonce, out, TLS_REC_HDR,
                 out + TLS_REC_HDR, plen + 1, out + TLS_REC_HDR, tag );
    memcpy( out + TLS_REC_HDR + plen + 1, tag, 16 );

    seq_inc( k->seq );
    return reclen;
}

static int tls_unprotect13(tls_keys *k, const unsigned char *rec, int reclen,
                           int *content_type, unsigned char *out, int outcap)
{
    unsigned char nonce[12];
    int len, clen;

    if ( reclen < TLS_REC_HDR + 1 + 16 ) return -1;
    if ( rec[0] != TLS_CT_APPLICATION_DATA ) return -1;
    len = ( rec[3] << 8 ) | rec[4];
    if ( TLS_REC_HDR + len != reclen ) return -1;
    clen = len - 16;
    if ( clen > outcap ) return -1;

    nonce13( k, nonce );
    if ( gcm_decrypt( k->rk, k->nr, k->H, nonce, rec, TLS_REC_HDR,
                      rec + TLS_REC_HDR, clen, rec + TLS_REC_HDR + clen, out ) != 0 ) {
        return -1;
    }
    seq_inc( k->seq );

    /* the real type is the last non-zero byte; zeros after it are padding */
    while ( clen > 0 && out[clen - 1] == 0 ) clen--;
    if ( clen == 0 ) return -1;                 /* no type at all */
    clen--;
    *content_type = out[clen];
    return clen;
}

int tls_protect(tls_keys *k, int content_type,
                const unsigned char *plain, int plen,
                nst_rng *rng, unsigned char *out, int outcap)
{
    unsigned char iv[16];
    if (k->is_gcm == 2)
        return tls_protect13(k, content_type, plain, plen, out, outcap);
    if (k->is_gcm)
        return tls_protect_gcm(k, content_type, plain, plen, out, outcap);
    rng_bytes(rng, iv, 16);
    return tls_protect_iv(k, content_type, plain, plen, iv, out, outcap);
}

int tls_unprotect(tls_keys *k, const unsigned char *rec, int reclen,
                  int *content_type, unsigned char *out, int outcap)
{
    const unsigned char *iv, *cipher;
    unsigned char *padded, mac[20], mac2[20];
    int type, len, clen, padval, i, contentlen, plen, bad;

    if (k->is_gcm == 2)
        return tls_unprotect13(k, rec, reclen, content_type, out, outcap);
    if (k->is_gcm)
        return tls_unprotect_gcm(k, rec, reclen, content_type, out, outcap);

    if (reclen < TLS_REC_HDR)
        return -1;
    type = rec[0];
    len  = (rec[3] << 8) | rec[4];
    if (TLS_REC_HDR + len != reclen)
        return -1;
    if (len < TLS_IV_LEN + 16 || ((len - TLS_IV_LEN) % 16) != 0)
        return -1;

    iv = rec + TLS_REC_HDR;
    cipher = iv + TLS_IV_LEN;
    clen = len - TLS_IV_LEN;

    padded = (unsigned char *)malloc(clen);
    if (padded == NULL)
        return -1;
    aes_cbc_decrypt(k->rk, k->nr, iv, cipher, padded, clen);

    /* Verify padding (constant value = padval, padval+1 bytes). */
    padval = padded[clen - 1];
    bad = 0;
    if (padval + 1 > clen)
        bad = 1;
    else
        for (i = 0; i <= padval; i++)
            if (padded[clen - 1 - i] != (unsigned char)padval)
                bad = 1;
    if (bad) { free(padded); return -1; }

    contentlen = clen - (padval + 1);           /* fragment || MAC */
    if (contentlen < TLS_MAC_LEN) { free(padded); return -1; }
    plen = contentlen - TLS_MAC_LEN;
    if (plen > outcap) { free(padded); return -1; }

    memcpy(mac, padded + plen, TLS_MAC_LEN);     /* received MAC */
    if (plen > 0)
        memcpy(out, padded, plen);

    /* Recompute the MAC over the recovered fragment and compare. */
    record_mac(k, type, out, plen, mac2);
    if (memcmp(mac, mac2, TLS_MAC_LEN) != 0) { free(padded); return -1; }

    free(padded);
    seq_inc(k->seq);
    *content_type = type;
    return plen;
}
