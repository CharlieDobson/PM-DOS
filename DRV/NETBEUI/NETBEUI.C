/*
 * NETBEUI.C - NETBEUI.SYS, NetBIOS over NetBEUI
 *
 * The NetBIOS Frames Protocol (NBF) that LAN Manager, Windows for
 * Workgroups and Windows NT speak without TCP/IP: NetBIOS names and
 * sessions straight on 802.2 LLC, SAP F0, in 802.3 frames.  A 32-bit
 * DEVICE= driver like TCPIP.SYS: it rides on a loaded card driver,
 * which it asks (NIC_BIND) for every frame with DSAP F0, so that
 * TCPIP.SYS can go on reading the rest.  Programs reach it through
 * 21h/F1h AL=25h on its row, the SMB redirector through NDS_FLAGCALL;
 * INC\NETBIOS.H is the interface.
 *
 * TWO LAYERS.  Names, and finding them, are UI frames (LLC type 1) to
 * the NetBIOS group address 03-00-00-00-00-01: ADD_NAME_QUERY when a
 * name is taken, NAME_QUERY to find one, NAME_RECOGNIZED back from its
 * owner.  Sessions ride on an LLC type 2 link to the other card - SABME
 * and UA to start it, then numbered I-frames acknowledged by RR - and a
 * session is SESSION_INITIALIZE / SESSION_CONFIRM, then messages cut
 * into DATA_FIRST_MIDDLE pieces and a DATA_ONLY_LAST, each answered by
 * DATA_ACK, and SESSION_END.  Several sessions share one link.
 *
 * KEPT SMALL ON PURPOSE.  One I-frame is out on a link at a time (a
 * window of 1): the other end may send us as many as it likes, and each
 * is acknowledged.  A message counts as delivered when the link has
 * acknowledged its last piece; a DATA_ACK is sent for every message
 * that arrives, and taken when it comes, but never waited for.  No
 * datagrams, no status queries, no group names: what SMB needs.
 *
 * WHERE IT RUNS is where every 32-bit driver runs: the clock tick and
 * the CALL entry move everything; nothing blocks, except INIT, which
 * waits a second and a half to hear whether its name is someone else's.
 *
 * Compiled like a card driver: wcc386 -mf -3r -s -zl, behind
 * DRV\NDSTART.ASM - see BUILD.CMD.
 */
#define ND_DRIVER
#include "ndrv.h"
#include "nic.h"
#include "socket.h"
#include "netbios.h"

#define NULL ((void *)0)

/* ---------------------------------------------------------------- */
/* the wire                                                          */
/* ---------------------------------------------------------------- */

#define SAP_NB       0xF0
#define SAP_RESP     0x01               /* in the SSAP: a response */

#define U_UI         0x03
#define U_SABME      0x6F
#define U_UA         0x63
#define U_DISC       0x43
#define U_DM         0x0F
#define U_FRMR       0x87
#define U_XID        0xAF
#define U_TEST       0xE3
#define U_PF         0x10               /* poll / final, in a U frame */

#define S_RR         0x01
#define S_RNR        0x05
#define S_REJ        0x09

/* NetBIOS frame commands */
#define NB_ADD_NAME_QUERY    0x01
#define NB_NAME_IN_CONFLICT  0x02
#define NB_NAME_QUERY        0x0A
#define NB_ADD_NAME_RESPONSE 0x0D
#define NB_NAME_RECOGNIZED   0x0E
#define NB_DATA_ACK          0x14
#define NB_DATA_FIRST_MIDDLE 0x15
#define NB_DATA_ONLY_LAST    0x16
#define NB_SESSION_CONFIRM   0x17
#define NB_SESSION_END       0x18
#define NB_SESSION_INIT      0x19
#define NB_NO_RECEIVE        0x1A
#define NB_RECEIVE_OUTSTAND  0x1B
#define NB_RECEIVE_CONTINUE  0x1C
#define NB_SESSION_ALIVE     0x1F

#define NB_UIHDR     44                 /* a UI frame's NetBIOS header */
#define NB_IHDR      14                 /* a session frame's */
#define DATA_MAX     1482               /* 1500 less LLC (4) and NB_IHDR */
#define SI_DATA1     0x03               /* version 2, largest frame 1500 */

#define ETH_HDR      14

static const u8 nb_group[6] = { 0x03, 0x00, 0x00, 0x00, 0x00, 0x01 };

/* times, in ticks (18.2 a second) */
#define T1           18                 /* an I-frame or SABME unanswered */
#define N2           8                  /* ...this many times: the link is gone */
#define T_QUERY      9                  /* NAME_QUERY / ADD_NAME_QUERY again */
#define N_QUERY      3
#define T_SESSION    182                /* a session that has not started */

/* ---------------------------------------------------------------- */
/* the state                                                         */
/* ---------------------------------------------------------------- */

#define LINK_MAX     4
#define CTLQ         8                  /* session control frames waiting */

#define LS_DOWN      0
#define LS_SETUP     1                  /* SABME out */
#define LS_UP        2

typedef struct {
    u8   used;
    u8   state;                         /* LS_* */
    u8   mac[6];
    u8   vs, vr;                        /* next N(S) to send, next expected */
    u8   unacked;                       /* an I-frame is out */
    u8   busy;                          /* they said RNR */
    u8   we_busy;                       /* we said RNR */
    u8   ackowed;                       /* an RR is owed for what came in */
    u8   tries;
    u32  t1_at;
    u8   ctl[CTLQ][NB_IHDR];            /* session control frames to send */
    u8   ctl_head, ctl_n;
    u8   rr;                            /* round robin over the sessions */
    u8   frame[ETH_HDR + 4 + NB_IHDR + DATA_MAX];   /* the I-frame out */
    u32  flen;
    u8   lastsess;                      /* the session whose piece it carries */
    u8   lastend;                       /* ...and it was a DATA_ONLY_LAST */
} LINK;

/* a session's step while it is NBX_CALLING or NBX_LISTEN */
#define SS_NONE      0
#define SS_QUERY     1                  /* NAME_QUERY out (calling) */
#define SS_LINK      2                  /* the link is starting (calling) */
#define SS_INIT      3                  /* SESSION_INITIALIZE out (calling) */
#define SS_ANSWERED  4                  /* NAME_RECOGNIZED sent (listening) */

#define SESS_MAX     4

