/* ==========================================================================
 * TLSHS.C - TLS 1.2 client handshake, resumable state machine.  C89.
 * ==========================================================================
 * Flow (RSA kx, no client cert, tickets off):
 *   -> ClientHello
 *   <- ServerHello, Certificate, ServerHelloDone
 *   -> ClientKeyExchange, ChangeCipherSpec, Finished
 *   <- ChangeCipherSpec, Finished
 *
 * The engine never blocks: tls_hs_pump() advances as far as the currently
 * buffered input allows, queues any output, and returns WANT_READ/DONE/ERROR.
 * tls_client_handshake() is a thin blocking wrapper over the same engine, so
 * there is a single code path.  Application data flows CBC-protected (TLSREC).
 * ========================================================================== */
#include "tlshs.h"
#include "tlsrec.h"
#include "sha.h"
#include "sha512.h"
#include "prf.h"
#include "rsa.h"
#include "x509.h"
#include "ecp256.h"
#include <string.h>
#include <stdlib.h>

#define SUITE_RSA_AES128_CBC_SHA   0x002F
#define SUITE_ECDHE_RSA_AES128_SHA 0xC013
#define SUITE_ECDHE_RSA_AES256_SHA 0xC014
#define SUITE_ECDHE_RSA_AES128_GCM 0xC02F
#define SUITE_ECDHE_RSA_AES256_GCM 0xC030
#define IS_ECDHE(c) ((c)->suite == SUITE_ECDHE_RSA_AES128_SHA || \
                     (c)->suite == SUITE_ECDHE_RSA_AES256_SHA || \
                     (c)->suite == SUITE_ECDHE_RSA_AES128_GCM || \
                     (c)->suite == SUITE_ECDHE_RSA_AES256_GCM)

/* Suite properties: AEAD vs CBC, the PRF/transcript hash, and the AES key size.
   Only ECDHE_RSA_AES256_GCM_SHA384 uses the SHA-384 PRF; the rest use SHA-256. */
static int suite_is_gcm(int s)
{ return s == SUITE_ECDHE_RSA_AES128_GCM || s == SUITE_ECDHE_RSA_AES256_GCM; }
static int suite_is_sha384(int s)
{ return s == SUITE_ECDHE_RSA_AES256_GCM; }
static int suite_keylen(int s)
{ return (s == SUITE_ECDHE_RSA_AES256_GCM || s == SUITE_ECDHE_RSA_AES256_SHA) ? 32 : 16; }

#define REC_MAX     18432               /* max record body we accept */
#define HS_BUF      20000               /* handshake reassembly buffer */
#define MAX_CHAIN   6                   /* max certs in the presented chain */
#define CERT_MAX    8192                /* max size of one DER certificate */

/* Handshake message types */
#define HS_CLIENT_HELLO         1
#define HS_SERVER_HELLO         2
#define HS_CERTIFICATE          11
#define HS_SERVER_KEY_EXCHANGE  12
#define HS_CERTIFICATE_REQUEST  13
#define HS_SERVER_HELLO_DONE    14
#define HS_CLIENT_KEY_EXCHANGE  16
#define HS_FINISHED             20

/* Engine states */
enum { HS_INIT, HS_FLIGHT, HS_EXPECT_CCS, HS_EXPECT_FIN, HS_DONE, HS_FAILED };

struct tls_conn {
    nst_rng      *rng;
    SHA256_CTX    hh;                   /* running handshake hash (SHA-256)  */
    SHA384_CTX    hh384;               /* running handshake hash (SHA-384)  */
    unsigned char crand[32];
    unsigned char srand[32];
    rsa_pubkey    skey;
    int           have_key;
    int           suite;                /* negotiated cipher suite          */
    unsigned char eph_priv[32];         /* our ephemeral ECDH private       */
    unsigned char eph_pub[65];          /* our ephemeral ECDH public        */
    unsigned char srv_point[65];        /* server ephemeral ECDH public     */
    int           have_srv_point;
    unsigned char master[48];
    tls_keys      kw;                   /* client write */
    tls_keys      kr;                   /* client read (server write) */

    unsigned char hbuf[HS_BUF];         /* reassembled handshake bytes */
    int           hlen, hpos;
    unsigned char rdrec[REC_MAX + 8];   /* scratch for blocking app-phase read */

