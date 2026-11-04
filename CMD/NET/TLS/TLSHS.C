/* ==========================================================================
 * TLSHS.C - TLS 1.3 and 1.2 client handshake, resumable state machine.  C89.
 * ==========================================================================
 * One ClientHello offers both versions (supported_versions), with a P-256
 * key share for 1.3 and the same ephemeral key reused for 1.2's ECDHE.
 *
 * TLS 1.3 (RFC 8446), full handshake, no PSK, no early data:
 *   -> ClientHello (+key_share)
 *  [<- HelloRetryRequest (a cookie)   -> ClientHello again]
 *   <- ServerHello                    (handshake keys from here)
 *   <- {EncryptedExtensions} {CertificateRequest} {Certificate}
 *      {CertificateVerify} {Finished}
 *   -> {Certificate (empty), if asked} {Finished}   (application keys)
 *   Dummy ChangeCipherSpec records are sent and dropped, for middleboxes
 *   (RFC 8446 D.4).  KeyUpdate is answered; session tickets are dropped.
 *
 * TLS 1.2 (RFC 5246), RSA or ECDHE key exchange, the server's key exchange
 *   signed with RSA (PKCS#1 or PSS) or ECDSA:
 *   -> ClientHello
 *   <- ServerHello, Certificate, ServerKeyExchange, [CertificateRequest],
 *      ServerHelloDone
 *   -> [Certificate (empty)], ClientKeyExchange, ChangeCipherSpec, Finished
 *   <- ChangeCipherSpec, Finished
 *
 * The engine never blocks: tls_hs_pump() advances as far as the currently
 * buffered input allows, queues any output, and returns WANT_READ/DONE/ERROR.
 * tls_client_handshake() is a thin blocking wrapper over the same engine, so
 * there is a single code path.
 * ========================================================================== */
#include "tlshs.h"
#include "tlsrec.h"
#include "sha.h"
#include "sha512.h"
#include "prf.h"
#include "rsa.h"
#include "x509.h"
#include "ecp256.h"
#include "ecp384.h"
#include <string.h>
#include <stdlib.h>

/* Key exchange families */
#define KX_RSA          1
#define KX_ECDHE_RSA    2
#define KX_ECDHE_ECDSA  3
#define KX_TLS13        4

typedef struct {
    unsigned short id;                  /* IANA cipher suite number */
    unsigned char  kx;                  /* KX_* */
    unsigned char  gcm;                 /* AES-GCM rather than CBC + HMAC-SHA1 */
    unsigned char  sha384;              /* SHA-384 PRF / transcript, else SHA-256 */
    unsigned char  keylen;              /* AES key bytes */
} suite_info;

/* Offered in this order.  AES-128 before AES-256 and ECDSA before RSA in
   each pair: both are the cheaper of the two on a slow CPU, and the server
   picks by its own order in the end anyway. */
static const suite_info SUITES[] = {
    { 0x1301, KX_TLS13,       1, 0, 16 },   /* TLS_AES_128_GCM_SHA256 */
    { 0x1302, KX_TLS13,       1, 1, 32 },   /* TLS_AES_256_GCM_SHA384 */
    { 0xC02B, KX_ECDHE_ECDSA, 1, 0, 16 },   /* ECDHE_ECDSA_AES_128_GCM_SHA256 */
    { 0xC02F, KX_ECDHE_RSA,   1, 0, 16 },   /* ECDHE_RSA_AES_128_GCM_SHA256 */
    { 0xC02C, KX_ECDHE_ECDSA, 1, 1, 32 },   /* ECDHE_ECDSA_AES_256_GCM_SHA384 */
    { 0xC030, KX_ECDHE_RSA,   1, 1, 32 },   /* ECDHE_RSA_AES_256_GCM_SHA384 */
    { 0xC009, KX_ECDHE_ECDSA, 0, 0, 16 },   /* ECDHE_ECDSA_WITH_AES_128_CBC_SHA */
    { 0xC013, KX_ECDHE_RSA,   0, 0, 16 },   /* ECDHE_RSA_WITH_AES_128_CBC_SHA */
    { 0xC00A, KX_ECDHE_ECDSA, 0, 0, 32 },   /* ECDHE_ECDSA_WITH_AES_256_CBC_SHA */
    { 0xC014, KX_ECDHE_RSA,   0, 0, 32 },   /* ECDHE_RSA_WITH_AES_256_CBC_SHA */
    { 0x002F, KX_RSA,         0, 0, 16 }    /* RSA_WITH_AES_128_CBC_SHA */
};
#define N_SUITES  ( (int)( sizeof( SUITES ) / sizeof( SUITES[0] ) ) )

/* The signature schemes offered (signature_algorithms), all of which
   x509_verify_scheme can check. */
static const unsigned short SIG_SCHEMES[] = {
    0x0403, 0x0503,                     /* ecdsa_secp256r1_sha256, secp384r1_sha384 */
    0x0804, 0x0805, 0x0806,             /* rsa_pss_rsae_sha256/384/512 */
    0x0401, 0x0501, 0x0601              /* rsa_pkcs1_sha256/384/512 */
};
#define N_SCHEMES ( (int)( sizeof( SIG_SCHEMES ) / sizeof( SIG_SCHEMES[0] ) ) )

/* Groups offered, P-256 first.  The first ClientHello carries a P-256 key
   share only: a 1.3 server that will use nothing but P-384 asks for that
   share with a HelloRetryRequest.  P-384 is listed also because a 1.2
   server offers an ECDSA certificate only on a listed curve. */
#define GROUP_P256  0x0017              /* secp256r1 */
#define GROUP_P384  0x0018              /* secp384r1 */
#define POINT_MAX   97                  /* a P-384 point, SEC1 uncompressed */

#define REC_MAX     18432               /* max record body we accept */
#define HS_BUF      20000               /* handshake reassembly buffer */
#define MAX_CHAIN   6                   /* max certs in the presented chain */
#define CERT_MAX    8192                /* max size of one DER certificate */
#define COOKIE_MAX  400                 /* largest HelloRetryRequest cookie kept */
#define CH_MAX      1000                /* ClientHello body, cookie included */

