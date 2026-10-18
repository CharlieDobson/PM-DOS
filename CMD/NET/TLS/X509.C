/* ==========================================================================
 * X509.C - DER walk to pull the RSA {n,e} out of a certificate.  C89.
 * ==========================================================================
 * DER layout we navigate:
 *   Certificate ::= SEQ { tbsCertificate, sigAlg, sigValue }
 *   tbs         ::= SEQ { [0] version?, serial, sig, issuer, validity,
 *                         subject, subjectPublicKeyInfo, ... }
 *   SPKI        ::= SEQ { algorithm SEQ, subjectPublicKey BIT STRING }
 *   BITSTRING   -> 00 || RSAPublicKey
 *   RSAPublicKey::= SEQ { modulus INTEGER, publicExponent INTEGER }
 * ========================================================================== */
#include "x509.h"
#include "sha.h"
#include "sha512.h"
#include "rsa.h"
#include "ecp256.h"
#include "ecp384.h"
#include <string.h>
/* PM-DOS: no <ctype.h> (its table lives in a C library we do not link) */
static int x_lower( int ch ) { return ( ch >= 'A' && ch <= 'Z' ) ? ch + 32 : ch; }

/* A cursor over a DER region. */
typedef struct {
    const unsigned char *p;
    const unsigned char *end;
} der;

/* Read one TLV at the cursor: return its tag, and point (val,vlen) at the
   contents; advance the cursor past the contents.  Returns 0 / -1. */
static int der_tlv(der *d, int *tag, const unsigned char **val, int *vlen)
{
    const unsigned char *p = d->p;
    int len, nlen;

    if (p + 2 > d->end)
        return -1;
    *tag = *p++;

    if ((*p & 0x80) == 0) {                 /* short form */
        len = *p++;
    } else {                                /* long form */
        nlen = *p++ & 0x7F;
        if (nlen == 0 || nlen > 3 || p + nlen > d->end)
            return -1;
        len = 0;
        while (nlen-- > 0)
            len = (len << 8) | *p++;
    }
    if (len < 0 || p + len > d->end)
        return -1;

    *val = p;
    *vlen = len;
    d->p = p + len;
    return 0;
}

/* Read a TLV expected to be constructed and descend into it. */
static int der_into(der *d, int wantTag, der *inner)
{
    int tag, vl;
    const unsigned char *v;
    if (der_tlv(d, &tag, &v, &vl) != 0 || tag != wantTag)
        return -1;
    inner->p = v;
    inner->end = v + vl;
    return 0;
}

/* Copy an INTEGER's magnitude (stripping a single leading 0x00) into buf. */
static int der_int(der *d, unsigned char *buf, int *buflen)
{
    int tag, vl;
    const unsigned char *v;
    if (der_tlv(d, &tag, &v, &vl) != 0 || tag != 0x02)      /* INTEGER */
        return X509_ERR_PARSE;
    while (vl > 1 && v[0] == 0x00) { v++; vl--; }            /* strip sign pad */
    if (vl > *buflen)
        return X509_ERR_TOOBIG;
    {
        int i;
        for (i = 0; i < vl; i++)
            buf[i] = v[i];
    }
    *buflen = vl;
    return X509_OK;
}

