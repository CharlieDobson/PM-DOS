/* ==========================================================================
 * SMBSRV.C - the SMB1 server protocol core.  See SMBSRV.H.
 * ==========================================================================
 * One client at a time, over the send/recv callbacks, with every file
 * operation through the srv_vfs vtable.  The wire formats here are the
 * ones the PM-DOS SMB1 client (SMB1.C) already round-trips against the
 * independent reference peer, so client and server interoperate.  C89,
 * memcpy / memset only (DRVRT on PM-DOS, the CRT on the host harness).
 * ========================================================================== */
#include "smbsrv.h"

void *memcpy( void *to, const void *from, unsigned len );
void *memset( void *to, int value, unsigned len );

static void put16( unsigned char *p, unsigned int v ) { p[0] = (unsigned char)v; p[1] = (unsigned char)( v >> 8 ); }
static void put32( unsigned char *p, unsigned long v ) { p[0] = (unsigned char)v; p[1] = (unsigned char)( v >> 8 ); p[2] = (unsigned char)( v >> 16 ); p[3] = (unsigned char)( v >> 24 ); }
static unsigned int  get16( const unsigned char *p ) { return (unsigned int)p[0] | ( (unsigned int)p[1] << 8 ); }
static unsigned long get32( const unsigned char *p ) { return (unsigned long)p[0] | ( (unsigned long)p[1] << 8 ) | ( (unsigned long)p[2] << 16 ) | ( (unsigned long)p[3] << 24 ); }
static int slen( const char *s ) { int n = 0; while ( s[n] ) n++; return n; }

#define SRV_BUF   33792
#define SRV_FIDS  64

typedef struct {
    smb_send_fn    send;
    smb_recv_fn    recv;
    void          *ioctx;
    const srv_vfs *fs;
    void          *fsctx;
    const char    *share;
    struct { int used; int vh; } fid[SRV_FIDS];
    unsigned char  in[SRV_BUF];
    unsigned char  out[SRV_BUF];
    int            inlen;
} srv_conn;

/* ---- transport ---- */
static int send_all( srv_conn *c, const unsigned char *buf, int len )
{
    int off = 0, n;
    while ( off < len ) { n = c->send( c->ioctx, buf + off, len - off ); if ( n <= 0 ) return -1; off += n; }
    return 0;
}
static int read_n( srv_conn *c, unsigned char *buf, int n )
{
    int off = 0, got;
    while ( off < n ) { got = c->recv( c->ioctx, buf + off, n - off ); if ( got <= 0 ) return -1; off += got; }
    return 0;
}

/* receive one NBT message into c->in; answer a session request on the spot */
static int recv_msg( srv_conn *c )
{
    unsigned char hdr[4];
    int len;
    for ( ;; ) {
        if ( read_n( c, hdr, 4 ) < 0 ) return -1;
        len = ( ( hdr[1] & 1 ) << 16 ) | ( hdr[2] << 8 ) | hdr[3];
        if ( hdr[0] == 0x81 ) {                         /* SESSION REQUEST */
            if ( len > 0 ) { if ( len > SRV_BUF ) return -1; if ( read_n( c, c->in, len ) < 0 ) return -1; }
            { unsigned char ok[4]; ok[0] = 0x82; ok[1] = 0; ok[2] = 0; ok[3] = 0; if ( send_all( c, ok, 4 ) < 0 ) return -1; }
            continue;
        }
        if ( len == 0 ) continue;
        if ( len > SRV_BUF ) return -1;
        if ( read_n( c, c->in, len ) < 0 ) return -1;
        return len;
    }
}