typedef struct {
    u8   state;                         /* NBX_* */
    u8   step;                          /* SS_* */
    u8   err;                           /* NBE_* */
    u8   lsn, rsn;                      /* our session number and theirs */
    u8   suffix;                        /* LISTEN: which of our names */
    u8   link;                          /* links[] */
    u8   mac[6];
    u8   rname[16];                     /* the other end's name, as sent */
    u16  qcorr;                         /* our NAME_QUERY's correlator */
    u16  icorr;                         /* NAME_RECOGNIZED's: SESSION_INIT's xmit */
    u16  dcorr;                         /* the DATA_ONLY_LAST out */
    u16  peer_max;                      /* the most data in one piece they take */
    u8   tries;
    u8   sent_si;                       /* our SESSION_INITIALIZE has gone */
    u32  at;                            /* the next retry / when to give up */
    u32  started;
    u8  *rx;                            /* a message arriving, or arrived */
    u32  rxlen, rxoff;
    u8   rxdone;                        /* rx holds a whole message */
    u8  *tx;                            /* the message going */
    u32  txlen, txoff;
    u8   txbusy;
    u8   txlastout;                     /* its last piece is on the link */
    u8   txhold;                        /* NO_RECEIVE: wait for them */
    u8   txresync;
    u32  txbuilt;                       /* NB_SENDP parts so far */
} SESS;

static LINK links[LINK_MAX];
static SESS sess[SESS_MAX];

static u8   my_mac[6];
static u8   my_names[2][16];            /* NAME<00>, NAME<20>, as on the wire */
static char my_name[16];                /* ASCIIZ */
static char card_name[32];
static u8   name_up;
static u8   name_conflict;
static u16  add_corr;
static u32  bindno;                     /* the card's NIC_BIND for SAP F0 */
static u16  corr_next = 1;
static u8   lsn_next = 1;
static u8   running;
static volatile u8 pumping;

static u8   tx_frame[ETH_HDR + 1500];
static u8   rx_buf[4 + 1536];

/* the one FIND.NAME going */
static struct {
    u8   active;
    u8   tries;
    u16  corr;
    u8   name[16];
    u32  at;
    u32  state;
    u8   mac[6];
} find;

/* ---------------------------------------------------------------- */
/* what a library would have supplied                                */
/* ---------------------------------------------------------------- */

static void mem_copy( void *to, const void *from, u32 len )
{
    u8 *dst = (u8 *)to;
    const u8 *src = (const u8 *)from;
    while ( len-- ) *dst++ = *src++;
}

static void mem_zero( void *to, u32 len )
{
    u8 *dst = (u8 *)to;
    while ( len-- ) *dst++ = 0;
}

static int mem_same( const void *one, const void *two, u32 len )
{
    const u8 *a = (const u8 *)one, *b = (const u8 *)two;
    while ( len-- ) if ( *a++ != *b++ ) return 0;
    return 1;
}

static void str_copy( char *to, const char *from, u32 max )
{
    u32 i;
    for ( i = 0; i + 1 < max && from[i]; i++ ) to[i] = from[i];
    to[i] = 0;
}

static u8 upper( u8 ch )
{
    return ( ch >= 'a' && ch <= 'z' ) ? (u8)( ch - 32 ) : ch;
}

static void put16le( u8 *buf, u16 value )
{
    buf[0] = (u8)value;
    buf[1] = (u8)( value >> 8 );
}

static u16 get16le( const u8 *buf )
{
    return (u16)( buf[0] | ( (u16)buf[1] << 8 ) );
}

static u16 new_corr( void )
{
    if ( ++corr_next == 0 ) corr_next = 1;
    return corr_next;
}

/* a NetBIOS name as it goes on the wire: 15 characters, upper case,
   padded with spaces, and the service in the 16th byte */
static void nb_raw( u8 *out, const char *name, u8 suffix )
{
    u32 i;
    int ended = 0;

    for ( i = 0; i < 15; i++ ) {
        if ( !ended && name[i] == 0 ) ended = 1;
        out[i] = ended ? (u8)' ' : upper( (u8)name[i] );
    }
    out[15] = suffix;
}

/* is the name on the wire one of ours?  -1 if not, else which */
static int nb_ours( const u8 *raw )
{
    int n, i;

    if ( !name_up && !add_corr ) return -1;
    for ( n = 0; n < 2; n++ ) {
        for ( i = 0; i < 16 && upper( raw[i] ) == my_names[n][i]; i++ ) ;
        if ( i == 16 ) return n;
    }
    return -1;
}

/* ---------------------------------------------------------------- */
/* the card                                                          */
/* ---------------------------------------------------------------- */

static void nic_send( const u8 *frame, u32 len )
{
    u32 answer;
    nds_niccall( 0, NIC_SEND, (void *)frame, len, &answer );
}

/* an 802.3 frame with an LLC header: DSAP F0, the SSAP given, the
   control bytes, then the information */
static void llc_send( const u8 *dst, u8 ssap, const u8 *ctl, u32 ctlen,
                      const u8 *info, u32 ilen )
{
    u8 *f = tx_frame;
    u32 llen = 2 + ctlen + ilen;

    if ( ETH_HDR + llen > sizeof( tx_frame ) ) return;
    mem_copy( f, dst, 6 );
    mem_copy( f + 6, my_mac, 6 );
    f[12] = (u8)( llen >> 8 );
    f[13] = (u8)llen;
    f[14] = SAP_NB;
    f[15] = ssap;
    mem_copy( f + 16, ctl, ctlen );
    if ( ilen ) mem_copy( f + 16 + ctlen, info, ilen );
    nic_send( f, ETH_HDR + llen );
}

static void u_send( const u8 *dst, u8 ssap, u8 ctl, const u8 *info, u32 ilen )
{
    llc_send( dst, ssap, &ctl, 1, info, ilen );
}

static void s_send( const u8 *dst, u8 ssap, u8 type, u8 nr, u8 pf )
{
    u8 ctl[2];
    ctl[0] = type;
    ctl[1] = (u8)( ( nr << 1 ) | ( pf ? 1 : 0 ) );
    llc_send( dst, ssap, ctl, 2, NULL, 0 );
}

/* a UI frame: the 44-byte NetBIOS header and the names */
static void ui_send( const u8 *dst, u8 cmd, u8 d1, u16 d2, u16 xc, u16 rc,
                     const u8 *dname, const u8 *sname )
{
    u8 h[NB_UIHDR];

    put16le( h, NB_UIHDR );
    h[2] = 0xFF;
    h[3] = 0xEF;
    h[4] = cmd;
    h[5] = d1;
    put16le( h + 6, d2 );
    put16le( h + 8, xc );
    put16le( h + 10, rc );
    if ( dname ) mem_copy( h + 12, dname, 16 ); else mem_zero( h + 12, 16 );
    if ( sname ) mem_copy( h + 28, sname, 16 ); else mem_zero( h + 28, 16 );
    u_send( dst, SAP_NB, U_UI, h, NB_UIHDR );
}

