/* ==========================================================================
 * PRF.C - TLS 1.2 PRF = P_SHA256 (RFC 5246).  Portable C89.
 * ========================================================================== */
#include "prf.h"
#include "sha.h"
#include "sha512.h"
#include <string.h>
#include <stdlib.h>

void tls12_prf(const unsigned char *secret, int secretlen,
               const char *label,
               const unsigned char *seed, int seedlen,
               unsigned char *out, int outlen)
{
    int   llen = (int)strlen(label);
    int   lslen = llen + seedlen;      /* the P_hash "seed" = label || seed */
    unsigned char *ls;                 /* label || seed                     */
    unsigned char *tmp;                /* A(i) || label || seed             */
    unsigned char A[SHA256_DIGEST_LEN];
    unsigned char Anew[SHA256_DIGEST_LEN];
    unsigned char block[SHA256_DIGEST_LEN];
    int   done = 0;

    if (outlen <= 0)
        return;

    ls  = (unsigned char *)malloc(lslen);
    tmp = (unsigned char *)malloc(SHA256_DIGEST_LEN + lslen);
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
void tls12_prf_sha384(const unsigned char *secret, int secretlen,
                      const char *label,
                      const unsigned char *seed, int seedlen,
                      unsigned char *out, int outlen)
{
    int   llen = (int)strlen(label);
    int   lslen = llen + seedlen;
    unsigned char *ls, *tmp;
    unsigned char A[SHA384_DIGEST_LEN];
    unsigned char Anew[SHA384_DIGEST_LEN];
    unsigned char block[SHA384_DIGEST_LEN];
    int   done = 0;

    if (outlen <= 0)
        return;

    ls  = (unsigned char *)malloc(lslen);
    tmp = (unsigned char *)malloc(SHA384_DIGEST_LEN + lslen);
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
static void hmac_by_len( int hlen, const unsigned char *key, int keylen,
                         const unsigned char *msg, int msglen, unsigned char *out )
{
    if ( hlen == 48 ) {
        hmac_sha384( key, keylen, msg, msglen, out );
    } else {
        hmac_sha256( key, keylen, msg, msglen, out );
    }
}

void tls13_hkdf_extract( int hlen, const unsigned char *salt, int saltlen,
                         const unsigned char *ikm, int ikmlen, unsigned char *out )
{
    unsigned char zeros[48];
    memset( zeros, 0, sizeof( zeros ) );
    if ( salt == NULL || saltlen == 0 ) {
        salt = zeros; saltlen = hlen;
    }
    if ( ikm == NULL || ikmlen == 0 ) {
        ikm = zeros; ikmlen = hlen;
    }
    hmac_by_len( hlen, salt, saltlen, ikm, ikmlen, out );
}

void tls13_expand_label( int hlen, const unsigned char *secret,
                         const char *label,
                         const unsigned char *ctx, int ctxlen,
                         unsigned char *out, int outlen )
{
    /* HkdfLabel = length(2) || "tls13 " + label <1 byte len> || context <1 byte len>
       then T(i) = HMAC( secret, T(i-1) || HkdfLabel || i ) */
    unsigned char msg[48 + 2 + 1 + 6 + TLS13_LABEL_MAX + 1 + TLS13_CTX_MAX + 1];
    unsigned char t[48];
    int llen = (int)strlen( label ), info, ilen, done = 0, tlen = 0, n;
    unsigned char counter = 1;

    if ( llen > TLS13_LABEL_MAX || ctxlen < 0 || ctxlen > TLS13_CTX_MAX ||
         outlen <= 0 || outlen > 255 * hlen ) {
        memset( out, 0, outlen > 0 ? outlen : 0 );  /* a caller's mistake: no key */
        return;
    }
    /* the info part sits after room for T(i-1) */
    info = 48;
    msg[info + 0] = (unsigned char)( outlen >> 8 );
    msg[info + 1] = (unsigned char)outlen;
    msg[info + 2] = (unsigned char)( 6 + llen );
    memcpy( msg + info + 3, "tls13 ", 6 );
    memcpy( msg + info + 9, label, llen );
    msg[info + 9 + llen] = (unsigned char)ctxlen;
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