int x509_extract_rsa_pubkey(const unsigned char *cert, int certlen,
                            unsigned char *n, int *nlen,
                            unsigned char *e, int *elen)
{
    der top, tbs, spki, rsapk, rsa;
    const unsigned char *v, *mark;
    int tag, vl, i, rc;

    top.p = cert; top.end = cert + certlen;

    /* Certificate SEQ -> tbsCertificate SEQ */
    if (der_into(&top, 0x30, &top) != 0)          return X509_ERR_PARSE;
    if (der_into(&top, 0x30, &tbs) != 0)          return X509_ERR_PARSE;

    /* Optional [0] EXPLICIT version. */
    mark = tbs.p;
    if (der_tlv(&tbs, &tag, &v, &vl) != 0)        return X509_ERR_PARSE;
    if (tag != 0xA0)
        tbs.p = mark;                             /* no version: rewind */

    /* Skip serialNumber, signature, issuer, validity, subject (5 fields). */
    for (i = 0; i < 5; i++)
        if (der_tlv(&tbs, &tag, &v, &vl) != 0)    return X509_ERR_PARSE;

    /* subjectPublicKeyInfo SEQ */
    if (der_into(&tbs, 0x30, &spki) != 0)         return X509_ERR_PARSE;
    /* algorithm SEQ (skip) */
    if (der_tlv(&spki, &tag, &v, &vl) != 0 || tag != 0x30) return X509_ERR_PARSE;
    /* subjectPublicKey BIT STRING */
    if (der_tlv(&spki, &tag, &v, &vl) != 0 || tag != 0x03) return X509_ERR_PARSE;
    if (vl < 1 || v[0] != 0x00)                   return X509_ERR_NOTRSA;

    /* BIT STRING contents (after the unused-bits byte) = RSAPublicKey DER. */
    rsapk.p = v + 1; rsapk.end = v + vl;
    if (der_into(&rsapk, 0x30, &rsa) != 0)        return X509_ERR_PARSE;

    rc = der_int(&rsa, n, nlen);                  /* modulus */
    if (rc != X509_OK) return rc;
    rc = der_int(&rsa, e, elen);                  /* publicExponent */
    if (rc != X509_OK) return rc;

    return X509_OK;
}

/* ==========================================================================
 * Signature verification, validity, hostname
 * ========================================================================== */
static const unsigned char OID_SHA256RSA[11] =
    { 0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b };

/* OID contents (the bytes after tag+len), for matching der_tlv values. */
static const unsigned char OIDB_SHA256RSA[9] =
    { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b };
static const unsigned char OIDB_ECDSA_SHA256[8] =
    { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02 };
static const unsigned char OIDB_ECDSA_SHA384[8] =
    { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03 };
static const unsigned char OIDB_RSA_PSS[9] =
    { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0a };
static const unsigned char OIDB_RSA_ENC[9] =
    { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01 };
static const unsigned char OIDB_EC_PUBKEY[7] =
    { 0x2a,0x86,0x48,0xce,0x3d,0x02,0x01 };
static const unsigned char OIDB_SECP256R1[8] =
    { 0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07 };
static const unsigned char OIDB_SECP384R1[5] =
    { 0x2b,0x81,0x04,0x00,0x22 };

/* Extract the subject public key (RSA {n,e} or EC-P256 point). */
int x509_get_pubkey(const unsigned char *cert, int certlen, x509_pubkey *out)
{
    der top, tbs, spki, alg, rsapk, rsa;
    const unsigned char *v, *mark, *oid;
    int tag, vl, i, rc, oidlen;

    top.p = cert; top.end = cert + certlen;
    if (der_into(&top, 0x30, &top) != 0)  return X509_ERR_PARSE;
    if (der_into(&top, 0x30, &tbs) != 0)  return X509_ERR_PARSE;
    mark = tbs.p;
    if (der_tlv(&tbs, &tag, &v, &vl) != 0) return X509_ERR_PARSE;
    if (tag != 0xA0) tbs.p = mark;                          /* optional version */
    for (i = 0; i < 5; i++)                                 /* skip 5 fields */
        if (der_tlv(&tbs, &tag, &v, &vl) != 0) return X509_ERR_PARSE;

    if (der_into(&tbs, 0x30, &spki) != 0) return X509_ERR_PARSE;
    if (der_into(&spki, 0x30, &alg) != 0) return X509_ERR_PARSE;   /* algorithm */
    if (der_tlv(&alg, &tag, &oid, &oidlen) != 0 || tag != 0x06) return X509_ERR_PARSE;
    if (der_tlv(&spki, &tag, &v, &vl) != 0 || tag != 0x03) return X509_ERR_PARSE; /* BIT STRING */
    if (vl < 1 || v[0] != 0x00) return X509_ERR_PARSE;

    if (oidlen == 9 && memcmp(oid, OIDB_RSA_ENC, 9) == 0) {
        out->type = X509_KEY_RSA;
        rsapk.p = v + 1; rsapk.end = v + vl;
        if (der_into(&rsapk, 0x30, &rsa) != 0) return X509_ERR_PARSE;
        out->nlen = (int)sizeof(out->n); out->elen = (int)sizeof(out->e);
        rc = der_int(&rsa, out->n, &out->nlen); if (rc != X509_OK) return rc;
        rc = der_int(&rsa, out->e, &out->elen); if (rc != X509_OK) return rc;
        return X509_OK;
    }
    if (oidlen == 7 && memcmp(oid, OIDB_EC_PUBKEY, 7) == 0) {
        const unsigned char *curve;
        int curvelen, ptlen = vl - 1;                      /* strip BIT STRING pad */
        if (der_tlv(&alg, &tag, &curve, &curvelen) != 0 || tag != 0x06)
            return X509_ERR_PARSE;                          /* namedCurve OID */
        if (ptlen < 1 || v[1] != 0x04) return X509_ERR_NOTRSA;  /* uncompressed only */
        if (curvelen == 8 && memcmp(curve, OIDB_SECP256R1, 8) == 0 && ptlen == 65) {
            out->type = X509_KEY_EC256;
            memcpy(out->point, v + 1, 65);
            return X509_OK;
        }
        if (curvelen == 5 && memcmp(curve, OIDB_SECP384R1, 5) == 0 && ptlen == 97) {
            out->type = X509_KEY_EC384;
            memcpy(out->point, v + 1, 97);
            return X509_OK;
        }
        return X509_ERR_NOTRSA;                              /* unsupported curve */
    }
    return X509_ERR_NOTRSA;                                 /* unsupported key type */
}