/* a session frame's 14-byte header */
static void ihdr( u8 *h, u8 cmd, u8 d1, u16 d2, u16 xc, u16 rc, u8 dnum, u8 snum )
{
    put16le( h, NB_IHDR );
    h[2] = 0xFF;
    h[3] = 0xEF;
    h[4] = cmd;
    h[5] = d1;
    put16le( h + 6, d2 );
    put16le( h + 8, xc );
    put16le( h + 10, rc );
    h[12] = dnum;
    h[13] = snum;
}

/* ---------------------------------------------------------------- */
/* the links (LLC type 2)                                            */
/* ---------------------------------------------------------------- */

static LINK *link_find( const u8 *mac )
{
    int i;
    for ( i = 0; i < LINK_MAX; i++ ) {
        if ( links[i].used && mem_same( links[i].mac, mac, 6 ) ) return &links[i];
    }
    return NULL;
}

static LINK *link_new( const u8 *mac )
{
    int i;
    for ( i = 0; i < LINK_MAX; i++ ) {
        if ( !links[i].used ) {
            mem_zero( &links[i], sizeof( LINK ) );
            links[i].used = 1;
            mem_copy( links[i].mac, mac, 6 );
            return &links[i];
        }
    }
    return NULL;
}

static u8 link_index( LINK *l )
{
    return (u8)( l - links );
}

static void sess_buffers_free( SESS *s );

/* the link is gone: every session on it ends */
static void link_down( LINK *l )
{
    int i;
    u8 idx = link_index( l );

    for ( i = 0; i < SESS_MAX; i++ ) {
        SESS *s = &sess[i];
        if ( s->state == NBX_FREE || s->link != idx ) continue;
        if ( s->state == NBX_LISTEN && s->step == SS_ANSWERED ) {
            s->step = SS_NONE;          /* back to listening */
            continue;
        }
        if ( s->state == NBX_UP ) { s->state = NBX_CLOSED; s->err = NBE_LINK; }
        else if ( s->state == NBX_CALLING && s->step >= SS_LINK ) { s->state = NBX_FAIL; s->err = NBE_LINK; }
    }
    l->used = 0;
    l->state = LS_DOWN;
}

/* reset the sequence numbers: a link that has just come up */
static void link_reset( LINK *l )
{
    l->vs = l->vr = 0;
    l->unacked = 0;
    l->busy = 0;
    l->we_busy = 0;
    l->ackowed = 0;
    l->tries = 0;
    l->ctl_n = 0;
    l->ctl_head = 0;
}

/* a session control frame, to go when the link is free */
static void ctl_queue( LINK *l, const u8 *h )
{
    u8 slot;
    if ( l->ctl_n == CTLQ ) return;     /* full: the frame is lost, as on a wire */
    slot = (u8)( ( l->ctl_head + l->ctl_n ) % CTLQ );
    mem_copy( l->ctl[slot], h, NB_IHDR );
    l->ctl_n++;
}

/* send the I-frame built in l->frame (its N(S) and N(R) set now) */
static void link_put_i( LINK *l, u8 pf )
{
    l->frame[16] = (u8)( l->vs << 1 );
    l->frame[17] = (u8)( ( l->vr << 1 ) | ( pf ? 1 : 0 ) );
    nic_send( l->frame, l->flen );
    l->ackowed = 0;                     /* N(R) went with it */
}

static void link_send_i( LINK *l, const u8 *h, const u8 *data, u32 dlen )
{
    u8 *f = l->frame;
    u32 llen = 4 + NB_IHDR + dlen;

    mem_copy( f, l->mac, 6 );
    mem_copy( f + 6, my_mac, 6 );
    f[12] = (u8)( llen >> 8 );
    f[13] = (u8)llen;
    f[14] = SAP_NB;
    f[15] = SAP_NB;                     /* a command */
    mem_copy( f + 18, h, NB_IHDR );
    if ( dlen ) mem_copy( f + 18 + NB_IHDR, data, dlen );
    l->flen = ETH_HDR + llen;
    link_put_i( l, 0 );
    l->vs = (u8)( ( l->vs + 1 ) & 0x7F );
    l->unacked = 1;
    l->tries = 0;
    l->t1_at = nds_ticks() + T1;
}

/* N(R) came: with one frame out, it is acknowledged when N(R) is V(S) */
static void link_ack( LINK *l, u8 nr )
{
    if ( !l->unacked || nr != l->vs ) return;
    l->unacked = 0;
    l->tries = 0;
    if ( l->lastend && l->lastsess < SESS_MAX ) {
        SESS *s = &sess[l->lastsess];
        if ( s->state == NBX_UP && s->txlastout ) {
            s->txbusy = 0;              /* the message's last piece is in */
            s->txlastout = 0;
        }
    }
    l->lastend = 0;
}

/* the next frame for the link, if it is free: a control frame first,
   then the next piece of a session's message, the sessions in turn */
static void link_kick( LINK *l )
{
    u8 h[NB_IHDR];
    int n, i;
    u8 idx = link_index( l );

    if ( l->state != LS_UP || l->unacked || l->busy ) return;

    if ( l->ctl_n ) {
        mem_copy( h, l->ctl[l->ctl_head], NB_IHDR );
        l->ctl_head = (u8)( ( l->ctl_head + 1 ) % CTLQ );
        l->ctl_n--;
        l->lastend = 0;
        link_send_i( l, h, NULL, 0 );
        return;
    }
    for ( n = 0; n < SESS_MAX; n++ ) {
        SESS *s;
        u32 chunk;
        int last;

        i = ( l->rr + n ) % SESS_MAX;
        s = &sess[i];
        if ( s->state != NBX_UP || s->link != idx || !s->txbusy || s->txlastout || s->txhold ) continue;
        chunk = s->txlen - s->txoff;
        if ( chunk > s->peer_max ) chunk = s->peer_max;
        last = ( s->txoff + chunk == s->txlen );
        if ( last ) s->dcorr = new_corr();
        ihdr( h, last ? NB_DATA_ONLY_LAST : NB_DATA_FIRST_MIDDLE, 0,
              (u16)( s->txresync ? 1 : 0 ), 0, last ? s->dcorr : 0, s->rsn, s->lsn );
        s->txresync = 0;
        l->lastsess = (u8)i;
        l->lastend = (u8)last;
        link_send_i( l, h, s->tx + s->txoff, chunk );
        s->txoff += chunk;
        if ( last ) s->txlastout = 1;
        l->rr = (u8)( ( i + 1 ) % SESS_MAX );
        return;
    }
}

/* the caller's SESSION_INITIALIZE, at once: Windows for Workgroups answers
   one that comes straight after the link, and ignores one a second late */
static void caller_si( LINK *l, SESS *s )
{
    u8 h[NB_IHDR];

    s->step = SS_INIT;
    if ( s->sent_si ) return;
    ihdr( h, NB_SESSION_INIT, SI_DATA1, DATA_MAX, s->icorr, new_corr(), s->rsn, s->lsn );
    ctl_queue( l, h );
    s->sent_si = 1;
    link_kick( l );
}

