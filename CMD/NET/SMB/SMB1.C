/* ==========================================================================
 * SMB1.C - the SMB1 client engine: "NT LM 0.12" (CIFS) and the LAN
 * Manager dialects.  See SMB1.H.
 * ==========================================================================
 * NetBIOS-over-TCP (port 139): a session request, then every SMB message
 * rides a 4-byte NBT header (type, flags, 16-bit big-endian length).  The
 * SMB header is the 32-byte CIFS header; OEM (8-bit) strings, no signing.
 * Authentication is the old LM + NTLMv1 challenge-response.
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
static void put16( unsigned char *p, unsigned int v ) { p[0] = (unsigned char)v; p[1] = (unsigned char)( v >> 8 ); }
static void put32( unsigned char *p, unsigned long v ) { p[0] = (unsigned char)v; p[1] = (unsigned char)( v >> 8 ); p[2] = (unsigned char)( v >> 16 ); p[3] = (unsigned char)( v >> 24 ); }
static unsigned int  get16( const unsigned char *p ) { return (unsigned int)p[0] | ( (unsigned int)p[1] << 8 ); }
static unsigned long get32( const unsigned char *p ) { return (unsigned long)p[0] | ( (unsigned long)p[1] << 8 ) | ( (unsigned long)p[2] << 16 ) | ( (unsigned long)p[3] << 24 ); }

static int slen( const char *s ) { int n = 0; while ( s[n] ) n++; return n; }
static char supper( char ch ) { return ( ch >= 'a' && ch <= 'z' ) ? (char)( ch - 32 ) : ch; }

/* copy an ASCIIZ string (with its NUL) to 'dst', uppercased when 'up';
   returns the number of bytes written, the NUL included */
static int put_strz( unsigned char *dst, const char *src, int up )
{
    int i = 0;
    while ( src[i] ) { dst[i] = (unsigned char)( up ? supper( src[i] ) : src[i] ); i++; }
    dst[i] = 0;
    return i + 1;
}

/* ====================================================================== */
/* transport: NBT framing over the send/recv callbacks                    */
/* ====================================================================== */

static int send_all( smb1_client *cl, const unsigned char *buf, int len )
{
    int off = 0, n;
    while ( off < len ) {
        n = cl->send( cl->ioctx, buf + off, len - off );
        if ( n <= 0 ) return -1;
        off += n;
    }
    return 0;
}

static int read_n( smb1_client *cl, unsigned char *buf, int n )
{
    int off = 0, got;
    while ( off < n ) {
        got = cl->recv( cl->ioctx, buf + off, n - off );
        if ( got <= 0 ) return -1;
        off += got;
    }
    return 0;
}

/* receive one NBT message into 'out' (max 'cap'); skip keep-alives (0x85).
   returns the payload length, or <0. */
