/* ==========================================================================
 * SMB2.C - the SMB2/3 client engine.  See SMB2.H.  C89.
 * ==========================================================================
 * The conversation: NEGOTIATE (2.0.2..3.1.1) -> SESSION_SETUP x2 (raw
 * NTLMSSP / NTLMv2 with a MIC) -> TREE_CONNECT -> CREATE (the share root as
 * a directory) -> QUERY_DIRECTORY.  Transport is Direct TCP (the 4-byte
 * length header); the crypto is SMBCRYPT's.  No signing in this cut.
 * ========================================================================== */
#include "smb2.h"
#include "smbcrypt.h"
#include "smbsign.h"
#include "sha.h"
#include "sha512.h"
#include <string.h>

/* ---- little-endian scalars ---- */
static void put16( unsigned char *p, unsigned int v ) { p[0] = (unsigned char)v; p[1] = (unsigned char)( v >> 8 ); }
static void put32( unsigned char *p, unsigned long v ) { p[0] = (unsigned char)v; p[1] = (unsigned char)( v >> 8 ); p[2] = (unsigned char)( v >> 16 ); p[3] = (unsigned char)( v >> 24 ); }
static unsigned int  get16( const unsigned char *p ) { return (unsigned int)p[0] | ( (unsigned int)p[1] << 8 ); }
static unsigned long get32( const unsigned char *p ) { return (unsigned long)p[0] | ( (unsigned long)p[1] << 8 ) | ( (unsigned long)p[2] << 16 ) | ( (unsigned long)p[3] << 24 ); }

/* ---- a small PRNG for challenges / GUID / salt (not for keys) ---- */
static unsigned long prng( smb2_client *cl )
{
    unsigned long seed = cl->seed;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    if ( seed == 0 ) seed = 0x1234567UL;
    cl->seed = seed;
    return seed;
}
static void fill_random( smb2_client *cl, unsigned char *out, int len )
{
    int i;
    for ( i = 0; i < len; i++ ) out[i] = (unsigned char)( prng( cl ) >> ( ( i & 3 ) * 8 ) );
}

/* ---- Direct TCP transport ---- */
static int send_all( smb2_client *cl, const unsigned char *buf, int len )
{
    int off = 0, n;
    while ( off < len ) {
        n = cl->send( cl->ioctx, buf + off, len - off );
        if ( n <= 0 ) return -1;
        off += n;
    }
    return 0;
}

static int read_n( smb2_client *cl, unsigned char *buf, int n )
{
    int off = 0, got;
    while ( off < n ) {
        got = cl->recv( cl->ioctx, buf + off, n - off );
        if ( got <= 0 ) return -1;
        off += got;
    }
    return 0;
}

/* fold one SMB2 message into the running 3.1.1 pre-auth integrity hash */
static void preauth_update( smb2_client *cl, const unsigned char *msg, int len )
{
    SHA384_CTX ctx;
    sha512_init( &ctx );
    sha384_update( &ctx, cl->preauth, 64 );
    sha384_update( &ctx, msg, (unsigned int)len );
    sha512_final( &ctx, cl->preauth );
}

/* send the SMB2 message of 'len' bytes built at cl->sendbuf+4, signing it
   once a session key exists */
static int send_msg( smb2_client *cl, int len )
{
    if ( cl->signing_active ) {
        unsigned char *msg = cl->sendbuf + 4;
        unsigned char  mac[32];
        unsigned long  flags = get32( msg + 16 );
        put32( msg + 16, flags | 0x00000008UL );        /* SMB2_FLAGS_SIGNED */
        memset( msg + 48, 0, 16 );                      /* zero the signature */
        if ( cl->signing_mode == 3 ) {
            aes_cmac( cl->signing_key, msg, len, mac );
        } else {
            hmac_sha256( cl->signing_key, 16, msg, len, mac );
        }
        memcpy( msg + 48, mac, 16 );
    }
    cl->sendbuf[0] = 0;                                 /* Direct TCP: 0 then 24-bit length */
    cl->sendbuf[1] = (unsigned char)( ( len >> 16 ) & 0xFF );
    cl->sendbuf[2] = (unsigned char)( ( len >> 8 ) & 0xFF );
    cl->sendbuf[3] = (unsigned char)( len & 0xFF );
    return send_all( cl, cl->sendbuf, len + 4 );
}

/* read one SMB2 message into cl->recvbuf; cl->recvlen set */
static int recv_msg( smb2_client *cl )
{
    unsigned char hdr[4];
    int len;
    if ( read_n( cl, hdr, 4 ) != 0 ) return -1;
    len = ( (int)hdr[1] << 16 ) | ( (int)hdr[2] << 8 ) | (int)hdr[3];
    if ( len < 64 || len > SMB2_RECVBUF ) return -1;
    if ( read_n( cl, cl->recvbuf, len ) != 0 ) return -1;
    cl->recvlen = len;
    cl->last_status = get32( cl->recvbuf + 8 );
    return 0;
}

/* ---- the SMB2 header ---- */
#define CMD_NEGOTIATE        0
#define CMD_SESSION_SETUP    1
#define CMD_LOGOFF           2
#define CMD_TREE_CONNECT     3
#define CMD_TREE_DISCONNECT  4
#define CMD_CREATE           5
#define CMD_CLOSE            6
#define CMD_READ             8
#define CMD_WRITE            9
#define CMD_QUERY_DIRECTORY  14
#define CMD_SET_INFO         17