    /* certificate validation */
    unsigned char chain[MAX_CHAIN][CERT_MAX];   /* presented chain, leaf first */
    int           chainlen[MAX_CHAIN];
    int           nchain;
    int           verify;
    char          vhost[256];
    char          vnow[15];
    const unsigned char *vca;
    int           vcalen;
    int           verr;

    /* resumable engine buffers */
    int           hs_state;
    char          hs_host[256];
    unsigned char *ibuf;                /* inbound record bytes (growable) */
    int           ilen, icap;
    unsigned char obuf[2048];           /* outbound handshake bytes */
    int           olen, opos;
};

/* --- blocking I/O helpers (app phase + the blocking wrapper) -------------- */
static int send_all(tls_send_fn s, void *ctx, const unsigned char *b, int len)
{
    int off = 0, n;
    while (off < len) { n = s(ctx, b + off, len - off); if (n <= 0) return -1; off += n; }
    return 0;
}
static int read_raw_record(tls_conn *c, tls_recv_fn recv, void *ctx, int *reclen)
{
    int off = 0, n, l;
    while (off < 5) { n = recv(ctx, c->rdrec + off, 5 - off); if (n <= 0) return -1; off += n; }
    l = (c->rdrec[3] << 8) | c->rdrec[4];
    if (l < 0 || l > REC_MAX) return -1;
    while (off < 5 + l) { n = recv(ctx, c->rdrec + off, 5 + l - off); if (n <= 0) return -1; off += n; }
    *reclen = 5 + l;
    return c->rdrec[0];
}

/* --- pure message helpers ------------------------------------------------- */
static int build_client_hello(tls_conn *c, const char *host, unsigned char *b)
{
    int i = 0, hlen = host ? (int)strlen(host) : 0;
    int extlenpos;
    b[i++] = 3; b[i++] = 3;
    rng_bytes(c->rng, c->crand, 32);
    memcpy(b + i, c->crand, 32); i += 32;
    b[i++] = 0;                                 /* session_id length */
    b[i++] = 0; b[i++] = 10;                    /* cipher_suites length = 10 */
    b[i++] = 0xC0; b[i++] = 0x30;               /* ECDHE_RSA_AES_256_GCM_SHA384   */
    b[i++] = 0xC0; b[i++] = 0x2F;               /* ECDHE_RSA_AES_128_GCM_SHA256   */
    b[i++] = 0xC0; b[i++] = 0x14;               /* ECDHE_RSA_WITH_AES_256_CBC_SHA */
    b[i++] = 0xC0; b[i++] = 0x13;               /* ECDHE_RSA_WITH_AES_128_CBC_SHA */
    b[i++] = 0x00; b[i++] = 0x2F;               /* RSA_WITH_AES_128_CBC_SHA       */
    b[i++] = 1; b[i++] = 0;                     /* null compression */

    extlenpos = i; i += 2;                      /* fill total ext length later */

    if (hlen > 0 && hlen < 256) {               /* server_name (SNI) */
        int list = 1 + 2 + hlen, sni = 2 + list;
        b[i++] = 0x00; b[i++] = 0x00;
        b[i++] = (unsigned char)(sni >> 8);  b[i++] = (unsigned char)sni;
        b[i++] = (unsigned char)(list >> 8); b[i++] = (unsigned char)list;
        b[i++] = 0x00;
        b[i++] = (unsigned char)(hlen >> 8); b[i++] = (unsigned char)hlen;
        memcpy(b + i, host, hlen); i += hlen;
    }
    /* supported_groups: secp256r1 (0x0017) */
    b[i++]=0x00; b[i++]=0x0A; b[i++]=0x00; b[i++]=0x04;
    b[i++]=0x00; b[i++]=0x02; b[i++]=0x00; b[i++]=0x17;
    /* ec_point_formats: uncompressed (0) */
    b[i++]=0x00; b[i++]=0x0B; b[i++]=0x00; b[i++]=0x02; b[i++]=0x01; b[i++]=0x00;
    /* signature_algorithms: rsa_pkcs1_sha256 (0x0401) */
    b[i++]=0x00; b[i++]=0x0D; b[i++]=0x00; b[i++]=0x04;
    b[i++]=0x00; b[i++]=0x02; b[i++]=0x04; b[i++]=0x01;

    { int extlen = i - extlenpos - 2;
      b[extlenpos]   = (unsigned char)(extlen >> 8);
      b[extlenpos+1] = (unsigned char)extlen; }
    return i;
}