/* build and send a reply: echoes tid/uid/pid/mid; words+bytes already laid out */
static int reply( srv_conn *c, int cmd, unsigned long status,
                  unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid,
                  const unsigned char *words, int wlen,
                  const unsigned char *bytes, int blen )
{
    unsigned char *p = c->out + 4;      /* SMB header start */
    int n;
    memset( c->out, 0, 37 );
    p[0] = 0xFF; p[1] = 'S'; p[2] = 'M'; p[3] = 'B';
    p[4] = (unsigned char)cmd;
    put32( p + 5, status );
    p[9] = 0x88;                        /* Flags: reply + case-insensitive */
    put16( p + 10, 0x4001 );            /* Flags2: LONG_NAMES | NT_STATUS */
    put16( p + 24, tid );
    put16( p + 26, pid );
    put16( p + 28, uid );
    put16( p + 30, mid );
    p[32] = (unsigned char)( wlen / 2 );             /* WordCount */
    if ( wlen ) memcpy( p + 33, words, (unsigned)wlen );
    put16( p + 33 + wlen, (unsigned int)blen );      /* ByteCount */
    if ( blen ) memcpy( p + 33 + wlen + 2, bytes, (unsigned)blen );
    n = 4 + 32 + 1 + wlen + 2 + blen;
    c->out[0] = 0; c->out[1] = 0;
    put16( c->out + 2, 0 );
    c->out[2] = (unsigned char)( ( n - 4 ) >> 8 );
    c->out[3] = (unsigned char)( n - 4 );
    return send_all( c, c->out, n );
}

/* copy a leading-NUL-free ASCIIZ out of a byte field at 'bf' */
static void getz( const unsigned char *bf, int max, char *dst, int cap )
{
    int i = 0;
    while ( i < max && i + 1 < cap && bf[i] ) { dst[i] = (char)bf[i]; i++; }
    dst[i] = 0;
}

/* ---- the per-command handlers ---- */

static void do_negotiate( srv_conn *c, const unsigned char *bytes, int bc,
                          unsigned int pid, unsigned int mid )
{
    unsigned char w[34], b[32];
    int idx = 0, o = 0, i = 0, bn;
    static const unsigned char chal[8] = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88 };

    /* choose NT LM 0.12 from the dialect list */
    while ( o < bc && bytes[o] == 0x02 ) {
        const char *d = (const char *)( bytes + o + 1 );
        int z = o + 1; while ( z < bc && bytes[z] ) z++;
        if ( slen( d ) == 10 &&
             d[0]=='N'&&d[1]=='T'&&d[2]==' '&&d[3]=='L'&&d[4]=='M'&&d[5]==' '&&d[6]=='0'&&d[7]=='.'&&d[8]=='1'&&d[9]=='2' )
            idx = i;
        i++; o = z + 1;
    }
    memset( w, 0, sizeof( w ) );
    put16( w + 0, (unsigned int)idx );           /* DialectIndex */
    w[2] = 0x03;                                 /* SecurityMode: user + challenge */
    put16( w + 3, 1 );                           /* MaxMpxCount */
    put16( w + 5, 1 );                           /* MaxNumberVcs */
    put32( w + 7, 16644 );                       /* MaxBufferSize */
    put32( w + 11, 65535 );                      /* MaxRawSize */
    put32( w + 15, 0 );                          /* SessionKey */
    put32( w + 19, 0x10UL | 0x40UL | 0x08UL );   /* Caps: NT_SMBS|NT_STATUS|LARGE_FILES */
    put32( w + 23, 0 ); put32( w + 27, 0 );      /* SystemTime (0) */
    put16( w + 31, 0 );                          /* ServerTimeZone */
    w[33] = 8;                                   /* EncryptionKeyLength */
    memcpy( b, chal, 8 );
    bn = 8;
    { static const char *dom = "WORKGROUP"; int k; for ( k = 0; dom[k]; k++ ) b[bn++] = (unsigned char)dom[k]; b[bn++] = 0; }
    reply( c, 0x72, 0, 0, 0, pid, mid, w, 34, b, bn );
}

static int alloc_fid( srv_conn *c, int vh )
{
    int i;
    for ( i = 0; i < SRV_FIDS; i++ ) if ( !c->fid[i].used ) { c->fid[i].used = 1; c->fid[i].vh = vh; return i + 1; }
    return 0;
}