/* the link is up: the callers waiting on it start their sessions */
static void link_is_up( LINK *l )
{
    int i;
    u8 idx = link_index( l );

    for ( i = 0; i < SESS_MAX; i++ ) {
        SESS *s = &sess[i];
        if ( s->state == NBX_CALLING && s->step == SS_LINK && s->link == idx ) caller_si( l, s );
    }
}

/* ---------------------------------------------------------------- */
/* the sessions                                                      */
/* ---------------------------------------------------------------- */

static SESS *sess_by_lsn( u8 lsn )
{
    int i;
    for ( i = 0; i < SESS_MAX; i++ ) {
        if ( sess[i].state != NBX_FREE && sess[i].lsn == lsn ) return &sess[i];
    }
    return NULL;
}

static u8 lsn_alloc( void )
{
    int tries;
    for ( tries = 0; tries < 254; tries++ ) {
        u8 n = lsn_next;
        lsn_next = (u8)( lsn_next == 254 ? 1 : lsn_next + 1 );
        if ( sess_by_lsn( n ) == NULL ) return n;
    }
    return 1;
}

static void sess_buffers_free( SESS *s )
{
    if ( s->rx ) { nds_free( s->rx ); s->rx = NULL; }
    if ( s->tx ) { nds_free( s->tx ); s->tx = NULL; }
}

/* a session frame inside an I-frame.  0: no room for it now (RNR) */
static int sess_frame( LINK *l, const u8 *p, u32 plen )
{
    u8 cmd, d1, dnum, snum;
    u16 d2, xc, rc;
    const u8 *data;
    u32 dlen;
    SESS *s;
    u8 h[NB_IHDR];

    if ( plen < NB_IHDR || p[2] != 0xFF || p[3] != 0xEF ) return 1;
    cmd = p[4];
    d1 = p[5];
    d2 = get16le( p + 6 );
    xc = get16le( p + 8 );
    rc = get16le( p + 10 );
    dnum = p[12];
    snum = p[13];
    data = p + NB_IHDR;
    dlen = plen - NB_IHDR;
    (void)xc;

    s = sess_by_lsn( dnum );
    if ( s == NULL ) return 1;          /* not ours: taken and forgotten */

    switch ( cmd ) {
    case NB_SESSION_INIT:
        if ( !mem_same( s->mac, l->mac, 6 ) ) return 1;
        /* the caller's, to us listening: confirm, and it is up.  Or the
           listener's, in answer to ours: Windows for Workgroups sends a
           SESSION_INITIALIZE, not a SESSION_CONFIRM, on a link that has
           just come up (and a SESSION_CONFIRM on one already up) - so
           confirm that too */
        if ( !( ( s->state == NBX_LISTEN && s->step == SS_ANSWERED ) ||
                ( s->state == NBX_CALLING && ( s->step == SS_INIT || s->step == SS_LINK ) ) ) ) return 1;
        s->rsn = snum;
        s->peer_max = d2 && d2 < DATA_MAX ? d2 : DATA_MAX;
        s->link = link_index( l );
        s->state = NBX_UP;
        s->step = SS_NONE;
        ihdr( h, NB_SESSION_CONFIRM, 0x01, DATA_MAX, rc, 0, s->rsn, s->lsn );
        ctl_queue( l, h );
        return 1;

    case NB_SESSION_CONFIRM:            /* our SESSION_INITIALIZE is taken */
        if ( s->state != NBX_CALLING || s->step != SS_INIT || !mem_same( s->mac, l->mac, 6 ) ) return 1;
        s->link = link_index( l );
        s->rsn = snum;
        s->peer_max = d2 && d2 < DATA_MAX ? d2 : DATA_MAX;
        s->state = NBX_UP;
        s->step = SS_NONE;
        return 1;
    }

    if ( s->state != NBX_UP || s->link != link_index( l ) ) return 1;

    switch ( cmd ) {
    case NB_DATA_FIRST_MIDDLE:
    case NB_DATA_ONLY_LAST:
        if ( s->rxdone ) return 0;      /* the last message is not read yet */
        if ( s->rxlen + dlen > NB_MSGMAX ) {
            ihdr( h, NB_SESSION_END, 0, 1, 0, 0, s->rsn, s->lsn );
            ctl_queue( l, h );
            s->state = NBX_CLOSED;
            return 1;
        }
        mem_copy( s->rx + s->rxlen, data, dlen );
        s->rxlen += dlen;
        if ( cmd == NB_DATA_FIRST_MIDDLE ) {
            if ( d1 & 0x01 ) {          /* they wait for RECEIVE_CONTINUE */
                ihdr( h, NB_RECEIVE_CONTINUE, 0, 0, rc, 0, s->rsn, s->lsn );
                ctl_queue( l, h );
            }
            return 1;
        }
        s->rxdone = 1;
        s->rxoff = 0;
        if ( d1 & 0x08 ) { s->txbusy = 0; s->txlastout = 0; }  /* our message's ack, carried */
        ihdr( h, NB_DATA_ACK, 0, 0, rc, 0, s->rsn, s->lsn );
        ctl_queue( l, h );
        return 1;

    case NB_DATA_ACK:                   /* already counted when the link took it */
        return 1;

    case NB_SESSION_END:
        s->state = NBX_CLOSED;
        s->err = NBE_NONE;
        return 1;

    case NB_NO_RECEIVE:                 /* they can take no more: wait */
        s->txoff = d2 <= s->txlen ? d2 : s->txlen;
        s->txlastout = 0;
        s->txhold = 1;
        return 1;

    case NB_RECEIVE_OUTSTAND:           /* they can now: go on from here */
        s->txoff = d2 <= s->txlen ? d2 : s->txlen;
        s->txlastout = 0;
        s->txhold = 0;
        s->txresync = 1;
        return 1;
    }
    return 1;                           /* SESSION_ALIVE and the rest */
}

/* ---------------------------------------------------------------- */
/* frames in                                                         */
/* ---------------------------------------------------------------- */