/* Handshake message types */
#define HS_CLIENT_HELLO         1
#define HS_SERVER_HELLO         2
#define HS_NEW_SESSION_TICKET   4
#define HS_ENCRYPTED_EXTENSIONS 8
#define HS_CERTIFICATE          11
#define HS_SERVER_KEY_EXCHANGE  12
#define HS_CERTIFICATE_REQUEST  13
#define HS_SERVER_HELLO_DONE    14
#define HS_CERTIFICATE_VERIFY   15
#define HS_CLIENT_KEY_EXCHANGE  16
#define HS_FINISHED             20
#define HS_KEY_UPDATE           24
#define HS_MESSAGE_HASH         254

/* Engine states */
enum { HS_INIT, HS_FLIGHT, HS_EXPECT_CCS, HS_EXPECT_FIN, HS_DONE, HS_FAILED };

/* TLS 1.3: what the encrypted server flight must send next */
enum { S13_EE, S13_CERT, S13_CV, S13_FIN };

struct tls_conn {
    nst_rng      *rng;
    SHA256_CTX    hh;                   /* running handshake hash (SHA-256)  */
    SHA384_CTX    hh384;                /* running handshake hash (SHA-384)  */
    unsigned char crand[32];
    unsigned char srand[32];
    unsigned char sid[32];              /* legacy_session_id (middlebox compat) */
    rsa_pubkey    skey;                 /* the leaf's RSA key (RSA key exchange) */
    x509_pubkey   lkey;                 /* the leaf's public key, RSA or EC */
    int           have_key;
    int           suite;                /* negotiated cipher suite          */
    const suite_info *si;               /* ...and what it means             */
    unsigned char eph_priv[48];         /* our ephemeral ECDH private       */
    unsigned char eph_pub[POINT_MAX];   /* our ephemeral ECDH public        */
    int           eph_group;            /* ...and its curve (GROUP_*)       */
    unsigned char srv_point[POINT_MAX]; /* server ephemeral ECDH public, on
                                           eph_group's curve                */
    int           have_srv_point;
    int           hrr_group;            /* the share a HRR asked for, or 0  */
    unsigned char master[48];           /* TLS 1.2 master secret            */
    tls_keys      kw;                   /* client write */
    tls_keys      kr;                   /* client read (server write) */

    /* TLS 1.3 */
    int           tls13;                /* the ServerHello chose 1.3        */
    int           got_sh;
    int           hrr;                  /* a HelloRetryRequest came         */
    int           ccs_sent;             /* our dummy ChangeCipherSpec       */
    int           keys13;               /* server records now encrypted     */
    int           step13;               /* S13_*                            */
    unsigned char cookie[COOKIE_MAX];
    int           cookielen;
    unsigned char hs_secret[48];        /* handshake secret                 */
    unsigned char c_secret[48];         /* client traffic secret (hs, then ap) */
    unsigned char s_secret[48];         /* server traffic secret (hs, then ap) */
    int           ku_send;              /* the server asked for a KeyUpdate */

    int           cert_req;             /* the server asked for a certificate */
    unsigned char req_ctx[255];         /* ...and its 1.3 request context   */
    int           req_ctxlen;