static void do_open( srv_conn *c, const unsigned char *words, const unsigned char *bytes, int bc,
                     unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    unsigned int access = get16( words + 6 );
    unsigned int openfn = get16( words + 16 );
    char path[300];
    unsigned long size = 0;
    int action = 1, dispo, write, vh, fid;
    unsigned char w[30];

    getz( bytes + 1, bc - 1, path, sizeof( path ) );        /* skip the 0x00 format byte */
    if ( openfn & 0x10 ) dispo = ( ( openfn & 0x03 ) == 2 ) ? 5 : 3;  /* create (+trunc) */
    else                 dispo = 1;                          /* open existing */
    write = ( access & 0x7 ) ? 1 : 0;

    vh = c->fs->vopen( c->fsctx, path, write, dispo, &size, &action );
    if ( vh < 0 ) { reply( c, 0x2D, 0xC0000034UL, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 ); return; }
    fid = alloc_fid( c, vh );
    if ( fid == 0 ) { c->fs->vclose( c->fsctx, vh ); reply( c, 0x2D, 0xC0000034UL, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 ); return; }

    memset( w, 0, sizeof( w ) );
    w[0] = 0xFF;                                 /* AndXCommand none */
    put16( w + 4, (unsigned int)fid );           /* FID */
    put16( w + 6, 0x20 );                        /* FileAttributes = archive */
    put32( w + 8, 0 );                           /* LastWriteTime */
    put32( w + 12, size );                       /* DataSize */
    put16( w + 16, access );                     /* GrantedAccess */
    put16( w + 22, (unsigned int)action );       /* Action */
    reply( c, 0x2D, 0, tid, uid, pid, mid, w, 30, (unsigned char *)0, 0 );
}

static void do_read( srv_conn *c, const unsigned char *words,
                     unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    unsigned int fid = get16( words + 4 );
    unsigned long off = get32( words + 6 );      /* low 32 (files here fit) */
    int maxc = (int)get16( words + 10 );
    unsigned char *p = c->out + 4;
    int n, total;

    if ( fid < 1 || fid > SRV_FIDS || !c->fid[fid - 1].used ) { reply( c, 0x2E, 0xC0000008UL, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 ); return; }
    if ( maxc > SRV_BUF - 128 ) maxc = SRV_BUF - 128;

    /* lay out the reply and read straight into the data offset (60), so
       the payload needs no copy: hdr 32 + wc 1 + words 24 + bc 2 + pad 1 */
    memset( p, 0, 60 );
    p[0] = 0xFF; p[1] = 'S'; p[2] = 'M'; p[3] = 'B';
    p[4] = 0x2E; p[9] = 0x88; put16( p + 10, 0x4001 );
    put16( p + 24, tid ); put16( p + 28, uid ); put16( p + 30, mid );
    p[32] = 12;                                  /* WordCount */
    p[33] = 0xFF;                                /* AndXCommand none */

    n = c->fs->vread( c->fsctx, c->fid[fid - 1].vh, off, p + 60, maxc );
    if ( n < 0 ) n = 0;

    put16( p + 43, (unsigned int)( n & 0xFFFF ) );         /* DataLength (words+10) */
    put16( p + 45, 60 );                                   /* DataOffset (words+12) */
    put16( p + 47, (unsigned int)( ( n >> 16 ) & 0xFFFF ) );/* DataLengthHigh (words+14) */
    put16( p + 57, (unsigned int)( n + 1 ) );              /* ByteCount = pad + data */

    total = 4 + 60 + n;
    c->out[0] = 0; c->out[1] = 0;
    c->out[2] = (unsigned char)( ( total - 4 ) >> 8 );
    c->out[3] = (unsigned char)( total - 4 );
    send_all( c, c->out, total );
}

