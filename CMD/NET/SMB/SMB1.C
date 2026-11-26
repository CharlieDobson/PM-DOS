/* ==========================================================================
 * SMB1.C - the SMB1 client engine: "NT LM 0.12" (CIFS) and the LAN
 * Manager dialects.  See SMB1.H.
 * ==========================================================================
 * NetBIOS-over-TCP (port 139): a session request, then every SMB message
 * rides a 4-byte NBT header (type, flags, 16-bit big-endian length).  The
 * SMB header is the 32-byte CIFS header; OEM (8-bit) strings, no signing.
 * Authentication is the old LM + NTLMv1 challenge-response.  A reply is
 * known by its MID, and a TRANS2 reply too big for the server's buffer
 * is put together from the several messages it comes in (smb1_rx,
 * trans2_txrx): the drives on one connection share one stream, so a
 * reply left half-read would answer the next request, on any of them.
 *
 * A LAN Manager server (WfW 3.11, LAN Manager 2.x, OS/2 LAN Server) gets
 * what WfW's own redirector sends: the 13-word NEGOTIATE answer, a
 * 10-word SESSION_SETUP with one password, and the core SEARCH, READ and
 * WRITE (8.3 names, DOS dates).  Its buffer is small (WfW's is 2,920
 * bytes), so reads, writes and listings are cut to fit.  It answers with
 * the DOS error class and code instead of NT status.
 *
 * The engine answers in SMB2.H's smb2_fh / smb2_dirent shapes so the
 * kernel redirector drives SMB1 and SMB2 the same way.  C89, and no C
 * runtime beyond memcpy / memset (DRVRT provides them in the driver, the
 * CRT on the host harness), like SMB2.C.
 * ========================================================================== */
#include "smb1.h"
#include "smbcrypt.h"

void *memcpy( void *to, const void *from, unsigned len );
void *memset( void *to, int value, unsigned len );

/* ---- little helpers ---- */
static void put16( uint8_t *p, unsigned int v ) { p[0] = (uint8_t)v; p[1] = (uint8_t)( v >> 8 ); }
static void put32( uint8_t *p, uint32_t v ) { p[0] = (uint8_t)v; p[1] = (uint8_t)( v >> 8 ); p[2] = (uint8_t)( v >> 16 ); p[3] = (uint8_t)( v >> 24 ); }
static unsigned int  get16( const uint8_t *p ) { return (unsigned int)p[0] | ( (unsigned int)p[1] << 8 ); }
static uint32_t      get32( const uint8_t *p ) { return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 ); }

static int slen( const char *s ) { int n = 0; while ( s[n] ) n++; return n; }
static char supper( char ch ) { return ( ch >= 'a' && ch <= 'z' ) ? (char)( ch - 32 ) : ch; }

/* copy an ASCIIZ string (with its NUL) to 'dst', uppercased when 'up';
   returns the number of bytes written, the NUL included */
static int put_strz( uint8_t *dst, const char *src, int up )
{
    int i = 0;
    while ( src[i] ) { dst[i] = (uint8_t)( up ? supper( src[i] ) : src[i] ); i++; }
    dst[i] = 0;
    return i + 1;
}

/* ====================================================================== */
/* transport: NBT framing over the send/recv callbacks                    */
/* ====================================================================== */

static int send_all( smb1_client *cl, const uint8_t *buf, int len )
{
    int off = 0, n;
    while ( off < len ) {
        n = cl->send( cl->ioctx, buf + off, len - off );
        if ( n <= 0 ) { cl->broken = 1; return -1; }
        off += n;
    }
    return 0;
}

static int read_n( smb1_client *cl, uint8_t *buf, int n )
{
    int off = 0, got;
    while ( off < n ) {
        got = cl->recv( cl->ioctx, buf + off, n - off );
        if ( got <= 0 ) { cl->broken = 1; return -1; }
        off += got;
    }
    return 0;
}

/* receive one NBT message into 'out' (max 'cap'); skip keep-alives (0x85).
   returns the payload length, or <0. */
static int nbt_recv( smb1_client *cl, uint8_t *out, int cap )
{
    uint8_t hdr[4];
    int len;
    for ( ;; ) {
        if ( read_n( cl, hdr, 4 ) < 0 ) return -1;
        len = ( ( hdr[1] & 1 ) << 16 ) | ( hdr[2] << 8 ) | hdr[3];
        if ( hdr[0] == 0x85 ) continue;                 /* session keep-alive */
        if ( len == 0 ) return 0;
        if ( len > cap ) { cl->broken = 1; return -1; }    /* out of step */
        if ( read_n( cl, out, len ) < 0 ) return -1;
        return len;
    }
}

/* ====================================================================== */
/* the SMB header, built at sendbuf+4 (the NBT header is sendbuf[0..3])    */
/* ====================================================================== */

#define SMB_FLAGS2   0x4001U     /* LONG_NAMES | NT_STATUS; OEM strings */

/* start a command: write the NBT placeholder and the 32-byte header,
   return the WordCount byte's address (the caller fills from there) */
static uint8_t *smb1_begin( smb1_client *cl, int command )
{
    uint8_t *p = cl->sendbuf + 4;               /* the SMB header start */
    memset( cl->sendbuf, 0, 37 );
    p[0] = 0xFF; p[1] = 'S'; p[2] = 'M'; p[3] = 'B';
    p[4] = (uint8_t)command;                    /* Command */
    p[9] = 0x18;                                /* Flags: canon + case-insensitive */
    put16( p + 10, cl->flags2 );                /* Flags2 */
    put16( p + 24, cl->tree_id );               /* TID */
    put16( p + 26, cl->pid );                   /* PID: ours, for the mount */
    put16( p + 28, cl->uid );                   /* UID */
    if ( ( cl->mid & 0xFFFFU ) == 0xFFFFU ) cl->mid++;   /* FFFFh is the server's
                                                   (its oplock breaks) */
    cl->req_mid = cl->mid & 0xFFFFU;
    put16( p + 30, cl->mid++ );                 /* MID */
    return p + 32;                              /* -> WordCount */
}

/* the most messages passed over while waiting for one reply */
#define SMB1_MAXSKIP 64

/* receive the reply to the request in flight into recvbuf.  A message
   with another MID is not that reply but a piece of an older one that
   was never read, and is passed over: taken as the answer, it would be
   an earlier listing's entries answering this request, and every reply
   after it one behind.  Stores cl->last_status.  0 if the reply came
   (even an error status), <0 on a transport failure or a malformed
   reply (a success whose word or byte block runs past what was
   received). */
static int smb1_rx( smb1_client *cl )
{
    int skipped = 0;

    cl->last_status = 0;                        /* no answer stays "no status" */
    for ( ;; ) {
        cl->recvlen = nbt_recv( cl, cl->recvbuf, (int)sizeof( cl->recvbuf ) );
        if ( cl->recvlen < 33 ) return -1;
        if ( cl->recvbuf[0] != 0xFF || cl->recvbuf[1] != 'S' ||
             cl->recvbuf[2] != 'M' || cl->recvbuf[3] != 'B' ) return -1;
        if ( get16( cl->recvbuf + 30 ) == cl->req_mid ) break;
        if ( ++skipped > SMB1_MAXSKIP ) { cl->broken = 1; return -1; }
    }
    cl->last_status = get32( cl->recvbuf + 5 );
    if ( cl->last_status == 0 ) {
        /* the words and bytes a good reply carries must have come in */
        int fixed = 33 + cl->recvbuf[32] * 2;
        if ( fixed + 2 > cl->recvlen ) return -1;
        if ( fixed + 2 + (int)get16( cl->recvbuf + fixed ) > cl->recvlen ) return -1;
    }
    return 0;
}

/* send what was built (end -> just past the last byte) and receive the
   reply into recvbuf (smb1_rx) */
