/* ==========================================================================
 * SMBSRV.C - the SMB1 server protocol core.  See SMBSRV.H.
 * ==========================================================================
 * One client at a time, over the send/recv callbacks, with every file
 * operation through the srv_vfs vtable.  The wire formats here are the
 * ones the PM-DOS SMB1 client (SMB1.C) already round-trips against the
 * independent reference peer, so client and server interoperate.  C89,
 * memcpy / memset only (DRVRT on PM-DOS, the CRT on the host harness).
 *
 * A client that offers no "NT LM 0.12" (Windows for Workgroups 3.11, the
 * LAN Manager 2.x redirectors) gets the newest LAN Manager dialect it
 * offers: the 13-word NEGOTIATE answer, Flags2 0 and the DOS error class
 * and code in place of NT status.  Its directory listings come at info
 * level 1 (8.3 names, DOS dates), cut to its buffer and resumed by key.
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
#define SRV_MAXBUF 16644            /* the MaxBufferSize we announce */

typedef struct {
    smb_send_fn    send;
    smb_recv_fn    recv;
    void          *ioctx;
    const srv_vfs *fs;
    void          *fsctx;
    const char    *share;
    struct { int used; int vh; unsigned long pos; } fid[SRV_FIDS];
    int            lm;                  /* a LAN Manager dialect was chosen */
    int            cli_max;             /* the client's MaxBufferSize */
    /* the one search FIND_NEXT2 continues: its directory, pattern
       (11-byte FCB form, or all), search attributes and how many
       matching entries have gone */
    char           sdir[300];
    unsigned char  spat[11];
    int            sall;
    unsigned int   sattr;
    int            ssent;
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

/* an NT status as a LAN Manager client reads it: the DOS error class in
   the low byte, the code in the high word */
static unsigned long dos_status( unsigned long nt )
{
    unsigned long cls = 1, code;
    switch ( nt ) {
    case 0:            return 0;
    case 0xC0000034UL: code = 2;   break;      /* file not found */
    case 0xC000003AUL: code = 3;   break;      /* path not found */
    case 0xC0000008UL: code = 6;   break;      /* bad handle */
    case 0xC0000022UL: code = 5;   break;      /* access denied */
    case 0xC0000035UL: code = 80;  break;      /* the file exists */
    case 0xC0000033UL: code = 123; break;      /* a bad name */
    case 0x80000006UL: code = 18;  break;      /* no more files */
    case 0xC0000002UL: code = 1;   break;      /* not done here */
    default:           cls = 2; code = 1; break;   /* ERRSRV: an error */
    }
    return cls | ( code << 16 );
}

/* build and send a reply: echoes tid/uid/pid/mid; words+bytes already laid out */
static int reply( srv_conn *c, int cmd, unsigned long status,
                  unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid,
                  const unsigned char *words, int wlen,
                  const unsigned char *bytes, int blen )
{
    unsigned char *p = c->out + 4;      /* SMB header start */
    int n;
    if ( wlen < 0 || blen < 0 || wlen > 510 || 4 + 33 + wlen + 2 + blen > SRV_BUF ) return -1;
    memset( c->out, 0, 37 );
    p[0] = 0xFF; p[1] = 'S'; p[2] = 'M'; p[3] = 'B';
    p[4] = (unsigned char)cmd;
    put32( p + 5, c->lm ? dos_status( status ) : status );
    p[9] = 0x88;                        /* Flags: reply + case-insensitive */
    put16( p + 10, c->lm ? 0 : 0x4001 );   /* Flags2: LONG_NAMES | NT_STATUS */
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

/* a reply with no words and no bytes: an error, or a bare "done" */
static void reply0( srv_conn *c, int cmd, unsigned long status,
                    unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    reply( c, cmd, status, tid, uid, pid, mid, (unsigned char *)0, 0, (unsigned char *)0, 0 );
}

/* copy a leading-NUL-free ASCIIZ out of a byte field at 'bf'; -1 if the
   name does not fit 'cap' */
static int getz( const unsigned char *bf, int max, char *dst, int cap )
{
    int i = 0;
    while ( i < max && i + 1 < cap && bf[i] ) { dst[i] = (char)bf[i]; i++; }
    dst[i] = 0;
    if ( i < max && i + 1 >= cap && bf[i] ) return -1;
    return 0;
}

/* a path in the byte field after its 0x04 buffer-format byte */
static int getpath( const unsigned char *bytes, int bc, char *dst, int cap )
{
    if ( bc < 1 ) { dst[0] = 0; return -1; }
    return getz( bytes + 1, bc - 1, dst, cap );
}

/* ---- dates: DOS date/time <-> seconds since 1970 (UTIME) ---- */

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
static unsigned long dos_utime( unsigned int date, unsigned int time )
{
    int month = (int)( ( date >> 5 ) & 0x0F ), day = (int)( date & 0x1F );
    if ( date == 0 ) return 0;
    if ( month < 1 ) month = 1;
    if ( month > 12 ) month = 12;
    if ( day < 1 ) day = 1;
    return (unsigned long)days_from_civil( 1980 + (int)( ( date >> 9 ) & 0x7F ), month, day ) * 86400UL +
           ( ( time >> 11 ) & 0x1F ) * 3600UL + ( ( time >> 5 ) & 0x3F ) * 60UL + ( time & 0x1F ) * 2UL;
}
static void utime_dos( unsigned long t, unsigned int *date, unsigned int *time )
{
    long z = (long)( t / 86400UL ) + 719468L, era, doe, yoe, year, doy, mp, day, month;
    unsigned long sod = t % 86400UL;
    era = z / 146097;                                   /* t >= 0: z > 0 */
    doe = z - era * 146097;
    yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
    year = yoe + era * 400;
    doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
    mp = ( 5 * doy + 2 ) / 153;
    day = doy - ( 153 * mp + 2 ) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year += ( month <= 2 );
    if ( year < 1980 ) { year = 1980; month = 1; day = 1; sod = 0; }
    if ( year > 2107 ) { year = 2107; month = 12; day = 31; sod = 86399UL; }
    *date = (unsigned int)( ( ( year - 1980 ) << 9 ) | ( month << 5 ) | day );
    *time = (unsigned int)( ( ( sod / 3600 ) << 11 ) | ( ( ( sod % 3600 ) / 60 ) << 5 ) | ( ( sod % 60 ) / 2 ) );
}

/* ---- the DOS wildcard match a DOS redirector expects of the server ---- */

static char supper( char ch ) { return ( ch >= 'a' && ch <= 'z' ) ? (char)( ch - 32 ) : ch; }

/* an 8.3 name -> its 11-byte FCB form ("." and ".." kept as they are) */
static void name_fcb( const char *name, unsigned char *fcb )
{
    int i, pos = 0;
    const char *dot = (const char *)0, *p;
    for ( i = 0; i < 11; i++ ) fcb[i] = ' ';
    if ( name[0] == '.' ) {                             /* "." or ".." */
        for ( i = 0; i < 2 && name[i] == '.'; i++ ) fcb[i] = '.';
        return;
    }
    for ( p = name; *p; p++ ) if ( *p == '.' ) dot = p;
    for ( p = name; *p && p != dot && pos < 8; p++ ) fcb[pos++] = (unsigned char)supper( *p );
    if ( dot ) { pos = 8; for ( p = dot + 1; *p && pos < 11; p++ ) fcb[pos++] = (unsigned char)supper( *p ); }
}

/* a wildcard pattern -> its FCB form ('*' fills with '?'); returns 1 for
   a pattern that matches every name ("*", "*.*", empty) */
static int pat_fcb( const char *pat, unsigned char *fcb )
{
    int i, pos = 0;
    const char *p = pat;
    if ( pat[0] == 0 || ( pat[0] == '*' && pat[1] == 0 ) ) return 1;
    for ( i = 0; i < 11; i++ ) fcb[i] = ' ';
    while ( *p && *p != '.' ) {
        if ( *p == '*' ) { while ( pos < 8 ) fcb[pos++] = '?'; while ( *p && *p != '.' ) p++; break; }
        if ( pos < 8 ) fcb[pos++] = (unsigned char)supper( *p );
        p++;
    }
    if ( *p == '.' ) p++;
    pos = 8;
    while ( *p ) {
        if ( *p == '*' ) { while ( pos < 11 ) fcb[pos++] = '?'; break; }
        if ( pos < 11 ) fcb[pos++] = (unsigned char)supper( *p );
        p++;
    }
    return 0;
}

static int match_entry( const unsigned char *pat, int all, unsigned int sattr, const srv_dirent *de )
{
    unsigned char fcb[11];
    int i;
    if ( de->attr & 0x16UL & ~(unsigned long)sattr ) return 0;      /* H, S, D not asked for */
    if ( all ) return 1;
    name_fcb( de->name, fcb );
    for ( i = 0; i < 11; i++ ) if ( pat[i] != '?' && pat[i] != fcb[i] ) return 0;
    return 1;
}
static int srch_match( srv_conn *c, const srv_dirent *de )
{
    return match_entry( c->spat, c->sall, c->sattr, de );
}

/* ---- the per-command handlers ---- */

/* the LAN Manager dialects we answer to, oldest first */
static const char *const lm_dialects[] = {
    "LM1.2X002", "DOS LM1.2X002", "DOS LANMAN2.1", "LANMAN2.1", "Windows for Workgroups 3.1a"
};
#define LM_DIALECTS 5

static int streq( const unsigned char *a, int alen, const char *b )
{
    int i;
    for ( i = 0; i < alen && b[i]; i++ ) if ( a[i] != (unsigned char)b[i] ) return 0;
    return i == alen && b[i] == 0;
}

static void do_negotiate( srv_conn *c, const unsigned char *bytes, int bc,
                          unsigned int pid, unsigned int mid )
{
    unsigned char w[34], b[32];
    int nt = -1, lm = -1, o = 0, i = 0, k, bn;
    static const unsigned char chal[8] = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88 };

    /* NT LM 0.12 when offered, else the newest LAN Manager dialect */
    while ( o < bc && bytes[o] == 0x02 ) {
        int z = o + 1;
        while ( z < bc && bytes[z] ) z++;
        if ( streq( bytes + o + 1, z - o - 1, "NT LM 0.12" ) ) nt = i;
        for ( k = 0; k < LM_DIALECTS; k++ ) if ( streq( bytes + o + 1, z - o - 1, lm_dialects[k] ) ) lm = i;
        i++; o = z + 1;
    }
    memcpy( b, chal, 8 );
    bn = 8;
    memset( w, 0, sizeof( w ) );
    if ( nt >= 0 ) {
        { static const char *dom = "WORKGROUP"; int n; for ( n = 0; dom[n]; n++ ) b[bn++] = (unsigned char)dom[n]; b[bn++] = 0; }
        put16( w + 0, (unsigned int)nt );            /* DialectIndex */
        w[2] = 0x03;                                 /* SecurityMode: user + challenge */
        put16( w + 3, 1 );                           /* MaxMpxCount */
        put16( w + 5, 1 );                           /* MaxNumberVcs */
        put32( w + 7, SRV_MAXBUF );                  /* MaxBufferSize */
        put32( w + 11, 65535 );                      /* MaxRawSize */
        put32( w + 15, 0 );                          /* SessionKey */
        put32( w + 19, 0x10UL | 0x40UL | 0x08UL );   /* Caps: NT_SMBS|NT_STATUS|LARGE_FILES */
        put32( w + 23, 0 ); put32( w + 27, 0 );      /* SystemTime (0) */
        put16( w + 31, 0 );                          /* ServerTimeZone */
        w[33] = 8;                                   /* EncryptionKeyLength */
        reply( c, 0x72, 0, 0, 0, pid, mid, w, 34, b, bn );
    } else if ( lm >= 0 ) {
        c->lm = 1;
        put16( w + 0, (unsigned int)lm );            /* DialectIndex */
        put16( w + 2, 0x0003 );                      /* SecurityMode: user + challenge */
        put16( w + 4, SRV_MAXBUF );                  /* MaxBufferSize */
        put16( w + 6, 1 );                           /* MaxMpxCount */
        put16( w + 8, 1 );                           /* MaxNumberVcs */
        put16( w + 10, 0 );                          /* RawMode: none */
        put32( w + 12, 0 );                          /* SessionKey */
        put16( w + 18, 0x0021 );                     /* ServerDate: 1980-01-01; time, zone 0 */
        put16( w + 22, 8 );                          /* EncryptionKeyLength */
        reply( c, 0x72, 0, 0, 0, pid, mid, w, 26, b, bn );   /* the key alone, as WfW's own */
    } else {
        put16( w + 0, 0xFFFF );                      /* none of them */
        reply( c, 0x72, 0, 0, 0, pid, mid, w, 2, (unsigned char *)0, 0 );
    }
}

static int alloc_fid( srv_conn *c, int vh )
{
    int i;
    for ( i = 0; i < SRV_FIDS; i++ ) if ( !c->fid[i].used ) { c->fid[i].used = 1; c->fid[i].vh = vh; c->fid[i].pos = 0; return i + 1; }
    return 0;
}

/* the open file a FID word names, or -1 */
static int fid_index( srv_conn *c, unsigned int fid )
{
    if ( fid < 1 || fid > SRV_FIDS || !c->fid[fid - 1].used ) return -1;
    return (int)fid - 1;
}

/* close what the client left open */
static void close_all( srv_conn *c )
{
    int i;
    for ( i = 0; i < SRV_FIDS; i++ ) if ( c->fid[i].used ) { c->fs->vclose( c->fsctx, c->fid[i].vh ); c->fid[i].used = 0; }
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
    srv_dirent st;

    if ( bc > 1 && bytes[0] == 0 ) { bytes++; bc--; }   /* a pad byte, as SMB1.C sent until the LAN Manager work */
    if ( getz( bytes, bc, path, sizeof( path ) ) < 0 ) {
        reply0( c, 0x2D, 0xC0000033UL, tid, uid, pid, mid );
        return;
    }
    if ( openfn & 0x10 ) dispo = ( ( openfn & 0x03 ) == 2 ) ? 5 : 3;  /* create (+trunc) */
    else                 dispo = 1;                          /* open existing */
    write = ( access & 0x7 ) ? 1 : 0;

    vh = c->fs->vopen( c->fsctx, path, write, dispo, &size, &action );
    if ( vh < 0 ) { reply0( c, 0x2D, 0xC0000034UL, tid, uid, pid, mid ); return; }
    fid = alloc_fid( c, vh );
    if ( fid == 0 ) { c->fs->vclose( c->fsctx, vh ); reply0( c, 0x2D, 0xC0000034UL, tid, uid, pid, mid ); return; }

    memset( w, 0, sizeof( w ) );
    w[0] = 0xFF;                                 /* AndXCommand none */
    put16( w + 4, (unsigned int)fid );           /* FID */
    put16( w + 6, 0x20 );                        /* FileAttributes = archive */
    if ( c->fs->vfstat( c->fsctx, vh, &st ) == 0 )
        put32( w + 8, dos_utime( st.dosdate, st.dostime ) );   /* LastWriteTime */
    put32( w + 12, size );                       /* DataSize */
    put16( w + 16, access & 0x7 );               /* GrantedAccess */
    put16( w + 22, (unsigned int)action );       /* Action */
    reply( c, 0x2D, 0, tid, uid, pid, mid, w, 30, (unsigned char *)0, 0 );
}

static void do_read( srv_conn *c, const unsigned char *words,
                     unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    int fx = fid_index( c, get16( words + 4 ) );
    unsigned long off = get32( words + 6 );      /* low 32 (files here fit) */
    int maxc = (int)get16( words + 10 );
    unsigned char *p = c->out + 4;
    int n, total;

    if ( fx < 0 ) { reply0( c, 0x2E, 0xC0000008UL, tid, uid, pid, mid ); return; }
    if ( maxc > SRV_BUF - 128 ) maxc = SRV_BUF - 128;

    /* lay out the reply and read straight into the data offset (60), so
       the payload needs no copy: hdr 32 + wc 1 + words 24 + bc 2 + pad 1 */
    memset( p, 0, 60 );
    p[0] = 0xFF; p[1] = 'S'; p[2] = 'M'; p[3] = 'B';
    p[4] = 0x2E; p[9] = 0x88; put16( p + 10, c->lm ? 0 : 0x4001 );
    put16( p + 24, tid ); put16( p + 26, pid ); put16( p + 28, uid ); put16( p + 30, mid );
    p[32] = 12;                                  /* WordCount */
    p[33] = 0xFF;                                /* AndXCommand none */

    n = c->fs->vread( c->fsctx, c->fid[fx].vh, off, p + 60, maxc );
    if ( n < 0 ) n = 0;
    c->fid[fx].pos = off + (unsigned long)n;

    put16( p + 43, (unsigned int)( n & 0xFFFF ) );         /* DataLength (words+10) */
    put16( p + 45, 60 );                                   /* DataOffset (words+12) */
    if ( !c->lm ) put16( p + 47, (unsigned int)( ( n >> 16 ) & 0xFFFF ) );   /* DataLengthHigh */
    put16( p + 57, (unsigned int)( n + 1 ) );              /* ByteCount = pad + data */

    total = 4 + 60 + n;
    c->out[0] = 0; c->out[1] = (unsigned char)( ( ( total - 4 ) >> 16 ) & 1 );
    c->out[2] = (unsigned char)( ( total - 4 ) >> 8 );
    c->out[3] = (unsigned char)( total - 4 );
    send_all( c, c->out, total );
}

static void do_write( srv_conn *c, const unsigned char *words, int wc, const unsigned char *smb,
                      unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    int fx = fid_index( c, get16( words + 4 ) );
    unsigned long off = get32( words + 6 );
    int dlen = (int)get16( words + 20 ) | ( (int)get16( words + 18 ) << 16 );
    int doff = (int)get16( words + 22 );
    unsigned char w[12];
    int n;

    if ( fx < 0 ) { reply0( c, 0x2F, 0xC0000008UL, tid, uid, pid, mid ); return; }
    if ( wc < 14 ) dlen &= 0xFFFF;               /* LAN Manager: words+18 is reserved */
    if ( doff < 32 || doff > c->inlen ) { doff = c->inlen; dlen = 0; }   /* untrusted offset */
    if ( dlen < 0 ) dlen = 0;
    if ( dlen > c->inlen - doff ) dlen = c->inlen - doff;
    n = c->fs->vwrite( c->fsctx, c->fid[fx].vh, off, smb + doff, dlen );
    if ( n < 0 ) n = 0;
    c->fid[fx].pos = off + (unsigned long)n;

    memset( w, 0, sizeof( w ) );
    w[0] = 0xFF;
    put16( w + 4, (unsigned int)( n & 0xFFFF ) );    /* Count */
    reply( c, 0x2F, 0, tid, uid, pid, mid, w, 12, (unsigned char *)0, 0 );
}

/* CLOSE: a LastWriteTime other than 0 and -1 is set on the file first */
static void do_close( srv_conn *c, const unsigned char *words, int wc,
                      unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    int fx = fid_index( c, get16( words ) );
    if ( fx >= 0 ) {
        unsigned long t = ( wc >= 3 ) ? get32( words + 2 ) : 0;
        if ( t != 0 && t != 0xFFFFFFFFUL ) {
            unsigned int date, time;
            utime_dos( t, &date, &time );
            c->fs->vsettime( c->fsctx, c->fid[fx].vh, date, time );
        }
        c->fs->vclose( c->fsctx, c->fid[fx].vh );
        c->fid[fx].used = 0;
    }
    reply0( c, 0x04, 0, tid, uid, pid, mid );
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

/* list the search's directory into 'data' from its c->ssent'th match on:
   at most 'maxcount' entries and 'cap' bytes, at info level 1 (with the
   resume key when 'keys') or 0x101.  Returns the byte length; *cnt is the
   count (-1 when the directory is not there), *eos 1 when the directory
   ran out, *lastname the last entry's name offset. */
static int build_listing( srv_conn *c, int level, int keys, unsigned char *data, int cap,
                          int maxcount, int *cnt, int *eos, int *lastname )
{
    void *it = c->fs->vfindopen( c->fsctx, c->sdir );
    int off = 0, count = 0, last = -1, skip = c->ssent;
    srv_dirent de;

    *eos = 1; *lastname = 0;
    if ( it == (void *)0 ) { *cnt = -1; return 0; }
    while ( c->fs->vfindnext( c->fsctx, it, &de ) ) {
        int namelen, entry, pad = 0, at;
        namelen = slen( de.name );
        if ( !srch_match( c, &de ) ) continue;
        if ( level == 1 && namelen > 255 ) continue;     /* level 1 counts it in a byte;
                                                            before the skip, so a resume
                                                            counts the same entries */
        if ( skip > 0 ) { skip--; continue; }
        if ( count >= maxcount ) { *eos = 0; break; }
        if ( level == 1 ) {
            entry = ( keys ? 27 : 23 ) + namelen + 1;
        } else {
            entry = 64 + namelen + 1;
            pad = ( -entry ) & 3;
        }
        if ( off + entry + pad > cap ) { *eos = 0; break; }
        at = off;
        if ( level == 1 ) {
            unsigned char *e = data + off;
            if ( keys ) { put32( e, (unsigned long)( c->ssent + count + 1 ) ); e += 4; }   /* ResumeKey */
            put16( e + 0, de.dosdate );  put16( e + 2, de.dostime );     /* Creation */
            put16( e + 4, de.dosdate );  put16( e + 6, de.dostime );     /* LastAccess */
            put16( e + 8, de.dosdate );  put16( e + 10, de.dostime );    /* LastWrite */
            put32( e + 12, de.size );                                    /* DataSize */
            put32( e + 16, ( de.size + 511 ) & ~511UL );                 /* AllocationSize */
            put16( e + 20, (unsigned int)( de.attr & 0x37 ) );           /* Attributes */
            e[22] = (unsigned char)namelen;                              /* FileNameLength */
            memcpy( e + 23, de.name, (unsigned)( namelen + 1 ) );        /* the name and its NUL */
            *lastname = (int)( ( e + 23 ) - data );
        } else {
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
            *lastname = off + 64;
        }
        last = at;
        off += entry + pad;
        count++;
    }
    c->fs->vfindclose( c->fsctx, it );
    if ( level != 1 && last >= 0 ) put32( data + last, 0 );   /* the last entry: NextEntryOffset 0 */
    c->ssent += count;
    *cnt = count;
    return off;
}

static void do_trans2( srv_conn *c, const unsigned char *words, int wc, const unsigned char *smb,
                       unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    int poff, pcnt, setup, maxdata, cap;
    const unsigned char *parm;
    static unsigned char data[SRV_BUF - 512];

    /* the client's offsets are untrusted: keep the parameter block inside
       the received message */
    if ( wc < 15 ) { reply0( c, 0x32, 0xC0000002UL, tid, uid, pid, mid ); return; }
    poff = (int)get16( words + 20 );
    pcnt = (int)get16( words + 18 );
    setup = (int)get16( words + 28 );
    maxdata = (int)get16( words + 6 );
    parm = smb + poff;
    if ( poff < 32 || pcnt < 0 || poff + pcnt > c->inlen ) {
        reply0( c, 0x32, 0xC0000002UL, tid, uid, pid, mid );
        return;
    }
    /* the reply's data fits what the client takes and what it can hold */
    cap = (int)sizeof( data );
    if ( cap > maxdata ) cap = maxdata;
    if ( cap > c->cli_max - 80 ) cap = c->cli_max - 80;
    if ( cap < 0 ) cap = 0;

    if ( setup == 1 && pcnt >= 12 ) {             /* FIND_FIRST2 */
        char spec[300];
        int i, cut = -1, cnt, eos, lastname, dlen, level = (int)get16( parm + 6 );
        int flags = (int)get16( parm + 4 ), want = (int)get16( parm + 2 );
        unsigned char rparm[10];
        if ( getz( parm + 12, pcnt - 12, spec, sizeof( spec ) ) < 0 ) { reply0( c, 0x32, 0xC0000033UL, tid, uid, pid, mid ); return; }
        for ( i = 0; spec[i]; i++ ) if ( spec[i] == '\\' || spec[i] == '/' ) cut = i;
        if ( cut <= 0 ) { c->sdir[0] = '\\'; c->sdir[1] = 0; }
        else { for ( i = 0; i < cut; i++ ) c->sdir[i] = spec[i]; c->sdir[i] = 0; }
        c->sall = pat_fcb( spec + cut + 1, c->spat );
        c->sattr = get16( parm + 0 );
        c->ssent = 0;
        if ( want <= 0 ) want = 1;
        dlen = build_listing( c, level, flags & 0x04, data, cap, want, &cnt, &eos, &lastname );
        if ( cnt < 0 ) { reply0( c, 0x32, 0xC000003AUL, tid, uid, pid, mid ); return; }
        if ( cnt == 0 && c->lm ) { reply0( c, 0x32, 0x80000006UL, tid, uid, pid, mid ); return; }   /* DOS: no more files */
        put16( rparm + 0, 1 );                    /* SID */
        put16( rparm + 2, (unsigned int)cnt );    /* SearchCount */
        put16( rparm + 4, (unsigned int)eos );    /* EndOfSearch */
        put16( rparm + 6, 0 );                    /* EaErrorOffset */
        put16( rparm + 8, (unsigned int)lastname );
        send_trans2( c, tid, uid, pid, mid, rparm, 10, data, dlen );
    } else if ( setup == 2 && pcnt >= 12 ) {      /* FIND_NEXT2 */
        int cnt, eos, lastname, dlen, level = (int)get16( parm + 4 );
        int flags = (int)get16( parm + 10 ), want = (int)get16( parm + 2 );
        unsigned long key = get32( parm + 6 );
        unsigned char rparm[8];
        if ( c->sdir[0] == 0 ) { reply0( c, 0x32, 0x80000006UL, tid, uid, pid, mid ); return; }
        if ( !( flags & 0x08 ) && key != 0 && key < 0x8000UL ) c->ssent = (int)key;   /* resume after it */
        if ( want <= 0 ) want = 1;
        dlen = build_listing( c, level, flags & 0x04, data, cap, want, &cnt, &eos, &lastname );
        if ( cnt < 0 ) { reply0( c, 0x32, 0xC000003AUL, tid, uid, pid, mid ); return; }
        put16( rparm + 0, (unsigned int)cnt );    /* SearchCount */
        put16( rparm + 2, (unsigned int)eos );    /* EndOfSearch */
        put16( rparm + 4, 0 );
        put16( rparm + 6, (unsigned int)lastname );
        send_trans2( c, tid, uid, pid, mid, rparm, 8, data, dlen );
    } else {
        reply0( c, 0x32, 0xC0000002UL, tid, uid, pid, mid );
    }
}

static void do_pathop( srv_conn *c, int cmd, const unsigned char *bytes, int bc,
                       unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    char path[300];
    int rc;
    if ( getpath( bytes, bc, path, sizeof( path ) ) < 0 ) {
        reply0( c, cmd, 0xC0000033UL, tid, uid, pid, mid );
        return;
    }
    if      ( cmd == 0x06 ) rc = c->fs->vunlink( c->fsctx, path );
    else if ( cmd == 0x00 ) rc = c->fs->vmkdir( c->fsctx, path );
    else                    rc = c->fs->vrmdir( c->fsctx, path );
    reply0( c, cmd, rc == 0 ? 0 : 0xC0000034UL, tid, uid, pid, mid );
}

/* RENAME (0x07): two 0x04-tagged paths */
static void do_rename( srv_conn *c, const unsigned char *bytes, int bc,
                       unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    char from[300], to[300];
    int n;
    if ( getpath( bytes, bc, from, sizeof( from ) ) < 0 ) { reply0( c, 0x07, 0xC0000033UL, tid, uid, pid, mid ); return; }
    n = 1 + slen( from ) + 1;                         /* past the first path's NUL */
    if ( n >= bc || getpath( bytes + n, bc - n, to, sizeof( to ) ) < 0 ) { reply0( c, 0x07, 0xC0000033UL, tid, uid, pid, mid ); return; }
    reply0( c, 0x07, c->fs->vrename( c->fsctx, from, to ) == 0 ? 0 : 0xC0000034UL, tid, uid, pid, mid );
}

/* QUERY_INFORMATION (0x08) and CHECK_DIRECTORY (0x10), by path */
static void do_qinfo( srv_conn *c, int cmd, const unsigned char *bytes, int bc,
                      unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    char path[300];
    srv_dirent de;
    unsigned char w[20];

    if ( getpath( bytes, bc, path, sizeof( path ) ) < 0 ) { reply0( c, cmd, 0xC0000033UL, tid, uid, pid, mid ); return; }
    if ( c->fs->vstat( c->fsctx, path, &de ) != 0 ) {
        reply0( c, cmd, cmd == 0x10 ? 0xC000003AUL : 0xC0000034UL, tid, uid, pid, mid );
        return;
    }
    if ( cmd == 0x10 ) {
        reply0( c, cmd, ( de.attr & 0x10 ) ? 0 : 0xC000003AUL, tid, uid, pid, mid );
        return;
    }
    memset( w, 0, sizeof( w ) );
    put16( w + 0, (unsigned int)( de.attr & 0x37 ) );           /* FileAttributes */
    put32( w + 2, dos_utime( de.dosdate, de.dostime ) );        /* LastWriteTime */
    put32( w + 6, de.size );                                    /* FileSize */
    reply( c, cmd, 0, tid, uid, pid, mid, w, 20, (unsigned char *)0, 0 );
}

/* SET_INFORMATION (0x09): the attributes (a time is not set by path) */
static void do_setinfo( srv_conn *c, const unsigned char *words, int wc, const unsigned char *bytes, int bc,
                        unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    char path[300];
    if ( wc < 1 || getpath( bytes, bc, path, sizeof( path ) ) < 0 ) { reply0( c, 0x09, 0xC0000033UL, tid, uid, pid, mid ); return; }
    reply0( c, 0x09, c->fs->vsetattr( c->fsctx, path, get16( words ) & 0x27 ) == 0 ? 0 : 0xC0000034UL, tid, uid, pid, mid );
}

/* QUERY_INFORMATION_DISK (0x80): four 16-bit counts, so a big disk is
   told in bigger units */
static void do_qdisk( srv_conn *c, unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    unsigned long units = 0, avail = 0, unitsize = 512, per = 1;
    unsigned char w[10];

    if ( c->fs->vdisk( c->fsctx, &units, &avail, &unitsize ) != 0 ) { reply0( c, 0x80, 0xC0000001UL, tid, uid, pid, mid ); return; }
    while ( unitsize >= 1024 && ( unitsize & 1 ) == 0 && per < 0x8000UL ) { unitsize >>= 1; per <<= 1; }  /* 512-byte blocks */
    while ( units > 0xFFFFUL && per < 0x8000UL ) { units >>= 1; avail >>= 1; per <<= 1; }
    if ( units > 0xFFFFUL ) units = 0xFFFFUL;
    if ( avail > units ) avail = units;
    memset( w, 0, sizeof( w ) );
    put16( w + 0, (unsigned int)units );         /* TotalUnits */
    put16( w + 2, (unsigned int)per );           /* BlocksPerUnit */
    put16( w + 4, (unsigned int)unitsize );      /* BlockSize */
    put16( w + 6, (unsigned int)avail );         /* FreeUnits */
    reply( c, 0x80, 0, tid, uid, pid, mid, w, 10, (unsigned char *)0, 0 );
}

/* SEEK (0x12): the file position the client keeps on the server */
static void do_seek( srv_conn *c, const unsigned char *words, int wc,
                     unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    int fx = ( wc >= 4 ) ? fid_index( c, get16( words ) ) : -1;
    unsigned long off, base = 0;
    unsigned char w[4];
    srv_dirent st;

    if ( fx < 0 ) { reply0( c, 0x12, 0xC0000008UL, tid, uid, pid, mid ); return; }
    off = get32( words + 4 );
    switch ( get16( words + 2 ) ) {
    case 1:  base = c->fid[fx].pos; break;
    case 2:  if ( c->fs->vfstat( c->fsctx, c->fid[fx].vh, &st ) == 0 ) base = st.size; break;
    default: base = 0; break;
    }
    c->fid[fx].pos = base + off;
    put32( w, c->fid[fx].pos );
    reply( c, 0x12, 0, tid, uid, pid, mid, w, 4, (unsigned char *)0, 0 );
}

/* QUERY_INFORMATION2 (0x23): the dates and size of an open file */
static void do_qinfo2( srv_conn *c, const unsigned char *words, int wc,
                       unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    int fx = ( wc >= 1 ) ? fid_index( c, get16( words ) ) : -1;
    srv_dirent st;
    unsigned char w[22];

    if ( fx < 0 || c->fs->vfstat( c->fsctx, c->fid[fx].vh, &st ) != 0 ) { reply0( c, 0x23, 0xC0000008UL, tid, uid, pid, mid ); return; }
    memset( w, 0, sizeof( w ) );
    put16( w + 0, st.dosdate );  put16( w + 2, st.dostime );     /* Creation */
    put16( w + 4, st.dosdate );  put16( w + 6, st.dostime );     /* LastAccess */
    put16( w + 8, st.dosdate );  put16( w + 10, st.dostime );    /* LastWrite */
    put32( w + 12, st.size );                                    /* FileDataSize */
    put32( w + 16, ( st.size + 511 ) & ~511UL );                 /* FileAllocationSize */
    put16( w + 20, 0x20 );                                       /* FileAttributes */
    reply( c, 0x23, 0, tid, uid, pid, mid, w, 22, (unsigned char *)0, 0 );
}

/* SET_INFORMATION2 (0x22): the last-write date and time of an open file */
static void do_setinfo2( srv_conn *c, const unsigned char *words, int wc,
                         unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    int fx = ( wc >= 7 ) ? fid_index( c, get16( words ) ) : -1;
    if ( fx < 0 ) { reply0( c, 0x22, 0xC0000008UL, tid, uid, pid, mid ); return; }
    if ( get16( words + 10 ) != 0 )
        c->fs->vsettime( c->fsctx, c->fid[fx].vh, get16( words + 10 ), get16( words + 12 ) );
    reply0( c, 0x22, 0, tid, uid, pid, mid );
}

/* ---- the core commands a DOS redirector (WfW 3.11's) sends ---- */

/* open a file for the core OPEN / CREATE / CREATE_NEW; the FID, or 0 with
   *status set */
static int core_open( srv_conn *c, const char *path, int write, int dispo,
                      unsigned long *size, unsigned long *status )
{
    int action = 1, vh, fid;
    vh = c->fs->vopen( c->fsctx, path, write, dispo, size, &action );
    if ( vh < 0 ) { *status = 0xC0000034UL; return 0; }
    fid = alloc_fid( c, vh );
    if ( fid == 0 ) { c->fs->vclose( c->fsctx, vh ); *status = 0xC0000034UL; return 0; }
    *status = 0;
    return fid;
}

/* OPEN (0x02), CREATE (0x03), CREATE_NEW (0x0F) */
static void do_core_open( srv_conn *c, int cmd, const unsigned char *words, int wc,
                          const unsigned char *bytes, int bc,
                          unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    char path[300];
    unsigned long size = 0, status;
    unsigned int access = ( wc >= 1 ) ? get16( words ) : 0;
    int fid;
    srv_dirent st;
    unsigned char w[14];

    if ( getpath( bytes, bc, path, sizeof( path ) ) < 0 ) { reply0( c, cmd, 0xC0000033UL, tid, uid, pid, mid ); return; }
    if ( cmd == 0x0F && c->fs->vstat( c->fsctx, path, &st ) == 0 ) { reply0( c, cmd, 0xC0000035UL, tid, uid, pid, mid ); return; }
    if ( cmd == 0x02 ) fid = core_open( c, path, ( access & 0x7 ) ? 1 : 0, 1, &size, &status );
    else               fid = core_open( c, path, 1, 5, &size, &status );
    if ( fid == 0 ) { reply0( c, cmd, status, tid, uid, pid, mid ); return; }

    memset( w, 0, sizeof( w ) );
    put16( w + 0, (unsigned int)fid );                          /* FID */
    if ( cmd != 0x02 ) { reply( c, cmd, 0, tid, uid, pid, mid, w, 2, (unsigned char *)0, 0 ); return; }
    put16( w + 2, 0x20 );                                       /* FileAttributes */
    if ( c->fs->vfstat( c->fsctx, c->fid[fid - 1].vh, &st ) == 0 )
        put32( w + 4, dos_utime( st.dosdate, st.dostime ) );   /* LastWriteTime */
    put32( w + 8, size );                                       /* FileSize */
    put16( w + 12, access & 0x7 );                              /* AccessMode */
    reply( c, cmd, 0, tid, uid, pid, mid, w, 14, (unsigned char *)0, 0 );
}

/* READ (0x0A): read straight into the reply's data, at offset 48 (header
   32, wc 1, five words, bc 2, buffer format 1, length 2) */
static void do_core_read( srv_conn *c, const unsigned char *words, int wc,
                          unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    int fx = ( wc >= 5 ) ? fid_index( c, get16( words ) ) : -1;
    int maxc, n, total;
    unsigned long off;
    unsigned char *p = c->out + 4;

    if ( fx < 0 ) { reply0( c, 0x0A, 0xC0000008UL, tid, uid, pid, mid ); return; }
    maxc = (int)get16( words + 2 );
    off = get32( words + 4 );
    if ( maxc > SRV_BUF - 128 ) maxc = SRV_BUF - 128;

    memset( p, 0, 48 );
    p[0] = 0xFF; p[1] = 'S'; p[2] = 'M'; p[3] = 'B';
    p[4] = 0x0A; p[9] = 0x88; put16( p + 10, c->lm ? 0 : 0x4001 );
    put16( p + 24, tid ); put16( p + 26, pid ); put16( p + 28, uid ); put16( p + 30, mid );
    p[32] = 5;                                   /* WordCount */

    n = c->fs->vread( c->fsctx, c->fid[fx].vh, off, p + 48, maxc );
    if ( n < 0 ) n = 0;
    c->fid[fx].pos = off + (unsigned long)n;

    put16( p + 33, (unsigned int)n );            /* Count */
    put16( p + 43, (unsigned int)( n + 3 ) );    /* ByteCount */
    p[45] = 0x01;                                /* buffer format: data block */
    put16( p + 46, (unsigned int)n );

    total = 4 + 48 + n;
    c->out[0] = 0; c->out[1] = 0;
    c->out[2] = (unsigned char)( ( total - 4 ) >> 8 );
    c->out[3] = (unsigned char)( total - 4 );
    send_all( c, c->out, total );
}

/* WRITE (0x0B): a count of 0 cuts (or grows) the file at the offset */
static void do_core_write( srv_conn *c, const unsigned char *words, int wc,
                           const unsigned char *bytes, int bc,
                           unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    int fx = ( wc >= 5 ) ? fid_index( c, get16( words ) ) : -1;
    int count, n;
    unsigned long off;
    unsigned char w[2];

    if ( fx < 0 ) { reply0( c, 0x0B, 0xC0000008UL, tid, uid, pid, mid ); return; }
    count = (int)get16( words + 2 );
    off = get32( words + 4 );
    if ( bc < 3 ) count = 0;
    else {
        if ( count > (int)get16( bytes + 1 ) ) count = (int)get16( bytes + 1 );
        if ( count > bc - 3 ) count = bc - 3;
    }
    n = c->fs->vwrite( c->fsctx, c->fid[fx].vh, off, bytes + 3, count );
    if ( n < 0 ) { reply0( c, 0x0B, 0xC0000022UL, tid, uid, pid, mid ); return; }
    c->fid[fx].pos = off + (unsigned long)n;
    put16( w, (unsigned int)n );
    reply( c, 0x0B, 0, tid, uid, pid, mid, w, 2, (unsigned char *)0, 0 );
}

/* SEARCH (0x81) and FCLOSE (0x84).  A search is resumed by the 21-byte key
   of the last entry it gave: byte 1 the directory's slot, 2..12 the
   pattern, 13..14 the matches given, 15 the attributes, 16 "all", 17..20
   the client's own state, given back as it came. */
#define SEARCH_ENTRY  43
#define SEARCH_SLOTS  4

static char g_sdirs[SEARCH_SLOTS][300];
static int  g_snext;

static void do_search( srv_conn *c, int cmd, const unsigned char *words, int wc,
                       const unsigned char *bytes, int bc,
                       unsigned int tid, unsigned int uid, unsigned int pid, unsigned int mid )
{
    char spec[300];
    unsigned char key[21], pat[11], *out, *e;
    int maxcount, slot, all, skip, given, count = 0, n, i, cap, keylen = 0;
    unsigned int sattr;
    void *it;
    srv_dirent de;

    if ( wc < 2 || getpath( bytes, bc, spec, sizeof( spec ) ) < 0 ) { reply0( c, cmd, 0xC0000033UL, tid, uid, pid, mid ); return; }
    maxcount = (int)get16( words );
    sattr = get16( words + 2 );
    n = 1 + slen( spec ) + 1;                        /* past the name's NUL: the key block */
    memset( key, 0, sizeof( key ) );
    if ( n + 3 <= bc && bytes[n] == 0x05 ) {
        keylen = (int)get16( bytes + n + 1 );
        if ( keylen >= 21 && n + 3 + 21 <= bc ) memcpy( key, bytes + n + 3, 21 );
        else keylen = 0;
    }
    if ( cmd == 0x84 ) {                             /* FCLOSE: nothing is held */
        unsigned char w[2], b[3];
        put16( w, 0 ); b[0] = 0x05; b[1] = 0; b[2] = 0;
        reply( c, cmd, 0, tid, uid, pid, mid, w, 2, b, 3 );
        return;
    }
    if ( spec[0] ) {                                 /* a new search */
        int cut = -1;
        if ( ( sattr & 0x08 ) != 0 ) { reply0( c, cmd, 0x80000006UL, tid, uid, pid, mid ); return; }  /* no label */
        for ( i = 0; spec[i]; i++ ) if ( spec[i] == '\\' || spec[i] == '/' ) cut = i;
        slot = g_snext;
        g_snext = ( g_snext + 1 ) % SEARCH_SLOTS;
        if ( cut <= 0 ) { g_sdirs[slot][0] = '\\'; g_sdirs[slot][1] = 0; }
        else { for ( i = 0; i < cut; i++ ) g_sdirs[slot][i] = spec[i]; g_sdirs[slot][i] = 0; }
        all = pat_fcb( spec + cut + 1, pat );
        skip = 0;
    } else if ( keylen ) {                           /* resume from the key */
        slot = key[1];
        if ( slot >= SEARCH_SLOTS || g_sdirs[slot][0] == 0 ) { reply0( c, cmd, 0x80000006UL, tid, uid, pid, mid ); return; }
        memcpy( pat, key + 2, 11 );
        skip = (int)get16( key + 13 );
        sattr = key[15];
        all = key[16];
    } else {
        reply0( c, cmd, 0xC0000033UL, tid, uid, pid, mid );
        return;
    }

    /* as many as asked, as fit the client's buffer and ours; the entries
       go straight where reply() puts the bytes (after wc 1, one word, bc) */
    out = c->out + 4 + 33 + 2 + 2;
    cap = ( c->cli_max - 48 ) / SEARCH_ENTRY;
    if ( cap > ( SRV_BUF - 4 - 37 - 3 ) / SEARCH_ENTRY ) cap = ( SRV_BUF - 4 - 37 - 3 ) / SEARCH_ENTRY;
    if ( maxcount > cap ) maxcount = cap;
    it = c->fs->vfindopen( c->fsctx, g_sdirs[slot] );
    if ( it == (void *)0 ) { reply0( c, cmd, 0xC000003AUL, tid, uid, pid, mid ); return; }
    given = skip;
    while ( count < maxcount && c->fs->vfindnext( c->fsctx, it, &de ) ) {
        int len = slen( de.name );
        if ( !match_entry( pat, all, sattr, &de ) || len > 12 ) continue;
        if ( skip > 0 ) { skip--; continue; }
        given++;
        e = out + 3 + count * SEARCH_ENTRY;
        memset( e, 0, SEARCH_ENTRY );
        e[1] = (unsigned char)slot;
        memcpy( e + 2, pat, 11 );
        put16( e + 13, (unsigned int)given );
        e[15] = (unsigned char)sattr;
        e[16] = (unsigned char)all;
        memcpy( e + 17, key + 17, 4 );               /* ClientState */
        e[21] = (unsigned char)( de.attr & 0x37 );
        put16( e + 22, de.dostime );
        put16( e + 24, de.dosdate );
        put32( e + 26, de.size );
        memcpy( e + 30, de.name, (unsigned)len );     /* NUL-filled to 13 */
        count++;
    }
    c->fs->vfindclose( c->fsctx, it );
    if ( count == 0 ) { reply0( c, cmd, 0x80000006UL, tid, uid, pid, mid ); return; }   /* no more files */

    out[0] = 0x05;                                   /* buffer format: variable block */
    put16( out + 1, (unsigned int)( count * SEARCH_ENTRY ) );
    {
        unsigned char w[2];
        put16( w, (unsigned int)count );
        reply( c, cmd, 0, tid, uid, pid, mid, w, 2, out, 3 + count * SEARCH_ENTRY );
    }
}

char smbsrv_name[16] = "PMDOS";

void smbsrv_serve( smb_send_fn send, smb_recv_fn recv, void *ioctx,
                   const srv_vfs *fs, void *fsctx, const char *sharename )
{
    static srv_conn conn;
    srv_conn *c = &conn;
    memset( c, 0, sizeof( *c ) );
    c->send = send; c->recv = recv; c->ioctx = ioctx; c->fs = fs; c->fsctx = fsctx;
    c->share = sharename;
    c->cli_max = SRV_BUF;

    for ( ;; ) {
        const unsigned char *smb, *words, *bytes;
        int wc, bc, cmd;
        unsigned int tid, pid, uid, mid;

        c->inlen = recv_msg( c );
        if ( c->inlen < 33 ) { close_all( c ); return; }
        smb = c->in;
        if ( smb[0] != 0xFF || smb[1] != 'S' || smb[2] != 'M' || smb[3] != 'B' ) { close_all( c ); return; }
        cmd = smb[4];
        tid = get16( smb + 24 ); pid = get16( smb + 26 ); uid = get16( smb + 28 ); mid = get16( smb + 30 );
        wc  = smb[32];
        words = smb + 33;
        bytes = smb + 33 + wc * 2 + 2;

        /* the counts are untrusted: keep the words and the byte field inside
           the message (the words are read past what wc promises only where
           the command checks wc) */
        if ( 33 + wc * 2 + 2 > c->inlen ) {
            reply0( c, cmd, 0xC0000002UL, tid, uid, pid, mid );
            continue;
        }
        bc  = (int)get16( smb + 33 + wc * 2 );
        if ( bc > c->inlen - ( 33 + wc * 2 + 2 ) ) bc = c->inlen - ( 33 + wc * 2 + 2 );

        switch ( cmd ) {
        case 0x72: do_negotiate( c, bytes, bc, pid, mid ); break;
        case 0x73: {   /* SESSION_SETUP_ANDX: accept any */
            unsigned char w[6]; unsigned char b[32]; int bn = 0; const char *s;
            if ( wc >= 3 ) {                     /* MaxBufferSize, in both forms */
                c->cli_max = (int)get16( words + 4 );
                if ( c->cli_max < 1024 ) c->cli_max = 1024;
            }
            memset( w, 0, sizeof( w ) ); w[0] = 0xFF;
            s = "Unix"; { int k; for ( k = 0; s[k]; k++ ) b[bn++] = (unsigned char)s[k]; } b[bn++] = 0;
            s = smbsrv_name; { int k; for ( k = 0; s[k]; k++ ) b[bn++] = (unsigned char)s[k]; } b[bn++] = 0;
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
        case 0x71: reply0( c, 0x71, 0, tid, uid, pid, mid ); break;
        case 0x74: { unsigned char w[6]; memset( w, 0, sizeof( w ) ); w[0] = 0xFF; reply( c, 0x74, 0, tid, uid, pid, mid, w, 6, (unsigned char *)0, 0 ); break; }
        case 0x2B:     /* ECHO: the reply is longer than the request */
            if ( bc > SRV_BUF - 41 ) bc = SRV_BUF - 41;
            reply( c, 0x2B, 0, tid, uid, pid, mid, (unsigned char *)words, 2, (unsigned char *)bytes, bc );
            break;
        case 0x2D: if ( wc >= 15 ) do_open( c, words, bytes, bc, tid, uid, pid, mid ); else reply0( c, cmd, 0xC0000002UL, tid, uid, pid, mid ); break;
        case 0x2E: if ( wc >= 10 ) do_read( c, words, tid, uid, pid, mid ); else reply0( c, cmd, 0xC0000002UL, tid, uid, pid, mid ); break;
        case 0x2F: if ( wc >= 12 ) do_write( c, words, wc, smb, tid, uid, pid, mid ); else reply0( c, cmd, 0xC0000002UL, tid, uid, pid, mid ); break;
        case 0x04: if ( wc >= 1 ) do_close( c, words, wc, tid, uid, pid, mid ); else reply0( c, cmd, 0xC0000002UL, tid, uid, pid, mid ); break;
        case 0x06: case 0x00: case 0x01: do_pathop( c, cmd, bytes, bc, tid, uid, pid, mid ); break;
        case 0x07: do_rename( c, bytes, bc, tid, uid, pid, mid ); break;
        case 0x08: case 0x10: do_qinfo( c, cmd, bytes, bc, tid, uid, pid, mid ); break;
        case 0x09: do_setinfo( c, words, wc, bytes, bc, tid, uid, pid, mid ); break;
        case 0x80: do_qdisk( c, tid, uid, pid, mid ); break;
        case 0x12: do_seek( c, words, wc, tid, uid, pid, mid ); break;
        case 0x22: do_setinfo2( c, words, wc, tid, uid, pid, mid ); break;
        case 0x23: do_qinfo2( c, words, wc, tid, uid, pid, mid ); break;
        case 0x05:     /* FLUSH: the back end writes through */
        case 0x34:     /* FIND_CLOSE2: the search keeps no handle */
            reply0( c, cmd, 0, tid, uid, pid, mid );
            break;
        case 0x02: case 0x03: case 0x0F: do_core_open( c, cmd, words, wc, bytes, bc, tid, uid, pid, mid ); break;
        case 0x0A: do_core_read( c, words, wc, tid, uid, pid, mid ); break;
        case 0x0B: do_core_write( c, words, wc, bytes, bc, tid, uid, pid, mid ); break;
        case 0x81: case 0x84: do_search( c, cmd, words, wc, bytes, bc, tid, uid, pid, mid ); break;
        case 0x0C:     /* LOCK_BYTE_RANGE, UNLOCK_BYTE_RANGE: as LOCKING_ANDX */
        case 0x0D:
        case 0x11:     /* PROCESS_EXIT */
            reply0( c, cmd, 0, tid, uid, pid, mid );
            break;
        case 0x24: {   /* LOCKING_ANDX: one client, so no lock can conflict */
            unsigned char w[4]; memset( w, 0, sizeof( w ) ); w[0] = 0xFF;
            reply( c, 0x24, 0, tid, uid, pid, mid, w, 4, (unsigned char *)0, 0 );
            break; }
        case 0x32: do_trans2( c, words, wc, smb, tid, uid, pid, mid ); break;
        default:   reply0( c, cmd, 0xC0000002UL, tid, uid, pid, mid ); break;
        }
    }
}