static void do_write( srv_conn *c, const unsigned char *words, const unsigned char *smb,
                      unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    unsigned int fid = get16( words + 4 );
    unsigned long off = get32( words + 6 );
    int dlen = (int)get16( words + 20 ) | ( (int)get16( words + 18 ) << 16 );
    int doff = (int)get16( words + 22 );
    unsigned char w[12];
    int n;

    if ( fid < 1 || fid > SRV_FIDS || !c->fid[fid - 1].used ) { reply( c, 0x2F, 0xC0000008UL, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 ); return; }
    if ( doff < 32 || doff > c->inlen ) { doff = c->inlen; dlen = 0; }   /* untrusted offset */
    if ( doff + dlen > c->inlen ) dlen = c->inlen - doff;
    if ( dlen < 0 ) dlen = 0;
    n = c->fs->vwrite( c->fsctx, c->fid[fid - 1].vh, off, smb + doff, dlen );
    if ( n < 0 ) n = 0;

    memset( w, 0, sizeof( w ) );
    w[0] = 0xFF;
    put16( w + 4, (unsigned int)( n & 0xFFFF ) );    /* Count */
    reply( c, 0x2F, 0, tid, uid, pid, mid, w, 12, (unsigned char *)0, 0 );
}

static void send_trans2( srv_conn *c, unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid,
                         const unsigned char *parm, int plen, const unsigned char *data, int dlen )
{
    unsigned char w[20];
    unsigned char *bp;
    int base = 32 + 1 + 20 + 2, ppad, poff, afterp, dpad, doff, blen;

    ppad = ( -base ) & 3; poff = base + ppad;
    afterp = poff + plen;
    dpad = ( -afterp ) & 3; doff = afterp + dpad;

    memset( w, 0, sizeof( w ) );
    put16( w + 0, (unsigned int)plen );          /* TotalParameterCount */
    put16( w + 2, (unsigned int)dlen );          /* TotalDataCount */
    put16( w + 6, (unsigned int)plen );          /* ParameterCount */
    put16( w + 8, (unsigned int)poff );          /* ParameterOffset */
    put16( w + 12, (unsigned int)dlen );         /* DataCount */
    put16( w + 14, (unsigned int)doff );         /* DataOffset */

    blen = ppad + plen + dpad + dlen;
    bp = c->out + 4 + 33 + 20 + 2;               /* where bytes go (after wc+words+bc) */
    memset( bp, 0, ppad );
    memcpy( bp + ppad, parm, (unsigned)plen );
    memset( bp + ppad + plen, 0, dpad );
    memcpy( bp + ppad + plen + dpad, data, (unsigned)dlen );
    reply( c, 0x32, 0, tid, uid, pid, mid, w, 20, bp, blen );
}

/* list a directory into 'data'; return the byte length, entries in *cnt */
static int build_listing( srv_conn *c, const char *dir, unsigned char *data, int cap, int *cnt )
{
    void *it = c->fs->vfindopen( c->fsctx, dir );
    int off = 0, count = 0, last = 0;
    srv_dirent de;
    if ( it == (void *)0 ) { *cnt = -1; return 0; }
    while ( c->fs->vfindnext( c->fsctx, it, &de ) ) {
        int namelen = slen( de.name ), entry, pad;
        entry = 64 + namelen + 1;
        pad = ( -entry ) & 3;
        if ( off + entry + pad > cap ) break;
        last = off;
        memset( data + off, 0, 64 );
        put32( data + off + 8,  de.ft_lo ); put32( data + off + 12, de.ft_hi );  /* Creation */
        put32( data + off + 16, de.ft_lo ); put32( data + off + 20, de.ft_hi );  /* Access */
        put32( data + off + 24, de.ft_lo ); put32( data + off + 28, de.ft_hi );  /* Write */
        put32( data + off + 32, de.ft_lo ); put32( data + off + 36, de.ft_hi );  /* Change */
        put32( data + off + 40, de.size ); put32( data + off + 44, 0 );          /* EndOfFile */
        put32( data + off + 48, ( de.size + 511 ) & ~511UL ); put32( data + off + 52, 0 );
        put32( data + off + 56, de.attr );                                       /* ExtFileAttributes */
        put32( data + off + 60, (unsigned long)namelen );                        /* FileNameLength */
        memcpy( data + off + 64, de.name, (unsigned)( namelen + 1 ) );
        memset( data + off + 64 + namelen + 1, 0, (unsigned)pad );
        put32( data + off, (unsigned long)( entry + pad ) );                     /* NextEntryOffset */
        off += entry + pad;
        count++;
    }
    c->fs->vfindclose( c->fsctx, it );
    if ( count > 0 ) put32( data + last, 0 );     /* last entry: NextEntryOffset = 0 */
    *cnt = count;
    return off;
}