static int parse_server_hello(tls_conn *c, const unsigned char *b, int blen)
{
    int sid, off, suite;
    if (blen < 38) return -1;
    memcpy(c->srand, b + 2, 32);
    sid = b[34];
    off = 35 + sid;
    if (off + 3 > blen) return -1;
    suite = (b[off] << 8) | b[off + 1];
    if (suite != SUITE_RSA_AES128_CBC_SHA &&
        suite != SUITE_ECDHE_RSA_AES128_SHA &&
        suite != SUITE_ECDHE_RSA_AES256_SHA &&
        suite != SUITE_ECDHE_RSA_AES128_GCM &&
        suite != SUITE_ECDHE_RSA_AES256_GCM)
        return -1;
    c->suite = suite;
    return 0;
}

/* Parse a ServerKeyExchange for ECDHE_RSA: named-curve params + RSA signature
   over client_random || server_random || ECParams.  0 ok / -1 bad. */
static int parse_ecdhe_ske(tls_conn *c, const unsigned char *b, int blen)
{
    int point_len, params_len, sig_hash, sig_alg, sig_len, off;
    unsigned char hash[32];
    SHA256_CTX sh;

    if (blen < 4) return -1;
    if (b[0] != 3) return -1;                       /* curve_type = named_curve */
    if (((b[1] << 8) | b[2]) != 0x0017) return -1;  /* secp256r1 */
    point_len = b[3];
    if (point_len != 65 || 4 + point_len > blen) return -1;
    memcpy(c->srv_point, b + 4, 65);
    c->have_srv_point = 1;

    params_len = 4 + point_len;                     /* ECParams length */
    off = params_len;
    if (off + 4 > blen) return -1;
    sig_hash = b[off]; sig_alg = b[off+1];          /* SignatureAndHashAlgorithm */
    off += 2;
    sig_len = (b[off] << 8) | b[off+1]; off += 2;
    if (off + sig_len > blen) return -1;
    if (sig_alg != 1 || sig_hash != 4) return -1;   /* rsa_pkcs1_sha256 only */
    if (!c->have_key) return -1;

    sha256_init(&sh);
    sha256_update(&sh, c->crand, 32);
    sha256_update(&sh, c->srand, 32);
    sha256_update(&sh, b, params_len);
    sha256_final(&sh, hash);
    if (rsa_pkcs1_verify_sha256(&c->skey, hash, b + off, sig_len) != 0) return -1;
    return 0;
}

static int parse_certificate(tls_conn *c, const unsigned char *b, int blen)
{
    int total, off;
    unsigned char nbuf[512], ebuf[16];
    int nlen, elen;

    if (blen < 3) return -1;
    total = (b[0] << 16) | (b[1] << 8) | b[2];      /* certificate_list length */
    off = 3;
    c->nchain = 0;

    while (off + 3 <= blen && off - 3 < total) {    /* walk each cert entry */
        int clen = (b[off] << 16) | (b[off+1] << 8) | b[off+2];
        off += 3;
        if (clen <= 0 || off + clen > blen) break;
        if (c->nchain < MAX_CHAIN && clen <= CERT_MAX) {
            memcpy(c->chain[c->nchain], b + off, clen);
            c->chainlen[c->nchain] = clen;
            c->nchain++;
        }
        off += clen;
    }
    if (c->nchain == 0) return -1;

    /* leaf public key drives the RSA key exchange */
    nlen = sizeof(nbuf); elen = sizeof(ebuf);
    if (x509_extract_rsa_pubkey(c->chain[0], c->chainlen[0], nbuf, &nlen, ebuf, &elen) != X509_OK)
        return -1;
    rsa_pubkey_init(&c->skey, nbuf, nlen, ebuf, elen);
    c->have_key = 1;
    return 0;
}

void tls_set_verify(tls_conn *c, const char *host, const char *now14,
                    const unsigned char *ca_der, int ca_len)
{
    c->verify = 1;
    c->vhost[0] = '\0';
    if (host) { strncpy(c->vhost, host, 255); c->vhost[255] = '\0'; }
    c->vnow[0] = '\0';
    if (now14) { strncpy(c->vnow, now14, 14); c->vnow[14] = '\0'; }
    c->vca = ca_der; c->vcalen = ca_len;
}
int tls_verify_error(tls_conn *c) { return c ? c->verr : 0; }