static int nbt_recv( smb1_client *cl, unsigned char *out, int cap )
{
    unsigned char hdr[4];
    int len;
    for ( ;; ) {
        if ( read_n( cl, hdr, 4 ) < 0 ) return -1;
        len = ( ( hdr[1] & 1 ) << 16 ) | ( hdr[2] << 8 ) | hdr[3];
        if ( hdr[0] == 0x85 ) continue;                 /* session keep-alive */
        if ( len == 0 ) return 0;
        if ( len > cap ) return -1;
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
static unsigned char *smb1_begin( smb1_client *cl, int command )
{
    unsigned char *p = cl->sendbuf + 4;         /* the SMB header start */
    memset( cl->sendbuf, 0, 37 );
    p[0] = 0xFF; p[1] = 'S'; p[2] = 'M'; p[3] = 'B';
    p[4] = (unsigned char)command;              /* Command */
    p[9] = 0x18;                                /* Flags: canon + case-insensitive */
    put16( p + 10, cl->flags2 );                /* Flags2 */
    put16( p + 24, cl->tree_id );               /* TID */
    put16( p + 26, cl->pid );                   /* PID: ours, for the mount */
    put16( p + 28, cl->uid );                   /* UID */
    put16( p + 30, cl->mid++ );                 /* MID */
    return p + 32;                              /* -> WordCount */
}

/* send what was built (end -> just past the last byte) and receive the
   reply into recvbuf.  Stores cl->last_status.  0 if a reply came (even
   an error status), <0 on a transport failure or a malformed reply (a
   success whose word or byte block runs past what was received). */
static int smb1_txrx( smb1_client *cl, unsigned char *end )
{
    unsigned char *p = cl->sendbuf + 4;
    int smblen = (int)( end - p );
    cl->sendbuf[0] = 0;
    cl->sendbuf[1] = (unsigned char)( ( smblen >> 16 ) & 1 );
    cl->sendbuf[2] = (unsigned char)( smblen >> 8 );
    cl->sendbuf[3] = (unsigned char)smblen;
    if ( send_all( cl, cl->sendbuf, smblen + 4 ) < 0 ) return -1;

    cl->recvlen = nbt_recv( cl, cl->recvbuf, (int)sizeof( cl->recvbuf ) );
    if ( cl->recvlen < 33 ) return -1;
    if ( cl->recvbuf[0] != 0xFF || cl->recvbuf[1] != 'S' ||
         cl->recvbuf[2] != 'M' || cl->recvbuf[3] != 'B' ) return -1;
    cl->last_status = get32( cl->recvbuf + 5 );
    if ( cl->last_status == 0 ) {
        /* the words and bytes a good reply carries must have come in */
        int fixed = 33 + cl->recvbuf[32] * 2;
        if ( fixed + 2 > cl->recvlen ) return -1;
        if ( fixed + 2 + (int)get16( cl->recvbuf + fixed ) > cl->recvlen ) return -1;
    }
    return 0;
}

/* response accessors (the reply SMB is at recvbuf[0]) */
static int r_wordcount( smb1_client *cl ) { return cl->recvbuf[32]; }
static const unsigned char *r_words( smb1_client *cl ) { return cl->recvbuf + 33; }
static int r_bytecount( smb1_client *cl )
{
    int wc = cl->recvbuf[32];
    return (int)get16( cl->recvbuf + 33 + wc * 2 );
}
static const unsigned char *r_bytes( smb1_client *cl )
{
    int wc = cl->recvbuf[32];
    return cl->recvbuf + 33 + wc * 2 + 2;
}

/* ====================================================================== */
/* the NetBIOS session request                                            */
/* ====================================================================== */

static void nb_encode( const char *name, unsigned char type, unsigned char *out )
{
    char padded[16];
    int i, n = slen( name );
    for ( i = 0; i < 15; i++ ) padded[i] = ( i < n ) ? supper( name[i] ) : ' ';
    padded[15] = (char)type;
    out[0] = 0x20;
    for ( i = 0; i < 16; i++ ) {
        out[1 + i * 2]     = (unsigned char)( 'A' + ( ( padded[i] >> 4 ) & 0x0F ) );
        out[1 + i * 2 + 1] = (unsigned char)( 'A' + ( padded[i] & 0x0F ) );
    }
    out[33] = 0;
}

char smb1_calling[16] = "PMDOS";

static int nbt_session_request( smb1_client *cl, const char *server )
{
    unsigned char pkt[4 + 68];
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
        unsigned char hdr[4];
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
    unsigned char *p, *b, *end;
    const unsigned char *w;
    unsigned long maxbuf;
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
static int lm_password( smb1_client *cl, const char *password, unsigned char *out )
{
    unsigned char h[16];
    int i;

    if ( password == (const char *)0 || password[0] == 0 ) { out[0] = 0; return 1; }
    if ( cl->secmode & 2 ) {
        lm_hash( password, h );
        desl( h, cl->challenge, out );
        return 24;
    }
    for ( i = 0; i < 63 && password[i]; i++ ) out[i] = (unsigned char)supper( password[i] );
    out[i] = 0;
    return i + 1;
}

static int do_session_setup( smb1_client *cl, const char *user,
                             const char *password, const char *domain )
{
    unsigned char *p = smb1_begin( cl, 0x73 );  /* SESSION_SETUP_ANDX */
    unsigned char *w = p + 1, *b, *q;
    unsigned char lm[24], nt[24], h[16];

    nt_hash( password, h ); desl( h, cl->challenge, nt );
    lm_hash( password, h ); desl( h, cl->challenge, lm );

    p[0] = 13;                                  /* WordCount */
    w[0] = 0xFF; w[1] = 0;                       /* AndXCommand = none */
    put16( w + 2, 0 );                          /* AndXOffset */
    put16( w + 4, (unsigned int)sizeof( cl->recvbuf ) - 256 ); /* MaxBufferSize */
    put16( w + 6, 1 );                          /* MaxMpxCount */
    put16( w + 8, 0 );                          /* VcNumber */
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
    unsigned char *p = smb1_begin( cl, 0x73 );  /* SESSION_SETUP_ANDX */
    unsigned char *w = p + 1, *b, *q;
    int pwlen;

    p[0] = 10;                                  /* WordCount */
    w[0] = 0xFF; w[1] = 0;                       /* AndXCommand = none */
    put16( w + 2, 0 );                          /* AndXOffset */
    put16( w + 4, (unsigned int)sizeof( cl->recvbuf ) - 256 ); /* MaxBufferSize */
    put16( w + 6, 1 );                          /* MaxMpxCount */
    put16( w + 8, 0 );                          /* VcNumber */
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
    unsigned char *p = smb1_begin( cl, 0x75 );  /* TREE_CONNECT_ANDX */
    unsigned char *w = p + 1, *b, *q;
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

int smb1_connect( smb1_client *cl, smb_send_fn send, smb_recv_fn recv, void *ioctx,
                  unsigned long seed,
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

static int build_path( unsigned char *dst, const char *dir, const char *leaf )
{
    int n = 0, i;
    dst[n++] = '\\';
    for ( i = 0; dir && dir[i]; i++ ) dst[n++] = (unsigned char)( ( dir[i] == '/' ) ? '\\' : dir[i] );
    if ( leaf ) {
        if ( n > 1 && dst[n - 1] != '\\' ) dst[n++] = '\\';
        for ( i = 0; leaf[i]; i++ ) dst[n++] = (unsigned char)leaf[i];
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
static void umul32( unsigned long a, unsigned long b, unsigned long *hi, unsigned long *lo )
{
    unsigned long a0 = a & 0xFFFF, a1 = a >> 16, b0 = b & 0xFFFF, b1 = b >> 16;
    unsigned long p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;
    unsigned long mid = ( p0 >> 16 ) + ( p1 & 0xFFFF ) + ( p2 & 0xFFFF );
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
static void dos_filetime( unsigned int date, unsigned int time, unsigned long *lo, unsigned long *hi )
{
    int year  = 1980 + (int)( ( date >> 9 ) & 0x7F );
    int month = (int)( ( date >> 5 ) & 0x0F );
    int day   = (int)( date & 0x1F );
    unsigned long secs, shi, slo, phi, plo, sum;

    if ( date == 0 ) { *lo = 0; *hi = 0; return; }        /* no date */
    if ( month < 1 ) month = 1;
    if ( month > 12 ) month = 12;
    if ( day < 1 ) day = 1;
    secs = ( ( time >> 11 ) & 0x1F ) * 3600UL + ( ( time >> 5 ) & 0x3F ) * 60UL + ( time & 0x1F ) * 2UL;
    umul32( (unsigned long)( days_from_civil( year, month, day ) + 134774L ), 86400UL, &shi, &slo );
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
    unsigned char key[21];
    int first = 1, maxcount, rounds = 0;

    maxcount = ( cl->max_buffer - 64 ) / SEARCH_ENTRY;
    if ( maxcount > ( (int)sizeof( cl->recvbuf ) - 64 ) / SEARCH_ENTRY ) maxcount = ( (int)sizeof( cl->recvbuf ) - 64 ) / SEARCH_ENTRY;
    if ( maxcount < 1 ) maxcount = 1;

    for ( ;; ) {
        unsigned char *p = smb1_begin( cl, 0x81 );    /* SEARCH */
        unsigned char *w = p + 1, *bc, *q;
        const unsigned char *rb, *e;
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
static int read_core( smb1_client *cl, const smb2_fh *fh, unsigned long off,
                      unsigned char *buf, int len )
{
    unsigned char *p = smb1_begin( cl, 0x0A );       /* READ */
    unsigned char *w = p + 1;
    const unsigned char *rb;
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
static int write_core( smb1_client *cl, const smb2_fh *fh, unsigned long off,
                       const unsigned char *buf, int len )
{
    unsigned char *p = smb1_begin( cl, 0x0B );       /* WRITE */
    unsigned char *w = p + 1, *q;

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

#define FIND_INFO_LEVEL  0x0101U    /* SMB_FIND_FILE_DIRECTORY_INFO */

/* walk one FIND response's data block, calling emit for each entry.
   returns the count, and sets *eos / *last_name (the resume name). */
static int find_walk( const unsigned char *data, int dcount, int want_resume,
                      char *last_name,
                      void ( *emit )( void *user, const smb2_dirent *entry ),
                      void *user )
{
    int off = 0, count = 0;
    (void)want_resume;
    while ( off + 64 <= dcount ) {
        const unsigned char *e = data + off;
        unsigned long next = get32( e + 0 );
        unsigned long namelen = get32( e + 60 );
        smb2_dirent de;
        int i, cap;

        de.attr     = get32( e + 56 );
        de.size_lo  = get32( e + 40 );
        de.size_hi  = get32( e + 44 );
        de.wtime_lo = get32( e + 24 );
        de.wtime_hi = get32( e + 28 );

        cap = (int)namelen;
        if ( cap > (int)sizeof( de.name ) - 1 ) cap = (int)sizeof( de.name ) - 1;
        if ( off + 64 + cap > dcount ) cap = dcount - off - 64;
        if ( cap < 0 ) cap = 0;
        for ( i = 0; i < cap; i++ ) de.name[i] = (char)e[64 + i];
        de.name[cap] = 0;
        if ( last_name ) { for ( i = 0; i <= cap; i++ ) last_name[i] = de.name[i]; }

        emit( user, &de );
        count++;

        if ( next == 0 ) break;
        if ( next < 64 || next > (unsigned long)( dcount - off ) ) break;
        off += (int)next;
    }
    return count;
}

int smb1_list_dir( smb1_client *cl, const char *dir, const char *pattern,
                   void ( *emit )( void *user, const smb2_dirent *entry ),
                   void *user )
{
    unsigned int  sid = 0;
    int           eos = 0, total = 0, first = 1;
    char          last_name[260];

    (void)pattern;
    last_name[0] = 0;
    if ( cl->lm ) return list_dir_search( cl, dir, emit, user );

    while ( !eos ) {
        unsigned char *p = smb1_begin( cl, 0x32 );   /* TRANSACTION2 */
        unsigned char *w = p + 1;
        unsigned char *bc, *name, *parm;
        const unsigned char *rp, *rdata;
        int plen, parmoff, rwc;
        int dcount, got;

        p[0] = 15;                                   /* WordCount */
        /* fixed TRANS2 words (14) then one setup word */
        put16( w + 0, 0 );   put16( w + 2, 0 );      /* TotalParam/Data (fill below) */
        put16( w + 4, 10 );                          /* MaxParameterCount */
        put16( w + 6, (unsigned int)sizeof( cl->recvbuf ) - 512 ); /* MaxDataCount */
        w[8] = 0; w[9] = 0;                          /* MaxSetupCount / Reserved */
        put16( w + 10, 0 );                          /* Flags */
        put32( w + 12, 0 );                          /* Timeout */
        put16( w + 16, 0 );                          /* Reserved */
        /* ParameterCount/Offset, DataCount/Offset at w+18..25 (filled below) */
        put16( w + 24, 0 );                          /* DataCount = 0 */
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
            put16( parm + 6, FIND_INFO_LEVEL );
            put32( parm + 8, 0 );                    /* SearchStorageType */
            plen = 12 + build_path( parm + 12, dir, pattern ? pattern : "*" ) + 1;
        } else {
            put16( parm + 0, sid );                  /* SID */
            put16( parm + 2, 256 );                  /* SearchCount */
            put16( parm + 4, FIND_INFO_LEVEL );
            put32( parm + 6, 0 );                    /* ResumeKey */
            put16( parm + 10, 0x000C );              /* Flags: continue + close@EOS */
            plen = 12 + put_strz( parm + 12, last_name, 0 );
        }

        put16( w + 0, (unsigned int)plen );          /* TotalParameterCount */
        put16( w + 18, (unsigned int)plen );         /* ParameterCount */
        put16( w + 20, (unsigned int)parmoff );      /* ParameterOffset */
        put16( w + 22, (unsigned int)( parmoff + plen ) ); /* DataOffset */
        put16( bc, (unsigned int)( ( parm + plen ) - ( bc + 2 ) ) ); /* ByteCount */

        cl->stage = first ? "find first" : "find next";
        if ( smb1_txrx( cl, parm + plen ) < 0 ) return -1;
        if ( cl->last_status != 0 ) {
            if ( first ) return -1;                  /* FINDFIRST failed */
            break;                                   /* NEXT: no more (ok) */
        }
        rwc = r_wordcount( cl );
        if ( rwc < 10 ) return -1;
        rp = r_words( cl );
        /* TRANS2 response params/data by their offsets from the SMB header */
        {
            int rparmoff = (int)get16( rp + 8 );
            int rdataoff = (int)get16( rp + 14 );
            const unsigned char *rparm = cl->recvbuf + rparmoff;
            int pcount = (int)get16( rp + 6 );
            dcount = (int)get16( rp + 12 );
            rdata = cl->recvbuf + rdataoff;
            if ( rdataoff + dcount > cl->recvlen ) dcount = cl->recvlen - rdataoff;
            if ( dcount < 0 ) dcount = 0;
            if ( pcount > 0 && rparmoff + pcount > cl->recvlen ) return -1;

            if ( first ) {
                if ( pcount >= 6 ) { sid = get16( rparm + 0 ); eos = (int)get16( rparm + 4 ); }
                first = 0;
            } else {
                if ( pcount >= 4 ) eos = (int)get16( rparm + 2 );
            }
        }

        got = find_walk( rdata, dcount, 1, last_name, emit, user );
        total += got;
        if ( got == 0 ) break;                       /* nothing more came */
    }
    return 0;
}

/* ====================================================================== */
/* open / read / write / close                                            */
/* ====================================================================== */

int smb1_open_disp( smb1_client *cl, const char *path, int write,
                    unsigned long disposition, smb2_fh *fh )
{
    unsigned char *p = smb1_begin( cl, 0x2D );        /* OPEN_ANDX */
    unsigned char *w = p + 1, *bc, *q;
    unsigned int openfn, access;

    /* OpenFunction: bit4 (0x10) create-if-absent; low bits action-if-present
       (1 open, 2 truncate).  Map the SMB2 dispositions the redirector uses. */
    if ( disposition == 5 )       openfn = 0x0012;    /* OVERWRITE_IF: create|trunc */
    else if ( disposition == 3 )  openfn = 0x0011;    /* OPEN_IF: create|open */
    else                          openfn = 0x0001;    /* OPEN: open existing */
    access = write ? 2 : 0;                           /* 0 read, 2 read/write */

    p[0] = 15;                                        /* WordCount */
    w[0] = 0xFF; w[1] = 0; put16( w + 2, 0 );         /* AndX none */
    put16( w + 4, 0 );                                /* Flags */
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
    { unsigned char pb[300]; int n = build_path( pb, path, (const char *)0 ); memcpy( q, pb, n + 1 ); q += n + 1; }
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
    return 0;
}

int smb1_read( smb1_client *cl, const smb2_fh *fh, unsigned long off_lo, unsigned long off_hi,
               unsigned char *buf, int len )
{
    unsigned char *p, *w;
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

int smb1_write( smb1_client *cl, const smb2_fh *fh, unsigned long off_lo, unsigned long off_hi,
                const unsigned char *buf, int len )
{
    unsigned char *p, *w, *bc, *data;
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
    unsigned char *p = smb1_begin( cl, 0x04 );        /* CLOSE */
    unsigned char *w = p + 1;

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
    unsigned char *p = smb1_begin( cl, command );
    unsigned char *w = p + 1, *bc, *q;

    if ( is_delete ) { p[0] = 1; put16( w, 0 ); bc = w + 2; }   /* SearchAttributes=0 */
    else             { p[0] = 0; bc = w; }
    q = bc + 2;
    *q++ = 0x04;                                      /* buffer format: ASCII */
    { unsigned char pb[300]; int n = build_path( pb, path, (const char *)0 ); memcpy( q, pb, n + 1 ); q += n + 1; }
    put16( bc, (unsigned int)( q - ( bc + 2 ) ) );

    cl->stage = "pathop";
    if ( smb1_txrx( cl, q ) < 0 ) return -1;
    return ( cl->last_status == 0 ) ? 0 : -1;
}

int smb1_delete( smb1_client *cl, const char *path ) { return path_cmd( cl, 0x06, 1, path ); }  /* DELETE */
int smb1_mkdir ( smb1_client *cl, const char *path ) { return path_cmd( cl, 0x00, 0, path ); }  /* CREATE_DIRECTORY */
int smb1_rmdir ( smb1_client *cl, const char *path ) { return path_cmd( cl, 0x01, 0, path ); }  /* DELETE_DIRECTORY */

/* RENAME (0x07): WordCount 1 = SearchAttributes, then two ASCII paths in
   the byte block, each 0x04-tagged and NUL-terminated.  The search
   attribute lets a hidden, system or directory entry be renamed too. */
int smb1_rename( smb1_client *cl, const char *oldpath, const char *newpath )
{
    unsigned char *p = smb1_begin( cl, 0x07 );
    unsigned char *w = p + 1, *bc, *q;
    unsigned char pb[300];
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
    unsigned char *p;
    if ( cl->tree_id ) {
        p = smb1_begin( cl, 0x71 );                   /* TREE_DISCONNECT */
        p[0] = 0; put16( p + 1, 0 );
        smb1_txrx( cl, p + 3 );
        cl->tree_id = 0;
    }
}

const char   *smb1_stage( const smb1_client *cl )  { return cl->stage ? cl->stage : "?"; }
unsigned long smb1_status( const smb1_client *cl ) { return cl->last_status; }