/* Cursor over the children of the outer Certificate SEQUENCE. */
static int cert_body(const unsigned char *cert, int certlen, der *body)
{
    der top;
    top.p = cert; top.end = cert + certlen;
    return der_into(&top, 0x30, body);
}

/* Cursor over the children of tbsCertificate, positioned past the optional
   [0] version field. */
static int tbs_after_version(const unsigned char *cert, int certlen, der *tbs)
{
    der body;
    const unsigned char *mark, *v;
    int tag, vl;
    if (cert_body(cert, certlen, &body) != 0) return -1;
    if (der_into(&body, 0x30, tbs) != 0) return -1;     /* into tbsCertificate */
    mark = tbs->p;
    if (der_tlv(tbs, &tag, &v, &vl) != 0) return -1;
    if (tag != 0xA0) tbs->p = mark;                     /* no version: rewind */
    return 0;
}

int x509_verify_signed_by(const unsigned char *cert, int certlen,
                          const unsigned char *n, int nlen,
                          const unsigned char *e, int elen)
{
    der body;
    const unsigned char *tbs, *v, *sig;
    int tag, vl, tbslen, siglen;
    unsigned char hash[32];
    rsa_pubkey k;

    if (cert_body(cert, certlen, &body) != 0) return X509_ERR_PARSE;

    /* tbsCertificate (raw element = what gets hashed) */
    tbs = body.p;
    if (der_tlv(&body, &tag, &v, &vl) != 0 || tag != 0x30) return X509_ERR_PARSE;
    tbslen = (int)((v + vl) - tbs);

    /* signatureAlgorithm - require sha256WithRSAEncryption */
    if (der_tlv(&body, &tag, &v, &vl) != 0 || tag != 0x30) return X509_ERR_PARSE;
    if (vl < 11 || memcmp(v, OID_SHA256RSA, 11) != 0) return X509_ERR_NOTRSA;

    /* signatureValue BIT STRING (skip the unused-bits byte) */
    if (der_tlv(&body, &tag, &v, &vl) != 0 || tag != 0x03) return X509_ERR_PARSE;
    if (vl < 1 || v[0] != 0x00) return X509_ERR_PARSE;
    sig = v + 1; siglen = vl - 1;

    sha256(tbs, (unsigned int)tbslen, hash);
    rsa_pubkey_init(&k, n, nlen, e, elen);
    if (rsa_pkcs1_verify_sha256(&k, hash, sig, siglen) != 0)
        return X509_ERR_SIG;
    return X509_OK;
}