/* Walk one DER TLV in the CA bundle (concatenated DER certs).  Advances pos
   past the cert; returns 1 and sets cert + certlen, or 0 at the end. */
static int ca_next(const unsigned char *bundle, int len, int *pos,
                   const unsigned char **cert, int *certlen)
{
    int p = *pos, l, hdr;
    if (p + 2 > len || bundle[p] != 0x30) return 0;     /* need a SEQUENCE */
    l = bundle[p+1]; hdr = 2;
    if (l & 0x80) {
        int nb = l & 0x7F, k;
        if (nb == 0 || nb > 4 || p + 2 + nb > len) return 0;
        l = 0;
        for (k = 0; k < nb; k++) l = (l << 8) | bundle[p+2+k];
        hdr = 2 + nb;
    }
    if (p + hdr + l > len) return 0;
    *cert = bundle + p;
    *certlen = hdr + l;
    *pos = p + hdr + l;
    return 1;
}

/* Is 'top' signed by any CA in the trust bundle? 0 = anchored, -1 = not.
   Handles RSA and ECDSA-P256 CA keys / signatures. */
static int anchored_by_ca(tls_conn *c, const unsigned char *top, int toplen)
{
    const unsigned char *ca;
    int pos = 0, calen;
    x509_pubkey ck;
    while (ca_next(c->vca, c->vcalen, &pos, &ca, &calen)) {
        if (x509_get_pubkey(ca, calen, &ck) != X509_OK)
            continue;
        if (x509_verify_signed(top, toplen, &ck) == X509_OK)
            return 0;
    }
    return -1;
}

static int validate_cert(tls_conn *c)
{
    int i;
    if (!c->verify) return 0;
    if (c->nchain <= 0) { c->verr = X509_ERR_PARSE; return -1; }

    /* hostname identity on the leaf */
    if (c->vhost[0] &&
        x509_check_host(c->chain[0], c->chainlen[0], c->vhost) != X509_OK) {
        c->verr = X509_ERR_HOST; return -1;
    }
    /* validity window on every presented cert */
    if (c->vnow[0]) {
        for (i = 0; i < c->nchain; i++)
            if (x509_check_validity(c->chain[i], c->chainlen[i], c->vnow) != X509_OK) {
                c->verr = X509_ERR_EXPIRED; return -1;
            }
    }
    /* signature chain: cert[i] signed by cert[i+1], then top anchored to a CA.
       Each link may be RSA or ECDSA-P256 (x509_verify_signed dispatches). */
    if (c->vca != NULL) {
        for (i = 0; i + 1 < c->nchain; i++) {
            x509_pubkey ik;
            if (x509_get_pubkey(c->chain[i+1], c->chainlen[i+1], &ik) != X509_OK) {
                c->verr = X509_ERR_PARSE; return -1;
            }
            if (x509_verify_signed(c->chain[i], c->chainlen[i], &ik) != X509_OK) {
                c->verr = X509_ERR_SIG; return -1;
            }
        }
        if (anchored_by_ca(c, c->chain[c->nchain-1], c->chainlen[c->nchain-1]) != 0) {
            c->verr = X509_ERR_SIG; return -1;
        }
    }
    return 0;
}

static void derive_keys(tls_conn *c, const unsigned char *premaster, int pmlen)
{
    unsigned char seed[64], kb[136];
    int sha384s = suite_is_sha384(c->suite);
    int kl = suite_keylen(c->suite);

    memcpy(seed, c->crand, 32); memcpy(seed + 32, c->srand, 32);
    if (sha384s) tls12_prf_sha384(premaster, pmlen, "master secret", seed, 64, c->master, 48);
    else         tls12_prf(premaster, pmlen, "master secret", seed, 64, c->master, 48);

    memcpy(seed, c->srand, 32); memcpy(seed + 32, c->crand, 32);
    if (suite_is_gcm(c->suite)) {
        /* AEAD key block: cwk(kl) swk(kl) civ(4) siv(4) - no MAC keys */
        int kblen = 2 * kl + 8;
        if (sha384s) tls12_prf_sha384(c->master, 48, "key expansion", seed, 64, kb, kblen);
        else         tls12_prf(c->master, 48, "key expansion", seed, 64, kb, kblen);
        tls_keys_init_gcm(&c->kw, kb,      kl, kb + 2*kl);       /* client write */
        tls_keys_init_gcm(&c->kr, kb + kl, kl, kb + 2*kl + 4);   /* server write */
    } else {
        /* CBC key block: cMAC(20) sMAC(20) cKey(kl) sKey(kl) - SHA-256 PRF */
        int kblen = 40 + 2 * kl;
        tls12_prf(c->master, 48, "key expansion", seed, 64, kb, kblen);
        tls_keys_init(&c->kw, kb,      kb + 40,      kl);
        tls_keys_init(&c->kr, kb + 20, kb + 40 + kl, kl);
    }
}