    unsigned char hbuf[HS_BUF];         /* reassembled handshake bytes */
    int           hlen, hpos;
    unsigned char rdrec[REC_MAX + 8];   /* scratch: app-phase read, 1.3 hs record */

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

static const suite_info *suite_find( int id )
{
    int i;
    for ( i = 0; i < N_SUITES; i++ ) {
        if ( SUITES[i].id == id ) return &SUITES[i];
    }
    return NULL;
}

static int scheme_offered( int scheme )
{
    int i;
    for ( i = 0; i < N_SCHEMES; i++ ) {
        if ( SIG_SCHEMES[i] == scheme ) return 1;
    }
    return 0;
}
/* SEC1 uncompressed point length on a group's curve, or 0 for one we lack */
static int point_len( int group )
{
    if ( group == GROUP_P256 ) return 65;
    if ( group == GROUP_P384 ) return 97;
    return 0;
}

/* A new ephemeral key pair on 'group'.  0 / -1. */
static int eph_make( tls_conn *c, int group )
{
    int rc;
    if ( group == GROUP_P384 ) {
        rc = ecp384_keygen( c->rng, c->eph_priv, c->eph_pub );
    } else {
        rc = ecp256_keygen( c->rng, c->eph_priv, c->eph_pub );
    }
    c->eph_group = group;
    return rc;
}

/* The ECDH shared secret (the X coordinate) from our key and the server's
   point; *slen gets 32 or 48.  0 / -1. */
static int ecdh_shared( tls_conn *c, unsigned char *shared, int *slen )
{
    if ( c->eph_group == GROUP_P384 ) {
        *slen = 48;
        return ecp384_ecdh( c->eph_priv, c->srv_point, shared );
    }
    *slen = 32;
    return ecp256_ecdh( c->eph_priv, c->srv_point, shared );
}

static int scheme_is_ecdsa( int scheme ) { return ( scheme & 0xFF ) == 0x03; }
static int scheme_is_pkcs1( int scheme ) { return ( scheme & 0xFF ) == 0x01; }

/* --- blocking I/O helpers (app phase + the blocking wrapper) -------------- */
static int send_all(tls_send_fn s, void *ctx, const unsigned char *b, int len)
{
    int off = 0, n;
    while (off < len) { n = s(ctx, b + off, len - off); if (n <= 0) return -1; off += n; }
    return 0;
}
static void consume_record(tls_conn *c, int rl)
{
    memmove(c->ibuf, c->ibuf + rl, c->ilen - rl);
    c->ilen -= rl;
}
/* Read up to 'want' bytes: first whatever the handshake left in ibuf
   (records that came in behind the server's Finished), then the socket. */
static int raw_read( tls_conn *c, tls_recv_fn recv, void *ctx,
                     unsigned char *dst, int want )
{
    if ( c->ilen > 0 ) {
        int n = want < c->ilen ? want : c->ilen;
        memcpy( dst, c->ibuf, n );
        consume_record( c, n );
        return n;
    }
    return recv( ctx, dst, want );
}
static int read_raw_record(tls_conn *c, tls_recv_fn recv, void *ctx, int *reclen)
{
    int off = 0, n, l;
    while (off < 5) { n = raw_read(c, recv, ctx, c->rdrec + off, 5 - off); if (n <= 0) return -1; off += n; }
    l = (c->rdrec[3] << 8) | c->rdrec[4];
    if (l < 0 || l > REC_MAX) return -1;
    while (off < 5 + l) { n = raw_read(c, recv, ctx, c->rdrec + off, 5 + l - off); if (n <= 0) return -1; off += n; }
    *reclen = 5 + l;
    return c->rdrec[0];
}

/* --- pure message helpers ------------------------------------------------- */
static int put16( unsigned char *b, int i, int v )
{
    b[i] = (unsigned char)( v >> 8 );
    b[i + 1] = (unsigned char)v;
    return i + 2;
}

/* A dotted IPv4 address may not be sent as a server name (RFC 6066 3). */
static int is_ipv4( const char *host )
{
    int dots = 0;
    if ( *host == '\0' ) return 0;
    for ( ; *host; host++ ) {
        if ( *host == '.' ) {
            dots++;
        } else if ( *host < '0' || *host > '9' ) {
            return 0;
        }
    }
    return dots == 3;
}

/* The ClientHello body.  The random, session id and key share were made in
   tls_hs_pump, so a second ClientHello after a HelloRetryRequest is the same
   but for the cookie, as RFC 8446 4.1.2 wants.  Returns the length, or -1. */
static int build_client_hello( tls_conn *c, unsigned char *b, int cap )
{
    int i = 0, hlen = (int)strlen( c->hs_host ), extlenpos, n;
    int plen = point_len( c->eph_group );

    /* the fixed part, every extension, the longest server name and cookie */
    if ( 2 + 32 + 33 + 2 + 2 * N_SUITES + 2 + 2 + 10 + 6 + 6 + 2 * N_SCHEMES + 9 +
         ( 10 + plen ) + ( 9 + hlen ) + ( 6 + c->cookielen ) > cap ) {
        return -1;
    }
    b[i++] = 3; b[i++] = 3;                     /* legacy_version: TLS 1.2 */
    memcpy( b + i, c->crand, 32 ); i += 32;
    b[i++] = 32;                                /* legacy_session_id */
    memcpy( b + i, c->sid, 32 ); i += 32;
    i = put16( b, i, 2 * N_SUITES );
    for ( n = 0; n < N_SUITES; n++ ) {
        i = put16( b, i, SUITES[n].id );
    }
    b[i++] = 1; b[i++] = 0;                     /* null compression */

    extlenpos = i; i += 2;                      /* fill total ext length later */

    if ( hlen > 0 && hlen < 256 && !is_ipv4( c->hs_host ) ) {   /* server_name */
        int list = 1 + 2 + hlen, sni = 2 + list;
        i = put16( b, i, 0x0000 );
        i = put16( b, i, sni );
        i = put16( b, i, list );
        b[i++] = 0x00;                          /* host_name */
        i = put16( b, i, hlen );
        memcpy( b + i, c->hs_host, hlen ); i += hlen;
    }
    /* supported_groups: secp256r1, secp384r1 */
    i = put16( b, i, 0x000A ); i = put16( b, i, 6 ); i = put16( b, i, 4 );
    i = put16( b, i, GROUP_P256 ); i = put16( b, i, GROUP_P384 );
    /* ec_point_formats: uncompressed */
    i = put16( b, i, 0x000B ); i = put16( b, i, 2 ); b[i++] = 1; b[i++] = 0;
    /* signature_algorithms */
    i = put16( b, i, 0x000D ); i = put16( b, i, 2 + 2 * N_SCHEMES );
    i = put16( b, i, 2 * N_SCHEMES );
    for ( n = 0; n < N_SCHEMES; n++ ) {
        i = put16( b, i, SIG_SCHEMES[n] );
    }
    /* supported_versions: TLS 1.3, TLS 1.2 */
    i = put16( b, i, 0x002B ); i = put16( b, i, 5 ); b[i++] = 4;
    i = put16( b, i, 0x0304 ); i = put16( b, i, 0x0303 );
    /* key_share: one share, P-256 unless a HRR asked for P-384 */
    i = put16( b, i, 0x0033 ); i = put16( b, i, 2 + 4 + plen );
    i = put16( b, i, 4 + plen );
    i = put16( b, i, c->eph_group ); i = put16( b, i, plen );
    memcpy( b + i, c->eph_pub, plen ); i += plen;
    /* cookie, echoed from a HelloRetryRequest */
    if ( c->cookielen > 0 ) {
        i = put16( b, i, 0x002C ); i = put16( b, i, 2 + c->cookielen );
        i = put16( b, i, c->cookielen );
        memcpy( b + i, c->cookie, c->cookielen ); i += c->cookielen;
    }

    put16( b, extlenpos, i - extlenpos - 2 );
    return i;
}

/* SHA-256("HelloRetryRequest"): a ServerHello with this random is a HRR. */
static const unsigned char HRR_RANDOM[32] = {
    0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
    0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C
};
/* A 1.3 server that answers 1.2 puts "DOWNGRD" + 01 at the end of its random
   (00 for 1.1 and older); a client that offered 1.3 must refuse it, because
   something in the path removed 1.3 from the ClientHello (RFC 8446 4.1.3). */
static const unsigned char DOWNGRADE[7] = { 0x44,0x4F,0x57,0x4E,0x47,0x52,0x44 };

#define SH_TLS12  0
#define SH_TLS13  1
#define SH_HRR    2

/* Parse a ServerHello (or HelloRetryRequest).  SH_* or -1. */
static int parse_server_hello( tls_conn *c, const unsigned char *b, int blen )
{
    int ver, sidlen, off, suite, extend, is_hrr, sel_ver = 0, have_share = 0;
    const suite_info *si;

    if ( blen < 38 ) return -1;
    ver = ( b[0] << 8 ) | b[1];
    is_hrr = ( memcmp( b + 2, HRR_RANDOM, 32 ) == 0 );
    sidlen = b[34];
    if ( sidlen > 32 || 35 + sidlen + 3 > blen ) return -1;
    off = 35 + sidlen;
    suite = ( b[off] << 8 ) | b[off + 1];
    if ( b[off + 2] != 0 ) return -1;           /* compression must be null */
    off += 3;
    extend = off;
    if ( off + 2 <= blen ) {                    /* extensions are optional in 1.2 */
        int elen = ( b[off] << 8 ) | b[off + 1];
        off += 2;
        if ( off + elen != blen ) return -1;
        extend = off + elen;
    } else if ( off != blen ) {
        return -1;
    }
    while ( off < extend ) {
        int et, el;
        const unsigned char *ev;
        if ( off + 4 > extend ) return -1;
        et = ( b[off] << 8 ) | b[off + 1];
        el = ( b[off + 2] << 8 ) | b[off + 3];
        ev = b + off + 4;
        off += 4;
        if ( el > extend - off ) return -1;
        if ( et == 0x002B ) {                   /* supported_versions */
            if ( el != 2 ) return -1;
            sel_ver = ( ev[0] << 8 ) | ev[1];
        } else if ( et == 0x0033 ) {            /* key_share */
            int group, plen;
            if ( el < 2 ) return -1;
            group = ( ev[0] << 8 ) | ev[1];
            if ( is_hrr ) {
                /* the group the server wants a share for: one offered,
                   and not the one already sent (RFC 8446 4.1.4) */
                if ( el != 2 || point_len( group ) == 0 || group == c->eph_group ) return -1;
                c->hrr_group = group;
            } else {
                plen = point_len( c->eph_group );
                if ( group != c->eph_group || el != 4 + plen ) return -1;
                if ( ( ( ev[2] << 8 ) | ev[3] ) != plen ) return -1;
                memcpy( c->srv_point, ev + 4, plen );
                have_share = 1;
            }
        } else if ( et == 0x002C ) {            /* cookie: HRR only */
            int cl;
            if ( !is_hrr || el < 2 ) return -1;
            cl = ( ev[0] << 8 ) | ev[1];
            if ( cl == 0 || cl + 2 != el || cl > COOKIE_MAX ) return -1;
            memcpy( c->cookie, ev + 2, cl );
            c->cookielen = cl;
        }
        off += el;
    }

    si = suite_find( suite );
    if ( si == NULL || ver != 0x0303 ) return -1;

    if ( sel_ver == 0x0304 ) {
        if ( si->kx != KX_TLS13 ) return -1;
        if ( sidlen != 32 || memcmp( b + 35, c->sid, 32 ) != 0 ) return -1;
        if ( is_hrr ) {
            if ( c->hrr ) return -1;            /* only one retry */
            if ( c->cookielen == 0 && c->hrr_group == 0 ) {
                return -1;                      /* a retry that would change nothing */
            }
            c->si = si; c->suite = suite;
            return SH_HRR;
        }
        if ( c->hrr && c->si != si ) return -1; /* must keep the HRR's suite */
        if ( !have_share ) return -1;
        memcpy( c->srand, b + 2, 32 );
        c->si = si; c->suite = suite;
        return SH_TLS13;
    }

    /* TLS 1.2 */
    if ( sel_ver != 0 || is_hrr || c->hrr || have_share ) return -1;
    if ( si->kx == KX_TLS13 ) return -1;
    memcpy( c->srand, b + 2, 32 );
    if ( memcmp( c->srand + 24, DOWNGRADE, 7 ) == 0 &&
         ( c->srand[31] == 0x00 || c->srand[31] == 0x01 ) ) {
        return -1;
    }
    c->si = si; c->suite = suite;
    return SH_TLS12;
}

/* Parse a TLS 1.2 ServerKeyExchange for ECDHE: named-curve params, then a
   signature over client_random || server_random || params by the leaf's key
   (RSA for ECDHE_RSA, ECDSA for ECDHE_ECDSA).  0 ok / -1 bad. */
static int parse_ecdhe_ske(tls_conn *c, const unsigned char *b, int blen)
{
    unsigned char msg[64 + 4 + POINT_MAX];
    int group, plen, params_len, scheme, sig_len, off;

    if ( blen < 4 || b[0] != 3 ) return -1;             /* named_curve */
    group = ( b[1] << 8 ) | b[2];
    plen = point_len( group );
    if ( plen == 0 || b[3] != plen ) return -1;         /* uncompressed point */
    params_len = 4 + plen;
    if ( blen < params_len + 4 ) return -1;
    /* our key goes in the ClientKeyExchange, on the server's curve */
    if ( group != c->eph_group && eph_make( c, group ) != 0 ) return -1;
    memcpy( c->srv_point, b + 4, plen );
    c->have_srv_point = 1;

    off = params_len;
    scheme = ( b[off] << 8 ) | b[off + 1];
    sig_len = ( b[off + 2] << 8 ) | b[off + 3];
    off += 4;
    if ( off + sig_len != blen ) return -1;
    if ( !scheme_offered( scheme ) || !c->have_key ) return -1;
    if ( c->si->kx == KX_ECDHE_ECDSA ) {
        if ( c->lkey.type == X509_KEY_RSA || !scheme_is_ecdsa( scheme ) ) return -1;
    } else {
        if ( c->lkey.type != X509_KEY_RSA || scheme_is_ecdsa( scheme ) ) return -1;
    }

    memcpy( msg, c->crand, 32 );
    memcpy( msg + 32, c->srand, 32 );
    memcpy( msg + 64, b, params_len );
    return ( x509_verify_scheme( &c->lkey, scheme, msg, 64 + params_len,
                                 b + off, sig_len ) == X509_OK ) ? 0 : -1;
}

/* Certificate, 1.2 or 1.3 form (1.3 adds a request context in front and
   extensions after each certificate).  Keeps up to MAX_CHAIN certificates and
   takes the leaf's public key.  0 ok / -1 bad. */
static int parse_certificate(tls_conn *c, const unsigned char *b, int blen)
{
    int off = 0, total;

    if ( c->tls13 ) {
        if ( blen < 1 || b[0] != 0 ) return -1;     /* context is empty for a server */
        off = 1;
    }
    if ( off + 3 > blen ) return -1;
    total = ( b[off] << 16 ) | ( b[off + 1] << 8 ) | b[off + 2];
    off += 3;
    if ( total != blen - off ) return -1;
    c->nchain = 0;

    while ( off < blen ) {                          /* walk each cert entry */
        int clen;
        if ( off + 3 > blen ) return -1;
        clen = ( b[off] << 16 ) | ( b[off + 1] << 8 ) | b[off + 2];
        off += 3;
        if ( clen <= 0 || clen > blen - off ) return -1;
        if ( c->nchain < MAX_CHAIN && clen <= CERT_MAX ) {
            memcpy( c->chain[c->nchain], b + off, clen );
            c->chainlen[c->nchain] = clen;
            c->nchain++;
        } else if ( c->nchain == 0 ) {
            return -1;                              /* a leaf too big to keep */
        }
        off += clen;
        if ( c->tls13 ) {                           /* per-certificate extensions */
            int el;
            if ( off + 2 > blen ) return -1;
            el = ( b[off] << 8 ) | b[off + 1];
            off += 2;
            if ( el > blen - off ) return -1;
            off += el;
        }
    }
    if ( c->nchain == 0 ) return -1;

    if ( x509_get_pubkey( c->chain[0], c->chainlen[0], &c->lkey ) != X509_OK ) return -1;
    if ( c->lkey.type == X509_KEY_RSA ) {
        rsa_pubkey_init( &c->skey, c->lkey.n, c->lkey.nlen, c->lkey.e, c->lkey.elen );
    }
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
        if (nb == 0 || nb > 3 || p + 2 + nb > len) return 0;
        l = 0;
        for (k = 0; k < nb; k++) l = (l << 8) | bundle[p+2+k];
        hdr = 2 + nb;
    }
    if (l < 0 || l > len - (p + hdr)) return 0;
    *cert = bundle + p;
    *certlen = hdr + l;
    *pos = p + hdr + l;
    return 1;
}