int x509_verify_signed(const unsigned char *cert, int certlen,
                       const x509_pubkey *issuer)
{
    der body, alg;
    const unsigned char *tbs, *v, *sig, *oid;
    int tag, vl, tbslen, siglen, oidlen, is_rsa, is_ecdsa256, is_ecdsa384, is_pss;
    unsigned char hash[32];

    if (cert_body(cert, certlen, &body) != 0) return X509_ERR_PARSE;

    tbs = body.p;                                           /* raw tbsCertificate */
    if (der_tlv(&body, &tag, &v, &vl) != 0 || tag != 0x30) return X509_ERR_PARSE;
    tbslen = (int)((v + vl) - tbs);

    if (der_into(&body, 0x30, &alg) != 0) return X509_ERR_PARSE;   /* signatureAlgorithm */
    if (der_tlv(&alg, &tag, &oid, &oidlen) != 0 || tag != 0x06) return X509_ERR_PARSE;
    is_rsa      = (oidlen == 9 && memcmp(oid, OIDB_SHA256RSA,    9) == 0);
    is_ecdsa256 = (oidlen == 8 && memcmp(oid, OIDB_ECDSA_SHA256, 8) == 0);
    is_ecdsa384 = (oidlen == 8 && memcmp(oid, OIDB_ECDSA_SHA384, 8) == 0);
    is_pss      = (oidlen == 9 && memcmp(oid, OIDB_RSA_PSS,      9) == 0);

    if (der_tlv(&body, &tag, &v, &vl) != 0 || tag != 0x03) return X509_ERR_PARSE; /* sigValue */
    if (vl < 1 || v[0] != 0x00) return X509_ERR_PARSE;
    sig = v + 1; siglen = vl - 1;

    sha256(tbs, (unsigned int)tbslen, hash);

    if (is_rsa && issuer->type == X509_KEY_RSA) {
        rsa_pubkey k;
        rsa_pubkey_init(&k, issuer->n, issuer->nlen, issuer->e, issuer->elen);
        return (rsa_pkcs1_verify_sha256(&k, hash, sig, siglen) == 0)
               ? X509_OK : X509_ERR_SIG;
    }
    if (is_pss && issuer->type == X509_KEY_RSA) {
        /* RSASSA-PSS; assume the common SHA-256/MGF1-SHA256/salt=32 params. */
        rsa_pubkey k;
        rsa_pubkey_init(&k, issuer->n, issuer->nlen, issuer->e, issuer->elen);
        return (rsa_pss_verify_sha256(&k, hash, sig, siglen) == 0)
               ? X509_OK : X509_ERR_SIG;
    }
    if ((is_ecdsa256 || is_ecdsa384) &&
        (issuer->type == X509_KEY_EC256 || issuer->type == X509_KEY_EC384)) {
        der sd, inner;
        const unsigned char *rp, *sp, *dg;
        int rl, sl, dglen;
        unsigned char h384[48];
        sd.p = sig; sd.end = sig + siglen;
        if (der_into(&sd, 0x30, &inner) != 0) return X509_ERR_PARSE;
        if (der_tlv(&inner, &tag, &rp, &rl) != 0 || tag != 0x02) return X509_ERR_PARSE;
        if (der_tlv(&inner, &tag, &sp, &sl) != 0 || tag != 0x02) return X509_ERR_PARSE;
        while (rl > 1 && rp[0] == 0x00) { rp++; rl--; }
        while (sl > 1 && sp[0] == 0x00) { sp++; sl--; }
        if (is_ecdsa384) { sha384(tbs, (unsigned int)tbslen, h384); dg = h384; dglen = 48; }
        else             { dg = hash; dglen = 32; }
        if (issuer->type == X509_KEY_EC384)
            return (ecdsa_verify_p384(issuer->point, dg, dglen, rp, rl, sp, sl) == 0)
                   ? X509_OK : X509_ERR_SIG;
        /* EC256 issuer: ecdsa_verify_p256 takes z = leftmost 256 bits of dg */
        return (ecdsa_verify_p256(issuer->point, dg, rp, rl, sp, sl) == 0)
               ? X509_OK : X509_ERR_SIG;
    }
    return X509_ERR_SIG;                                    /* alg/key mismatch */
}