#define ST_SUCCESS           0x00000000UL
#define ST_MORE_PROCESSING   0xC0000016UL
#define ST_NO_MORE_FILES     0x80000006UL

static unsigned char *build_header( smb2_client *cl, int cmd )
{
    unsigned char *msg = cl->sendbuf + 4;
    memset( msg, 0, 64 );
    msg[0] = 0xFE; msg[1] = 'S'; msg[2] = 'M'; msg[3] = 'B';
    put16( msg + 4, 64 );                   /* StructureSize */
    put16( msg + 6, 1 );                    /* CreditCharge */
    put16( msg + 12, (unsigned int)cmd );   /* Command */
    put16( msg + 14, 256 );                 /* CreditRequest */
    put32( msg + 24, cl->msgid_lo );        /* MessageId (low 32) */
    put32( msg + 28, cl->msgid_hi );
    put32( msg + 36, cl->tree_id );         /* TreeId */
    memcpy( msg + 40, cl->session_id, 8 );  /* SessionId */
    cl->msgid_lo++;
    if ( cl->msgid_lo == 0 ) cl->msgid_hi++;
    return msg;
}

/* ==========================================================================
 * NEGOTIATE
 * ========================================================================== */
static int do_negotiate( smb2_client *cl )
{
    unsigned char *msg = build_header( cl, CMD_NEGOTIATE );
    unsigned char *b = msg + 64;
    int off, ctxoff, msglen;

    cl->stage = "negotiate";

    put16( b + 0, 36 );                     /* StructureSize */
    put16( b + 2, 5 );                      /* DialectCount */
    put16( b + 4, 1 );                      /* SecurityMode = SIGNING_ENABLED */
    put16( b + 6, 0 );                      /* Reserved */
    put32( b + 8, 0 );                      /* Capabilities */
    fill_random( cl, b + 12, 16 );          /* ClientGuid */
    /* b+28..35: NegotiateContextOffset/Count/Reserved2 (filled after dialects) */
    put16( b + 36, 0x0202 );
    put16( b + 38, 0x0210 );
    put16( b + 40, 0x0300 );
    put16( b + 42, 0x0302 );
    put16( b + 44, 0x0311 );
    off = 46;                               /* body offset after the dialects */

    /* 8-byte-align the context list against the SMB2 header start (64+off) */
    while ( ( ( 64 + off ) & 7 ) != 0 ) b[off++] = 0;
    ctxoff = 64 + off;                      /* NegotiateContextOffset */

    /* SMB2_PREAUTH_INTEGRITY_CAPABILITIES */
    put16( b + off + 0, 0x0001 );           /* ContextType */
    put16( b + off + 2, 38 );               /* DataLength */
    put32( b + off + 4, 0 );                /* Reserved */
    put16( b + off + 8, 1 );                /* HashAlgorithmCount */
    put16( b + off + 10, 32 );              /* SaltLength */
    put16( b + off + 12, 0x0001 );          /* SHA-512 */
    fill_random( cl, b + off + 14, 32 );    /* Salt */
    off += 8 + 38;
    while ( ( ( 64 + off ) & 7 ) != 0 ) b[off++] = 0;

    /* SMB2_ENCRYPTION_CAPABILITIES (advertised; we do not actually encrypt) */
    put16( b + off + 0, 0x0002 );
    put16( b + off + 2, 6 );
    put32( b + off + 4, 0 );
    put16( b + off + 8, 2 );                /* CipherCount */
    put16( b + off + 10, 0x0002 );          /* AES-128-GCM */
    put16( b + off + 12, 0x0001 );          /* AES-128-CCM */
    off += 8 + 6;

    put32( b + 28, (unsigned long)ctxoff ); /* NegotiateContextOffset */
    put16( b + 32, 2 );                     /* NegotiateContextCount */
    put16( b + 34, 0 );                     /* Reserved2 */

    msglen = 64 + off;
    if ( send_msg( cl, msglen ) != 0 ) return -1;
    preauth_update( cl, cl->sendbuf + 4, msglen );
    if ( recv_msg( cl ) != 0 ) return -1;
    preauth_update( cl, cl->recvbuf, cl->recvlen );
    if ( cl->last_status != ST_SUCCESS ) return -1;
    if ( cl->recvlen < 72 ) return -1;                  /* need the dialect field */

    cl->dialect = (int)get16( cl->recvbuf + 64 + 4 );
    cl->signing_required = ( get16( cl->recvbuf + 64 + 2 ) & 0x0002 ) ? 1 : 0;
    return 0;
}

/* ==========================================================================
 * NTLMSSP / NTLMv2
 * ========================================================================== */
#define NTLMF_UNICODE        0x00000001UL
#define NTLMF_REQUEST_TARGET 0x00000004UL
#define NTLMF_SIGN           0x00000010UL
#define NTLMF_NTLM           0x00000200UL
#define NTLMF_ALWAYS_SIGN    0x00008000UL
#define NTLMF_EXT_SESSEC     0x00080000UL
#define NTLMF_VERSION        0x02000000UL
#define NTLMF_128            0x20000000UL
#define NTLMF_56             0x80000000UL