static void handshake_hash(tls_conn *c, unsigned char out[32])
{
    SHA256_CTX tmp = c->hh;
    sha256_final(&tmp, out);
}
/* SHA-384 transcript snapshot (for the AES_256_GCM_SHA384 Finished/PRF). */
static void handshake_hash384(tls_conn *c, unsigned char out[48])
{
    SHA384_CTX tmp = c->hh384;
    sha384_final(&tmp, out);
}
/* Fold a handshake message into both running transcript hashes. */
static void transcript_update(tls_conn *c, const unsigned char *p, int n)
{
    sha256_update(&c->hh, p, n);
    sha384_update(&c->hh384, p, n);
}

/* --- resumable engine internals ------------------------------------------ */
static int obuf_put(tls_conn *c, const unsigned char *d, int n)
{
    if (c->olen + n > (int)sizeof(c->obuf)) return -1;
    memcpy(c->obuf + c->olen, d, n); c->olen += n; return 0;
}

/* Build + hash a client handshake message, wrap it in a record, queue it. */
static int queue_handshake(tls_conn *c, int mtype, const unsigned char *body,
                           int blen, int encrypt)
{
    unsigned char msg[1024], rec[1200];
    int mlen, reclen;
    if (4 + blen > (int)sizeof(msg)) return -1;
    msg[0] = (unsigned char)mtype;
    msg[1] = (unsigned char)(blen >> 16);
    msg[2] = (unsigned char)(blen >> 8);
    msg[3] = (unsigned char)blen;
    memcpy(msg + 4, body, blen);
    mlen = 4 + blen;
    transcript_update(c, msg, mlen);
    if (encrypt)
        reclen = tls_protect(&c->kw, TLS_CT_HANDSHAKE, msg, mlen, c->rng, rec, sizeof(rec));
    else
        reclen = tls_plain_record(TLS_CT_HANDSHAKE, msg, mlen, rec, sizeof(rec));
    if (reclen < 0) return -1;
    return obuf_put(c, rec, reclen);
}
static int queue_ccs(tls_conn *c)
{
    unsigned char ccs = 1, rec[16];
    int reclen = tls_plain_record(TLS_CT_CHANGE_CIPHER_SPEC, &ccs, 1, rec, sizeof(rec));
    if (reclen < 0) return -1;
    return obuf_put(c, rec, reclen);
}

/* Complete record at the front of ibuf?  reclen / 0 (need more) / -1 (bad). */
static int have_record(tls_conn *c)
{
    int len;
    if (c->ilen < 5) return 0;
    len = (c->ibuf[3] << 8) | c->ibuf[4];
    if (len > REC_MAX) return -1;
    if (c->ilen < 5 + len) return 0;
    return 5 + len;
}
static void consume_record(tls_conn *c, int rl)
{
    memmove(c->ibuf, c->ibuf + rl, c->ilen - rl);
    c->ilen -= rl;
}

static int hbuf_append(tls_conn *c, const unsigned char *p, int n)
{
    if (c->hpos > 0) {
        memmove(c->hbuf, c->hbuf + c->hpos, c->hlen - c->hpos);
        c->hlen -= c->hpos; c->hpos = 0;
    }
    if (c->hlen + n > HS_BUF) return -1;
    memcpy(c->hbuf + c->hlen, p, n); c->hlen += n; return 0;
}

/* Next complete handshake message from hbuf: 1 have / 0 need more / -1 bad. */
static int hbuf_next(tls_conn *c, int *mtype, unsigned char **body, int *blen)
{
    unsigned char *p;
    int len;
    if (c->hlen - c->hpos < 4) return 0;
    p = c->hbuf + c->hpos;
    len = (p[1] << 16) | (p[2] << 8) | p[3];
    if (len < 0 || len > REC_MAX) return -1;
    if (c->hlen - c->hpos < 4 + len) return 0;
    transcript_update(c, p, 4 + len);
    *mtype = p[0]; *body = p + 4; *blen = len;
    c->hpos += 4 + len;
    return 1;
}