/* Normalize a UTCTime/GeneralizedTime into "YYYYMMDDHHMMSS". */
static int norm_time(int tag, const unsigned char *v, int vl, char *out)
{
    if (tag == 0x17) {                          /* UTCTime YYMMDDHHMMSSZ */
        int yy;
        if (vl < 12) return -1;
        yy = (v[0]-'0')*10 + (v[1]-'0');
        out[0] = (yy >= 50) ? '1' : '2';
        out[1] = (yy >= 50) ? '9' : '0';
        memcpy(out + 2, v, 12);
        out[14] = '\0';
    } else if (tag == 0x18) {                   /* GeneralizedTime YYYYMMDD... */
        if (vl < 14) return -1;
        memcpy(out, v, 14);
        out[14] = '\0';
    } else {
        return -1;
    }
    return 0;
}

int x509_get_validity(const unsigned char *cert, int certlen,
                      char notBefore[15], char notAfter[15])
{
    der tbs, val;
    const unsigned char *v;
    int tag, vl, i;

    if (tbs_after_version(cert, certlen, &tbs) != 0) return X509_ERR_PARSE;
    for (i = 0; i < 3; i++)                      /* serial, sigAlg, issuer */
        if (der_tlv(&tbs, &tag, &v, &vl) != 0) return X509_ERR_PARSE;
    if (der_into(&tbs, 0x30, &val) != 0) return X509_ERR_PARSE;  /* validity */
    if (der_tlv(&val, &tag, &v, &vl) != 0 || norm_time(tag, v, vl, notBefore))
        return X509_ERR_PARSE;
    if (der_tlv(&val, &tag, &v, &vl) != 0 || norm_time(tag, v, vl, notAfter))
        return X509_ERR_PARSE;
    return X509_OK;
}

int x509_check_validity(const unsigned char *cert, int certlen, const char *now)
{
    char nb[15], na[15];
    if (x509_get_validity(cert, certlen, nb, na) != X509_OK) return X509_ERR_PARSE;
    if (strcmp(now, nb) < 0) return X509_ERR_EXPIRED;   /* not yet valid */
    if (strcmp(now, na) > 0) return X509_ERR_EXPIRED;   /* expired */
    return X509_OK;
}

/* --- hostname matching --- */
static int ci_eq(const unsigned char *a, int alen, const char *b)
{
    int i, bl = (int)strlen(b);
    if (alen != bl) return 0;
    for (i = 0; i < alen; i++)
        if (x_lower(a[i]) != x_lower((unsigned char)b[i])) return 0;
    return 1;
}
static int host_match(const unsigned char *pat, int patlen, const char *host)
{
    if (patlen >= 2 && pat[0] == '*' && pat[1] == '.') {    /* *.example.com */
        const char *dot = strchr(host, '.');
        if (!dot) return 0;
        return ci_eq(pat + 2, patlen - 2, dot + 1);
    }
    return ci_eq(pat, patlen, host);
}
static int parse_ipv4(const char *s, unsigned char ip[4])
{
    int part[4], n = 0, val = 0, dig = 0;
    const char *p = s;
    for (;;) {
        if (*p >= '0' && *p <= '9') { val = val*10 + (*p-'0'); dig++; if (val > 255) return 0; }
        else if (*p == '.' || *p == '\0') {
            if (!dig || n >= 4) return 0;
            part[n++] = val; val = 0; dig = 0;
            if (*p == '\0') break;
        } else return 0;
        p++;
    }
    if (n != 4) return 0;
    ip[0]=(unsigned char)part[0]; ip[1]=(unsigned char)part[1];
    ip[2]=(unsigned char)part[2]; ip[3]=(unsigned char)part[3];
    return 1;
}