static int build_ntlm_negotiate( smb2_client *cl, unsigned char *out )
{
    memset( out, 0, 40 );
    memcpy( out, "NTLMSSP", 8 );            /* includes the NUL */
    put32( out + 8, 1 );                    /* MessageType = NEGOTIATE */
    put32( out + 12, NTLMF_UNICODE | NTLMF_REQUEST_TARGET | NTLMF_NTLM |
                     NTLMF_ALWAYS_SIGN | NTLMF_EXT_SESSEC | NTLMF_VERSION |
                     NTLMF_128 | NTLMF_56 );
    /* DomainNameFields (8) + WorkstationFields (8) all zero -> no names */
    /* Version (8) at offset 32 - left zero */
    return 40;
}

/* find an AV pair in the CHALLENGE target info; returns value ptr+len or 0 */
static const unsigned char *av_find( const unsigned char *ti, int tilen, int id, int *vlen )
{
    int pos = 0;
    while ( pos + 4 <= tilen ) {
        int avid = (int)get16( ti + pos );
        int avlen = (int)get16( ti + pos + 2 );
        if ( avid == 0 ) break;             /* MsvAvEOL */
        if ( pos + 4 + avlen > tilen ) break;
        if ( avid == id ) { *vlen = avlen; return ti + pos + 4; }
        pos += 4 + avlen;
    }
    return (const unsigned char *)0;
}

/* Build the NTLMv2 AUTHENTICATE (type 3) with a MIC into 'out'; returns len.
   Needs the stored NEGOTIATE and CHALLENGE messages for the MIC. */
static int build_ntlm_authenticate( smb2_client *cl, unsigned char *out,
                                     const char *user, const char *password, const char *domain )
{
    const unsigned char *chal = cl->ntlm_challenge;
    int   challen = cl->ntlm_challenge_len;
    const unsigned char *server_chal;
    const unsigned char *ti;
    int   tilen, tioff;
    const unsigned char *ts;
    int   tslen = 0;
    unsigned char respkey[16];
    unsigned char client_chal[8];
    unsigned char temp[1024];
    int   templen;
    unsigned char ntproof[16];
    unsigned char ntresp[16 + 1024];
    int   ntresp_len;
    unsigned char session_base[16];
    unsigned char exported_key[16];
    unsigned char timestamp[8];
    int   pos, payload;
    int   dlen, ulen, nt_off, payoff;
    unsigned char dwide[512], uwide[512];
    int   dwlen, uwlen;

    if ( challen < 56 ) return -1;
    server_chal = chal + 24;                /* ServerChallenge */
    tioff = (int)get32( chal + 44 );        /* TargetInfoFields: offset */
    tilen = (int)get16( chal + 40 );        /* TargetInfoFields: len */
    /* bound each against challen separately so a hostile offset cannot
       overflow the sum */
    if ( tioff < 0 || tilen < 0 || tioff > challen || tilen > challen - tioff ) return -1;
    ti = chal + tioff;

    ntowf_v2( password, user, domain, respkey );

    /* timestamp: MsvAvTimestamp (0x07) from the target info, else zero */
    memset( timestamp, 0, 8 );
    ts = av_find( ti, tilen, 0x07, &tslen );
    if ( ts && tslen == 8 ) memcpy( timestamp, ts, 8 );

    fill_random( cl, client_chal, 8 );

    /* temp = 01 01 00*6 | timestamp | client_chal | 00*4 | targetinfo(+MsvAvFlags) | 00*4 */
    pos = 0;
    temp[pos++] = 0x01; temp[pos++] = 0x01;
    temp[pos++] = 0; temp[pos++] = 0; temp[pos++] = 0; temp[pos++] = 0; temp[pos++] = 0; temp[pos++] = 0;
    memcpy( temp + pos, timestamp, 8 ); pos += 8;
    memcpy( temp + pos, client_chal, 8 ); pos += 8;
    temp[pos++] = 0; temp[pos++] = 0; temp[pos++] = 0; temp[pos++] = 0;
    /* copy the server's AV pairs up to (not including) MsvAvEOL */
    {
        int tp = 0;
        while ( tp + 4 <= tilen ) {
            int avid = (int)get16( ti + tp );
            int avlen = (int)get16( ti + tp + 2 );
            if ( avid == 0 ) break;
            if ( tp + 4 + avlen > tilen ) break;
            if ( pos + 4 + avlen > (int)sizeof( temp ) - 16 ) return -1;
            memcpy( temp + pos, ti + tp, 4 + avlen ); pos += 4 + avlen;
            tp += 4 + avlen;
        }
        /* MsvAvFlags = 0x02 (a MIC follows) */
        put16( temp + pos, 0x0006 ); put16( temp + pos + 2, 4 ); put32( temp + pos + 4, 2 ); pos += 8;
        /* MsvAvEOL */
        put16( temp + pos, 0 ); put16( temp + pos + 2, 0 ); pos += 4;
    }
    temp[pos++] = 0; temp[pos++] = 0; temp[pos++] = 0; temp[pos++] = 0;
    templen = pos;

    /* NTProofStr = HMAC-MD5(respkey, server_chal || temp) */
    {
        unsigned char cat[8 + 1024];
        memcpy( cat, server_chal, 8 );
        memcpy( cat + 8, temp, templen );
        hmac_md5( respkey, 16, cat, 8 + templen, ntproof );
    }
    memcpy( ntresp, ntproof, 16 );
    memcpy( ntresp + 16, temp, templen );
    ntresp_len = 16 + templen;

    hmac_md5( respkey, 16, ntproof, 16, session_base );
    memcpy( exported_key, session_base, 16 );   /* no key exchange */
    memcpy( cl->session_key, exported_key, 16 );

    /* names in UTF-16LE */
    dwlen = utf16le( domain, dwide, sizeof( dwide ) );
    uwlen = utf16le( user, uwide, sizeof( uwide ) );
    dlen = dwlen; ulen = uwlen;

    /* fixed header: 8 sig + 4 type + 6*8 fields + 4 flags + 8 version + 16 MIC = 88 */
    memset( out, 0, 88 );
    memcpy( out, "NTLMSSP", 8 );
    put32( out + 8, 3 );                    /* MessageType = AUTHENTICATE */
    put32( out + 60, NTLMF_UNICODE | NTLMF_REQUEST_TARGET | NTLMF_NTLM |
                     NTLMF_ALWAYS_SIGN | NTLMF_EXT_SESSEC | NTLMF_VERSION |
                     NTLMF_128 | NTLMF_56 );
    payoff = 88;
    payload = payoff;

    /* LmChallengeResponse: 24 zero bytes (timestamp present) */
    put16( out + 12, 24 ); put16( out + 14, 24 ); put32( out + 16, (unsigned long)payload );
    memset( out + payload, 0, 24 ); payload += 24;

    /* NtChallengeResponse */
    nt_off = payload;
    put16( out + 20, (unsigned int)ntresp_len ); put16( out + 22, (unsigned int)ntresp_len );
    put32( out + 24, (unsigned long)nt_off );
    memcpy( out + payload, ntresp, ntresp_len ); payload += ntresp_len;

    /* DomainName */
    put16( out + 28, (unsigned int)dlen ); put16( out + 30, (unsigned int)dlen ); put32( out + 32, (unsigned long)payload );
    memcpy( out + payload, dwide, dlen ); payload += dlen;

    /* UserName */
    put16( out + 36, (unsigned int)ulen ); put16( out + 38, (unsigned int)ulen ); put32( out + 40, (unsigned long)payload );
    memcpy( out + payload, uwide, ulen ); payload += ulen;

    /* Workstation (empty) */
    put16( out + 44, 0 ); put16( out + 46, 0 ); put32( out + 48, (unsigned long)payload );

    /* EncryptedRandomSessionKey (empty, no key exchange) */
    put16( out + 52, 0 ); put16( out + 54, 0 ); put32( out + 56, (unsigned long)payload );

    /* MIC = HMAC-MD5(exported_key, NEGOTIATE || CHALLENGE || AUTHENTICATE(MIC=0)) */
    {
        static unsigned char mbuf[4096];    /* >= negotiate(<=64) + challenge(<=1024) + authenticate(<=2172) */
        int mlen = 0;
        memset( out + 72, 0, 16 );          /* MIC field zeroed for the computation */
        memcpy( mbuf + mlen, cl->ntlm_negotiate, cl->ntlm_negotiate_len ); mlen += cl->ntlm_negotiate_len;
        memcpy( mbuf + mlen, cl->ntlm_challenge, cl->ntlm_challenge_len ); mlen += cl->ntlm_challenge_len;
        memcpy( mbuf + mlen, out, payload ); mlen += payload;
        hmac_md5( exported_key, 16, mbuf, mlen, out + 72 );
    }

    return payload;
}