static int smb1_txrx( smb1_client *cl, uint8_t *end )
{
    uint8_t *p = cl->sendbuf + 4;
    int smblen = (int)( end - p );
    cl->sendbuf[0] = 0;
    cl->sendbuf[1] = (uint8_t)( ( smblen >> 16 ) & 1 );
    cl->sendbuf[2] = (uint8_t)( smblen >> 8 );
    cl->sendbuf[3] = (uint8_t)smblen;
    cl->last_status = 0;                        /* no answer stays "no status" */
    if ( send_all( cl, cl->sendbuf, smblen + 4 ) < 0 ) return -1;
    return smb1_rx( cl );
}

/* response accessors (the reply SMB is at recvbuf[0]) */
static int r_wordcount( smb1_client *cl ) { return cl->recvbuf[32]; }
static const uint8_t *r_words( smb1_client *cl ) { return cl->recvbuf + 33; }
static int r_bytecount( smb1_client *cl )
{
    int wc = cl->recvbuf[32];
    return (int)get16( cl->recvbuf + 33 + wc * 2 );
}
static const uint8_t *r_bytes( smb1_client *cl )
{
    int wc = cl->recvbuf[32];
    return cl->recvbuf + 33 + wc * 2 + 2;
}

/* send a TRANS2 request and put its reply together.  A reply that does
   not fit the server's buffer comes in several messages (Windows Server
   2003's buffer is 16,644 bytes, so a listing of more than about 120
   long names does), each with the request's MID, the totals, and a
   piece of the parameters and of the data at a displacement.  The
   pieces go to cl->trparm (*pcount bytes in all) and cl->trdata
   (*dcount).  The request asked for no more than SMB1_TRANSPARM and
   SMB1_TRANSDATA bytes.  0 when the reply came - an error status too,
   which has no pieces - else <0.  A message that is not a TRANS2
   answer, a reply larger than was asked for, or a piece outside it
   ends the connection (cl->broken): the rest of that reply may still
   be on its way. */
static int trans2_txrx( smb1_client *cl, uint8_t *end, int *pcount, int *dcount )
{
    int tp = -1, td = -1, gotp = 0, gotd = 0;

    *pcount = *dcount = 0;
    if ( smb1_txrx( cl, end ) < 0 ) return -1;
    if ( cl->last_status != 0 ) return 0;       /* an error: no pieces */
    for ( ;; ) {
        const uint8_t *rp;
        int wc, ntp, ntd, pc, po, pd, dc, dof, dd, base;

        wc = r_wordcount( cl );
        if ( wc < 10 ) break;                    /* not a TRANS2 answer */
        rp = r_words( cl );
        ntp = (int)get16( rp + 0 );              /* TotalParameterCount */
        ntd = (int)get16( rp + 2 );              /* TotalDataCount */
        pc  = (int)get16( rp + 6 );              /* ParameterCount */
        po  = (int)get16( rp + 8 );              /* ParameterOffset */
        pd  = (int)get16( rp + 10 );             /* ParameterDisplacement */
        dc  = (int)get16( rp + 12 );             /* DataCount */
        dof = (int)get16( rp + 14 );             /* DataOffset */
        dd  = (int)get16( rp + 16 );             /* DataDisplacement */
        /* the first message sets the totals; a later one may only lower them */
        if ( tp < 0 || ntp < tp ) tp = ntp;
        if ( td < 0 || ntd < td ) td = ntd;
        if ( tp > SMB1_TRANSPARM || td > SMB1_TRANSDATA ) break;
        base = 33 + wc * 2 + 2;                  /* where the byte block starts */
        if ( pc > 0 && ( po < base || po + pc > cl->recvlen || pd + pc > tp ) ) break;
        if ( dc > 0 && ( dof < base || dof + dc > cl->recvlen || dd + dc > td ) ) break;
        if ( pc + dc == 0 && ( gotp < tp || gotd < td ) ) break;   /* a piece of nothing */
        if ( pc > 0 ) memcpy( cl->trparm + pd, cl->recvbuf + po, (unsigned)pc );
        if ( dc > 0 ) memcpy( cl->trdata + dd, cl->recvbuf + dof, (unsigned)dc );
        gotp += pc;
        gotd += dc;
        if ( gotp >= tp && gotd >= td ) {
            *pcount = tp;
            *dcount = td;
            return 0;
        }
        if ( smb1_rx( cl ) < 0 ) return -1;      /* the next piece */
        if ( cl->last_status != 0 ) return -1;   /* an error part-way */
    }
    cl->broken = 1;
    return -1;
}

/* ====================================================================== */
/* the NetBIOS session request                                            */
/* ====================================================================== */

static void nb_encode( const char *name, uint8_t type, uint8_t *out )
{
    char padded[16];
    int i, n = slen( name );
    for ( i = 0; i < 15; i++ ) padded[i] = ( i < n ) ? supper( name[i] ) : ' ';
    padded[15] = (char)type;
    out[0] = 0x20;
    for ( i = 0; i < 16; i++ ) {
        out[1 + i * 2]     = (uint8_t)( 'A' + ( ( padded[i] >> 4 ) & 0x0F ) );
        out[1 + i * 2 + 1] = (uint8_t)( 'A' + ( padded[i] & 0x0F ) );
    }
    out[33] = 0;
}

char smb1_calling[16] = "PMDOS";

/* SESSION_SETUP's VcNumber.  0 says "this client has just started", and
   Windows Server 2003 then ends every other connection from it, so a
   second logon to a server that already has one says 1 (SMBFS sets it). */
unsigned int smb1_vc_number = 0;

static int nbt_session_request( smb1_client *cl, const char *server )
{
    uint8_t pkt[4 + 68];
    const char *called = server;
    int n = slen( server ), i, isip = 1;

    /* a dotted-decimal address is not a NetBIOS name; call "*SMBSERVER" */
    for ( i = 0; i < n; i++ ) if ( !( ( server[i] >= '0' && server[i] <= '9' ) || server[i] == '.' ) ) { isip = 0; break; }
    if ( isip || n == 0 || n > 15 ) called = "*SMBSERVER";

    pkt[0] = 0x81; pkt[1] = 0;                  /* SESSION REQUEST */
    pkt[2] = 0; pkt[3] = 68;
    nb_encode( called, 0x20, pkt + 4 );         /* CALLED  (server service) */
    nb_encode( smb1_calling, 0x00, pkt + 4 + 34 );  /* CALLING (workstation)  */
    if ( send_all( cl, pkt, sizeof( pkt ) ) < 0 ) return -1;

    /* the response is a bare 4-byte NBT header: 0x82 positive, else no */
    {
        uint8_t hdr[4];
        if ( read_n( cl, hdr, 4 ) < 0 ) return -1;
        if ( hdr[0] != 0x82 ) return -1;
    }
    return 0;
}

/* ====================================================================== */
/* negotiate / session setup / tree connect                               */
/* ====================================================================== */

#define CAP_NT_SMBS   0x00000010UL
#define CAP_NT_STATUS 0x00000040UL

/* the dialects offered, oldest first; the server answers with the index
   of the newest one it speaks.  All but the last are LAN Manager
   dialects with the 13-word answer: WfW 3.11 knows "Windows for
   Workgroups 3.1a", LAN Manager 2.1 and OS/2 LAN Server "LANMAN2.1". */
static const char *const dialects[] = {
    "LM1.2X002", "DOS LANMAN2.1", "LANMAN2.1", "Windows for Workgroups 3.1a", "NT LM 0.12"
};
#define DIALECTS    5
#define DIALECT_NT  4

