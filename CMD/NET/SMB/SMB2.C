/* ==========================================================================
 * SMB2.C - the SMB2/3 client engine.  See SMB2.H.  C89.
 * ==========================================================================
 * The conversation: an SMB1 NEGOTIATE offering the SMB2 dialects, as
 * Windows opens -> NEGOTIATE (2.0.2..3.1.1) -> SESSION_SETUP x2 (raw
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
static void put16( uint8_t *p, unsigned int v ) { p[0] = (uint8_t)v; p[1] = (uint8_t)( v >> 8 ); }
static void put32( uint8_t *p, uint32_t v ) { p[0] = (uint8_t)v; p[1] = (uint8_t)( v >> 8 ); p[2] = (uint8_t)( v >> 16 ); p[3] = (uint8_t)( v >> 24 ); }
static unsigned int  get16( const uint8_t *p ) { return (unsigned int)p[0] | ( (unsigned int)p[1] << 8 ); }
static uint32_t      get32( const uint8_t *p ) { return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 ); }

/* ---- a small PRNG for challenges / GUID / salt (not for keys) ---- */
static uint32_t prng( smb2_client *cl )
{
    uint32_t seed = cl->seed;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    if ( seed == 0 ) seed = 0x1234567UL;
    cl->seed = seed;
    return seed;
}
static void fill_random( smb2_client *cl, uint8_t *out, int len )
{
    int i;
    for ( i = 0; i < len; i++ ) out[i] = (uint8_t)( prng( cl ) >> ( ( i & 3 ) * 8 ) );
}

/* ---- Direct TCP transport ---- */
static int send_all( smb2_client *cl, const uint8_t *buf, int len )
{
    int off = 0, n;
    while ( off < len ) {
        n = cl->send( cl->ioctx, buf + off, len - off );
        if ( n <= 0 ) { cl->broken = 1; return -1; }
        off += n;
    }
    return 0;
}

static int read_n( smb2_client *cl, uint8_t *buf, int n )
{
    int off = 0, got;
    while ( off < n ) {
        got = cl->recv( cl->ioctx, buf + off, n - off );
        if ( got <= 0 ) { cl->broken = 1; return -1; }
        off += got;
    }
    return 0;
}

/* fold one SMB2 message into the running 3.1.1 pre-auth integrity hash */
static void preauth_update( smb2_client *cl, const uint8_t *msg, int len )
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
        uint8_t       *msg = cl->sendbuf + 4;
        uint8_t        mac[32];
        uint32_t       flags = get32( msg + 16 );
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
    cl->sendbuf[1] = (uint8_t)( ( len >> 16 ) & 0xFF );
    cl->sendbuf[2] = (uint8_t)( ( len >> 8 ) & 0xFF );
    cl->sendbuf[3] = (uint8_t)( len & 0xFF );
    return send_all( cl, cl->sendbuf, len + 4 );
}