/* Queue ClientKeyExchange + derive keys + ChangeCipherSpec + client Finished. */
static int hs_send_keys(tls_conn *c)
{
    unsigned char hash[32], vfy[12];

    if (IS_ECDHE(c)) {                              /* ECDHE key exchange */
        unsigned char shared[32], cke[66];
        if (!c->have_srv_point) return -1;
        if (ecp256_keygen(c->rng, c->eph_priv, c->eph_pub) != 0) return -1;
        if (ecp256_ecdh(c->eph_priv, c->srv_point, shared) != 0) return -1;
        cke[0] = 65;                                /* ECPoint length */
        memcpy(cke + 1, c->eph_pub, 65);
        if (queue_handshake(c, HS_CLIENT_KEY_EXCHANGE, cke, 66, 0) != 0) return -1;
        derive_keys(c, shared, 32);                 /* premaster = shared X */
    } else {                                        /* RSA key exchange */
        unsigned char premaster[48], enc[512], cke[520], rnd[512];
        int enclen;
        premaster[0] = 3; premaster[1] = 3;
        rng_bytes(c->rng, premaster + 2, 46);
        rng_bytes(c->rng, rnd, sizeof(rnd));
        if (rsa_pkcs1_encrypt(&c->skey, premaster, 48, rnd, enc) != 0) return -1;
        enclen = c->skey.klen;
        cke[0] = (unsigned char)(enclen >> 8); cke[1] = (unsigned char)enclen;
        memcpy(cke + 2, enc, enclen);
        if (queue_handshake(c, HS_CLIENT_KEY_EXCHANGE, cke, enclen + 2, 0) != 0) return -1;
        derive_keys(c, premaster, 48);
    }

    if (queue_ccs(c) != 0) return -1;
    if (suite_is_sha384(c->suite)) {
        unsigned char h384[48];
        handshake_hash384(c, h384);
        tls12_prf_sha384(c->master, 48, "client finished", h384, 48, vfy, 12);
    } else {
        handshake_hash(c, hash);
        tls12_prf(c->master, 48, "client finished", hash, 32, vfy, 12);
    }
    if (queue_handshake(c, HS_FINISHED, vfy, 12, 1) != 0) return -1;
    return 0;
}

/* Drain complete records into hbuf, then process complete handshake messages
   until ServerHelloDone (then queue our second flight).  0 ok / <0 error. */
static int hs_flight(tls_conn *c)
{
    int rl, r, mtype, blen;
    unsigned char *body;

    while ((rl = have_record(c)) > 0) {
        if (c->ibuf[0] != TLS_CT_HANDSHAKE) return -1;      /* alert/unexpected */
        if (hbuf_append(c, c->ibuf + 5, rl - 5) != 0) return -1;
        consume_record(c, rl);
    }
    if (rl < 0) return -1;

    while ((r = hbuf_next(c, &mtype, &body, &blen)) == 1) {
        switch (mtype) {
        case HS_SERVER_HELLO:      if (parse_server_hello(c, body, blen) != 0) return -1; break;
        case HS_CERTIFICATE:       if (parse_certificate(c, body, blen) != 0) return -1; break;
        case HS_SERVER_KEY_EXCHANGE:
            if (IS_ECDHE(c) && parse_ecdhe_ske(c, body, blen) != 0) return -1;
            break;
        case HS_SERVER_HELLO_DONE:
            if (!c->have_key) return -1;
            if (validate_cert(c) != 0) return -1;           /* verr is set */
            if (hs_send_keys(c) != 0) return -1;
            c->hs_state = HS_EXPECT_CCS;
            return 0;
        default: break;                                     /* ignore SKE/CR */
        }
    }
    return (r < 0) ? -1 : 0;
}

void tls_hs_start(tls_conn *c, const char *host)
{
    c->hs_state = HS_INIT;
    c->hs_host[0] = '\0';
    if (host) { strncpy(c->hs_host, host, 255); c->hs_host[255] = '\0'; }
    c->olen = c->opos = 0;
    c->ilen = 0;
}

