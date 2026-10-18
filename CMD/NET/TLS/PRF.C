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