static void ui_in( const u8 *src, const u8 *p, u32 plen )
{
    u8 cmd, lsn;
    u16 d2, xc, rc;
    int n, i;
    SESS *s;

    if ( plen < NB_UIHDR || p[2] != 0xFF || p[3] != 0xEF ) return;
    cmd = p[4];
    d2 = get16le( p + 6 );
    xc = get16le( p + 8 );
    rc = get16le( p + 10 );

    switch ( cmd ) {
    case NB_ADD_NAME_QUERY:             /* somebody wants a name: is it ours? */
        if ( name_up && nb_ours( p + 28 ) >= 0 )
            ui_send( src, NB_ADD_NAME_RESPONSE, 0, 0, rc, 0, p + 28, p + 28 );
        break;

    case NB_ADD_NAME_RESPONSE:          /* the name we are taking is somebody's */
        if ( add_corr && xc == add_corr && nb_ours( p + 12 ) >= 0 ) name_conflict = 1;
        break;

    case NB_NAME_QUERY:
        if ( !name_up ) break;
        n = nb_ours( p + 12 );
        if ( n < 0 ) break;
        lsn = (u8)d2;
        if ( lsn == 0 ) {               /* FIND.NAME: just say it is here */
            ui_send( src, NB_NAME_RECOGNIZED, 0, 0, rc, 0, p + 28, my_names[n] );
            break;
        }
        /* a call: the listen it was answered with already, or a new one */
        s = NULL;
        for ( i = 0; i < SESS_MAX; i++ ) {
            SESS *t = &sess[i];
            if ( t->state == NBX_LISTEN && t->step == SS_ANSWERED &&
                 mem_same( t->mac, src, 6 ) && mem_same( t->rname, p + 28, 16 ) ) { s = t; break; }
        }
        for ( i = 0; s == NULL && i < SESS_MAX; i++ ) {
            SESS *t = &sess[i];
            if ( t->state == NBX_LISTEN && t->step == SS_NONE &&
                 t->suffix == my_names[n][15] ) s = t;
        }
        if ( s == NULL ) {              /* the name, but nobody listening */
            ui_send( src, NB_NAME_RECOGNIZED, 0, 0, rc, 0, p + 28, my_names[n] );
            break;
        }
        if ( s->step == SS_NONE ) {
            s->step = SS_ANSWERED;
            mem_copy( s->mac, src, 6 );
            mem_copy( s->rname, p + 28, 16 );
            s->icorr = new_corr();
            s->qcorr = rc;              /* the caller's NAME_QUERY correlator */
            s->rsn = lsn;               /* the caller's session number */
            s->sent_si = 0;
            s->at = nds_ticks() + T_SESSION;
        }
        ui_send( src, NB_NAME_RECOGNIZED, 0, s->lsn, rc, s->icorr, p + 28, my_names[n] );
        break;

    case NB_NAME_RECOGNIZED:
        if ( find.active && xc == find.corr ) {
            mem_copy( find.mac, src, 6 );
            find.state = NBF_DONE;
            find.active = 0;
            break;
        }
        for ( i = 0; i < SESS_MAX; i++ ) {
            LINK *l;

            s = &sess[i];
            if ( s->state != NBX_CALLING || s->step != SS_QUERY || xc != s->qcorr ) continue;
            if ( (u8)d2 == 0x00 || (u8)d2 == 0xFF ) {
                s->state = NBX_FAIL;
                s->err = NBE_NOLISTEN;
                break;
            }
            s->rsn = (u8)d2;
            s->icorr = rc;
            mem_copy( s->mac, src, 6 );
            l = link_find( src );
            if ( l == NULL ) {
                l = link_new( src );
                if ( l == NULL ) { s->state = NBX_FAIL; s->err = NBE_LINK; break; }
                l->state = LS_SETUP;
                l->tries = 0;
                l->t1_at = nds_ticks() + T1;
                u_send( src, SAP_NB, U_SABME | U_PF, NULL, 0 );
            }
            s->link = link_index( l );
            if ( l->state == LS_UP ) {
                caller_si( l, s );
            } else {
                s->step = SS_LINK;
            }
            break;
        }
        break;
    }
}

static void frame_in( const u8 *f, u32 len )
{
    const u8 *llc, *src = f + 6;
    u32 llen;
    u8 ssap, ctl, pf, nr, cr;
    LINK *l;

    if ( len < ETH_HDR + 3 ) return;
    llen = ( (u32)f[12] << 8 ) | f[13];
    if ( llen > 1500 ) return;          /* not 802.3 */
    if ( llen > len - ETH_HDR ) llen = len - ETH_HDR;
    if ( llen < 3 ) return;
    llc = f + ETH_HDR;
    if ( llc[0] != SAP_NB ) return;
    ssap = llc[1];
    cr = (u8)( ssap & SAP_RESP );
    ctl = llc[2];

    if ( ( ctl & 3 ) == 3 ) {           /* U frame */
        pf = (u8)( ctl & U_PF );
        switch ( ctl & ~U_PF ) {
        case U_UI:
            ui_in( src, llc + 3, llen - 3 );
            return;
        case U_SABME:                   /* they start (or restart) the link */
            if ( cr ) return;
            l = link_find( src );
            if ( l ) link_down( l );    /* a restart: the old sessions are gone */
            l = link_new( src );
            if ( l == NULL ) { u_send( src, SAP_NB | SAP_RESP, (u8)( U_DM | pf ), NULL, 0 ); return; }
            link_reset( l );
            l->state = LS_UP;
            u_send( src, SAP_NB | SAP_RESP, (u8)( U_UA | pf ), NULL, 0 );
            link_is_up( l );
            return;
        case U_UA:
            l = link_find( src );
            if ( l && l->state == LS_SETUP ) {
                link_reset( l );
                l->state = LS_UP;
                link_is_up( l );
            }
            return;
        case U_DISC:
            if ( cr ) return;
            u_send( src, SAP_NB | SAP_RESP, (u8)( ( link_find( src ) ? U_UA : U_DM ) | pf ), NULL, 0 );
            l = link_find( src );
            if ( l ) link_down( l );
            return;
        case U_DM:
        case U_FRMR:
            l = link_find( src );
            if ( l ) link_down( l );
            return;
        case U_XID:
            if ( !cr ) {
                static const u8 xid[3] = { 0x81, 0x03, 0x02 };   /* types 1 and 2, window 1 */
                u_send( src, SAP_NB | SAP_RESP, (u8)( U_XID | pf ), xid, 3 );
            }
            return;
        case U_TEST:
            if ( !cr ) u_send( src, SAP_NB | SAP_RESP, (u8)( U_TEST | pf ), llc + 3, llen - 3 );
            return;
        }
        return;
    }

    if ( llen < 4 ) return;
    nr = (u8)( llc[3] >> 1 );
    pf = (u8)( llc[3] & 1 );
    l = link_find( src );

    if ( ( ctl & 1 ) == 0 ) {           /* I frame */
        u8 ns = (u8)( ctl >> 1 );
        if ( l == NULL || l->state != LS_UP ) {
            u_send( src, SAP_NB | SAP_RESP, (u8)( U_DM | ( pf ? U_PF : 0 ) ), NULL, 0 );
            return;
        }
        link_ack( l, nr );
        if ( ns != l->vr ) {            /* out of order: say where we are */
            s_send( src, SAP_NB | SAP_RESP, l->we_busy ? S_RNR : S_RR, l->vr, pf );
            return;
        }
        if ( !sess_frame( l, llc + 4, llen - 4 ) ) {
            l->we_busy = 1;             /* no room: they send it again later */
            s_send( src, SAP_NB | SAP_RESP, S_RNR, l->vr, pf );
            return;
        }
        l->vr = (u8)( ( l->vr + 1 ) & 0x7F );
        if ( pf ) s_send( src, SAP_NB | SAP_RESP, S_RR, l->vr, 1 );
        else l->ackowed = 1;
        return;
    }

    /* S frame */
    if ( l == NULL || l->state != LS_UP ) {
        if ( !cr && pf ) u_send( src, SAP_NB | SAP_RESP, U_DM | U_PF, NULL, 0 );
        return;
    }
    link_ack( l, nr );
    switch ( ctl & 0x0F ) {
    case S_RR:  l->busy = 0; break;
    case S_RNR:                         /* busy, but there: start counting again */
        l->busy = 1;
        l->t1_at = nds_ticks() + T1;
        if ( cr ) l->tries = 0;
        break;
    case S_REJ:
        l->busy = 0;
        if ( l->unacked ) { link_put_i( l, 0 ); l->t1_at = nds_ticks() + T1; }
        break;
    }
    if ( !cr && pf ) {                  /* a poll: answer where we are */
        s_send( src, SAP_NB | SAP_RESP, l->we_busy ? S_RNR : S_RR, l->vr, 1 );
        l->ackowed = 0;
    }
}