static void do_trans2( srv_conn *c, const unsigned char *words, const unsigned char *smb,
                       unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    int poff = (int)get16( words + 20 );
    int pcnt = (int)get16( words + 18 );
    int setup = (int)get16( words + 28 );
    const unsigned char *parm = smb + poff;

    /* the client's offsets are untrusted: keep the parameter block inside
       the received message */
    if ( poff < 32 || pcnt < 0 || poff + pcnt > c->inlen ) {
        reply( c, 0x32, 0xC0000002UL, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 );
        return;
    }

    if ( setup == 1 ) {                           /* FIND_FIRST2 */
        char spec[300], dir[300];
        int i, cut = -1, cnt;
        unsigned char rparm[10];
        static unsigned char data[SRV_BUF - 512];
        int dlen;
        getz( parm + 12, pcnt - 12, spec, sizeof( spec ) );
        for ( i = 0; spec[i]; i++ ) if ( spec[i] == '\\' || spec[i] == '/' ) cut = i;
        if ( cut <= 0 ) { dir[0] = '\\'; dir[1] = 0; }
        else { for ( i = 0; i < cut; i++ ) dir[i] = spec[i]; dir[i] = 0; }
        dlen = build_listing( c, dir, data, (int)sizeof( data ), &cnt );
        if ( cnt < 0 ) { reply( c, 0x32, 0xC000003AUL, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 ); return; }
        put16( rparm + 0, 1 );                    /* SID */
        put16( rparm + 2, (unsigned int)cnt );    /* SearchCount */
        put16( rparm + 4, 1 );                    /* EndOfSearch */
        put16( rparm + 6, 0 );
        put16( rparm + 8, 0 );
        send_trans2( c, tid, uid, pid, mid, rparm, 10, data, dlen );
    } else if ( setup == 2 ) {                    /* FIND_NEXT2: nothing more */
        unsigned char rparm[8];
        put16( rparm + 0, 0 ); put16( rparm + 2, 1 ); put16( rparm + 4, 0 ); put16( rparm + 6, 0 );
        send_trans2( c, tid, uid, pid, mid, rparm, 8, (unsigned char *)0, 0 );
    } else {
        reply( c, 0x32, 0xC0000002UL, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 );
    }
}

static void do_pathop( srv_conn *c, int cmd, const unsigned char *words, const unsigned char *bytes, int bc,
                       unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    char path[300];
    int rc, skip = ( cmd == 0x06 ) ? 1 : 1;       /* all three: 0x04 format byte, then name */
    (void)words;
    getz( bytes + skip, bc - skip, path, sizeof( path ) );
    if      ( cmd == 0x06 ) rc = c->fs->vunlink( c->fsctx, path );
    else if ( cmd == 0x00 ) rc = c->fs->vmkdir( c->fsctx, path );
    else                    rc = c->fs->vrmdir( c->fsctx, path );
    reply( c, cmd, rc == 0 ? 0 : 0xC0000034UL, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 );
}