/* Is 'top' signed by any CA in the trust bundle? 0 = anchored, -1 = not.
   Handles RSA and ECDSA CA keys / signatures. */
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
       Each link may be RSA or ECDSA (x509_verify_signed dispatches). */
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
    int sha384s = c->si->sha384;
    int kl = c->si->keylen;

    memcpy(seed, c->crand, 32); memcpy(seed + 32, c->srand, 32);
    if (sha384s) tls12_prf_sha384(premaster, pmlen, "master secret", seed, 64, c->master, 48);
    else         tls12_prf(premaster, pmlen, "master secret", seed, 64, c->master, 48);

    memcpy(seed, c->srand, 32); memcpy(seed + 32, c->crand, 32);
    if (c->si->gcm) {
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
    memset(kb, 0, sizeof(kb));
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

/* --- TLS 1.3 key schedule (RFC 8446 7.1) ---------------------------------- */
static int th_len( const tls_conn *c ) { return c->si->sha384 ? 48 : 32; }

/* the transcript so far, under the suite's hash */
static void th_hash( tls_conn *c, unsigned char *out )
{
    if ( c->si->sha384 ) {
        handshake_hash384( c, out );
    } else {
        handshake_hash( c, out );
    }
}

static void hmac_th( const tls_conn *c, const unsigned char *key,
                     const unsigned char *msg, int msglen, unsigned char *out )
{
    if ( c->si->sha384 ) {
        hmac_sha384( key, 48, msg, msglen, out );
    } else {
        hmac_sha256( key, 32, msg, msglen, out );
    }
}

/* Derive-Secret( secret, "derived", "" ): the salt for the next stage */
static void derived_salt( const tls_conn *c, const unsigned char *secret,
                          unsigned char *out )
{
    unsigned char empty[48];
    int hl = th_len( c );
    if ( hl == 48 ) {
        sha384( "", 0, empty );
    } else {
        sha256( "", 0, empty );
    }
    tls13_expand_label( hl, secret, "derived", empty, hl, out, hl );
}

/* Install one direction's record keys from a traffic secret. */
static void install13( const tls_conn *c, tls_keys *k, const unsigned char *secret )
{
    unsigned char key[32], iv[12];
    int hl = th_len( c ), kl = c->si->keylen;
    tls13_expand_label( hl, secret, "key", NULL, 0, key, kl );
    tls13_expand_label( hl, secret, "iv", NULL, 0, iv, 12 );
    tls_keys_init13( k, key, kl, iv );
    memset( key, 0, sizeof( key ) );
    memset( iv, 0, sizeof( iv ) );
}

/* A KeyUpdate's next traffic secret, in place. */
static void next_secret( const tls_conn *c, unsigned char *secret )
{
    unsigned char next[48];
    int hl = th_len( c );
    tls13_expand_label( hl, secret, "traffic upd", NULL, 0, next, hl );
    memcpy( secret, next, hl );
    memset( next, 0, sizeof( next ) );
}

/* The ServerHello is in the transcript: make the handshake secret and the
   handshake traffic keys for both directions.  0 / -1. */
static int start13( tls_conn *c )
{
    unsigned char shared[48], early[48], salt[48], th[48];
    int hl = th_len( c ), slen;

    if ( ecdh_shared( c, shared, &slen ) != 0 ) return -1;
    tls13_hkdf_extract( hl, NULL, 0, NULL, 0, early );        /* no PSK */
    derived_salt( c, early, salt );
    tls13_hkdf_extract( hl, salt, hl, shared, slen, c->hs_secret );
    th_hash( c, th );                                          /* CH .. SH */
    tls13_expand_label( hl, c->hs_secret, "c hs traffic", th, hl, c->c_secret, hl );
    tls13_expand_label( hl, c->hs_secret, "s hs traffic", th, hl, c->s_secret, hl );
    install13( c, &c->kw, c->c_secret );
    install13( c, &c->kr, c->s_secret );
    c->keys13 = 1;
    c->step13 = S13_EE;
    memset( shared, 0, sizeof( shared ) );
    memset( early, 0, sizeof( early ) );
    memset( salt, 0, sizeof( salt ) );
    memset( c->eph_priv, 0, sizeof( c->eph_priv ) );
    return 0;
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
/* TLS 1.3's dummy ChangeCipherSpec: once, before the second flight */
static int queue_ccs13( tls_conn *c )
{
    if ( c->ccs_sent ) return 0;
    c->ccs_sent = 1;
    return queue_ccs( c );
}

static int queue_client_hello( tls_conn *c )
{
    unsigned char ch[CH_MAX];
    int chlen = build_client_hello( c, ch, (int)sizeof( ch ) );
    if ( chlen < 0 ) return -1;
    return queue_handshake( c, HS_CLIENT_HELLO, ch, chlen, 0 );
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

static int hbuf_append(tls_conn *c, const unsigned char *p, int n)
{
    if (c->hpos > 0) {
        memmove(c->hbuf, c->hbuf + c->hpos, c->hlen - c->hpos);
        c->hlen -= c->hpos; c->hpos = 0;
    }
    if (c->hlen + n > HS_BUF) return -1;
    memcpy(c->hbuf + c->hlen, p, n); c->hlen += n; return 0;
}
static int hbuf_empty( const tls_conn *c ) { return c->hpos == c->hlen; }

/* Next complete handshake message from hbuf: 1 have / 0 need more / -1 bad.
   NOT folded into the transcript: TLS 1.3's CertificateVerify and Finished
   are checked against the transcript before them, so the caller folds. */
static int hbuf_next(tls_conn *c, int *mtype, unsigned char **body, int *blen)
{
    unsigned char *p;
    int len;
    if (c->hlen - c->hpos < 4) return 0;
    p = c->hbuf + c->hpos;
    len = (p[1] << 16) | (p[2] << 8) | p[3];
    if (len < 0 || len > REC_MAX) return -1;
    if (c->hlen - c->hpos < 4 + len) return 0;
    *mtype = p[0]; *body = p + 4; *blen = len;
    c->hpos += 4 + len;
    return 1;
}

/* TLS 1.2: queue [Certificate] + ClientKeyExchange, derive keys, then
   ChangeCipherSpec + client Finished. */
static int hs_send_keys(tls_conn *c)
{
    unsigned char hash[32], vfy[12];

    if ( c->cert_req ) {                            /* no certificate to give */
        unsigned char none[3];
        none[0] = none[1] = none[2] = 0;
        if ( queue_handshake( c, HS_CERTIFICATE, none, 3, 0 ) != 0 ) return -1;
    }
    if ( c->si->kx != KX_RSA ) {                    /* ECDHE key exchange */
        unsigned char shared[48], cke[1 + POINT_MAX];
        int slen, plen = point_len( c->eph_group );
        if (!c->have_srv_point) return -1;
        if ( ecdh_shared( c, shared, &slen ) != 0 ) return -1;
        cke[0] = (unsigned char)plen;               /* ECPoint length */
        memcpy(cke + 1, c->eph_pub, plen);
        if (queue_handshake(c, HS_CLIENT_KEY_EXCHANGE, cke, 1 + plen, 0) != 0) return -1;
        derive_keys(c, shared, slen);               /* premaster = shared X */
        memset(shared, 0, sizeof(shared));
    } else {                                        /* RSA key exchange */
        unsigned char premaster[48], enc[512], cke[520], rnd[512];
        int enclen;
        if ( c->lkey.type != X509_KEY_RSA ) return -1;
        premaster[0] = 3; premaster[1] = 3;
        rng_bytes(c->rng, premaster + 2, 46);
        rng_bytes(c->rng, rnd, sizeof(rnd));
        if (rsa_pkcs1_encrypt(&c->skey, premaster, 48, rnd, enc) != 0) return -1;
        enclen = c->skey.klen;
        cke[0] = (unsigned char)(enclen >> 8); cke[1] = (unsigned char)enclen;
        memcpy(cke + 2, enc, enclen);
        if (queue_handshake(c, HS_CLIENT_KEY_EXCHANGE, cke, enclen + 2, 0) != 0) return -1;
        derive_keys(c, premaster, 48);
        memset(premaster, 0, sizeof(premaster));
    }

    if (queue_ccs(c) != 0) return -1;
    if (c->si->sha384) {
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

/* One TLS 1.2 server message after the ServerHello, already folded into
   the transcript.  0 ok / -1 bad. */
static int hs_msg12( tls_conn *c, int mtype, const unsigned char *body, int blen )
{
    switch ( mtype ) {
    case HS_CERTIFICATE:
        return parse_certificate( c, body, blen );
    case HS_SERVER_KEY_EXCHANGE:
        if ( c->si->kx == KX_RSA ) return -1;       /* not for RSA key exchange */
        return parse_ecdhe_ske( c, body, blen );
    case HS_CERTIFICATE_REQUEST:
        c->cert_req = 1;
        return 0;
    case HS_SERVER_HELLO_DONE:
        if ( !c->have_key ) return -1;
        if ( ( c->si->kx == KX_ECDHE_ECDSA ) != ( c->lkey.type != X509_KEY_RSA ) ) {
            return -1;                              /* the key does not suit the suite */
        }
        if ( validate_cert( c ) != 0 ) return -1;   /* verr is set */
        if ( hs_send_keys( c ) != 0 ) return -1;
        c->hs_state = HS_EXPECT_CCS;
        return 0;
    default:
        return -1;                                  /* nothing else belongs here */
    }
}

/* TLS 1.3 CertificateVerify: the leaf's signature over the transcript up to
   the Certificate ('th', taken before this message was folded in). */
static int check_cv13( tls_conn *c, const unsigned char *b, int blen,
                       const unsigned char *th )
{
    static const char ctx[] = "TLS 1.3, server CertificateVerify";
    unsigned char msg[64 + sizeof( ctx ) + 48];
    int scheme, slen, hl = th_len( c ), mlen;

    if ( blen < 4 ) return -1;
    scheme = ( b[0] << 8 ) | b[1];
    slen = ( b[2] << 8 ) | b[3];
    if ( 4 + slen != blen ) return -1;
    /* PKCS#1 v1.5 is not allowed here, and an ECDSA scheme names its curve */
    if ( !scheme_offered( scheme ) || scheme_is_pkcs1( scheme ) ) return -1;
    if ( scheme == 0x0403 && c->lkey.type != X509_KEY_EC256 ) return -1;
    if ( scheme == 0x0503 && c->lkey.type != X509_KEY_EC384 ) return -1;
    if ( !scheme_is_ecdsa( scheme ) && c->lkey.type != X509_KEY_RSA ) return -1;

    memset( msg, 0x20, 64 );
    memcpy( msg + 64, ctx, sizeof( ctx ) );         /* the terminating 0 included */
    memcpy( msg + 64 + sizeof( ctx ), th, hl );
    mlen = 64 + (int)sizeof( ctx ) + hl;
    return ( x509_verify_scheme( &c->lkey, scheme, msg, mlen, b + 4, slen ) == X509_OK )
           ? 0 : -1;
}

/* TLS 1.3 server Finished checked: make the application secrets, queue our
   flight (dummy CCS, empty Certificate if asked, Finished) under the client
   handshake key, then switch both directions to the application keys. */
static int finish13( tls_conn *c )
{
    unsigned char th[48], salt[48], master[48], c_ap[48], s_ap[48], fk[48], vfy[48];
    int hl = th_len( c );

    th_hash( c, th );                               /* CH .. server Finished */
    derived_salt( c, c->hs_secret, salt );
    tls13_hkdf_extract( hl, salt, hl, NULL, 0, master );
    tls13_expand_label( hl, master, "c ap traffic", th, hl, c_ap, hl );
    tls13_expand_label( hl, master, "s ap traffic", th, hl, s_ap, hl );

    if ( queue_ccs13( c ) != 0 ) return -1;
    if ( c->cert_req ) {
        unsigned char none[1 + 255 + 3];
        int n = 0;
        none[n++] = (unsigned char)c->req_ctxlen;
        memcpy( none + n, c->req_ctx, c->req_ctxlen ); n += c->req_ctxlen;
        none[n++] = 0; none[n++] = 0; none[n++] = 0;    /* no certificates */
        if ( queue_handshake( c, HS_CERTIFICATE, none, n, 1 ) != 0 ) return -1;
    }
    th_hash( c, th );                               /* .. our Certificate, if sent */
    tls13_expand_label( hl, c->c_secret, "finished", NULL, 0, fk, hl );
    hmac_th( c, fk, th, hl, vfy );
    if ( queue_handshake( c, HS_FINISHED, vfy, hl, 1 ) != 0 ) return -1;

    memcpy( c->c_secret, c_ap, hl );
    memcpy( c->s_secret, s_ap, hl );
    install13( c, &c->kw, c->c_secret );
    install13( c, &c->kr, c->s_secret );
    memset( c->hs_secret, 0, sizeof( c->hs_secret ) );
    memset( master, 0, sizeof( master ) );
    memset( salt, 0, sizeof( salt ) );
    memset( c_ap, 0, sizeof( c_ap ) );
    memset( s_ap, 0, sizeof( s_ap ) );
    memset( fk, 0, sizeof( fk ) );
    c->hlen = c->hpos = 0;                          /* free for post-handshake messages */
    c->hs_state = HS_DONE;
    return 0;
}

/* One TLS 1.3 encrypted server message, in the order RFC 8446 4 fixes.
   'raw' is the whole message (header included), not yet folded.  0 / -1. */
static int hs_msg13( tls_conn *c, int mtype, const unsigned char *body, int blen,
                     const unsigned char *raw )
{
    unsigned char th[48];
    int hl = th_len( c );

    if ( mtype == HS_CERTIFICATE_VERIFY || mtype == HS_FINISHED ) {
        th_hash( c, th );                           /* the transcript before it */
    }
    transcript_update( c, raw, 4 + blen );

    switch ( c->step13 ) {
    case S13_EE:
        if ( mtype != HS_ENCRYPTED_EXTENSIONS ) return -1;
        if ( blen < 2 || ( ( body[0] << 8 ) | body[1] ) != blen - 2 ) return -1;
        c->step13 = S13_CERT;
        return 0;
    case S13_CERT:
        if ( mtype == HS_CERTIFICATE_REQUEST && !c->cert_req ) {
            int cl;
            if ( blen < 1 ) return -1;
            cl = body[0];
            if ( 1 + cl > blen ) return -1;
            memcpy( c->req_ctx, body + 1, cl );
            c->req_ctxlen = cl;
            c->cert_req = 1;
            return 0;
        }
        if ( mtype != HS_CERTIFICATE ) return -1;
        if ( parse_certificate( c, body, blen ) != 0 ) return -1;
        c->step13 = S13_CV;
        return 0;
    case S13_CV:
        if ( mtype != HS_CERTIFICATE_VERIFY ) return -1;
        /* the certificate first, so a refused one is reported as such */
        if ( validate_cert( c ) != 0 ) return -1;   /* verr is set */
        if ( check_cv13( c, body, blen, th ) != 0 ) return -1;
        c->step13 = S13_FIN;
        return 0;
    case S13_FIN: {
        unsigned char fk[48], expect[48];
        int i, diff = 0;
        if ( mtype != HS_FINISHED || blen != hl ) return -1;
        tls13_expand_label( hl, c->s_secret, "finished", NULL, 0, fk, hl );
        hmac_th( c, fk, th, hl, expect );
        for ( i = 0; i < hl; i++ ) {
            diff |= expect[i] ^ body[i];
        }
        memset( fk, 0, sizeof( fk ) );
        if ( diff ) return -1;
        /* a key change follows: nothing may be left behind it */
        if ( !hbuf_empty( c ) ) return -1;
        return finish13( c );
    }
    default:
        return -1;
    }
}

/* HelloRetryRequest: the transcript restarts as message_hash(Hash(CH1)) ||
   HRR (RFC 8446 4.4.1), and the ClientHello goes again with the cookie. */
static int do_hrr( tls_conn *c, const unsigned char *raw, int rawlen )
{
    unsigned char h[48], mh[4];
    int hl = th_len( c );

    th_hash( c, h );                                /* Hash( ClientHello1 ) */
    sha256_init( &c->hh );
    sha384_init( &c->hh384 );
    mh[0] = HS_MESSAGE_HASH; mh[1] = 0; mh[2] = 0; mh[3] = (unsigned char)hl;
    transcript_update( c, mh, 4 );
    transcript_update( c, h, hl );
    transcript_update( c, raw, rawlen );
    c->hrr = 1;
    if ( c->hrr_group != 0 && eph_make( c, c->hrr_group ) != 0 ) return -1;
    if ( queue_ccs13( c ) != 0 ) return -1;
    return queue_client_hello( c );
}

/* Process every complete handshake message buffered.  0 ok / -1 bad. */
static int hs_messages( tls_conn *c )
{
    int r, mtype, blen;
    unsigned char *body;

    while ( c->hs_state == HS_FLIGHT ) {
        r = hbuf_next( c, &mtype, &body, &blen );
        if ( r <= 0 ) return r;
        if ( !c->got_sh ) {
            int kind;
            if ( mtype != HS_SERVER_HELLO ) return -1;
            kind = parse_server_hello( c, body, blen );
            if ( kind < 0 ) return -1;
            /* the hello and a retry each end a record (a key change or a
               new ClientHello follows), so nothing may come behind them */
            if ( kind != SH_TLS12 && !hbuf_empty( c ) ) return -1;
            if ( kind == SH_HRR ) {
                if ( do_hrr( c, body - 4, blen + 4 ) != 0 ) return -1;
                continue;
            }
            transcript_update( c, body - 4, blen + 4 );
            c->got_sh = 1;
            if ( kind == SH_TLS13 ) {
                c->tls13 = 1;
                if ( start13( c ) != 0 ) return -1;
            }
            continue;
        }
        if ( c->tls13 ) {
            if ( hs_msg13( c, mtype, body, blen, body - 4 ) != 0 ) return -1;
        } else {
            transcript_update( c, body - 4, blen + 4 );
            if ( hs_msg12( c, mtype, body, blen ) != 0 ) return -1;
        }
    }
    return 0;
}

/* Take complete records into hbuf one at a time, processing the messages
   after each: in TLS 1.3 the record after the ServerHello is already under
   the handshake key, and the one after the server's Finished under the
   application key.  0 ok / <0 error. */
static int hs_flight(tls_conn *c)
{
    int rl;

    while ( c->hs_state == HS_FLIGHT ) {
        int ct;
        rl = have_record( c );
        if ( rl <= 0 ) return rl;
        ct = c->ibuf[0];
        if ( ct == TLS_CT_CHANGE_CIPHER_SPEC && ( c->tls13 || c->hrr ) ) {
            /* RFC 8446 D.4: a plaintext dummy, dropped unread */
            if ( rl != 6 || c->ibuf[5] != 1 ) return -1;
            consume_record( c, rl );
            continue;
        }
        if ( c->keys13 ) {
            int ict, plen;
            if ( ct != TLS_CT_APPLICATION_DATA ) return -1;
            plen = tls_unprotect( &c->kr, c->ibuf, rl, &ict, c->rdrec, (int)sizeof( c->rdrec ) );
            consume_record( c, rl );
            if ( plen < 0 || ict != TLS_CT_HANDSHAKE ) return -1;   /* or an alert */
            if ( hbuf_append( c, c->rdrec, plen ) != 0 ) return -1;
        } else {
            if ( ct != TLS_CT_HANDSHAKE ) return -1;                /* alert/unexpected */
            if ( hbuf_append( c, c->ibuf + 5, rl - 5 ) != 0 ) return -1;
            consume_record( c, rl );
        }
        if ( hs_messages( c ) != 0 ) return -1;
    }
    return 0;
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
        /* made once: a ClientHello sent again after a retry keeps them */
        rng_bytes( c->rng, c->crand, 32 );
        rng_bytes( c->rng, c->sid, 32 );
        if ( eph_make( c, GROUP_P256 ) != 0 ||
             queue_client_hello( c ) != 0 ) {
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
            if (c->si->sha384) {
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
    if (c) {
        if (c->ibuf) free(c->ibuf);
        memset(c, 0, sizeof(*c));               /* keys and secrets go with it */
        free(c);
    }
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

/* TLS 1.3 handshake messages after the handshake: KeyUpdate is acted on (a
   request for one is answered with the next record we send), a session
   ticket is dropped (no resumption is offered).  TLS 1.2's are ignored, as
   they always were.  0 ok / -1 bad. */
static int post_handshake( tls_conn *c, const unsigned char *p, int n )
{
    int r, mtype, blen;
    unsigned char *body;

    if ( !c->tls13 ) return 0;
    if ( hbuf_append( c, p, n ) != 0 ) return -1;
    while ( ( r = hbuf_next( c, &mtype, &body, &blen ) ) == 1 ) {
        if ( mtype == HS_KEY_UPDATE ) {
            if ( blen != 1 || body[0] > 1 ) return -1;
            next_secret( c, c->s_secret );
            install13( c, &c->kr, c->s_secret );
            if ( body[0] == 1 ) c->ku_send = 1;
        } else if ( mtype != HS_NEW_SESSION_TICKET ) {
            return -1;
        }
    }
    if ( hbuf_empty( c ) ) c->hlen = c->hpos = 0;
    return r < 0 ? -1 : 0;
}

/* Protect one outbound fragment.  If the server asked for a KeyUpdate, ours
   goes first, under the old key, and this record under the new one - both in
   'out'.  Returns the bytes written, or -1. */
#define KU_REC  ( TLS_REC_HDR + 4 + 1 + 1 + 16 )    /* a KeyUpdate record */
static int protect_out( tls_conn *c, int type, const unsigned char *in, int len,
                        unsigned char *out, int outcap )
{
    int pre = 0, n;
    if ( c->ku_send ) {
        unsigned char ku[5];
        if ( outcap < KU_REC + TLS_REC_HDR + len + 1 + 16 ) return -1;
        ku[0] = HS_KEY_UPDATE; ku[1] = 0; ku[2] = 0; ku[3] = 1;
        ku[4] = 0;                                  /* update_not_requested */
        pre = tls_protect( &c->kw, TLS_CT_HANDSHAKE, ku, 5, c->rng, out, outcap );
        if ( pre < 0 ) return -1;
        next_secret( c, c->c_secret );
        install13( c, &c->kw, c->c_secret );
        c->ku_send = 0;
    }
    n = tls_protect( &c->kw, type, in, len, c->rng, out + pre, outcap - pre );
    return n < 0 ? -1 : pre + n;
}

int tls_write_app(tls_conn *c, tls_send_fn send, void *ioctx,
                  const unsigned char *data, int len)
{
    unsigned char *rec;
    int reclen, rc;
    rec = (unsigned char *)malloc(len + 128);
    if (rec == NULL) return -1;
    reclen = protect_out(c, TLS_CT_APPLICATION_DATA, data, len, rec, len + 128);
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
        /* an empty record is allowed, and 0 would read as the end */
        if (ct == TLS_CT_APPLICATION_DATA && plen > 0) return plen;
        if (ct == TLS_CT_ALERT) return 0;
        if (ct == TLS_CT_HANDSHAKE && post_handshake(c, out, plen) != 0) return -1;
        /* read more */
    }
}

int tls_app_encrypt(tls_conn *c, const unsigned char *in, int len,
                    unsigned char *out, int outcap)
{
    if (len > TLS_APP_MAX) len = TLS_APP_MAX;
    return protect_out(c, TLS_CT_APPLICATION_DATA, in, len, out, outcap);
}
int tls_app_decrypt(tls_conn *c, const unsigned char *rec, int reclen,
                    unsigned char *out, int outcap, int *content_type)
{
    int plen = tls_unprotect(&c->kr, rec, reclen, content_type, out, outcap);
    if (plen >= 0 && *content_type == TLS_CT_HANDSHAKE &&
        post_handshake(c, out, plen) != 0) {
        return -1;
    }
    return plen;
}

/* --- introspection -------------------------------------------------------
 * Read-only accessors over the private tls_conn, added so a host that does not
 * see this struct (NSTLS32.DLL) can still report what was negotiated and show
 * the peer's chain.  No state is modified.
 * ------------------------------------------------------------------------ */
int tls_get_suite(const tls_conn *c)
{ return (c && c->hs_state == HS_DONE) ? c->suite : 0; }

int tls_get_version(const tls_conn *c)
{
    if (c == NULL || c->hs_state != HS_DONE) return 0;
    return c->tls13 ? 0x0304 : 0x0303;
}

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
    return protect_out(c, content_type, in, len, out, outcap);
}