/* ---------------------------------------------------------------- */
/* the pump and the timers                                           */
/* ---------------------------------------------------------------- */

static void pump( void )
{
    u32 answer;
    int budget = 32, i;

    while ( budget-- > 0 ) {
        *(u32 *)rx_buf = bindno;
        if ( nds_niccall( 0, NIC_RECVB, rx_buf, sizeof( rx_buf ), &answer ) != 0 || answer == 0 ) break;
        if ( answer > sizeof( rx_buf ) - 4 ) answer = sizeof( rx_buf ) - 4;
        frame_in( rx_buf + 4, answer );
    }
    for ( i = 0; i < LINK_MAX; i++ ) {
        LINK *l = &links[i];
        if ( !l->used || l->state != LS_UP ) continue;
        link_kick( l );
        if ( l->ackowed ) {             /* nothing went to carry N(R): an RR */
            s_send( l->mac, SAP_NB | SAP_RESP, l->we_busy ? S_RNR : S_RR, l->vr, 0 );
            l->ackowed = 0;
        }
    }
}

static void timers( void )
{
    u32 now = nds_ticks();
    int i;

    for ( i = 0; i < LINK_MAX; i++ ) {
        LINK *l = &links[i];
        if ( !l->used || (long)( now - l->t1_at ) < 0 ) continue;
        if ( l->state == LS_SETUP ) {
            if ( ++l->tries > N_QUERY + 2 ) { link_down( l ); continue; }
            u_send( l->mac, SAP_NB, U_SABME | U_PF, NULL, 0 );
            l->t1_at = now + T1;
        } else if ( l->state == LS_UP && ( l->unacked || l->busy ) ) {
            if ( ++l->tries > N2 ) { link_down( l ); continue; }
            if ( l->unacked ) link_put_i( l, 1 );       /* again, asking for an answer */
            else s_send( l->mac, SAP_NB, S_RR, l->vr, 1 );   /* busy: are you still? */
            l->t1_at = now + T1;
        }
    }

    for ( i = 0; i < SESS_MAX; i++ ) {
        SESS *s = &sess[i];
        if ( s->state == NBX_CALLING ) {
            if ( s->step == SS_QUERY && (long)( now - s->at ) >= 0 ) {
                if ( ++s->tries >= N_QUERY ) { s->state = NBX_FAIL; s->err = NBE_NONAME; continue; }
                ui_send( nb_group, NB_NAME_QUERY, 0, s->lsn, 0, s->qcorr, s->rname, my_names[0] );
                s->at = now + T_QUERY;
            }
            if ( s->state == NBX_CALLING && (long)( now - s->started ) >= T_SESSION ) {
                s->state = NBX_FAIL;
                s->err = s->step == SS_LINK ? NBE_LINK : NBE_TIMEOUT;
            }
        } else if ( s->state == NBX_LISTEN && s->step == SS_ANSWERED && (long)( now - s->at ) >= 0 ) {
            s->step = SS_NONE;          /* they never started it */
        }
    }

    if ( find.active && (long)( now - find.at ) >= 0 ) {
        if ( ++find.tries >= N_QUERY ) { find.state = NBF_FAIL; find.active = 0; }
        else {
            ui_send( nb_group, NB_NAME_QUERY, 0, 0, 0, find.corr, find.name, my_names[0] );
            find.at = now + T_QUERY;
        }
    }
}

/* ---------------------------------------------------------------- */
/* the calls (nd_call)                                               */
/* ---------------------------------------------------------------- */

static SESS *sess_by_handle( u32 handle )
{
    if ( handle < 1 || handle > SESS_MAX ) return NULL;
    if ( sess[handle - 1].state == NBX_FREE ) return NULL;
    return &sess[handle - 1];
}

/* a free session with its two message buffers */
static SESS *sess_open( void )
{
    int i;
    for ( i = 0; i < SESS_MAX; i++ ) {
        SESS *s = &sess[i];
        if ( s->state != NBX_FREE ) continue;
        mem_zero( s, sizeof( SESS ) );
        s->rx = (u8 *)nds_alloc( NB_MSGMAX );
        s->tx = (u8 *)nds_alloc( NB_MSGMAX );
        if ( s->rx == NULL || s->tx == NULL ) { sess_buffers_free( s ); return NULL; }
        s->lsn = lsn_alloc();
        s->started = nds_ticks();
        return s;
    }
    return NULL;
}

static int name_ok( const char *name )
{
    u32 n;
    for ( n = 0; name[n]; n++ ) if ( n >= 15 ) return 0;
    return n > 0;
}

static int do_ifstat( void *buf, u32 len, u32 *answer )
{
    NB_IF *info = (NB_IF *)buf;
    int i;

    if ( len < NBIF_LEN ) return ERR_BADLEN;
    mem_zero( info, NBIF_LEN );
    str_copy( info->name, my_name, sizeof( info->name ) );
    mem_copy( info->mac, my_mac, 6 );
    info->up = name_up;
    for ( i = 0; i < SESS_MAX; i++ ) if ( sess[i].state != NBX_FREE ) info->sessions++;
    str_copy( info->card, card_name, sizeof( info->card ) );
    *answer = NBIF_LEN;
    return 0;
}