int tls_hs_feed(tls_conn *c, const unsigned char *data, int len)
{
    int need = c->ilen + len;
    if (need > c->icap) {
        int nc = c->icap ? c->icap : 8192;
        unsigned char *nb;
        while (nc < need) nc *= 2;
        nb = (unsigned char *)realloc(c->ibuf, nc);
        if (nb == NULL) return -1;
        c->ibuf = nb; c->icap = nc;
    }
    memcpy(c->ibuf + c->ilen, data, len);
    c->ilen += len;
    return len;
}

int tls_hs_pump(tls_conn *c)
{
    if (c->hs_state == HS_INIT) {
        unsigned char ch[600];
        int chlen = build_client_hello(c, c->hs_host, ch);
        if (queue_handshake(c, HS_CLIENT_HELLO, ch, chlen, 0) != 0) {
            c->hs_state = HS_FAILED; return TLS_HS_ERROR;
        }
        c->hs_state = HS_FLIGHT;
    }

    for (;;) {
        switch (c->hs_state) {
        case HS_FLIGHT:
            if (hs_flight(c) < 0) { c->hs_state = HS_FAILED; return TLS_HS_ERROR; }
            if (c->hs_state != HS_FLIGHT) continue;         /* advanced */
            return TLS_HS_WANT_READ;

        case HS_EXPECT_CCS: {
            int rl = have_record(c);
            if (rl == 0) return TLS_HS_WANT_READ;
            if (rl < 0 || c->ibuf[0] != TLS_CT_CHANGE_CIPHER_SPEC) {
                c->hs_state = HS_FAILED; return TLS_HS_ERROR;
            }
            consume_record(c, rl);
            c->hs_state = HS_EXPECT_FIN;
            continue;
        }

        case HS_EXPECT_FIN: {
            unsigned char fin[64], hash[32], expect[12];
            int rl = have_record(c), ct, plen, i;
            if (rl == 0) return TLS_HS_WANT_READ;
            if (rl < 0 || c->ibuf[0] != TLS_CT_HANDSHAKE) {
                c->hs_state = HS_FAILED; return TLS_HS_ERROR;
            }
            plen = tls_unprotect(&c->kr, c->ibuf, rl, &ct, fin, sizeof(fin));
            consume_record(c, rl);
            if (plen < 16 || ct != TLS_CT_HANDSHAKE || fin[0] != HS_FINISHED) {
                c->hs_state = HS_FAILED; return TLS_HS_ERROR;
            }
            if (suite_is_sha384(c->suite)) {
                unsigned char h384[48];
                handshake_hash384(c, h384);
                tls12_prf_sha384(c->master, 48, "server finished", h384, 48, expect, 12);
            } else {
                handshake_hash(c, hash);
                tls12_prf(c->master, 48, "server finished", hash, 32, expect, 12);
            }
            for (i = 0; i < 12; i++)
                if (fin[4 + i] != expect[i]) { c->hs_state = HS_FAILED; return TLS_HS_ERROR; }
            c->hs_state = HS_DONE;
            return TLS_HS_DONE;
        }

        case HS_DONE:  return TLS_HS_DONE;
        default:       return TLS_HS_ERROR;
        }
    }
}

int  tls_hs_out_ptr(tls_conn *c, unsigned char **ptr)
{ *ptr = c->obuf + c->opos; return c->olen - c->opos; }
void tls_hs_out_consume(tls_conn *c, int n)
{ c->opos += n; if (c->opos >= c->olen) { c->opos = c->olen = 0; } }
int  tls_hs_pending_output(tls_conn *c) { return c->olen - c->opos; }
int  tls_hs_leftover(tls_conn *c, unsigned char **ptr) { *ptr = c->ibuf; return c->ilen; }

/* --- lifecycle ------------------------------------------------------------ */
tls_conn *tls_new(nst_rng *rng)
{
    tls_conn *c = (tls_conn *)malloc(sizeof(tls_conn));
    if (c == NULL) return NULL;
    memset(c, 0, sizeof(*c));
    c->rng = rng;
    sha256_init(&c->hh);
    sha384_init(&c->hh384);
    return c;
}
void tls_free(tls_conn *c)
{
    if (c) { if (c->ibuf) free(c->ibuf); free(c); }
}