/* derive the signing key from the session key once auth is done */
static void derive_signing( smb2_client *cl )
{
    if ( cl->dialect == 0x0311 ) {
        smb3_kdf( cl->session_key, (const unsigned char *)"SMBSigningKey", 14,
                  cl->preauth, 64, cl->signing_key );
        cl->signing_mode = 3;
    } else if ( cl->dialect == 0x0300 || cl->dialect == 0x0302 ) {
        smb3_kdf( cl->session_key, (const unsigned char *)"SMB2AESCMAC", 12,
                  (const unsigned char *)"SmbSign", 8, cl->signing_key );
        cl->signing_mode = 3;
    } else {                                    /* 2.0.2 / 2.1: HMAC-SHA256 */
        memcpy( cl->signing_key, cl->session_key, 16 );
        cl->signing_mode = 2;
    }
    cl->signing_active = 1;
}

/* ==========================================================================
 * SESSION_SETUP (two round trips)
 * ========================================================================== */
static int do_session_setup( smb2_client *cl, const char *user, const char *password, const char *domain )
{
    unsigned char *msg, *b;
    unsigned char ntlm[4096];           /* holds the NTLMv2 AUTHENTICATE, bounded by the <=1024 challenge */
    int ntlmlen, secoff, msglen, blen;

    cl->stage = "session-setup(1)";

    /* --- request 1: NTLMSSP NEGOTIATE --- */
    ntlmlen = build_ntlm_negotiate( cl, ntlm );
    memcpy( cl->ntlm_negotiate, ntlm, ntlmlen );
    cl->ntlm_negotiate_len = ntlmlen;

    msg = build_header( cl, CMD_SESSION_SETUP );
    b = msg + 64;
    memset( b, 0, 24 );
    put16( b + 0, 25 );                     /* StructureSize */
    b[2] = 0;                               /* Flags */
    b[3] = 1;                               /* SecurityMode = SIGNING_ENABLED */
    put32( b + 4, 0 );                      /* Capabilities */
    put32( b + 8, 0 );                      /* Channel */
    secoff = 64 + 24;
    put16( b + 12, (unsigned int)secoff );  /* SecurityBufferOffset */
    put16( b + 14, (unsigned int)ntlmlen ); /* SecurityBufferLength */
    memcpy( msg + secoff, ntlm, ntlmlen );
    msglen = secoff + ntlmlen;
    if ( send_msg( cl, msglen ) != 0 ) return -1;
    preauth_update( cl, cl->sendbuf + 4, msglen );
    if ( recv_msg( cl ) != 0 ) return -1;
    preauth_update( cl, cl->recvbuf, cl->recvlen );
    if ( cl->last_status != ST_MORE_PROCESSING ) return -1;
    if ( cl->recvlen < 72 ) return -1;                  /* need the SecurityBuffer fields */

    /* keep the assigned SessionId and the CHALLENGE token */
    memcpy( cl->session_id, cl->recvbuf + 40, 8 );
    b = cl->recvbuf + 64;
    secoff = (int)get16( b + 4 );
    blen = (int)get16( b + 6 );
    if ( secoff < 0 || blen < 0 || secoff + blen > cl->recvlen || blen > (int)sizeof( cl->ntlm_challenge ) ) return -1;
    memcpy( cl->ntlm_challenge, cl->recvbuf + secoff, blen );
    cl->ntlm_challenge_len = blen;

    /* --- request 2: NTLMSSP AUTHENTICATE --- */
    cl->stage = "session-setup(2)";
    ntlmlen = build_ntlm_authenticate( cl, ntlm, user, password, domain );
    if ( ntlmlen < 0 ) return -1;

    msg = build_header( cl, CMD_SESSION_SETUP );
    b = msg + 64;
    memset( b, 0, 24 );
    put16( b + 0, 25 );
    b[2] = 0;
    b[3] = 1;
    secoff = 64 + 24;
    put16( b + 12, (unsigned int)secoff );
    put16( b + 14, (unsigned int)ntlmlen );
    memcpy( msg + secoff, ntlm, ntlmlen );
    msglen = secoff + ntlmlen;
    if ( send_msg( cl, msglen ) != 0 ) return -1;
    preauth_update( cl, cl->sendbuf + 4, msglen );      /* request included; final response not */
    if ( recv_msg( cl ) != 0 ) return -1;
    if ( cl->last_status != ST_SUCCESS ) return -1;

    derive_signing( cl );                               /* keys ready; sign from here on */
    return 0;
}