static int do_negotiate( smb1_client *cl )
{
    uint8_t *p, *b, *end;
    const uint8_t *w;
    uint32_t maxbuf;
    int keylen, wc, idx, i;

    cl->flags2 = SMB_FLAGS2;
    p = smb1_begin( cl, 0x72 );                 /* NEGOTIATE */
    p[0] = 0;                                   /* WordCount */
    b = p + 3;                                  /* Bytes (ByteCount at p+1) */
    end = b;
    for ( i = 0; i < DIALECTS; i++ ) {
        *end++ = 0x02;                          /* dialect buffer format */
        end += put_strz( end, dialects[i], 0 );
    }
    put16( p + 1, (unsigned int)( end - b ) );  /* ByteCount */

    cl->stage = "negotiate";
    if ( smb1_txrx( cl, end ) < 0 ) return -1;
    if ( cl->last_status != 0 ) return -1;
    wc = r_wordcount( cl );
    if ( wc < 1 ) return -1;
    w = r_words( cl );
    idx = (int)get16( w );
    if ( idx >= DIALECTS ) return -1;           /* 0xFFFF: none of them */

    if ( idx == DIALECT_NT ) {
        if ( wc < 17 ) return -1;
        cl->secmode = w[2];
        maxbuf = get32( w + 7 );
        cl->sesskey = get32( w + 15 );
        cl->capabilities = get32( w + 19 );
        keylen = w[33];
    } else {
        if ( wc < 13 ) return -1;
        cl->lm = 1;
        cl->flags2 = 0;                         /* 8.3 names, DOS errors */
        cl->secmode = get16( w + 2 );
        maxbuf = get16( w + 4 );
        cl->sesskey = get32( w + 12 );
        keylen = (int)get16( w + 22 );
        if ( keylen == 0 ) keylen = r_bytecount( cl );  /* WfW 3.11 says 0, sends 8 */
        if ( maxbuf < 1024 ) return -1;         /* too small to carry a listing */
    }
    cl->max_buffer = ( maxbuf > 65535UL ) ? 65535 : (int)maxbuf;
    if ( !cl->lm || ( cl->secmode & 2 ) ) {     /* the server encrypts */
        if ( keylen < 8 || r_bytecount( cl ) < 8 ) return -1;   /* need a challenge */
        memcpy( cl->challenge, r_bytes( cl ), 8 );
    }
    return 0;
}

/* the password as a LAN Manager logon or a share-level share takes it:
   the 24-byte LM response when the server encrypts, else the password in
   capitals with its NUL; no password is a lone NUL.  Returns the length
   written to 'out' (64 bytes at most). */
static int lm_password( smb1_client *cl, const char *password, uint8_t *out )
{
    uint8_t h[16];
    int i;

    if ( password == (const char *)0 || password[0] == 0 ) { out[0] = 0; return 1; }
    if ( cl->secmode & 2 ) {
        lm_hash( password, h );
        desl( h, cl->challenge, out );
        return 24;
    }
    for ( i = 0; i < 63 && password[i]; i++ ) out[i] = (uint8_t)supper( password[i] );
    out[i] = 0;
    return i + 1;
}

static int do_session_setup( smb1_client *cl, const char *user,
                             const char *password, const char *domain )
{
    uint8_t *p = smb1_begin( cl, 0x73 );        /* SESSION_SETUP_ANDX */
    uint8_t *w = p + 1, *b, *q;
    uint8_t lm[24], nt[24], h[16];

    nt_hash( password, h ); desl( h, cl->challenge, nt );
    lm_hash( password, h ); desl( h, cl->challenge, lm );

    p[0] = 13;                                  /* WordCount */
    w[0] = 0xFF; w[1] = 0;                       /* AndXCommand = none */
    put16( w + 2, 0 );                          /* AndXOffset */
    put16( w + 4, (unsigned int)sizeof( cl->recvbuf ) - 256 ); /* MaxBufferSize */
    put16( w + 6, 1 );                          /* MaxMpxCount */
    put16( w + 8, smb1_vc_number );             /* VcNumber */
    put32( w + 10, cl->sesskey );               /* SessionKey: the server's, given back */
    put16( w + 14, 24 );                        /* OEM (LM) password length */
    put16( w + 16, 24 );                        /* Unicode (NT) password length */
    put32( w + 18, 0 );                         /* Reserved */
    put32( w + 22, CAP_NT_SMBS | CAP_NT_STATUS );/* Capabilities (no ext.sec.) */

    b = w + 26 + 2;                             /* ByteCount is at w+26 */
    q = b;
    memcpy( q, lm, 24 ); q += 24;
    memcpy( q, nt, 24 ); q += 24;
    q += put_strz( q, user ? user : "", 0 );
    q += put_strz( q, domain ? domain : "", 0 );
    q += put_strz( q, "PM-DOS", 0 );            /* NativeOS */
    q += put_strz( q, "PMDOS SMB1", 0 );        /* NativeLanMan */
    put16( w + 26, (unsigned int)( q - b ) );   /* ByteCount */

    cl->stage = "session setup";
    if ( smb1_txrx( cl, q ) < 0 ) return -1;
    if ( cl->last_status != 0 ) return -1;
    cl->uid = get16( cl->recvbuf + 28 );        /* the server's UID */
    return 0;
}

/* SESSION_SETUP_ANDX in its LAN Manager form: 10 words, one password.  A
   share-level server gets no password here (the share takes it). */
static int do_session_setup_lm( smb1_client *cl, const char *user,
                                const char *password, const char *domain )
{
    uint8_t *p = smb1_begin( cl, 0x73 );        /* SESSION_SETUP_ANDX */
    uint8_t *w = p + 1, *b, *q;
    int pwlen;

    p[0] = 10;                                  /* WordCount */
    w[0] = 0xFF; w[1] = 0;                       /* AndXCommand = none */
    put16( w + 2, 0 );                          /* AndXOffset */
    put16( w + 4, (unsigned int)sizeof( cl->recvbuf ) - 256 ); /* MaxBufferSize */
    put16( w + 6, 1 );                          /* MaxMpxCount */
    put16( w + 8, smb1_vc_number );             /* VcNumber */
    put32( w + 10, cl->sesskey );               /* SessionKey: the server's, given back */
    put32( w + 16, 0 );                         /* Reserved */

    b = w + 20 + 2;                             /* ByteCount is at w+20 */
    q = b;
    if ( cl->secmode & 1 ) pwlen = lm_password( cl, password, q );
    else { q[0] = 0; pwlen = 1; }
    put16( w + 14, (unsigned int)pwlen );       /* PasswordLength */
    q += pwlen;
    q += put_strz( q, user ? user : "", 1 );
    q += put_strz( q, domain ? domain : "", 1 );
    q += put_strz( q, "PM-DOS", 0 );            /* NativeOS */
    q += put_strz( q, "PMDOS SMB1", 0 );        /* NativeLanMan */
    put16( w + 20, (unsigned int)( q - b ) );   /* ByteCount */

    cl->stage = "session setup";
    if ( smb1_txrx( cl, q ) < 0 ) return -1;
    if ( cl->last_status != 0 ) return -1;
    cl->uid = get16( cl->recvbuf + 28 );        /* the server's UID */
    return 0;
}

static int do_tree_connect( smb1_client *cl, const char *server, const char *share,
                            const char *password )
{
    uint8_t *p = smb1_begin( cl, 0x75 );        /* TREE_CONNECT_ANDX */
    uint8_t *w = p + 1, *b, *q;
    int pwlen;

    p[0] = 4;                                   /* WordCount */
    w[0] = 0xFF; w[1] = 0;
    put16( w + 2, 0 );                          /* AndXOffset */
    put16( w + 4, 0 );                          /* Flags */

    b = w + 8 + 2;                              /* ByteCount at w+8 */
    q = b;
    if ( cl->secmode & 1 ) { q[0] = 0; pwlen = 1; }     /* user level: logged on */
    else pwlen = lm_password( cl, password, q );        /* share level */
    put16( w + 6, (unsigned int)pwlen );        /* PasswordLength */
    q += pwlen;
    *q++ = '\\'; *q++ = '\\';
    q += put_strz( q, server, 1 ) - 1;          /* drop the NUL between parts */
    *q++ = '\\';
    q += put_strz( q, share, 1 ) - 1;
    *q++ = 0;                                   /* the path's NUL */
    q += put_strz( q, "?????", 0 );             /* Service: any */
    put16( w + 8, (unsigned int)( q - b ) );    /* ByteCount */

    cl->stage = "tree connect";
    if ( smb1_txrx( cl, q ) < 0 ) return -1;
    if ( cl->last_status != 0 ) return -1;
    cl->tree_id = get16( cl->recvbuf + 24 );    /* the server's TID */
    return 0;
}