static int do_find( void *buf, u32 len, u32 *answer )
{
    NB_FINDREQ *req = (NB_FINDREQ *)buf;
    u8 raw[16];

    if ( len < NBFIND_LEN ) return ERR_BADLEN;
    req->name[15] = 0;
    if ( !name_ok( req->name ) ) return ERR_BADDATA;
    nb_raw( raw, req->name, req->suffix );
    if ( !find.active && find.state != NBF_PENDING && mem_same( raw, find.name, 16 ) ) {
        req->state = find.state;        /* the answer to the last one */
        mem_copy( req->mac, find.mac, 6 );
        find.state = NBF_PENDING;       /* told once: the next call asks again */
        mem_zero( find.name, 16 );
        *answer = req->state;
        return 0;
    }
    if ( !find.active || !mem_same( raw, find.name, 16 ) ) {
        mem_copy( find.name, raw, 16 );
        find.corr = new_corr();
        find.tries = 0;
        find.state = NBF_PENDING;
        find.active = 1;
        find.at = nds_ticks() + T_QUERY;
        ui_send( nb_group, NB_NAME_QUERY, 0, 0, 0, find.corr, find.name, my_names[0] );
    }
    req->state = NBF_PENDING;
    *answer = NBF_PENDING;
    return 0;
}

static int do_call( void *buf, u32 len, u32 *answer )
{
    NB_NAMEREQ *req = (NB_NAMEREQ *)buf;
    SESS *s;

    if ( len < NBNAME_LEN ) return ERR_BADLEN;
    req->name[15] = 0;
    if ( !name_ok( req->name ) ) return ERR_BADDATA;
    if ( !name_up ) return ERR_GENERAL;
    s = sess_open();
    if ( s == NULL ) return ERR_GENERAL;
    nb_raw( s->rname, req->name, req->suffix );
    s->state = NBX_CALLING;
    s->step = SS_QUERY;
    s->qcorr = new_corr();
    s->at = nds_ticks() + T_QUERY;
    ui_send( nb_group, NB_NAME_QUERY, 0, s->lsn, 0, s->qcorr, s->rname, my_names[0] );
    *answer = (u32)( s - sess ) + 1;
    return 0;
}

static int do_listen( void *buf, u32 len, u32 *answer )
{
    NB_NAMEREQ *req = (NB_NAMEREQ *)buf;
    SESS *s;

    if ( len < NBNAME_LEN ) return ERR_BADLEN;
    if ( req->suffix != my_names[0][15] && req->suffix != my_names[1][15] ) return ERR_BADDATA;
    if ( !name_up ) return ERR_GENERAL;
    s = sess_open();
    if ( s == NULL ) return ERR_GENERAL;
    s->state = NBX_LISTEN;
    s->step = SS_NONE;
    s->suffix = req->suffix;
    *answer = (u32)( s - sess ) + 1;
    return 0;
}

/* NB_SENDP (part) and NB_SEND (the last part, and go) */
static int do_send( void *buf, u32 len, u32 *answer, int part )
{
    NB_IO *io = (NB_IO *)buf;
    SESS *s;

    if ( len < NBIO_HDR ) return ERR_BADLEN;
    s = sess_by_handle( io->handle );
    if ( s == NULL ) return ERR_BADDATA;
    if ( io->len > len - NBIO_HDR ) return ERR_BADLEN;
    if ( s->state != NBX_UP ) return ERR_GENERAL;
    if ( s->txbusy ) { *answer = 0; return 0; }
    if ( s->txbuilt + io->len > NB_MSGMAX ) { s->txbuilt = 0; return ERR_BADLEN; }
    mem_copy( s->tx + s->txbuilt, (u8 *)buf + NBIO_HDR, io->len );
    s->txbuilt += io->len;
    if ( part ) { *answer = io->len; return 0; }
    s->txlen = s->txbuilt;
    s->txbuilt = 0;
    s->txoff = 0;
    s->txlastout = 0;
    s->txhold = 0;
    s->txbusy = 1;
    link_kick( &links[s->link] );
    *answer = io->len;
    return 0;
}

static int do_recv( void *buf, u32 len, u32 *answer )
{
    NB_IO *io = (NB_IO *)buf;
    SESS *s;
    u32 n;

    if ( len < NBIO_HDR ) return ERR_BADLEN;
    s = sess_by_handle( io->handle );
    if ( s == NULL ) return ERR_BADDATA;
    if ( !s->rxdone ) { io->len = 0; *answer = 0; return 0; }
    n = s->rxlen - s->rxoff;
    if ( n > len - NBIO_HDR ) n = len - NBIO_HDR;
    mem_copy( (u8 *)buf + NBIO_HDR, s->rx + s->rxoff, n );
    s->rxoff += n;
    if ( s->rxoff == s->rxlen ) {       /* all of it read: room for the next */
        s->rxdone = 0;
        s->rxlen = 0;
        s->rxoff = 0;
        if ( s->state == NBX_UP && links[s->link].used && links[s->link].we_busy ) {
            LINK *l = &links[s->link];
            l->we_busy = 0;
            s_send( l->mac, SAP_NB | SAP_RESP, S_RR, l->vr, 0 );
        }
    }
    io->len = n;
    *answer = n;
    return 0;
}

static int do_state( void *buf, u32 len, u32 *answer )
{
    NB_STAT *st = (NB_STAT *)buf;
    SESS *s;

    if ( len < NBSTAT_LEN ) return ERR_BADLEN;
    s = sess_by_handle( st->handle );
    if ( s == NULL ) {
        st->state = NBX_FREE;
        st->rxready = st->txbusy = st->err = 0;
        *answer = NBX_FREE;
        return 0;
    }
    st->state = s->state;
    st->rxready = s->rxdone ? s->rxlen - s->rxoff : 0;
    st->txbusy = s->txbusy;
    st->err = s->err;
    *answer = s->state;
    return 0;
}

static int do_hangup( void *buf, u32 len )
{
    SESS *s;
    u8 h[NB_IHDR];

    if ( len < 4 ) return ERR_BADLEN;
    s = sess_by_handle( *(u32 *)buf );
    if ( s == NULL ) return ERR_BADDATA;
    if ( s->state == NBX_UP && links[s->link].used ) {
        ihdr( h, NB_SESSION_END, 0, 0, 0, 0, s->rsn, s->lsn );
        ctl_queue( &links[s->link], h );
        link_kick( &links[s->link] );
    }
    sess_buffers_free( s );
    s->state = NBX_FREE;
    return 0;
}