/* ==========================================================================
 * TREE_CONNECT
 * ========================================================================== */
static int do_tree_connect( smb2_client *cl, const char *server, const char *share )
{
    unsigned char *msg, *b;
    char path[300];
    unsigned char wide[600];
    int widelen, pathoff, msglen, pos = 0;

    cl->stage = "tree-connect";

    /* \\server\share */
    path[pos++] = '\\'; path[pos++] = '\\';
    { int i; for ( i = 0; server[i] && pos < 290; i++ ) path[pos++] = server[i]; }
    path[pos++] = '\\';
    { int i; for ( i = 0; share[i] && pos < 298; i++ ) path[pos++] = share[i]; }
    path[pos] = 0;
    widelen = utf16le( path, wide, sizeof( wide ) );

    msg = build_header( cl, CMD_TREE_CONNECT );
    b = msg + 64;
    put16( b + 0, 9 );                      /* StructureSize */
    put16( b + 2, 0 );                      /* Flags/Reserved */
    pathoff = 64 + 8;
    put16( b + 4, (unsigned int)pathoff );  /* PathOffset */
    put16( b + 6, (unsigned int)widelen );  /* PathLength */
    memcpy( msg + pathoff, wide, widelen );
    msglen = pathoff + widelen;
    if ( send_msg( cl, msglen ) != 0 ) return -1;
    if ( recv_msg( cl ) != 0 ) return -1;
    if ( cl->last_status != ST_SUCCESS ) return -1;

    cl->tree_id = get32( cl->recvbuf + 36 );
    return 0;
}

/* ==========================================================================
 * CREATE the share root as a directory, then QUERY_DIRECTORY
 * ========================================================================== */
/* CREATE: open 'path' (share-relative, backslash, no leading slash; "" is the
   share root) with the given access/options/disposition.  Fills file_id and,
   when asked, the EndOfFile size and attributes from the response. */
static int do_create( smb2_client *cl, const char *path,
                      unsigned long access, unsigned long options, unsigned long disposition,
                      unsigned char file_id[16],
                      unsigned long *size_lo, unsigned long *size_hi, unsigned long *attr )
{
    unsigned char *msg, *b;
    unsigned char wide[600];
    int namelen, msglen;

    namelen = utf16le( path, wide, sizeof( wide ) );

    msg = build_header( cl, CMD_CREATE );
    b = msg + 64;
    memset( b, 0, 57 );
    put16( b + 0, 57 );                     /* StructureSize (56 fixed + 1 buffer) */
    put32( b + 8, 0x00000002UL );           /* ImpersonationLevel = Impersonation */
    put32( b + 24, access );
    put32( b + 32, 0x00000007UL );          /* ShareAccess READ|WRITE|DELETE */
    put32( b + 36, disposition );
    put32( b + 40, options );
    put16( b + 44, (unsigned int)( 64 + 56 ) ); /* NameOffset */
    put16( b + 46, (unsigned int)namelen );     /* NameLength */
    if ( namelen > 0 ) {
        memcpy( msg + 64 + 56, wide, namelen );
        msglen = 64 + 56 + namelen;
    } else {
        msglen = 64 + 57;                   /* the 1-byte Buffer placeholder */
    }
    if ( send_msg( cl, msglen ) != 0 ) return -1;
    if ( recv_msg( cl ) != 0 ) return -1;
    if ( cl->last_status != ST_SUCCESS ) return -1;
    if ( cl->recvlen < 64 + 64 + 16 ) return -1;        /* FileId must be present */

    b = cl->recvbuf + 64;
    if ( attr )    *attr    = get32( b + 56 );          /* FileAttributes */
    if ( size_lo ) *size_lo = get32( b + 48 );          /* EndOfFile low */
    if ( size_hi ) *size_hi = get32( b + 52 );          /* EndOfFile high */
    memcpy( file_id, cl->recvbuf + 64 + 64, 16 );
    return 0;
}