int smb1_tree_connect( smb1_client *cl, const char *server, const char *share,
                       const char *password )
{
    cl->tree_id = 0;
    if ( do_tree_connect( cl, server, share, password ) < 0 ) return -1;
    cl->stage = "connected";
    return 0;
}

int smb1_connect( smb1_client *cl, smb_send_fn send, smb_recv_fn recv, void *ioctx,
                  uint32_t seed,
                  const char *server, const char *share,
                  const char *user, const char *password, const char *domain )
{
    memset( cl, 0, sizeof( *cl ) );
    cl->send = send; cl->recv = recv; cl->ioctx = ioctx;
    cl->seed = seed; cl->mid = 1;
    cl->flags2 = SMB_FLAGS2;
    /* an ordinary PID: WfW 3.11's server refuses every file command from
       PID 0xFEFF (what this engine sent before) as "invalid function" */
    cl->pid = (unsigned int)( seed % 0x7E00UL ) + 0x100;

    cl->stage = "session request";
    if ( nbt_session_request( cl, server ) < 0 ) return -1;
    if ( do_negotiate( cl ) < 0 ) return -1;
    if ( !cl->lm ) {
        if ( do_session_setup( cl, user, password, domain ) < 0 ) return -1;
    } else if ( do_session_setup_lm( cl, user, password, domain ) < 0 ) {
        if ( cl->secmode & 1 ) return -1;       /* share level: the share decides */
    }
    if ( do_tree_connect( cl, server, share, password ) < 0 ) return -1;
    cl->stage = "connected";
    return 0;
}

/* ====================================================================== */
/* share-relative path -> "\dir\name" (backslash, leading backslash)      */
/* ====================================================================== */

static int build_path( uint8_t *dst, const char *dir, const char *leaf )
{
    int n = 0, i;
    dst[n++] = '\\';
    for ( i = 0; dir && dir[i]; i++ ) dst[n++] = (uint8_t)( ( dir[i] == '/' ) ? '\\' : dir[i] );
    if ( leaf ) {
        if ( n > 1 && dst[n - 1] != '\\' ) dst[n++] = '\\';
        for ( i = 0; leaf[i]; i++ ) dst[n++] = (uint8_t)leaf[i];
    }
    dst[n] = 0;
    return n;
}

/* ====================================================================== */
/* a LAN Manager server: the core SEARCH, READ and WRITE                  */
/* ====================================================================== */

/* WfW 3.11's server answers TRANS2 with an error, and its own redirector
   lists with SEARCH (0x81) and reads with READ (0x0A); every LAN Manager
   server has them, so a LAN Manager dialect gets these. */

/* a DOS date and time -> a Windows FILETIME, so a SEARCH entry comes back
   in the same smb2_dirent shape as an NT one.  32x32->64 multiplies, no
   64-bit type (SMBSRVDOS.C does the same). */