/* Cursor over subjectAltName GeneralNames, if present. */
static int get_san(const unsigned char *cert, int certlen, der *san)
{
    der tbs, exts, ev;
    const unsigned char *v, *mark;
    int tag, vl, i;

    if (tbs_after_version(cert, certlen, &tbs) != 0) return -1;
    for (i = 0; i < 5; i++)                     /* serial,sigAlg,issuer,validity,subject */
        if (der_tlv(&tbs, &tag, &v, &vl) != 0) return -1;
    if (der_tlv(&tbs, &tag, &v, &vl) != 0 || tag != 0x30) return -1;  /* SPKI */
    mark = tbs.p;
    if (der_tlv(&tbs, &tag, &v, &vl) != 0) return -1;
    if (tag != 0xA3) { tbs.p = mark; return -1; }   /* no extensions */

    ev.p = v; ev.end = v + vl;                  /* [3] content */
    if (der_into(&ev, 0x30, &exts) != 0) return -1; /* Extensions SEQ */
    while (exts.p < exts.end) {
        der ext;
        if (der_into(&exts, 0x30, &ext) != 0) return -1;
        if (der_tlv(&ext, &tag, &v, &vl) != 0) return -1;           /* extnID */
        if (tag == 0x06 && vl == 3 && v[0]==0x55 && v[1]==0x1d && v[2]==0x11) {
            if (der_tlv(&ext, &tag, &v, &vl) != 0) return -1;
            if (tag == 0x01)                    /* skip optional critical BOOL */
                if (der_tlv(&ext, &tag, &v, &vl) != 0) return -1;
            if (tag != 0x04) return -1;         /* extnValue OCTET STRING */
            ev.p = v; ev.end = v + vl;
            return der_into(&ev, 0x30, san);    /* GeneralNames SEQ */
        }
    }
    return -1;
}

/* CN from the subject Name. */
static int get_cn(const unsigned char *cert, int certlen, char *out, int outcap)
{
    der tbs, subj;
    const unsigned char *v;
    int tag, vl, i;

    if (tbs_after_version(cert, certlen, &tbs) != 0) return -1;
    for (i = 0; i < 4; i++)                     /* serial,sigAlg,issuer,validity */
        if (der_tlv(&tbs, &tag, &v, &vl) != 0) return -1;
    if (der_into(&tbs, 0x30, &subj) != 0) return -1;    /* subject Name */
    while (subj.p < subj.end) {
        der rdn;
        if (der_into(&subj, 0x31, &rdn) != 0) return -1;    /* RDN SET */
        while (rdn.p < rdn.end) {
            der atv;
            if (der_into(&rdn, 0x30, &atv) != 0) break;     /* ATV SEQ */
            if (der_tlv(&atv, &tag, &v, &vl) != 0) break;   /* type OID */
            if (tag == 0x06 && vl == 3 && v[0]==0x55 && v[1]==0x04 && v[2]==0x03) {
                if (der_tlv(&atv, &tag, &v, &vl) != 0) return -1;   /* value */
                if (vl >= outcap) vl = outcap - 1;
                memcpy(out, v, vl); out[vl] = '\0';
                return 0;
            }
        }
    }
    return -1;
}

int x509_check_host(const unsigned char *cert, int certlen, const char *host)
{
    der san;
    unsigned char ip[4];
    int isip = parse_ipv4(host, ip);
    char cn[256];

    if (get_san(cert, certlen, &san) == 0) {    /* SAN present: authoritative */
        int tag, vl;
        const unsigned char *v;
        while (san.p < san.end) {
            if (der_tlv(&san, &tag, &v, &vl) != 0) break;
            if (tag == 0x82 && !isip && host_match(v, vl, host)) return X509_OK;
            if (tag == 0x87 && isip && vl == 4 && memcmp(v, ip, 4) == 0) return X509_OK;
        }
        return X509_ERR_HOST;                   /* SAN present, no match */
    }
    if (get_cn(cert, certlen, cn, sizeof(cn)) == 0 &&
        host_match((const unsigned char *)cn, (int)strlen(cn), host))
        return X509_OK;
    return X509_ERR_HOST;
}