int smb2_open_disp( smb2_client *cl, const char *path, int write,
                    unsigned long disposition, smb2_fh *fh )
{
    unsigned long access = 0x00100081UL;    /* FILE_READ_DATA|READ_ATTRIBUTES|SYNCHRONIZE */

    cl->stage = "open";
    if ( write )  access |= 0x00000002UL | 0x00000004UL;    /* FILE_WRITE_DATA|FILE_APPEND_DATA */
    memset( fh, 0, sizeof( *fh ) );
    return do_create( cl, path, access, 0x00000040UL /* FILE_NON_DIRECTORY_FILE */,
                      disposition, fh->file_id, &fh->size_lo, &fh->size_hi, &fh->attr );
}

int smb2_open_file( smb2_client *cl, const char *path, int write, int create, smb2_fh *fh )
{
    return smb2_open_disp( cl, path, write, create ? 0x00000003UL : 0x00000001UL, fh );
}

int smb2_read( smb2_client *cl, const smb2_fh *fh, unsigned long off_lo, unsigned long off_hi,
               unsigned char *buf, int len )
{
    unsigned char *msg, *b;
    int msglen, dataoff, datalen, maxlen;

    maxlen = SMB2_RECVBUF - 512;
    if ( len > maxlen ) len = maxlen;
    if ( len <= 0 ) return 0;

    cl->stage = "read";
    msg = build_header( cl, CMD_READ );
    b = msg + 64;
    memset( b, 0, 48 );
    put16( b + 0, 49 );                     /* StructureSize (48 fixed + 1 buffer) */
    put32( b + 4, (unsigned long)len );     /* Length */
    put32( b + 8, off_lo );                  /* Offset low */
    put32( b + 12, off_hi );                 /* Offset high */
    memcpy( b + 16, fh->file_id, 16 );
    msglen = 64 + 49;
    if ( send_msg( cl, msglen ) != 0 ) return -1;
    if ( recv_msg( cl ) != 0 ) return -1;
    if ( cl->last_status == 0xC0000011UL ) return 0;    /* STATUS_END_OF_FILE */
    if ( cl->last_status != ST_SUCCESS ) return -1;
    if ( cl->recvlen < 64 + 16 ) return -1;

    b = cl->recvbuf + 64;
    dataoff = (int)b[2];                    /* DataOffset, from the SMB2 header start */
    datalen = (int)get32( b + 4 );          /* DataLength */
    if ( dataoff < 64 || datalen < 0 || dataoff > cl->recvlen || datalen > cl->recvlen - dataoff )
        return -1;
    if ( datalen > len ) datalen = len;
    memcpy( buf, cl->recvbuf + dataoff, datalen );
    return datalen;
}

int smb2_write( smb2_client *cl, const smb2_fh *fh, unsigned long off_lo, unsigned long off_hi,
                const unsigned char *buf, int len )
{
    unsigned char *msg, *b;
    int msglen, dataoff, maxlen;

    maxlen = SMB2_SENDBUF - 128;
    if ( len > maxlen ) len = maxlen;
    if ( len <= 0 ) return 0;

    cl->stage = "write";
    msg = build_header( cl, CMD_WRITE );
    b = msg + 64;
    memset( b, 0, 48 );
    dataoff = 64 + 48;
    put16( b + 0, 49 );                     /* StructureSize */
    put16( b + 2, (unsigned int)dataoff );  /* DataOffset */
    put32( b + 4, (unsigned long)len );     /* Length */
    put32( b + 8, off_lo );
    put32( b + 12, off_hi );
    memcpy( b + 16, fh->file_id, 16 );
    memcpy( msg + dataoff, buf, len );
    msglen = dataoff + len;
    if ( send_msg( cl, msglen ) != 0 ) return -1;
    if ( recv_msg( cl ) != 0 ) return -1;
    if ( cl->last_status != ST_SUCCESS ) return -1;
    if ( cl->recvlen < 64 + 8 ) return -1;
    return (int)get32( cl->recvbuf + 64 + 4 );          /* Count written */
}

int smb2_delete( smb2_client *cl, const char *path )
{
    smb2_fh fh;
    cl->stage = "delete";
    memset( &fh, 0, sizeof( fh ) );
    /* DELETE access, FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, FILE_OPEN */
    if ( do_create( cl, path, 0x00010000UL, 0x00001040UL, 0x00000001UL,
                    fh.file_id, (unsigned long *)0, (unsigned long *)0, (unsigned long *)0 ) != 0 )
        return -1;
    return smb2_close_file( cl, &fh );
}

int smb2_mkdir( smb2_client *cl, const char *path )
{
    smb2_fh fh;
    cl->stage = "mkdir";
    memset( &fh, 0, sizeof( fh ) );
    /* GENERIC-ish access, FILE_DIRECTORY_FILE, FILE_CREATE (fail if exists) */
    if ( do_create( cl, path, 0x00100081UL, 0x00000001UL, 0x00000002UL,
                    fh.file_id, (unsigned long *)0, (unsigned long *)0, (unsigned long *)0 ) != 0 )
        return -1;
    return smb2_close_file( cl, &fh );
}