void smbsrv_serve( smb_send_fn send, smb_recv_fn recv, void *ioctx,
                   const srv_vfs *fs, void *fsctx, const char *sharename )
{
    static srv_conn conn;
    srv_conn *c = &conn;
    memset( c, 0, sizeof( *c ) );
    c->send = send; c->recv = recv; c->ioctx = ioctx; c->fs = fs; c->fsctx = fsctx;
    c->share = sharename;

    for ( ;; ) {
        const unsigned char *smb, *words, *bytes;
        int wc, bc, cmd;
        unsigned int tid, pid, uid, mid;

        c->inlen = recv_msg( c );
        if ( c->inlen < 33 ) return;
        smb = c->in;
        if ( smb[0] != 0xFF || smb[1] != 'S' || smb[2] != 'M' || smb[3] != 'B' ) return;
        cmd = smb[4];
        tid = get16( smb + 24 ); pid = get16( smb + 26 ); uid = get16( smb + 28 ); mid = get16( smb + 30 );
        wc  = smb[32];
        words = smb + 33;
        bc  = (int)get16( smb + 33 + wc * 2 );
        bytes = smb + 33 + wc * 2 + 2;

        switch ( cmd ) {
        case 0x72: do_negotiate( c, bytes, bc, pid, mid ); break;
        case 0x73: {   /* SESSION_SETUP_ANDX: accept any */
            unsigned char w[6]; unsigned char b[32]; int bn = 0; const char *s;
            memset( w, 0, sizeof( w ) ); w[0] = 0xFF;
            s = "Unix"; { int k; for ( k = 0; s[k]; k++ ) b[bn++] = (unsigned char)s[k]; } b[bn++] = 0;
            s = "PMDOS"; { int k; for ( k = 0; s[k]; k++ ) b[bn++] = (unsigned char)s[k]; } b[bn++] = 0;
            s = "WORKGROUP"; { int k; for ( k = 0; s[k]; k++ ) b[bn++] = (unsigned char)s[k]; } b[bn++] = 0;
            reply( c, 0x73, 0, tid, 100, pid, mid, w, 6, b, bn );
            break; }
        case 0x75: {   /* TREE_CONNECT_ANDX */
            unsigned char w[6]; unsigned char b[16]; int bn = 0;
            memset( w, 0, sizeof( w ) ); w[0] = 0xFF; put16( w + 4, 1 );   /* OptionalSupport */
            b[bn++] = 'A'; b[bn++] = ':'; b[bn++] = 0;
            b[bn++] = 'N'; b[bn++] = 'T'; b[bn++] = 'F'; b[bn++] = 'S'; b[bn++] = 0;
            reply( c, 0x75, 0, 1, uid, pid, mid, w, 6, b, bn );
            break; }
        case 0x71: reply( c, 0x71, 0, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 ); break;
        case 0x74: { unsigned char w[6]; memset( w, 0, sizeof( w ) ); w[0] = 0xFF; reply( c, 0x74, 0, tid, uid, pid, mid, w, 6, (unsigned char *)0, 0 ); break; }
        case 0x2B: reply( c, 0x2B, 0, tid, uid, pid, mid, (unsigned char *)words, 2, (unsigned char *)bytes, bc ); break;
        case 0x2D: do_open( c, words, bytes, bc, tid, uid, pid, mid ); break;
        case 0x2E: do_read( c, words, tid, uid, pid, mid ); break;
        case 0x2F: do_write( c, words, smb, tid, uid, pid, mid ); break;
        case 0x04: { unsigned int fid = get16( words ); if ( fid >= 1 && fid <= SRV_FIDS && c->fid[fid - 1].used ) { c->fs->vclose( c->fsctx, c->fid[fid - 1].vh ); c->fid[fid - 1].used = 0; } reply( c, 0x04, 0, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 ); break; }
        case 0x06: case 0x00: case 0x01: do_pathop( c, cmd, words, bytes, bc, tid, uid, pid, mid ); break;
        case 0x32: do_trans2( c, words, smb, tid, uid, pid, mid ); break;
        default:   reply( c, cmd, 0xC0000002UL, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 ); break;
        }
    }
}