/* read one SMB2 message into cl->recvbuf; cl->recvlen set */
static int recv_msg( smb2_client *cl )
{
    uint8_t hdr[4];
    int len;
    cl->last_status = 0;                    /* no answer stays "no status" */
    if ( read_n( cl, hdr, 4 ) != 0 ) return -1;
    len = ( (int)hdr[1] << 16 ) | ( (int)hdr[2] << 8 ) | (int)hdr[3];
    if ( len < 64 || len > SMB2_RECVBUF ) { cl->broken = 1; return -1; }   /* out of step */
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
#define CMD_QUERY_INFO       16
#define CMD_SET_INFO         17

#define ST_SUCCESS           0x00000000UL
#define ST_MORE_PROCESSING   0xC0000016UL
#define ST_NO_MORE_FILES     0x80000006UL

static uint8_t *build_header( smb2_client *cl, int cmd )
{
    uint8_t *msg = cl->sendbuf + 4;
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
 * The opening: an SMB1 NEGOTIATE offering only the SMB2 dialects, as
 * Windows sends.  An SMB2 server answers it in SMB2: dialect 02FF asks for
 * the SMB2 NEGOTIATE proper next, with MessageId 1; 0202 is a server that
 * stops at 2.0.2, and that answer is the whole negotiate.  A server that
 * speaks only SMB1 answers in SMB1, and cl->smb1_only says so - Windows
 * Server 2003 and 2000 say nothing at all to an SMB2 NEGOTIATE.
 * 1: negotiated; 0: the SMB2 NEGOTIATE follows; -1: failed.
 * ========================================================================== */
static int do_opening( smb2_client *cl )
{
    static const char dialects[] = "\002SMB 2.002\000\002SMB 2.\?\?\?";
    uint8_t *msg = cl->sendbuf + 4;
    uint8_t hdr[4];
    int dlen = (int)sizeof( dialects );         /* the last NUL counts */
    int len, dialect;

    cl->stage = "negotiate";
    memset( msg, 0, 35 );
    msg[0] = 0xFF; msg[1] = 'S'; msg[2] = 'M'; msg[3] = 'B';
    msg[4] = 0x72;                              /* SMB_COM_NEGOTIATE */
    msg[9] = 0x18;                              /* Flags */
    put16( msg + 10, 0xC801 );                  /* Flags2: Unicode, NT status, long names */
    put16( msg + 26, 0xFEFF );                  /* PID */
    put16( msg + 33, (unsigned int)dlen );      /* WordCount 0, ByteCount */
    memcpy( msg + 35, dialects, dlen );
    if ( send_msg( cl, 35 + dlen ) != 0 ) return -1;

    /* not recv_msg: an SMB1 answer can be shorter than an SMB2 header */
    cl->last_status = 0;
    if ( read_n( cl, hdr, 4 ) != 0 ) return -1;
    len = ( (int)hdr[1] << 16 ) | ( (int)hdr[2] << 8 ) | (int)hdr[3];
    if ( len < 4 || len > SMB2_RECVBUF ) return -1;
    if ( read_n( cl, cl->recvbuf, len ) != 0 ) return -1;
    cl->recvlen = len;
    if ( cl->recvbuf[1] != 'S' || cl->recvbuf[2] != 'M' || cl->recvbuf[3] != 'B' ) return -1;
    if ( cl->recvbuf[0] == 0xFF ) { cl->smb1_only = 1; return -1; }
    if ( cl->recvbuf[0] != 0xFE || len < 72 ) return -1;
    cl->last_status = get32( cl->recvbuf + 8 );
    if ( cl->last_status != ST_SUCCESS ) return -1;

    cl->msgid_lo = 1;                           /* the opening was MessageId 0 */
    dialect = (int)get16( cl->recvbuf + 64 + 4 );
    if ( dialect == 0x02FF ) return 0;
    if ( dialect != 0x0202 ) return -1;
    cl->dialect = dialect;
    cl->signing_required = ( get16( cl->recvbuf + 64 + 2 ) & 0x0002 ) ? 1 : 0;
    return 1;
}

/* ==========================================================================
 * NEGOTIATE
 * ========================================================================== */
static int do_negotiate( smb2_client *cl )
{
    uint8_t *msg = build_header( cl, CMD_NEGOTIATE );
    uint8_t *b = msg + 64;
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

    put32( b + 28, (uint32_t)ctxoff ); /* NegotiateContextOffset */
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

static int build_ntlm_negotiate( smb2_client *cl, uint8_t *out )
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
static const uint8_t *av_find( const uint8_t *ti, int tilen, int id, int *vlen )
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
    return (const uint8_t *)0;
}

/* Build the NTLMv2 AUTHENTICATE (type 3) with a MIC into 'out'; returns len.
   Needs the stored NEGOTIATE and CHALLENGE messages for the MIC. */
static int build_ntlm_authenticate( smb2_client *cl, uint8_t *out,
                                     const char *user, const char *password, const char *domain )
{
    const uint8_t *chal = cl->ntlm_challenge;
    int   challen = cl->ntlm_challenge_len;
    const uint8_t *server_chal;
    const uint8_t *ti;
    int   tilen, tioff;
    const uint8_t *ts;
    int   tslen = 0;
    uint8_t respkey[16];
    uint8_t client_chal[8];
    uint8_t temp[1024];
    int   templen;
    uint8_t ntproof[16];
    uint8_t ntresp[16 + 1024];
    int   ntresp_len;
    uint8_t session_base[16];
    uint8_t exported_key[16];
    uint8_t timestamp[8];
    int   pos, payload;
    int   dlen, ulen, nt_off, payoff;
    uint8_t dwide[512], uwide[512];
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
        uint8_t cat[8 + 1024];
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
    put16( out + 12, 24 ); put16( out + 14, 24 ); put32( out + 16, (uint32_t)payload );
    memset( out + payload, 0, 24 ); payload += 24;

    /* NtChallengeResponse */
    nt_off = payload;
    put16( out + 20, (unsigned int)ntresp_len ); put16( out + 22, (unsigned int)ntresp_len );
    put32( out + 24, (uint32_t)nt_off );
    memcpy( out + payload, ntresp, ntresp_len ); payload += ntresp_len;

    /* DomainName */
    put16( out + 28, (unsigned int)dlen ); put16( out + 30, (unsigned int)dlen ); put32( out + 32, (uint32_t)payload );
    memcpy( out + payload, dwide, dlen ); payload += dlen;

    /* UserName */
    put16( out + 36, (unsigned int)ulen ); put16( out + 38, (unsigned int)ulen ); put32( out + 40, (uint32_t)payload );
    memcpy( out + payload, uwide, ulen ); payload += ulen;

    /* Workstation (empty) */
    put16( out + 44, 0 ); put16( out + 46, 0 ); put32( out + 48, (uint32_t)payload );

    /* EncryptedRandomSessionKey (empty, no key exchange) */
    put16( out + 52, 0 ); put16( out + 54, 0 ); put32( out + 56, (uint32_t)payload );

    /* MIC = HMAC-MD5(exported_key, NEGOTIATE || CHALLENGE || AUTHENTICATE(MIC=0)) */
    {
        static uint8_t mbuf[4096];          /* >= negotiate(<=64) + challenge(<=1024) + authenticate(<=2172) */
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
        smb3_kdf( cl->session_key, (const uint8_t *)"SMBSigningKey", 14,
                  cl->preauth, 64, cl->signing_key );
        cl->signing_mode = 3;
    } else if ( cl->dialect == 0x0300 || cl->dialect == 0x0302 ) {
        smb3_kdf( cl->session_key, (const uint8_t *)"SMB2AESCMAC", 12,
                  (const uint8_t *)"SmbSign", 8, cl->signing_key );
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
    uint8_t *msg, *b;
    uint8_t ntlm[4096];                 /* holds the NTLMv2 AUTHENTICATE, bounded by the <=1024 challenge */
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
    if ( secoff < 0 || blen < 0 || secoff > cl->recvlen || blen > cl->recvlen - secoff ) return -1;
    /* Windows answers the raw NTLMSSP NEGOTIATE with a raw CHALLENGE; a
       server that wraps it in SPNEGO puts it further in.  Its offsets
       count from its own "NTLMSSP" signature, so start there. */
    {
        const uint8_t *blob = cl->recvbuf + secoff;
        int at;
        for ( at = 0; at + 12 <= blen; at++ ) {
            if ( blob[at] == 'N' && blob[at + 1] == 'T' && blob[at + 2] == 'L' && blob[at + 3] == 'M' &&
                 blob[at + 4] == 'S' && blob[at + 5] == 'S' && blob[at + 6] == 'P' && blob[at + 7] == 0 &&
                 get32( blob + at + 8 ) == 2 ) break;
        }
        if ( at + 12 > blen ) return -1;
        secoff += at;
        blen -= at;
        if ( at > 0 && blen >= 48 ) {
            /* SPNEGO may follow it (a mechListMIC), and the MIC covers
               the CHALLENGE alone: it ends where its last field ends */
            long end = 48, tn, ti;
            tn = (long)get32( cl->recvbuf + secoff + 16 ) + (long)get16( cl->recvbuf + secoff + 12 );
            ti = (long)get32( cl->recvbuf + secoff + 44 ) + (long)get16( cl->recvbuf + secoff + 40 );
            if ( blen >= 56 ) end = 56;
            if ( tn > end ) end = tn;
            if ( ti > end ) end = ti;
            if ( end < blen ) blen = (int)end;
        }
    }
    if ( blen > (int)sizeof( cl->ntlm_challenge ) ) return -1;
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
    uint8_t *msg, *b;
    char path[300];
    uint8_t wide[600];
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
                      uint32_t access, uint32_t options, uint32_t disposition,
                      uint8_t file_id[16],
                      uint32_t *size_lo, uint32_t *size_hi, uint32_t *attr )
{
    uint8_t *msg, *b;
    uint8_t wide[600];
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
                    uint32_t disposition, smb2_fh *fh )
{
    uint32_t access = 0x00100081UL;         /* FILE_READ_DATA|READ_ATTRIBUTES|SYNCHRONIZE */

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

int smb2_read( smb2_client *cl, const smb2_fh *fh, uint32_t off_lo, uint32_t off_hi,
               uint8_t *buf, int len )
{
    uint8_t *msg, *b;
    int msglen, dataoff, datalen, maxlen;

    maxlen = SMB2_RECVBUF - 512;
    if ( len > maxlen ) len = maxlen;
    if ( len <= 0 ) return 0;

    cl->stage = "read";
    msg = build_header( cl, CMD_READ );
    b = msg + 64;
    memset( b, 0, 48 );
    put16( b + 0, 49 );                     /* StructureSize (48 fixed + 1 buffer) */
    put32( b + 4, (uint32_t)len );          /* Length */
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

int smb2_write( smb2_client *cl, const smb2_fh *fh, uint32_t off_lo, uint32_t off_hi,
                const uint8_t *buf, int len )
{
    uint8_t *msg, *b;
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
    put32( b + 4, (uint32_t)len );          /* Length */
    put32( b + 8, off_lo );
    put32( b + 12, off_hi );
    memcpy( b + 16, fh->file_id, 16 );
    memcpy( msg + dataoff, buf, len );
    msglen = dataoff + len;
    if ( send_msg( cl, msglen ) != 0 ) return -1;
    if ( recv_msg( cl ) != 0 ) return -1;
    if ( cl->last_status != ST_SUCCESS ) return -1;
    if ( cl->recvlen < 64 + 8 ) return -1;
    {
        uint32_t count = get32( cl->recvbuf + 64 + 4 );        /* Count written */
        if ( count > (uint32_t)len ) count = (uint32_t)len;
        return (int)count;
    }
}

int smb2_delete( smb2_client *cl, const char *path )
{
    smb2_fh fh;
    cl->stage = "delete";
    memset( &fh, 0, sizeof( fh ) );
    /* DELETE access, FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, FILE_OPEN */
    if ( do_create( cl, path, 0x00010000UL, 0x00001040UL, 0x00000001UL,
                    fh.file_id, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0 ) != 0 )
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
                    fh.file_id, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0 ) != 0 )
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
                    fh.file_id, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0 ) != 0 )
        return -1;
    return smb2_close_file( cl, &fh );
}

int smb2_isdir( smb2_client *cl, const char *path )
{
    smb2_fh fh;
    cl->stage = "isdir";
    memset( &fh, 0, sizeof( fh ) );
    /* READ_ATTRIBUTES|SYNCHRONIZE, FILE_DIRECTORY_FILE, FILE_OPEN: a file
       by that name is refused (NOT_A_DIRECTORY), as is no name at all */
    if ( do_create( cl, path, 0x00100080UL, 0x00000001UL, 0x00000001UL,
                    fh.file_id, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0 ) != 0 )
        return -1;
    return smb2_close_file( cl, &fh );
}

/* QUERY_INFO of the share root: SMB2_0_INFO_FILESYSTEM, FileFsSizeInformation
   (class 3) - TotalAllocationUnits (8), AvailableAllocationUnits (8, what
   the user may still use), SectorsPerAllocationUnit (4), BytesPerSector (4). */
int smb2_fs_size( smb2_client *cl, smb_fs_size *fs )
{
    smb2_fh fh;
    uint8_t *msg, *b;
    int outoff, outlen, rc = -1;

    cl->stage = "fs-size";
    memset( fs, 0, sizeof( *fs ) );
    memset( &fh, 0, sizeof( fh ) );
    if ( do_create( cl, "", 0x00100080UL, 0x00000001UL, 0x00000001UL,
                    fh.file_id, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0 ) != 0 )
        return -1;

    msg = build_header( cl, CMD_QUERY_INFO );
    b = msg + 64;
    memset( b, 0, 41 );
    put16( b + 0, 41 );                     /* StructureSize (40 fixed + 1 buffer) */
    b[2] = 0x02;                            /* InfoType = SMB2_0_INFO_FILESYSTEM */
    b[3] = 3;                               /* FileInfoClass = FileFsSizeInformation */
    put32( b + 4, 24 );                     /* OutputBufferLength */
    memcpy( b + 24, fh.file_id, 16 );       /* FileId */
    if ( send_msg( cl, 64 + 41 ) == 0 && recv_msg( cl ) == 0 &&
         cl->last_status == ST_SUCCESS && cl->recvlen >= 64 + 8 ) {
        b = cl->recvbuf + 64;
        outoff = (int)get16( b + 2 );       /* from the SMB2 header start */
        outlen = (int)get32( b + 4 );
        if ( outoff >= 64 + 8 && outlen >= 24 && outoff <= cl->recvlen &&
             outlen <= cl->recvlen - outoff ) {
            b = cl->recvbuf + outoff;
            fs->total_lo = get32( b + 0 );
            fs->total_hi = get32( b + 4 );
            fs->free_lo  = get32( b + 8 );
            fs->free_hi  = get32( b + 12 );
            fs->sec_per_unit = get32( b + 16 );
            fs->bytes_per_sec = get32( b + 20 );
            rc = 0;
        }
    }
    if ( smb2_close_file( cl, &fh ) != 0 ) rc = -1;
    return rc;
}

/* RENAME: open the source for DELETE, then SET_INFO with
   FileRenameInformation (the new name, UTF-16, relative to the share root
   with no leading separator), then close.  Only ever reached for an SMB2/3
   server; the WfW-bridge rig proves the SMB1 path (smb1_rename). */
int smb2_rename( smb2_client *cl, const char *oldpath, const char *newpath )
{
    smb2_fh fh;
    uint8_t *msg, *b, *rbuf;
    uint8_t wide[600];
    const char *nm = newpath;
    int namelen, msglen, buflen;

    cl->stage = "rename";
    memset( &fh, 0, sizeof( fh ) );
    /* DELETE | SYNCHRONIZE access, no special options (file or dir), FILE_OPEN */
    if ( do_create( cl, oldpath, 0x00010000UL | 0x00100000UL, 0x00000000UL, 0x00000001UL,
                    fh.file_id, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0 ) != 0 )
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
    put32( b + 4, (uint32_t)buflen );       /* BufferLength */
    put16( b + 8, (unsigned int)( 64 + 32 ) );  /* BufferOffset (from header start) */
    memcpy( b + 16, fh.file_id, 16 );       /* FileId */

    rbuf = msg + 64 + 32;
    memset( rbuf, 0, 20 );
    rbuf[0] = 1;                            /* ReplaceIfExists */
    put32( rbuf + 16, (uint32_t)namelen ); /* FileNameLength (bytes) */
    memcpy( rbuf + 20, wide, namelen );

    msglen = 64 + 32 + buflen;
    if ( send_msg( cl, msglen ) != 0 )          { smb2_close_file( cl, &fh ); return -1; }
    if ( recv_msg( cl ) != 0 )                  { smb2_close_file( cl, &fh ); return -1; }
    if ( cl->last_status != ST_SUCCESS )        { smb2_close_file( cl, &fh ); return -1; }
    return smb2_close_file( cl, &fh );
}

int smb2_close_file( smb2_client *cl, const smb2_fh *fh )
{
    uint8_t *msg, *b;

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
    uint8_t file_id[16];
    uint8_t *msg, *b;
    uint8_t wide[520];                      /* a whole name as the pattern */
    smb2_fh dirfh;
    int widelen, patoff, msglen, restart = 1, rc = 0;

    /* open the directory: FILE_LIST_DIRECTORY|READ_ATTRIBUTES|SYNCHRONIZE,
       FILE_DIRECTORY_FILE, FILE_OPEN */
    cl->stage = "create";
    if ( do_create( cl, dir, 0x00100081UL, 0x00000001UL, 0x00000001UL,
                    file_id, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0 ) != 0 )
        return -1;
    widelen = utf16le( pattern, wide, sizeof( wide ) );

    for ( ;; ) {
        int outoff, outlen, pos;

        cl->stage = "query-directory";
        msg = build_header( cl, CMD_QUERY_DIRECTORY );
        b = msg + 64;
        memset( b, 0, 32 );
        put16( b + 0, 33 );                 /* StructureSize */
        b[2] = 0x03;                        /* FileBothDirectoryInformation */
        b[3] = (uint8_t)( restart ? 0x01 : 0x00 );         /* RESTART_SCANS */
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
        if ( cl->recvlen < 64 + 8 ) { rc = -1; break; }

        b = cl->recvbuf + 64;
        outoff = (int)get16( b + 2 );
        outlen = (int)get32( b + 4 );
        /* bound each against recvlen separately: a hostile length must not
           overflow the sum, and the window must lie inside what we received */
        if ( outoff < 0 || outlen < 0 || outoff > cl->recvlen || outlen > cl->recvlen - outoff ) { rc = -1; break; }
        if ( outlen > 0 && outoff < 64 + 8 ) { rc = -1; break; }

        /* every read below is kept inside [outoff, outoff+outlen).  An
           entry is FILE_BOTH_DIR_INFORMATION: 94 fixed bytes - the short
           name's length at 68 and its 24 bytes at 70 - then the name */
        pos = outoff;
        while ( pos + 94 <= outoff + outlen ) {
            const uint8_t *e = cl->recvbuf + pos;
            uint32_t      nextoff = get32( e + 0 );
            uint32_t      namelen = get32( e + 60 );
            unsigned int  avail   = (unsigned int)( outoff + outlen - pos - 94 );
            unsigned int  shortlen = e[68];
            smb2_dirent ent;
            unsigned int i, j = 0;

            ent.wtime_lo = get32( e + 24 );             /* LastWriteTime */
            ent.wtime_hi = get32( e + 28 );
            ent.size_lo  = get32( e + 40 );             /* EndOfFile */
            ent.size_hi  = get32( e + 44 );
            ent.attr     = get32( e + 56 );             /* FileAttributes */
            if ( namelen > avail ) namelen = avail;     /* clamp the name to the buffer */
            for ( i = 0; i + 1 < namelen && j + 1 < sizeof( ent.name ); i += 2 )
                ent.name[j++] = (char)e[94 + i];        /* UTF-16LE -> ASCII (test names are ASCII) */
            ent.name[j] = 0;
            if ( shortlen > 24 ) shortlen = 24;
            for ( i = 0, j = 0; i + 1 < shortlen && j + 1 < sizeof( ent.short_name ); i += 2 )
                ent.short_name[j++] = (char)e[70 + i];
            ent.short_name[j] = 0;
            emit( user, &ent );

            if ( nextoff == 0 || nextoff > (uint32_t)outlen ) break;        /* last, or out of range */
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
typedef struct { void ( *emit )( void *, const char *, int, uint32_t ); void *user; } list_shim_ctx;

static void list_shim( void *user, const smb2_dirent *entry )
{
    list_shim_ctx *ctx = (list_shim_ctx *)user;
    ctx->emit( ctx->user, entry->name, ( entry->attr & 0x10 ) ? 1 : 0, entry->size_lo );
}

int smb2_list( smb2_client *cl, const char *pattern,
               void ( *emit )( void *user, const char *name, int isdir, uint32_t size ),
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
                  uint32_t seed,
                  const char *server, const char *share,
                  const char *user, const char *password, const char *domain )
{
    int rc;

    memset( cl, 0, sizeof( *cl ) );
    cl->send = send; cl->recv = recv; cl->ioctx = ioctx;
    cl->seed = seed ? seed : 0x2545F491UL;
    cl->stage = "start";

    rc = do_opening( cl );
    if ( rc < 0 ) return -1;
    if ( rc == 0 && do_negotiate( cl ) != 0 ) return -1;
    if ( do_session_setup( cl, user, password, domain ) != 0 ) return -1;
    if ( do_tree_connect( cl, server, share ) != 0 ) return -1;
    cl->stage = "connected";
    return 0;
}

int smb2_tree_connect( smb2_client *cl, const char *server, const char *share )
{
    cl->tree_id = 0;
    if ( do_tree_connect( cl, server, share ) != 0 ) return -1;
    cl->stage = "connected";
    return 0;
}

void smb2_tree_disconnect( smb2_client *cl )
{
    uint8_t *msg, *b;

    if ( cl->tree_id ) {
        msg = build_header( cl, CMD_TREE_DISCONNECT );
        b = msg + 64;
        put16( b + 0, 4 ); put16( b + 2, 0 );
        if ( send_msg( cl, 64 + 4 ) == 0 ) recv_msg( cl );
        cl->tree_id = 0;
    }
}

void smb2_disconnect( smb2_client *cl )
{
    uint8_t *msg, *b;

    smb2_tree_disconnect( cl );
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
uint32_t      smb2_status( const smb2_client *cl )  { return cl->last_status; }
int           smb2_smb1_only( const smb2_client *cl ) { return cl->smb1_only; }