int smb2_rmdir( smb2_client *cl, const char *path )
{
    smb2_fh fh;
    cl->stage = "rmdir";
    memset( &fh, 0, sizeof( fh ) );
    /* DELETE access, FILE_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, FILE_OPEN */
    if ( do_create( cl, path, 0x00010000UL, 0x00001001UL, 0x00000001UL,
                    fh.file_id, (unsigned long *)0, (unsigned long *)0, (unsigned long *)0 ) != 0 )
        return -1;
    return smb2_close_file( cl, &fh );
}

/* RENAME: open the source for DELETE, then SET_INFO with
   FileRenameInformation (the new name, UTF-16, relative to the share root
   with no leading separator), then close.  Only ever reached for an SMB2/3
   server; the WfW-bridge rig proves the SMB1 path (smb1_rename). */
int smb2_rename( smb2_client *cl, const char *oldpath, const char *newpath )
{
    smb2_fh fh;
    unsigned char *msg, *b, *rbuf;
    unsigned char wide[600];
    const char *nm = newpath;
    int namelen, msglen, buflen;

    cl->stage = "rename";
    memset( &fh, 0, sizeof( fh ) );
    /* DELETE | SYNCHRONIZE access, no special options (file or dir), FILE_OPEN */
    if ( do_create( cl, oldpath, 0x00010000UL | 0x00100000UL, 0x00000000UL, 0x00000001UL,
                    fh.file_id, (unsigned long *)0, (unsigned long *)0, (unsigned long *)0 ) != 0 )
        return -1;

    while ( *nm == '\\' || *nm == '/' ) nm++;
    namelen = utf16le( nm, wide, sizeof( wide ) );
    buflen = 20 + namelen;                  /* FILE_RENAME_INFORMATION_TYPE_2 */

    msg = build_header( cl, CMD_SET_INFO );
    b = msg + 64;
    memset( b, 0, 32 );
    put16( b + 0, 33 );                     /* StructureSize (32 fixed + 1 buffer) */
    b[2] = 0x01;                            /* InfoType = SMB2_0_INFO_FILE */
    b[3] = 10;                              /* FileInfoClass = FileRenameInformation */
    put32( b + 4, (unsigned long)buflen );  /* BufferLength */
    put16( b + 8, (unsigned int)( 64 + 32 ) );  /* BufferOffset (from header start) */
    memcpy( b + 16, fh.file_id, 16 );       /* FileId */

    rbuf = msg + 64 + 32;
    memset( rbuf, 0, 20 );
    rbuf[0] = 1;                            /* ReplaceIfExists */
    put32( rbuf + 16, (unsigned long)namelen ); /* FileNameLength (bytes) */
    memcpy( rbuf + 20, wide, namelen );

    msglen = 64 + 32 + buflen;
    if ( send_msg( cl, msglen ) != 0 )          { smb2_close_file( cl, &fh ); return -1; }
    if ( recv_msg( cl ) != 0 )                  { smb2_close_file( cl, &fh ); return -1; }
    if ( cl->last_status != ST_SUCCESS )        { smb2_close_file( cl, &fh ); return -1; }
    return smb2_close_file( cl, &fh );
}

int smb2_close_file( smb2_client *cl, const smb2_fh *fh )
{
    unsigned char *msg, *b;

    cl->stage = "close-file";
    msg = build_header( cl, CMD_CLOSE );
    b = msg + 64;
    memset( b, 0, 24 );
    put16( b + 0, 24 );                     /* StructureSize */
    memcpy( b + 8, fh->file_id, 16 );
    if ( send_msg( cl, 64 + 24 ) != 0 ) return -1;
    if ( recv_msg( cl ) != 0 ) return -1;
    if ( cl->last_status != ST_SUCCESS ) return -1;
    return 0;
}