int __cdecl nd_call( u32 fn, void *buf, u32 len, u32 *answer )
{
    int result;

    pumping = 1;
    pump();
    timers();                           /* a program polling us holds the tick off */

    switch ( fn ) {
    case NB_IFSTAT: result = do_ifstat( buf, len, answer ); break;
    case NB_FIND:   result = do_find( buf, len, answer ); break;
    case NB_CALL:   result = do_call( buf, len, answer ); break;
    case NB_LISTEN: result = do_listen( buf, len, answer ); break;
    case NB_SEND:   result = do_send( buf, len, answer, 0 ); break;
    case NB_SENDP:  result = do_send( buf, len, answer, 1 ); break;
    case NB_RECV:   result = do_recv( buf, len, answer ); break;
    case NB_STATE:  result = do_state( buf, len, answer ); break;
    case NB_HANGUP: result = do_hangup( buf, len ); if ( !result ) *answer = 0; break;
    default:        result = ERR_FUNC; break;
    }

    pumping = 0;
    return result;
}

/* ---------------------------------------------------------------- */
/* the DEVICE= line and INIT                                         */
/* ---------------------------------------------------------------- */

/* /NAME=, kept in upper case; 0 if it is not a name */
static int parse_line( const char *line, int *bad )
{
    const char *p = line;
    u32 n;

    *bad = 0;
    while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' ) p++;
    for ( ; *p && *p != '\r' && *p != '\n'; p++ ) {
        if ( *p != '/' ) continue;
        if ( ( p[1] | 0x20 ) == 'n' && ( p[2] | 0x20 ) == 'a' && ( p[3] | 0x20 ) == 'm' &&
             ( p[4] | 0x20 ) == 'e' && ( p[5] == '=' || p[5] == ':' ) ) {
            p += 6;
            for ( n = 0; *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '/'; p++ ) {
                u8 ch = (u8)*p;
                if ( !( ( ch >= 'A' && ch <= 'Z' ) || ( ch >= 'a' && ch <= 'z' ) || ( ch >= '0' && ch <= '9' ) ||
                        ch == '-' || ch == '_' || ch == '!' || ch == '#' || ch == '$' || ch == '%' ||
                        ch == '&' || ch == '\'' || ch == '(' || ch == ')' || ch == '^' || ch == '{' ||
                        ch == '}' || ch == '~' ) || n >= 15 ) { *bad = 1; return 0; }
                my_name[n++] = (char)upper( ch );
            }
            my_name[n] = 0;
            if ( n == 0 ) { *bad = 1; return 0; }
            p--;
        } else {
            nds_print( "NetBEUI: only /NAME= is a NETBEUI.SYS switch\r\n" );
        }
    }
    return my_name[0] != 0;
}

/* no /NAME=: TCPIP.SYS's, or one made from the card's address */
static void name_default( void )
{
    SOCK_IF info;
    u32 answer;
    static const char hex[] = "0123456789ABCDEF";
    int i;

    mem_zero( &info, SOCKIF_LEN2 );
    if ( nds_sockcall( SOCK_IFSTAT, &info, SOCKIF_LEN2, &answer ) == 0 && answer >= SOCKIF_LEN2 &&
         info.nbname[0] ) {
        str_copy( my_name, info.nbname, sizeof( my_name ) );
        return;
    }
    str_copy( my_name, "PMDOS-", sizeof( my_name ) );
    for ( i = 0; i < 3; i++ ) {
        my_name[6 + i * 2] = hex[my_mac[3 + i] >> 4];
        my_name[7 + i * 2] = hex[my_mac[3 + i] & 15];
    }
    my_name[12] = 0;
}

int __cdecl nd_init( const char *line )
{
    NIC_INFOBLOCK info;
    u8 req[NICB_LEN];
    u32 answer, start;
    int bad, n, round;
    char text[64];

    if ( nds_niccount() == 0 ) {
        nds_print( "NetBEUI: no network card is loaded - load the card driver first\r\n" );
        return 1;
    }
    if ( nds_niccall( 0, NIC_INFO, &info, sizeof( info ), &answer ) != 0 ) {
        nds_print( "NetBEUI: the card would not say what it is\r\n" );
        return 1;
    }
    mem_copy( my_mac, info.mac, 6 );
    str_copy( card_name, info.name, sizeof( card_name ) );

    if ( !parse_line( line, &bad ) ) {
        if ( bad ) {
            nds_print( "NetBEUI: /NAME= must be 1 to 15 letters and digits\r\n" );
            return 1;
        }
        name_default();
    }
    nb_raw( my_names[0], my_name, 0x00 );
    nb_raw( my_names[1], my_name, 0x20 );

    /* every frame for SAP F0, apart from TCPIP.SYS's */
    mem_zero( req, sizeof( req ) );
    req[2] = SAP_NB;
    if ( nds_niccall( 0, NIC_BIND, req, NICB_LEN, &bindno ) != 0 ) {
        nds_print( "NetBEUI: the card driver cannot share the card - it needs NIC_BIND\r\n" );
        return 1;
    }

    nds_tickhook();
    running = 1;

    /* take the two names: ask three times, half a second apart, whether
       anybody has them */
    add_corr = new_corr();
    for ( round = 0; round < N_QUERY && !name_conflict; round++ ) {
        for ( n = 0; n < 2; n++ )
            ui_send( nb_group, NB_ADD_NAME_QUERY, 0, 0, 0, add_corr, NULL, my_names[n] );
        start = nds_ticks();
        while ( !name_conflict && nds_ticks() - start < T_QUERY ) ;
    }
    if ( name_conflict ) {
        running = 0;
        nds_tickunhook();
        str_copy( text, "NetBEUI: ", sizeof( text ) );
        str_copy( text + 9, my_name, sizeof( text ) - 9 );
        n = 0; while ( text[n] ) n++;
        str_copy( text + n, " is already in use on the network\r\n", sizeof( text ) - n );
        nds_print( text );
        return 1;
    }
    add_corr = 0;
    name_up = 1;

    str_copy( text, "NetBEUI name: ", sizeof( text ) );
    str_copy( text + 14, my_name, sizeof( text ) - 14 );
    n = 0; while ( text[n] ) n++;
    str_copy( text + n, "\r\n", sizeof( text ) - n );
    nds_print( text );
    return 0;
}

void __cdecl nd_park( int back )
{
    running = (u8)( back ? 1 : 0 );
}

void __cdecl nd_halt( void )
{
    running = 0;
}

int __cdecl nd_irq( u32 line )
{
    (void)line;
    return 0;
}

void __cdecl nd_tick( void )
{
    if ( !running || pumping ) return;
    pumping = 1;
    pump();
    timers();
    pumping = 0;
}

/* ---------------------------------------------------------------- */
/* the driver table (NDSTART.ASM's veneers)                          */
/* ---------------------------------------------------------------- */

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NETBIOS,
    { 'N', 'E', 'T', 'B', 'E', 'U', 'I', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