/* --- blocking wrapper over the resumable engine (standalone / tests) ------ */
int tls_client_handshake(tls_conn *c, const char *host,
                         tls_send_fn send, tls_recv_fn recv, void *ioctx)
{
    unsigned char tmp[REC_MAX + 8];
    unsigned char *p;
    int st, n, m;

    tls_hs_start(c, host);
    for (;;) {
        st = tls_hs_pump(c);
        while ((m = tls_hs_out_ptr(c, &p)) > 0) {           /* flush output */
            if (send_all(send, ioctx, p, m) != 0) return -1;
            tls_hs_out_consume(c, m);
        }
        if (st == TLS_HS_DONE) return 0;
        if (st == TLS_HS_ERROR) return -1;
        n = recv(ioctx, tmp, sizeof(tmp));                  /* WANT_READ */
        if (n <= 0) return -1;
        if (tls_hs_feed(c, tmp, n) < 0) return -1;
    }
}

/* --- application data ----------------------------------------------------- */
int tls_write_app(tls_conn *c, tls_send_fn send, void *ioctx,
                  const unsigned char *data, int len)
{
    unsigned char *rec;
    int reclen, rc;
    rec = (unsigned char *)malloc(len + 128);
    if (rec == NULL) return -1;
    reclen = tls_protect(&c->kw, TLS_CT_APPLICATION_DATA, data, len, c->rng, rec, len + 128);
    if (reclen < 0) { free(rec); return -1; }
    rc = send_all(send, ioctx, rec, reclen);
    free(rec);
    return rc == 0 ? len : -1;
}

int tls_read_app(tls_conn *c, tls_recv_fn recv, void *ioctx,
                 unsigned char *out, int outcap)
{
    int reclen, type, ct, plen;
    for (;;) {
        type = read_raw_record(c, recv, ioctx, &reclen);
        if (type < 0) return -1;
        plen = tls_unprotect(&c->kr, c->rdrec, reclen, &ct, out, outcap);
        if (plen < 0) return -1;
        if (ct == TLS_CT_APPLICATION_DATA) return plen;
        if (ct == TLS_CT_ALERT) return 0;
        /* else post-handshake message (ticket): skip, read more */
    }
}

int tls_app_encrypt(tls_conn *c, const unsigned char *in, int len,
                    unsigned char *out, int outcap)
{
    if (len > TLS_APP_MAX) len = TLS_APP_MAX;
    return tls_protect(&c->kw, TLS_CT_APPLICATION_DATA, in, len, c->rng, out, outcap);
}
int tls_app_decrypt(tls_conn *c, const unsigned char *rec, int reclen,
                    unsigned char *out, int outcap, int *content_type)
{
    return tls_unprotect(&c->kr, rec, reclen, content_type, out, outcap);
}

/* --- introspection -------------------------------------------------------
 * Read-only accessors over the private tls_conn, added so a host that does not
 * see this struct (NSTLS32.DLL) can still report what was negotiated and show
 * the peer's chain.  No state is modified.
 * ------------------------------------------------------------------------ */
int tls_get_suite(const tls_conn *c)
{ return (c && c->hs_state == HS_DONE) ? c->suite : 0; }

int tls_get_version(const tls_conn *c)
{ return (c && c->hs_state == HS_DONE) ? 0x0303 : 0; }   /* TLS 1.2 */

int tls_is_established(const tls_conn *c)
{ return (c && c->hs_state == HS_DONE) ? 1 : 0; }

int tls_get_cert_count(const tls_conn *c)
{ return c ? c->nchain : 0; }

int tls_get_cert(const tls_conn *c, int index, unsigned char *out, int outcap)
{
    int n;
    if (c == NULL || index < 0 || index >= c->nchain) return 0;
    n = c->chainlen[index];
    if (out == NULL) return n;              /* size query */
    if (outcap < n) return -1;
    memcpy(out, c->chain[index], n);
    return n;
}

/* Encrypt one fragment as an arbitrary content type (the app-data path is
   hard-wired to TLS_CT_APPLICATION_DATA).  Needed to emit a protected alert -
   close_notify - at shutdown.  Returns the record length, or -1. */
int tls_app_encrypt_type(tls_conn *c, int content_type,
                         const unsigned char *in, int len,
                         unsigned char *out, int outcap)
{
    if (c == NULL || len > TLS_APP_MAX) return -1;
    return tls_protect(&c->kw, content_type, in, len, c->rng, out, outcap);
}