int smb2_list_dir( smb2_client *cl, const char *dir, const char *pattern,
                   void ( *emit )( void *user, const smb2_dirent *entry ),
                   void *user )
{
    unsigned char file_id[16];
    unsigned char *msg, *b;
    unsigned char wide[16];
    smb2_fh dirfh;
    int widelen, patoff, msglen, restart = 1, rc = 0;

    /* open the directory: FILE_LIST_DIRECTORY|READ_ATTRIBUTES|SYNCHRONIZE,
       FILE_DIRECTORY_FILE, FILE_OPEN */
    cl->stage = "create";
    if ( do_create( cl, dir, 0x00100081UL, 0x00000001UL, 0x00000001UL,
                    file_id, (unsigned long *)0, (unsigned long *)0, (unsigned long *)0 ) != 0 )
        return -1;
    widelen = utf16le( pattern, wide, sizeof( wide ) );

    for ( ;; ) {
        int outoff, outlen, pos;

        cl->stage = "query-directory";
        msg = build_header( cl, CMD_QUERY_DIRECTORY );
        b = msg + 64;
        memset( b, 0, 32 );
        put16( b + 0, 33 );                 /* StructureSize */
        b[2] = 0x01;                        /* FileDirectoryInformation */
        b[3] = (unsigned char)( restart ? 0x01 : 0x00 );   /* RESTART_SCANS */
        put32( b + 4, 0 );                  /* FileIndex */
        memcpy( b + 8, file_id, 16 );
        patoff = 64 + 32;
        put16( b + 24, (unsigned int)patoff );
        put16( b + 26, (unsigned int)widelen );
        put32( b + 28, SMB2_RECVBUF - 512 );/* OutputBufferLength */
        memcpy( msg + patoff, wide, widelen );
        msglen = patoff + widelen;
        restart = 0;

        if ( send_msg( cl, msglen ) != 0 ) { rc = -1; break; }
        if ( recv_msg( cl ) != 0 ) { rc = -1; break; }
        if ( cl->last_status == ST_NO_MORE_FILES ) break;
        if ( cl->last_status != ST_SUCCESS ) { rc = -1; break; }

        b = cl->recvbuf + 64;
        outoff = (int)get16( b + 2 );
        outlen = (int)get32( b + 4 );
        /* bound each against recvlen separately: a hostile length must not
           overflow the sum, and the window must lie inside what we received */
        if ( outoff < 0 || outlen < 0 || outoff > cl->recvlen || outlen > cl->recvlen - outoff ) { rc = -1; break; }

        /* every read below is kept inside [outoff, outoff+outlen) */
        pos = outoff;
        while ( pos + 64 <= outoff + outlen ) {
            const unsigned char *e = cl->recvbuf + pos;
            unsigned long nextoff = get32( e + 0 );
            unsigned long namelen = get32( e + 60 );
            unsigned long avail   = (unsigned long)( outoff + outlen - pos - 64 );
            smb2_dirent ent;
            unsigned int i, j = 0;

            ent.wtime_lo = get32( e + 24 );             /* LastWriteTime */
            ent.wtime_hi = get32( e + 28 );
            ent.size_lo  = get32( e + 40 );             /* EndOfFile */
            ent.size_hi  = get32( e + 44 );
            ent.attr     = get32( e + 56 );             /* FileAttributes */
            if ( namelen > avail ) namelen = avail;     /* clamp the name to the buffer */
            for ( i = 0; i + 1 < namelen && j + 1 < sizeof( ent.name ); i += 2 )
                ent.name[j++] = (char)e[64 + i];        /* UTF-16LE -> ASCII (test names are ASCII) */
            ent.name[j] = 0;
            emit( user, &ent );

            if ( nextoff == 0 || nextoff > (unsigned long)outlen ) break;   /* last, or out of range */
            pos += (int)nextoff;                         /* forward (nextoff <= outlen); while re-bounds */
        }
    }

    /* close the directory handle however the loop ended */
    memset( &dirfh, 0, sizeof( dirfh ) );
    memcpy( dirfh.file_id, file_id, 16 );
    smb2_close_file( cl, &dirfh );
    return rc;
}

/* the old name: list the share root, with the plain (name, isdir, size)
   callback the host harness and SMBTEST use */
typedef struct { void ( *emit )( void *, const char *, int, unsigned long ); void *user; } list_shim_ctx;

static void list_shim( void *user, const smb2_dirent *entry )
{
    list_shim_ctx *ctx = (list_shim_ctx *)user;
    ctx->emit( ctx->user, entry->name, ( entry->attr & 0x10 ) ? 1 : 0, entry->size_lo );
}

int smb2_list( smb2_client *cl, const char *pattern,
               void ( *emit )( void *user, const char *name, int isdir, unsigned long size ),
               void *user )
{
    list_shim_ctx ctx;
    ctx.emit = emit;
    ctx.user = user;
    return smb2_list_dir( cl, "", pattern, list_shim, &ctx );
}

/* ==========================================================================
 * connect / disconnect
 * ========================================================================== */
int smb2_connect( smb2_client *cl, smb_send_fn send, smb_recv_fn recv, void *ioctx,
                  unsigned long seed,
                  const char *server, const char *share,
                  const char *user, const char *password, const char *domain )
{
    memset( cl, 0, sizeof( *cl ) );
    cl->send = send; cl->recv = recv; cl->ioctx = ioctx;
    cl->seed = seed ? seed : 0x2545F491UL;
    cl->stage = "start";

    if ( do_negotiate( cl ) != 0 ) return -1;
    if ( do_session_setup( cl, user, password, domain ) != 0 ) return -1;
    if ( do_tree_connect( cl, server, share ) != 0 ) return -1;
    cl->stage = "connected";
    return 0;
}

void smb2_disconnect( smb2_client *cl )
{
    unsigned char *msg, *b;

    if ( cl->tree_id ) {
        msg = build_header( cl, CMD_TREE_DISCONNECT );
        b = msg + 64;
        put16( b + 0, 4 ); put16( b + 2, 0 );
        if ( send_msg( cl, 64 + 4 ) == 0 ) recv_msg( cl );
        cl->tree_id = 0;
    }
    if ( cl->session_id[0] || cl->session_id[1] || cl->session_id[2] || cl->session_id[3] ) {
        msg = build_header( cl, CMD_LOGOFF );
        b = msg + 64;
        put16( b + 0, 4 ); put16( b + 2, 0 );
        if ( send_msg( cl, 64 + 4 ) == 0 ) recv_msg( cl );
        memset( cl->session_id, 0, 8 );
    }
}

int           smb2_dialect( const smb2_client *cl ) { return cl->dialect; }
const char   *smb2_stage( const smb2_client *cl )   { return cl->stage ? cl->stage : "?"; }
unsigned long smb2_status( const smb2_client *cl )  { return cl->last_status; }