static void umul32( uint32_t a, uint32_t b, uint32_t *hi, uint32_t *lo )
{
    uint32_t a0 = a & 0xFFFF, a1 = a >> 16, b0 = b & 0xFFFF, b1 = b >> 16;
    uint32_t p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;
    uint32_t mid = ( p0 >> 16 ) + ( p1 & 0xFFFF ) + ( p2 & 0xFFFF );
    *lo = ( p0 & 0xFFFF ) | ( mid << 16 );
    *hi = p3 + ( p1 >> 16 ) + ( p2 >> 16 ) + ( mid >> 16 );
}
static long days_from_civil( int year, int month, int day )
{
    long era; unsigned int yoe, doy, doe;
    year -= ( month <= 2 );
    era = (long)( ( year >= 0 ? year : year - 399 ) / 400 );
    yoe = (unsigned int)( year - era * 400 );
    doy = (unsigned int)( ( 153 * ( month > 2 ? month - 3 : month + 9 ) + 2 ) / 5 + day - 1 );
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static void dos_filetime( unsigned int date, unsigned int time, uint32_t *lo, uint32_t *hi )
{
    int year  = 1980 + (int)( ( date >> 9 ) & 0x7F );
    int month = (int)( ( date >> 5 ) & 0x0F );
    int day   = (int)( date & 0x1F );
    uint32_t secs, shi, slo, phi, plo, sum;

    if ( date == 0 ) { *lo = 0; *hi = 0; return; }        /* no date */
    if ( month < 1 ) month = 1;
    if ( month > 12 ) month = 12;
    if ( day < 1 ) day = 1;
    secs = ( ( time >> 11 ) & 0x1F ) * 3600UL + ( ( time >> 5 ) & 0x3F ) * 60UL + ( time & 0x1F ) * 2UL;
    umul32( (uint32_t)( days_from_civil( year, month, day ) + 134774L ), 86400UL, &shi, &slo );
    sum = slo + secs;
    if ( sum < slo ) shi++;
    umul32( sum, 10000000UL, &phi, &plo );                 /* seconds -> 100ns */
    *lo = plo;
    *hi = phi + shi * 10000000UL;
}

#define SEARCH_ENTRY  43            /* key 21, attr 1, time 2, date 2, size 4, name 13 */

/* SEARCH (0x81) the whole directory: the first call names "\dir\????????.???",
   each next one carries the last entry's resume key.  The server ends the
   search itself when it runs out ("no more files", or a count of 0). */
static int list_dir_search( smb1_client *cl, const char *dir,
                            void ( *emit )( void *user, const smb2_dirent *entry ),
                            void *user )
{
    uint8_t key[21];
    int first = 1, maxcount, rounds = 0;

    maxcount = ( cl->max_buffer - 64 ) / SEARCH_ENTRY;
    if ( maxcount > ( (int)sizeof( cl->recvbuf ) - 64 ) / SEARCH_ENTRY ) maxcount = ( (int)sizeof( cl->recvbuf ) - 64 ) / SEARCH_ENTRY;
    if ( maxcount < 1 ) maxcount = 1;

    for ( ;; ) {
        uint8_t *p = smb1_begin( cl, 0x81 );          /* SEARCH */
        uint8_t *w = p + 1, *bc, *q;
        const uint8_t *rb, *e;
        int count, dlen, i, k;

        p[0] = 2;                                     /* WordCount */
        put16( w + 0, (unsigned int)maxcount );       /* MaxCount */
        put16( w + 2, 0x0016 );                       /* SearchAttributes: H|S|D */
        bc = w + 4;
        q = bc + 2;
        *q++ = 0x04;                                  /* buffer format: ASCII */
        if ( first ) {
            q += build_path( q, dir, "????????.???" ) + 1;
            *q++ = 0x05; *q++ = 0; *q++ = 0;          /* no resume key */
        } else {
            *q++ = 0;                                 /* no name: resume */
            *q++ = 0x05; *q++ = 21; *q++ = 0;         /* variable block, 21 bytes */
            memcpy( q, key, 21 ); q += 21;
        }
        put16( bc, (unsigned int)( q - ( bc + 2 ) ) );

        cl->stage = first ? "search" : "search next";
        if ( smb1_txrx( cl, q ) < 0 ) return -1;
        if ( cl->last_status != 0 ) {
            /* "no more files" (18) or "file not found" (2): the end, or an
               empty directory */
            if ( cl->last_status == 0x00120001UL || cl->last_status == 0x00020001UL ) return 0;
            return first ? -1 : 0;
        }
        if ( r_wordcount( cl ) < 1 ) return -1;
        count = (int)get16( r_words( cl ) );
        if ( count == 0 ) return 0;
        if ( r_bytecount( cl ) < 3 ) return -1;
        rb = r_bytes( cl );
        dlen = (int)get16( rb + 1 );
        if ( dlen > r_bytecount( cl ) - 3 ) dlen = r_bytecount( cl ) - 3;
        if ( count > dlen / SEARCH_ENTRY ) count = dlen / SEARCH_ENTRY;
        for ( i = 0; i < count; i++ ) {
            smb2_dirent de;
            e = rb + 3 + i * SEARCH_ENTRY;
            memset( &de, 0, sizeof( de ) );
            de.attr = e[21];
            dos_filetime( get16( e + 24 ), get16( e + 22 ), &de.wtime_lo, &de.wtime_hi );
            de.size_lo = get32( e + 26 );
            for ( k = 0; k < 12 && e[30 + k] && e[30 + k] != ' '; k++ ) de.name[k] = (char)e[30 + k];
            de.name[k] = 0;
            emit( user, &de );
        }
        memcpy( key, rb + 3 + ( count - 1 ) * SEARCH_ENTRY, 21 );
        first = 0;
        if ( ++rounds > 4096 ) return 0;             /* a server that never ends */
    }
}

/* READ (0x0A): 5 words (FID, count, offset, remaining); the answer's data
   follows a 0x01 buffer format and its own length */
static int read_core( smb1_client *cl, const smb2_fh *fh, uint32_t off,
                      uint8_t *buf, int len )
{
    uint8_t *p = smb1_begin( cl, 0x0A );             /* READ */
    uint8_t *w = p + 1;
    const uint8_t *rb;
    int dlen;

    p[0] = 5;                                         /* WordCount */
    put16( w + 0, get16( fh->file_id ) );             /* FID */
    put16( w + 2, (unsigned int)len );                /* Count */
    put32( w + 4, off );                              /* Offset */
    put16( w + 8, 0 );                                /* Remaining */
    put16( w + 10, 0 );                               /* ByteCount */

    cl->stage = "read";
    if ( smb1_txrx( cl, w + 12 ) < 0 ) return -1;
    if ( cl->last_status != 0 ) return -1;
    if ( r_wordcount( cl ) < 1 ) return -1;
    if ( r_bytecount( cl ) < 3 ) return 0;            /* nothing: the end */
    rb = r_bytes( cl );
    dlen = (int)get16( rb + 1 );
    if ( dlen > r_bytecount( cl ) - 3 ) dlen = r_bytecount( cl ) - 3;
    if ( dlen > len ) dlen = len;
    memcpy( buf, rb + 3, (unsigned)dlen );
    return dlen;
}

/* WRITE (0x0B): 5 words, then the data after a 0x01 buffer format and its
   length.  (A count of 0 would cut the file at 'off': never sent here.) */
static int write_core( smb1_client *cl, const smb2_fh *fh, uint32_t off,
                       const uint8_t *buf, int len )
{
    uint8_t *p = smb1_begin( cl, 0x0B );             /* WRITE */
    uint8_t *w = p + 1, *q;

    p[0] = 5;                                         /* WordCount */
    put16( w + 0, get16( fh->file_id ) );             /* FID */
    put16( w + 2, (unsigned int)len );                /* Count */
    put32( w + 4, off );                              /* Offset */
    put16( w + 8, 0 );                                /* Remaining */
    q = w + 12;
    q[0] = 0x01;                                      /* buffer format: data block */
    put16( q + 1, (unsigned int)len );
    memcpy( q + 3, buf, (unsigned)len );
    put16( w + 10, (unsigned int)( len + 3 ) );       /* ByteCount */

    cl->stage = "write";
    if ( smb1_txrx( cl, q + 3 + len ) < 0 ) return -1;
    if ( cl->last_status != 0 ) return -1;
    if ( r_wordcount( cl ) < 1 ) return -1;
    {
        int n = (int)get16( r_words( cl ) );          /* Count written */
        if ( n > len ) n = len;
        return n;
    }
}

/* ====================================================================== */
/* TRANS2 FIND_FIRST2 / FIND_NEXT2, info level 0x101                       */
/* ====================================================================== */

#define FIND_BOTH_LEVEL  0x0104U    /* SMB_FIND_FILE_BOTH_DIRECTORY_INFO */
#define FIND_DIR_LEVEL   0x0101U    /* SMB_FIND_FILE_DIRECTORY_INFO */

/* walk one FIND response's data block, calling emit for each entry.
   returns the count, and sets *eos / *last_name (the resume name).
   'fixed' is the entry's size before its name: 94 at level 0x104,
   which has the short name's length at 68 and its 24 bytes at 70
   (Unicode whatever the session's strings are, on Windows and Samba
   alike), 64 at level 0x101, which has none. */
static int find_walk( const uint8_t *data, int dcount, int fixed,
                      char *last_name,
                      void ( *emit )( void *user, const smb2_dirent *entry ),
                      void *user )
{
    int off = 0, count = 0;
    while ( off + fixed <= dcount ) {
        const uint8_t *e = data + off;
        uint32_t next = get32( e + 0 );
        uint32_t namelen = get32( e + 60 );
        smb2_dirent de;
        int i, cap;

        de.attr     = get32( e + 56 );
        de.size_lo  = get32( e + 40 );
        de.size_hi  = get32( e + 44 );
        de.wtime_lo = get32( e + 24 );
        de.wtime_hi = get32( e + 28 );

        cap = (int)namelen;
        if ( cap > (int)sizeof( de.name ) - 1 ) cap = (int)sizeof( de.name ) - 1;
        if ( off + fixed + cap > dcount ) cap = dcount - off - fixed;
        if ( cap < 0 ) cap = 0;
        for ( i = 0; i < cap; i++ ) de.name[i] = (char)e[fixed + i];
        de.name[cap] = 0;

        de.short_name[0] = 0;
        if ( fixed == 94 ) {
            int slen83 = e[68], j = 0;
            if ( slen83 > 24 ) slen83 = 24;
            if ( slen83 >= 2 && e[71] == 0 ) {           /* UTF-16LE */
                for ( i = 0; i + 1 < slen83 && j < 12; i += 2 ) de.short_name[j++] = (char)e[70 + i];
            } else {                                     /* an OEM server's */
                for ( i = 0; i < slen83 && j < 12 && e[70 + i]; i++ ) de.short_name[j++] = (char)e[70 + i];
            }
            de.short_name[j] = 0;
        }
        if ( last_name ) { for ( i = 0; i <= cap; i++ ) last_name[i] = de.name[i]; }

        emit( user, &de );
        count++;

        if ( next == 0 ) break;
        if ( next < 64 || next > (uint32_t)( dcount - off ) ) break;
        off += (int)next;
    }
    return count;
}

/* FIND_CLOSE2 (0x34): end a search the server still holds; its answer
   does not matter */
static void find_close( smb1_client *cl, unsigned int sid )
{
    uint8_t *p = smb1_begin( cl, 0x34 );
    p[0] = 1;                                    /* WordCount */
    put16( p + 1, sid );                         /* SID */
    put16( p + 3, 0 );                           /* ByteCount */
    cl->stage = "find close";
    smb1_txrx( cl, p + 5 );
}

int smb1_list_dir( smb1_client *cl, const char *dir, const char *pattern,
                   void ( *emit )( void *user, const smb2_dirent *entry ),
                   void *user )
{
    unsigned int  sid = 0, level = FIND_BOTH_LEVEL;
    int           eos = 0, first = 1, open = 0, rc = 0;
    char          last_name[260];

    (void)pattern;
    last_name[0] = 0;
    if ( cl->lm ) return list_dir_search( cl, dir, emit, user );

    while ( !eos ) {
        uint8_t *p = smb1_begin( cl, 0x32 );         /* TRANSACTION2 */
        uint8_t *w = p + 1;
        uint8_t *bc, *name, *parm;
        int plen, parmoff;
        int pcount, dcount, got;

        p[0] = 15;                                   /* WordCount */
        /* fixed TRANS2 words (14) then one setup word */
        put16( w + 0, 0 );   put16( w + 2, 0 );      /* TotalParam/Data (fill below) */
        put16( w + 4, 10 );                          /* MaxParameterCount */
        put16( w + 6, SMB1_TRANSDATA );              /* MaxDataCount: what trdata holds */
        w[8] = 0; w[9] = 0;                          /* MaxSetupCount / Reserved */
        put16( w + 10, 0 );                          /* Flags */
        put32( w + 12, 0 );                          /* Timeout */
        put16( w + 16, 0 );                          /* Reserved */
        /* ParameterCount/Offset, DataCount/Offset at w+18..25 (filled below) */
        put16( w + 22, 0 );                          /* DataCount = 0 */
        w[26] = 1; w[27] = 0;                         /* SetupCount=1 / Reserved */
        put16( w + 28, first ? 1 : 2 );              /* Setup[0]: FIND_FIRST2/NEXT2 */

        bc = w + 30;                                 /* ByteCount (2) */
        name = bc + 2;
        name[0] = 0;                                 /* TRANS2 name: just a NUL */
        /* 4-align the parameters from the SMB header start */
        parm = name + 1;
        while ( ( (int)( parm - ( cl->sendbuf + 4 ) ) ) & 3 ) *parm++ = 0;
        parmoff = (int)( parm - ( cl->sendbuf + 4 ) );

        if ( first ) {
            put16( parm + 0, 0x0016 );               /* SearchAttributes: H|S|D */
            put16( parm + 2, 256 );                  /* SearchCount */
            put16( parm + 4, 0x0006 );               /* Flags: resume keys + close@EOS */
            put16( parm + 6, level );
            put32( parm + 8, 0 );                    /* SearchStorageType */
            plen = 12 + build_path( parm + 12, dir, pattern ? pattern : "*" ) + 1;
        } else {
            put16( parm + 0, sid );                  /* SID */
            put16( parm + 2, 256 );                  /* SearchCount */
            put16( parm + 4, level );
            put32( parm + 6, 0 );                    /* ResumeKey */
            put16( parm + 10, 0x000E );              /* Flags: continue + resume keys
                                                        + close@EOS */
            plen = 12 + put_strz( parm + 12, last_name, 0 );
        }

        put16( w + 0, (unsigned int)plen );          /* TotalParameterCount */
        put16( w + 18, (unsigned int)plen );         /* ParameterCount */
        put16( w + 20, (unsigned int)parmoff );      /* ParameterOffset */
        /* DataOffset after DataCount: swapped, Windows answers every FIND
           with "invalid SMB" (0x00010002) */
        put16( w + 24, (unsigned int)( parmoff + plen ) ); /* DataOffset */
        put16( bc, (unsigned int)( ( parm + plen ) - ( bc + 2 ) ) ); /* ByteCount */

        cl->stage = first ? "find first" : "find next";
        if ( trans2_txrx( cl, parm + plen, &pcount, &dcount ) < 0 ) { rc = -1; break; }
        if ( cl->last_status != 0 ) {
            /* a server without the short names' level: the plain one */
            if ( first && level == FIND_BOTH_LEVEL &&
                 ( cl->last_status == 0xC0000003UL ||          /* INVALID_INFO_CLASS */
                   cl->last_status == 0xC0000002UL ||          /* NOT_IMPLEMENTED */
                   cl->last_status == 0xC000000DUL ||          /* INVALID_PARAMETER */
                   cl->last_status == 0xC0000148UL ||          /* INVALID_LEVEL */
                   cl->last_status == 0x00010002UL ) ) {       /* ERRSRV/ERRerror */
                level = FIND_DIR_LEVEL;
                continue;
            }
            if ( first ) rc = -1;                    /* FINDFIRST failed */
            break;                                   /* NEXT: no more (ok) */
        }

        /* FIND_FIRST2's parameters: SID, SearchCount, EndOfSearch, ...;
           FIND_NEXT2's: SearchCount, EndOfSearch, ... */
        if ( first ) {
            if ( pcount < 6 ) { rc = -1; break; }
            sid = get16( cl->trparm + 0 );
            eos = (int)get16( cl->trparm + 4 );
            open = 1;
            first = 0;
        } else {
            if ( pcount < 4 ) { rc = -1; break; }
            eos = (int)get16( cl->trparm + 2 );
        }
        if ( eos ) open = 0;                         /* close@EOS: the server ended it */

        got = find_walk( cl->trdata, dcount, ( level == FIND_BOTH_LEVEL ) ? 94 : 64, last_name, emit, user );
        if ( got == 0 ) break;                       /* nothing more came */
    }

    /* a search the server still holds (ended early, or a FIND_NEXT2
       failed) is closed, or it stays open there until the logoff: a
       server keeps only so many.  What went wrong stays reported. */
    if ( open && !cl->broken ) {
        const char   *stage  = cl->stage;
        uint32_t status = cl->last_status;
        find_close( cl, sid );
        cl->stage = stage;
        cl->last_status = status;
    }
    return rc;
}

/* ====================================================================== */
/* open / read / write / close                                            */
/* ====================================================================== */

/* TRANS2 QUERY_FILE_INFORMATION (Setup 7) at level 0x102, SMB_QUERY_FILE_
   STANDARD_INFO: AllocationSize (8), EndOfFile (8), NumberOfLinks (4),
   DeletePending (1), Directory (1).  Parameters FID (2) and the level
   (2); the answer's one parameter word is EaErrorOffset.  It is asked
   after every open because OPEN_ANDX's DataSize is 32 bits: a file of
   4GB and more looked 4GB smaller, and a disk image on a share past
   that size would not mount (DOS\IMAGE.INC, IMF_NET).  0, or -1 when
   the server will not say - WfW's answers every TRANS2 with an error -
   and the caller keeps the 32-bit size. */
#define FILE_STD_LEVEL  0x0102U     /* SMB_QUERY_FILE_STANDARD_INFO */

static int file_size_trans2( smb1_client *cl, unsigned int fid,
                             uint32_t *size_lo, uint32_t *size_hi )
{
    uint8_t *p = smb1_begin( cl, 0x32 );             /* TRANSACTION2 */
    uint8_t *w = p + 1, *bc, *parm;
    const uint8_t *d;
    int parmoff, pcount, dcount;

    p[0] = 15;                                       /* WordCount */
    memset( w, 0, 30 );
    put16( w + 0, 4 );                               /* TotalParameterCount */
    put16( w + 4, 2 );                               /* MaxParameterCount */
    put16( w + 6, 64 );                              /* MaxDataCount */
    w[26] = 1;                                       /* SetupCount */
    put16( w + 28, 7 );                              /* Setup[0]: QUERY_FILE_INFORMATION */
    bc = w + 30;
    parm = bc + 2;
    *parm++ = 0;                                     /* the TRANS2 name: a NUL */
    while ( ( (int)( parm - ( cl->sendbuf + 4 ) ) ) & 3 ) *parm++ = 0;
    parmoff = (int)( parm - ( cl->sendbuf + 4 ) );
    put16( parm, fid );
    put16( parm + 2, FILE_STD_LEVEL );
    put16( w + 18, 4 );                              /* ParameterCount */
    put16( w + 20, (unsigned int)parmoff );          /* ParameterOffset */
    put16( w + 24, (unsigned int)( parmoff + 4 ) );  /* DataOffset (no data) */
    put16( bc, (unsigned int)( ( parm + 4 ) - ( bc + 2 ) ) );

    cl->stage = "file-size";
    if ( trans2_txrx( cl, parm + 4, &pcount, &dcount ) < 0 ) return -1;
    if ( cl->last_status != 0 || dcount < 16 ) return -1;
    d = cl->trdata;
    *size_lo = get32( d + 8 );                       /* EndOfFile */
    *size_hi = get32( d + 12 );
    return 0;
}

int smb1_open_disp( smb1_client *cl, const char *path, int write,
                    uint32_t disposition, smb2_fh *fh )
{
    uint8_t *p = smb1_begin( cl, 0x2D );              /* OPEN_ANDX */
    uint8_t *w = p + 1, *bc, *q;
    unsigned int openfn, access;

    /* OpenFunction: bit4 (0x10) create-if-absent; low bits action-if-present
       (1 open, 2 truncate).  Map the SMB2 dispositions the redirector uses. */
    if ( disposition == 5 )       openfn = 0x0012;    /* OVERWRITE_IF: create|trunc */
    else if ( disposition == 3 )  openfn = 0x0011;    /* OPEN_IF: create|open */
    else                          openfn = 0x0001;    /* OPEN: open existing */
    access = write ? 2 : 0;                           /* 0 read, 2 read/write */

    p[0] = 15;                                        /* WordCount */
    w[0] = 0xFF; w[1] = 0; put16( w + 2, 0 );         /* AndX none */
    put16( w + 4, 0x0001 );                           /* Flags: send the size and attributes
                                                         (Windows answers 0 without) */
    put16( w + 6, access );                           /* AccessMode */
    put16( w + 8, 0 );                                /* SearchAttributes */
    put16( w + 10, 0 );                               /* FileAttributes */
    put32( w + 12, 0 );                               /* CreationTime */
    put16( w + 16, openfn );                          /* OpenFunction */
    put32( w + 18, 0 );                               /* AllocationSize */
    put32( w + 22, 0 );                               /* Timeout */
    put32( w + 26, 0 );                               /* Reserved */

    bc = w + 30;                                      /* ByteCount (2) at w+30 */
    q = bc + 2;
    { uint8_t pb[300]; int n = build_path( pb, path, (const char *)0 ); memcpy( q, pb, n + 1 ); q += n + 1; }
    put16( bc, (unsigned int)( q - ( bc + 2 ) ) );

    cl->stage = "open";
    if ( smb1_txrx( cl, q ) < 0 ) return -1;
    if ( cl->last_status != 0 ) return -1;
    if ( r_wordcount( cl ) < 15 ) return -1;

    memset( fh, 0, sizeof( *fh ) );
    put16( fh->file_id, get16( r_words( cl ) + 4 ) );   /* FID in file_id[0..1] */
    fh->attr    = get16( r_words( cl ) + 6 );
    fh->size_lo = get32( r_words( cl ) + 12 );          /* DataSize (32-bit) */
    fh->size_hi = 0;

    /* ...which is all OPEN_ANDX has room for.  The dialect with TRANS2
       (not LANMAN's) is asked for the 64-bit size; a server that will
       not say keeps the 32-bit one, and the open stands either way. */
    if ( !cl->lm && !( fh->attr & 0x10 ) ) {
        uint32_t lo, hi;
        const char *stage = cl->stage;
        if ( file_size_trans2( cl, get16( fh->file_id ), &lo, &hi ) == 0 ) {
            fh->size_lo = lo;
            fh->size_hi = hi;
        }
        cl->stage = stage;
        cl->last_status = 0;
    }
    return 0;
}

int smb1_read( smb1_client *cl, const smb2_fh *fh, uint32_t off_lo, uint32_t off_hi,
               uint8_t *buf, int len )
{
    uint8_t *p, *w;
    int dlen, doff;

    if ( len <= 0 ) return 0;
    if ( len > (int)sizeof( cl->recvbuf ) - 128 ) len = (int)sizeof( cl->recvbuf ) - 128;
    if ( cl->lm ) {                                   /* READ, in its buffer */
        if ( off_hi != 0 ) return -1;
        if ( len > cl->max_buffer - 64 ) len = cl->max_buffer - 64;
        return read_core( cl, fh, off_lo, buf, len );
    }
    p = smb1_begin( cl, 0x2E );                       /* READ_ANDX */
    w = p + 1;

    p[0] = 12;                                        /* WordCount */
    w[0] = 0xFF; w[1] = 0; put16( w + 2, 0 );
    put16( w + 4, get16( fh->file_id ) );             /* FID */
    put32( w + 6, off_lo );                           /* Offset (low) */
    put16( w + 10, (unsigned int)len );               /* MaxCount */
    put16( w + 12, (unsigned int)len );               /* MinCount */
    put32( w + 14, 0 );                               /* MaxCountHigh */
    put16( w + 18, 0 );                               /* Remaining */
    put32( w + 20, off_hi );                          /* OffsetHigh */
    put16( w + 24, 0 );                               /* ByteCount */

    cl->stage = "read";
    if ( smb1_txrx( cl, w + 26 ) < 0 ) return -1;
    if ( cl->last_status != 0 ) return -1;
    if ( r_wordcount( cl ) < 12 ) return -1;

    dlen = (int)get16( r_words( cl ) + 10 ) | ( (int)get16( r_words( cl ) + 14 ) << 16 );
    doff = (int)get16( r_words( cl ) + 12 );
    if ( doff > cl->recvlen ) return 0;                /* a bogus data offset */
    if ( dlen < 0 ) dlen = 0;
    if ( dlen > cl->recvlen - doff ) dlen = cl->recvlen - doff;
    if ( dlen < 0 ) dlen = 0;
    if ( dlen > len ) dlen = len;
    memcpy( buf, cl->recvbuf + doff, (unsigned)dlen );
    return dlen;
}

int smb1_write( smb1_client *cl, const smb2_fh *fh, uint32_t off_lo, uint32_t off_hi,
                const uint8_t *buf, int len )
{
    uint8_t *p, *w, *bc, *data;
    int doff;

    if ( len <= 0 ) return 0;
    if ( len > (int)sizeof( cl->sendbuf ) - 128 ) len = (int)sizeof( cl->sendbuf ) - 128;
    if ( cl->lm ) {                                   /* WRITE, in its buffer */
        if ( off_hi != 0 ) return -1;
        if ( len > cl->max_buffer - 64 ) len = cl->max_buffer - 64;
        return write_core( cl, fh, off_lo, buf, len );
    }
    p = smb1_begin( cl, 0x2F );                       /* WRITE_ANDX */
    w = p + 1;

    p[0] = 14;                                        /* WordCount */
    w[0] = 0xFF; w[1] = 0; put16( w + 2, 0 );
    put16( w + 4, get16( fh->file_id ) );             /* FID */
    put32( w + 6, off_lo );                           /* Offset */
    put32( w + 10, 0 );                               /* Reserved (timeout) */
    put16( w + 14, 0 );                               /* WriteMode */
    put16( w + 16, 0 );                               /* Remaining */
    put16( w + 18, 0 );                               /* DataLengthHigh */
    put16( w + 20, (unsigned int)len );               /* DataLength */
    bc = w + 28;                                      /* ByteCount at w+28 */
    data = bc + 2 + 1;                                /* one pad byte */
    bc[2] = 0;                                         /* pad */
    doff = (int)( data - ( cl->sendbuf + 4 ) );
    put16( w + 22, (unsigned int)doff );              /* DataOffset */
    put32( w + 24, off_hi );                          /* OffsetHigh */
    memcpy( data, buf, (unsigned)len );
    put16( bc, (unsigned int)( len + 1 ) );           /* ByteCount (pad + data) */

    cl->stage = "write";
    if ( smb1_txrx( cl, data + len ) < 0 ) return -1;
    if ( cl->last_status != 0 ) return -1;
    if ( r_wordcount( cl ) < 6 ) return -1;
    {
        int n = (int)get16( r_words( cl ) + 4 );      /* Count written */
        if ( n > len ) n = len;
        return n;
    }
}

int smb1_close_file( smb1_client *cl, const smb2_fh *fh )
{
    uint8_t *p = smb1_begin( cl, 0x04 );              /* CLOSE */
    uint8_t *w = p + 1;

    p[0] = 3;                                         /* WordCount */
    put16( w + 0, get16( fh->file_id ) );             /* FID */
    put32( w + 2, 0xFFFFFFFFUL );                     /* LastWriteTime: leave */
    put16( w + 6, 0 );                                /* ByteCount */

    cl->stage = "close";
    if ( smb1_txrx( cl, w + 8 ) < 0 ) return -1;
    return ( cl->last_status == 0 ) ? 0 : -1;
}

/* ====================================================================== */
/* delete / mkdir / rmdir (the classic, widely-served commands)            */
/* ====================================================================== */

static int path_cmd( smb1_client *cl, int command, int is_delete, const char *path )
{
    uint8_t *p = smb1_begin( cl, command );
    uint8_t *w = p + 1, *bc, *q;

    if ( is_delete ) { p[0] = 1; put16( w, 0 ); bc = w + 2; }   /* SearchAttributes=0 */
    else             { p[0] = 0; bc = w; }
    q = bc + 2;
    *q++ = 0x04;                                      /* buffer format: ASCII */
    { uint8_t pb[300]; int n = build_path( pb, path, (const char *)0 ); memcpy( q, pb, n + 1 ); q += n + 1; }
    put16( bc, (unsigned int)( q - ( bc + 2 ) ) );

    cl->stage = "pathop";
    if ( smb1_txrx( cl, q ) < 0 ) return -1;
    return ( cl->last_status == 0 ) ? 0 : -1;
}

int smb1_delete( smb1_client *cl, const char *path ) { return path_cmd( cl, 0x06, 1, path ); }  /* DELETE */
int smb1_mkdir ( smb1_client *cl, const char *path ) { return path_cmd( cl, 0x00, 0, path ); }  /* CREATE_DIRECTORY */
int smb1_rmdir ( smb1_client *cl, const char *path ) { return path_cmd( cl, 0x01, 0, path ); }  /* DELETE_DIRECTORY */
int smb1_isdir ( smb1_client *cl, const char *path ) { return path_cmd( cl, 0x10, 0, path ); }  /* CHECK_DIRECTORY */

/* ====================================================================== */
/* the share's size                                                        */
/* ====================================================================== */

#define FS_SIZE_LEVEL  0x0103U      /* SMB_QUERY_FS_SIZE_INFO */

/* TRANS2 QUERY_FS_INFORMATION (Setup 3), one parameter word: the level.
   The answer is data only - TotalAllocationUnits (8),
   TotalFreeAllocationUnits (8), SectorsPerAllocationUnit (4),
   BytesPerSector (4). */
static int fs_size_trans2( smb1_client *cl, smb_fs_size *fs )
{
    uint8_t *p = smb1_begin( cl, 0x32 );             /* TRANSACTION2 */
    uint8_t *w = p + 1, *bc, *parm;
    const uint8_t *d;
    int parmoff, pcount, dcount;

    p[0] = 15;                                       /* WordCount */
    memset( w, 0, 30 );
    put16( w + 0, 2 );                               /* TotalParameterCount */
    put16( w + 4, 0 );                               /* MaxParameterCount */
    put16( w + 6, 64 );                              /* MaxDataCount */
    w[26] = 1;                                       /* SetupCount */
    put16( w + 28, 3 );                              /* Setup[0]: QUERY_FS_INFORMATION */
    bc = w + 30;
    parm = bc + 2;
    *parm++ = 0;                                     /* the TRANS2 name: a NUL */
    while ( ( (int)( parm - ( cl->sendbuf + 4 ) ) ) & 3 ) *parm++ = 0;
    parmoff = (int)( parm - ( cl->sendbuf + 4 ) );
    put16( parm, FS_SIZE_LEVEL );
    put16( w + 18, 2 );                              /* ParameterCount */
    put16( w + 20, (unsigned int)parmoff );          /* ParameterOffset */
    put16( w + 24, (unsigned int)( parmoff + 2 ) );  /* DataOffset (no data) */
    put16( bc, (unsigned int)( ( parm + 2 ) - ( bc + 2 ) ) );

    cl->stage = "fs-size";
    if ( trans2_txrx( cl, parm + 2, &pcount, &dcount ) < 0 ) return -1;
    if ( cl->last_status != 0 || dcount < 24 ) return -1;
    d = cl->trdata;
    fs->total_lo = get32( d + 0 );
    fs->total_hi = get32( d + 4 );
    fs->free_lo  = get32( d + 8 );
    fs->free_hi  = get32( d + 12 );
    fs->sec_per_unit  = get32( d + 16 );
    fs->bytes_per_sec = get32( d + 20 );
    return 0;
}

/* QUERY_INFORMATION_DISK (0x80), the core command: TotalUnits,
   BlocksPerUnit, BlockSize, FreeUnits - 16 bits each, so a big disk is
   reported smaller than it is (the server picks the shape). */
static int fs_size_core( smb1_client *cl, smb_fs_size *fs )
{
    uint8_t *p = smb1_begin( cl, 0x80 );
    const uint8_t *rw;

    p[0] = 0;                                        /* WordCount */
    put16( p + 1, 0 );                               /* ByteCount */
    cl->stage = "fs-size";
    if ( smb1_txrx( cl, p + 3 ) < 0 ) return -1;
    if ( cl->last_status != 0 || r_wordcount( cl ) < 4 ) return -1;
    rw = r_words( cl );
    fs->total_lo = get16( rw + 0 );
    fs->sec_per_unit = get16( rw + 2 );
    fs->bytes_per_sec = get16( rw + 4 );
    fs->free_lo = get16( rw + 6 );
    fs->total_hi = fs->free_hi = 0;
    return 0;
}

int smb1_fs_size( smb1_client *cl, smb_fs_size *fs )
{
    memset( fs, 0, sizeof( *fs ) );
    if ( !cl->lm && fs_size_trans2( cl, fs ) == 0 ) return 0;
    memset( fs, 0, sizeof( *fs ) );
    return fs_size_core( cl, fs );
}

/* RENAME (0x07): WordCount 1 = SearchAttributes, then two ASCII paths in
   the byte block, each 0x04-tagged and NUL-terminated.  The search
   attribute lets a hidden, system or directory entry be renamed too. */
int smb1_rename( smb1_client *cl, const char *oldpath, const char *newpath )
{
    uint8_t *p = smb1_begin( cl, 0x07 );
    uint8_t *w = p + 1, *bc, *q;
    uint8_t pb[300];
    int n;

    p[0] = 1;                                         /* WordCount */
    put16( w, 0x16 );                                 /* SearchAttributes: hidden,system,dir */
    bc = w + 2;
    q = bc + 2;
    *q++ = 0x04;                                      /* buffer format: ASCII (old name) */
    n = build_path( pb, oldpath, (const char *)0 );
    memcpy( q, pb, n + 1 ); q += n + 1;
    *q++ = 0x04;                                      /* buffer format: ASCII (new name) */
    n = build_path( pb, newpath, (const char *)0 );
    memcpy( q, pb, n + 1 ); q += n + 1;
    put16( bc, (unsigned int)( q - ( bc + 2 ) ) );    /* ByteCount */

    cl->stage = "rename";
    if ( smb1_txrx( cl, q ) < 0 ) return -1;
    return ( cl->last_status == 0 ) ? 0 : -1;
}

void smb1_disconnect( smb1_client *cl )
{
    uint8_t *p;
    if ( cl->tree_id ) {
        p = smb1_begin( cl, 0x71 );                   /* TREE_DISCONNECT */
        p[0] = 0; put16( p + 1, 0 );
        smb1_txrx( cl, p + 3 );
        cl->tree_id = 0;
    }
}

const char   *smb1_stage( const smb1_client *cl )  { return cl->stage ? cl->stage : "?"; }
uint32_t smb1_status( const smb1_client *cl ) { return cl->last_status; }
