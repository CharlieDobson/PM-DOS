/* ==========================================================================
 * PRF.C - TLS 1.2 PRF = P_SHA256 (RFC 5246).  Portable C89.
 * ========================================================================== */
#include "prf.h"
#include "sha.h"
#include "sha512.h"
#include <string.h>
#include <stdlib.h>

void tls12_prf(const uint8_t *secret, int secretlen,
               const char *label,
               const uint8_t *seed, int seedlen,
               uint8_t *out, int outlen)
{
    int   llen = (int)strlen(label);
    int   lslen = llen + seedlen;      /* the P_hash "seed" = label || seed */
    uint8_t *ls;                       /* label || seed                     */
    uint8_t *tmp;                      /* A(i) || label || seed             */
    uint8_t A[SHA256_DIGEST_LEN];
    uint8_t Anew[SHA256_DIGEST_LEN];
    uint8_t block[SHA256_DIGEST_LEN];
    int   done = 0;

    if (outlen <= 0)
        return;

    ls  = (uint8_t *)malloc(lslen);
    tmp = (uint8_t *)malloc(SHA256_DIGEST_LEN + lslen);
    if (ls == NULL || tmp == NULL) {   /* out of memory: zero-fill defensively */
        if (ls)  free(ls);
        if (tmp) free(tmp);
        memset(out, 0, outlen);
        return;
    }
    memcpy(ls, label, llen);
    memcpy(ls + llen, seed, seedlen);

    /* A(1) = HMAC(secret, A(0)=seed) */
    hmac_sha256(secret, secretlen, ls, lslen, A);

    while (done < outlen) {
        int n;
        /* HMAC(secret, A(i) || seed) */
        memcpy(tmp, A, SHA256_DIGEST_LEN);
        memcpy(tmp + SHA256_DIGEST_LEN, ls, lslen);
        hmac_sha256(secret, secretlen, tmp, SHA256_DIGEST_LEN + lslen, block);

        n = outlen - done;
        if (n > SHA256_DIGEST_LEN)
            n = SHA256_DIGEST_LEN;
        memcpy(out + done, block, n);
        done += n;

        /* A(i+1) = HMAC(secret, A(i)) */
        hmac_sha256(secret, secretlen, A, SHA256_DIGEST_LEN, Anew);
        memcpy(A, Anew, SHA256_DIGEST_LEN);
    }

    free(tmp);
    free(ls);
}

/* P_SHA384 variant (digest length 48).  Same structure as tls12_prf. */
void tls12_prf_sha384(const uint8_t *secret, int secretlen,
                      const char *label,
                      const uint8_t *seed, int seedlen,
                      uint8_t *out, int outlen)
{
    int   llen = (int)strlen(label);
    int   lslen = llen + seedlen;
    uint8_t *ls, *tmp;
    uint8_t A[SHA384_DIGEST_LEN];
    uint8_t Anew[SHA384_DIGEST_LEN];
    uint8_t block[SHA384_DIGEST_LEN];
    int   done = 0;

    if (outlen <= 0)
        return;

    ls  = (uint8_t *)malloc(lslen);
    tmp = (uint8_t *)malloc(SHA384_DIGEST_LEN + lslen);
    if (ls == NULL || tmp == NULL) {
        if (ls)  free(ls);
        if (tmp) free(tmp);
        memset(out, 0, outlen);
        return;
    }
    memcpy(ls, label, llen);
    memcpy(ls + llen, seed, seedlen);

    hmac_sha384(secret, secretlen, ls, lslen, A);   /* A(1) */

    while (done < outlen) {
        int n;
        memcpy(tmp, A, SHA384_DIGEST_LEN);
        memcpy(tmp + SHA384_DIGEST_LEN, ls, lslen);
        hmac_sha384(secret, secretlen, tmp, SHA384_DIGEST_LEN + lslen, block);

        n = outlen - done;
        if (n > SHA384_DIGEST_LEN)
            n = SHA384_DIGEST_LEN;
        memcpy(out + done, block, n);
        done += n;

        hmac_sha384(secret, secretlen, A, SHA384_DIGEST_LEN, Anew);
        memcpy(A, Anew, SHA384_DIGEST_LEN);
    }

    free(tmp);
    free(ls);
}

/* --------------------------------------------------------------------------
 * TLS 1.3 key schedule pieces (RFC 8446 7.1, RFC 5869): HKDF over
 * HMAC-SHA256 (hlen 32) or HMAC-SHA384 (hlen 48).
 * ------------------------------------------------------------------------ */
static void hmac_by_len( int hlen, const uint8_t *key, int keylen,
                         const uint8_t *msg, int msglen, uint8_t *out )
{
    if ( hlen == 48 ) {
        hmac_sha384( key, keylen, msg, msglen, out );
    } else {
        hmac_sha256( key, keylen, msg, msglen, out );
    }
}

void tls13_hkdf_extract( int hlen, const uint8_t *salt, int saltlen,
                         const uint8_t *ikm, int ikmlen, uint8_t *out )
{
    uint8_t zeros[48];
    memset( zeros, 0, sizeof( zeros ) );
    if ( salt == NULL || saltlen == 0 ) {
        salt = zeros; saltlen = hlen;
    }
    if ( ikm == NULL || ikmlen == 0 ) {
        ikm = zeros; ikmlen = hlen;
    }
    hmac_by_len( hlen, salt, saltlen, ikm, ikmlen, out );
}

void tls13_expand_label( int hlen, const uint8_t *secret,
                         const char *label,
                         const uint8_t *ctx, int ctxlen,
                         uint8_t *out, int outlen )
{
    /* HkdfLabel = length(2) || "tls13 " + label <1 byte len> || context <1 byte len>
       then T(i) = HMAC( secret, T(i-1) || HkdfLabel || i ) */
    uint8_t msg[48 + 2 + 1 + 6 + TLS13_LABEL_MAX + 1 + TLS13_CTX_MAX + 1];
    uint8_t t[48];
    int llen = (int)strlen( label ), info, ilen, done = 0, tlen = 0, n;
    uint8_t counter = 1;

    if ( llen > TLS13_LABEL_MAX || ctxlen < 0 || ctxlen > TLS13_CTX_MAX ||
         outlen <= 0 || outlen > 255 * hlen ) {
        memset( out, 0, outlen > 0 ? outlen : 0 );  /* a caller's mistake: no key */
        return;
    }
    /* the info part sits after room for T(i-1) */
    info = 48;
    msg[info + 0] = (uint8_t)( outlen >> 8 );
    msg[info + 1] = (uint8_t)outlen;
    msg[info + 2] = (uint8_t)( 6 + llen );
    memcpy( msg + info + 3, "tls13 ", 6 );
    memcpy( msg + info + 9, label, llen );
    msg[info + 9 + llen] = (uint8_t)ctxlen;
    if ( ctxlen > 0 ) memcpy( msg + info + 10 + llen, ctx, ctxlen );
    ilen = 10 + llen + ctxlen;

    while ( done < outlen ) {
        /* T(i-1) goes just in front of the info */
        if ( tlen > 0 ) memcpy( msg + info - tlen, t, tlen );
        msg[info + ilen] = counter++;
        hmac_by_len( hlen, secret, hlen, msg + info - tlen, tlen + ilen + 1, t );
        tlen = hlen;
        n = outlen - done;
        if ( n > hlen ) n = hlen;
        memcpy( out + done, t, n );
        done += n;
    }
    memset( t, 0, sizeof( t ) );
}
