/*
 * TCPIP.C - TCPIP.SYS, the PM-DOS TCP/IP stack
 *
 * The fourth step of the network: a 32-bit DEVICE= driver that rides
 * on a loaded card driver (it calls one through NDS_NICCALL, the same
 * path a program takes to a driver through 21h/F1h AL=25h) and gives
 * programs sockets through 21h/F1h AL=26h (INC\SOCKET.H).  It is the
 * stack NICTEST built by hand to prove a card: ARP, IPv4, ICMP, UDP,
 * DHCP and DNS, and a small but honest TCP - a handshake, a window,
 * retransmission and an orderly close.
 *
 * WHERE IT RUNS is where every 32-bit driver runs: ring 0, flat, the
 * kernel's memory, called at INIT, at a park, at a halt, on every
 * clock tick, and whenever a program makes a socket call.  It touches
 * no hardware of its own - the card driver does that - so it has no
 * interrupt of its own; the clock tick and the socket calls are when
 * it works.  DHCP and the first ARP for the gateway are done at INIT,
 * which is the one entry that may take its time.
 *
 * NOTHING BLOCKS once it is up.  A tick drains whatever the card has
 * received and runs the timers; a socket call drains too, then does
 * what it can at once and returns.  The waiting - for a handshake to
 * finish, for a byte to arrive - is the caller's, in TOOLS\SOCKLIB.
 *
 * AN ADDRESS IS A u32 HERE, host order (10.0.2.2 is 0x0A000202), and a
 * port is a u16 in the natural order; the big-endian wire fields are
 * built and read a byte at a time (put16/get16).  So there is no need
 * to care which way the processor stores a word.
 *
 * Compiled like a card driver: wcc386 -mf -3r -s -zl, behind
 * DRV\NDSTART.ASM - see BUILD.CMD.
 */
#define ND_DRIVER
#include "ndrv.h"
#include "nic.h"
#include "socket.h"
#include "tlshs.h"            /* kernel-side TLS: the vendored NSTLS core */
#include "rng.h"

#define NULL ((void *)0)

/* ---------------------------------------------------------------- */
/* the wire                                                          */
/* ---------------------------------------------------------------- */

#define ETH_HDR      14
#define ETH_ARP      0x0806
#define ETH_IP       0x0800

#define ARP_REQUEST  1
#define ARP_REPLY    2

#define IP_ICMP      1
#define IP_TCP       6
#define IP_UDP       17

#define ICMP_ECHOREP 0
#define ICMP_ECHOREQ 8

#define TCP_FIN      0x01
#define TCP_SYN      0x02
#define TCP_RST      0x04
#define TCP_PSH      0x08
#define TCP_ACK      0x10

#define TCP_MSS      1460
#define IP_BCAST     0xFFFFFFFFL

/* the internal TCP states (SOCKET.H's SS_* are what a program sees) */
#define T_CLOSED     0
#define T_LISTEN     1
#define T_SYNSENT    2
#define T_SYNRCVD    3
#define T_ESTAB      4
#define T_FINWAIT1   5          /* our FIN sent, not yet acked */
#define T_FINWAIT2   6          /* our FIN acked, waiting the peer's */
#define T_CLOSEWAIT  7          /* peer's FIN seen, app may still send */
#define T_CLOSING    8          /* both FINs out, neither acked */
#define T_LASTACK    9          /* our FIN after the peer's, waiting ack */
#define T_TIMEWAIT   10

/* ---------------------------------------------------------------- */
/* the sockets                                                       */
/* ---------------------------------------------------------------- */

#define SOCK_MAX     8
#define RXCAP        4096
#define TXCAP        4096

typedef struct {
    u8   used;
    u8   type;                  /* SOCK_STREAM or SOCK_DGRAM */
    u8   tstate;                /* T_* */
    u8   err;                   /* NET_FAIL when the peer reset us */

    u32  lip, rip;              /* local and remote address */
    u16  lport, rport;

    /* TCP sequence space */
    u32  iss;                   /* our initial sequence number */
    u32  snd_una;               /* oldest unacked - the first txbuf byte */
    u32  snd_nxt;               /* next sequence to send */
    u32  snd_wnd;               /* what the peer will take */
    u32  irs;                   /* the peer's initial sequence number */
    u32  rcv_nxt;               /* next sequence expected */

    u8  *txbuf;                 /* bytes queued to send, from snd_una */
    u32  txstart, txlen;
    u8  *rxbuf;                 /* bytes arrived, waiting to be read */
    u32  rxstart, rxlen;

    u8   finsent;               /* our FIN has gone out */
    u32  finseq;                /* ...occupying this sequence */
    u8   finacked;
    u8   closing;               /* the app has asked to close */

    u32  rto;                   /* the retransmit timeout, in ticks */
    u32  rto_at;                /* when to retransmit next (0: no timer) */
    u8   retries;
    u32  wait_at;               /* TIME_WAIT / handshake give-up deadline */

    /* a listening socket keeps the port; children carry the connection */
    u16  listenport;
    u8   listening;

    /* TLS (SOCK_TLS): the handshake and record crypto run kernel-side over
       the raw TCP stream.  The TLS state rides above tstate: tls_state is
       TLSX_* while tstate is the TCP connection underneath. */
    u8   tls;                   /* 1: a TLS socket */
    u8   tls_state;             /* TLSX_* */
    u8   tls_eof;               /* the peer sent close_notify */
    tls_conn *tc;               /* the TLS connection (arena, from tls_new) */
    nst_rng  *rng;              /* its RNG (arena; must outlive tc) */
    u8  *trx;                   /* one inbound record, being assembled */
    u32  trxlen;
    u8  *tapp;                  /* decrypted plaintext waiting for the app */
    u32  tapstart, taplen;
    char host[128];             /* server name: SNI and the cert to match */
} SOCKET;

/* tls_state */
#define TLSX_IDLE    0          /* TCP connecting/established, TLS not begun */
#define TLSX_HS      1          /* the handshake is going */
#define TLSX_UP      2          /* the handshake is done; plaintext may pass */
#define TLSX_FAIL    3          /* the handshake failed (cert or protocol) */

#define TLS_REC_MAX  18432           /* largest record body (matches NSTLS REC_MAX) */
#define TLS_RECCAP   (TLS_REC_MAX + 8)  /* room for one record (header + body) */
#define TLS_STK_SIZE 32768           /* the crypto's private stack (from the pool) */
#define TLS_POOL     0x50000UL        /* the TLS heap, grabbed at load (320K) */

static SOCKET sock[SOCK_MAX];

/* the trust bundle (SOCK_SETCA), the crypto's big stack, and a record-sized
   scratch for SOCK_SEND, all out of the kernel arena */
static u8  *tls_ca;             /* concatenated DER roots, or NULL */
static u32  tls_calen;          /* bytes present so far */
static u32  tls_catotal;        /* bytes the bundle will be */
static u8  *tls_stk;            /* the crypto stack's base */
static u32  tls_stktop;         /* ...and its top (ESP starts here) */
static u8   tls_scratch[TLS_RECCAP];   /* one outbound record */

extern u32 __cdecl tls_run( void *newtop, void *fn, void *arg );
extern u32 tlsrt_init( u32 want );     /* grab the TLS heap pool; 0 if none */
extern void *malloc( unsigned bytes ); /* the TLS pool (TLSRT.C), not DOS */
extern void  free( void *block );

#ifndef TLS_CT_APPLICATION_DATA
#define TLS_CT_ALERT             21
#define TLS_CT_APPLICATION_DATA  23
#endif

static void tls_release( SOCKET *s );
static void tls_service( void );

/* ---------------------------------------------------------------- */
/* the interface                                                     */
/* ---------------------------------------------------------------- */

static u8   if_mac[6];
static u32  if_ip, if_mask, if_gw, if_dns;
static u32  if_lease;
static u8   if_up;
static u8   if_dhcp;            /* DHS_* */
static char if_name[32];

static u16  ip_id = 1;
static u8   running;
static volatile u8 pumping;     /* a pump is in progress: a tick stands off */

static u8   tx_frame[1600];
static u8   rx_frame[1600];

/* Frames the stack does not own (not IP, not ARP - NetBEUI and the like)
   are held in a small ring for the WfW bridge to collect through
   SOCK_RXFOREIGN and indicate to its bound protocol.  Without a bridge
   nothing drains the ring; it simply drops the oldest and stays small. */
#define FGN_N    4                      /* a power of two; holds FGN_N-1 frames */
#define FGN_CAP  1518
static u8   fgn_buf[FGN_N][FGN_CAP];
static u16  fgn_len[FGN_N];
static u8   fgn_head, fgn_tail;         /* head = next to read, tail = next to fill */

/* the one echo and the one name lookup the stack carries at a time */
static struct {
    u8   active;
    u32  ip;
    u16  id, seq;
    u32  state;
    u32  sent_at;
    u32  rtt;
    u32  give_up;
    u8   tries;
} ping;

static struct {
    u8   active;
    u32  ip;
    u32  state;
    u16  id;
    u16  lport;
    u32  give_up;
    u8   tries;
    char name[128];
} res;

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

/* the foreign-frame ring (declared up by rx_frame): hold a frame the stack
   does not own, give the oldest back to the bridge on request */
static void fgn_put( const u8 *frame, u32 len )
{
    u8 next;

    if ( len > FGN_CAP ) return;        /* a foreign frame this large is not ours */
    next = (u8)( ( fgn_tail + 1 ) & ( FGN_N - 1 ) );
    if ( next == fgn_head ) {           /* ring full: drop the oldest */
        fgn_head = (u8)( ( fgn_head + 1 ) & ( FGN_N - 1 ) );
    }
    mem_copy( fgn_buf[fgn_tail], frame, len );
    fgn_len[fgn_tail] = (u16)len;
    fgn_tail = next;
}

static u32 fgn_get( u8 *buf, u32 max )
{
    u32 len;

    if ( fgn_head == fgn_tail ) return 0;       /* empty */
    len = fgn_len[fgn_head];
    if ( len > max ) len = max;
    mem_copy( buf, fgn_buf[fgn_head], len );
    fgn_head = (u8)( ( fgn_head + 1 ) & ( FGN_N - 1 ) );
    return len;
}

static void str_copy( char *to, const char *from, u32 max )
{
    u32 i;
    for ( i = 0; i + 1 < max && from[i]; i++ ) to[i] = from[i];
    to[i] = 0;
}

static int str_same( const char *one, const char *two )
{
    while ( *one && *one == *two ) { one++; two++; }
    return *one == *two;
}

static void put16( u8 *buf, u16 value )
{
    buf[0] = (u8)( value >> 8 );
    buf[1] = (u8)value;
}

static void put32( u8 *buf, u32 value )
{
    buf[0] = (u8)( value >> 24 );
    buf[1] = (u8)( value >> 16 );
    buf[2] = (u8)( value >> 8 );
    buf[3] = (u8)value;
}

static u16 get16( const u8 *buf )
{
    return (u16)( ( (u16)buf[0] << 8 ) | buf[1] );
}

static u32 get32( const u8 *buf )
{
    return ( (u32)buf[0] << 24 ) | ( (u32)buf[1] << 16 ) |
           ( (u32)buf[2] << 8 ) | buf[3];
}

/* the Internet checksum, over data read as big-endian 16-bit words */
static u32 cksum_add( const u8 *buf, u32 len, u32 sum )
{
    u32 i;
    for ( i = 0; i + 1 < len; i += 2 ) sum += ( (u32)buf[i] << 8 ) | buf[i + 1];
    if ( i < len ) sum += (u32)buf[i] << 8;
    return sum;
}

static u16 cksum_done( u32 sum )
{
    while ( sum >> 16 ) sum = ( sum & 0xFFFF ) + ( sum >> 16 );
    return (u16)( ~sum & 0xFFFF );
}

/* a pseudo-random seed for sequence numbers and ids, out of the clock */
static u32 rnd_state;
static u32 rnd( void )
{
    rnd_state = rnd_state * 1103515245L + 12345L;
    return rnd_state;
}

/* ---------------------------------------------------------------- */
/* the card, reached through the kernel                              */
/* ---------------------------------------------------------------- */

static int nic_send( const u8 *frame, u32 len )
{
    u32 answer;
    return nds_niccall( 0, NIC_SEND, (void *)frame, len, &answer ) == 0;
}

static u32 nic_recv( u8 *buf, u32 max )
{
    u32 answer;
    if ( nds_niccall( 0, NIC_RECV, buf, max, &answer ) != 0 ) return 0;
    return answer;
}

/* drain the card's hardware ring into its software queue - what the
   IRQ and the clock tick do natively.  The WfW bridge has no IRQ and no
   tick of its own, so SOCK_PUMP asks for this before it pumps */
static void nic_service( void )
{
    u32 answer;
    nds_niccall( 0, NIC_SERVICE, (void *)0, 0, &answer );
}

/* ---------------------------------------------------------------- */
/* ARP: a small cache, and who-has for the gateway                   */
/* ---------------------------------------------------------------- */

#define ARP_N   8

static struct { u32 ip; u8 mac[6]; u8 valid; u32 at; } arp[ARP_N];

static u8 *arp_find( u32 ip )
{
    int i;
    for ( i = 0; i < ARP_N; i++ ) {
        if ( arp[i].valid && arp[i].ip == ip ) return arp[i].mac;
    }
    return NULL;
}

static void arp_put( u32 ip, const u8 *mac )
{
    int i, slot = 0;
    u32 oldest = 0xFFFFFFFFL;

    for ( i = 0; i < ARP_N; i++ ) {
        if ( arp[i].valid && arp[i].ip == ip ) { slot = i; break; }
        if ( !arp[i].valid ) { slot = i; break; }
        if ( arp[i].at < oldest ) { oldest = arp[i].at; slot = i; }
    }
    arp[slot].ip = ip;
    mem_copy( arp[slot].mac, mac, 6 );
    arp[slot].valid = 1;
    arp[slot].at = nds_ticks();
}

static void arp_send( u16 op, u32 target_ip, const u8 *target_mac )
{
    u8 *frame = tx_frame;
    static const u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    mem_copy( frame, op == ARP_REQUEST ? bcast : target_mac, 6 );
    mem_copy( frame + 6, if_mac, 6 );
    put16( frame + 12, ETH_ARP );
    put16( frame + 14, 1 );             /* Ethernet */
    put16( frame + 16, ETH_IP );        /* IPv4 */
    frame[18] = 6;
    frame[19] = 4;
    put16( frame + 20, op );
    mem_copy( frame + 22, if_mac, 6 );
    put32( frame + 28, if_ip );
    if ( op == ARP_REPLY ) mem_copy( frame + 32, target_mac, 6 );
    else mem_zero( frame + 32, 6 );
    put32( frame + 38, target_ip );
    nic_send( frame, 42 );
}

static void arp_request( u32 ip )
{
    arp_send( ARP_REQUEST, ip, NULL );
}

static void arp_input( const u8 *frame, u32 len )
{
    u16 op;
    u32 sender_ip, target_ip;

    if ( len < 42 ) return;
    if ( get16( frame + 16 ) != ETH_IP ) return;
    op = get16( frame + 20 );
    sender_ip = get32( frame + 28 );
    target_ip = get32( frame + 38 );

    arp_put( sender_ip, frame + 22 );   /* learn the sender either way */

    if ( op == ARP_REQUEST && if_ip && target_ip == if_ip ) {
        arp_send( ARP_REPLY, sender_ip, frame + 22 );
    }
}

/* ---------------------------------------------------------------- */
/* IP out: the headers around a payload already placed at +34        */
/* ---------------------------------------------------------------- */

static int same_subnet( u32 ip )
{
    return if_mask && ( ( ip ^ if_ip ) & if_mask ) == 0;
}

static int ip_send( u32 dst, u8 proto, u32 plen )
{
    u8 *mac, *ih = tx_frame + ETH_HDR;
    u32 nexthop;
    static const u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    if ( dst == IP_BCAST ) {
        mac = (u8 *)bcast;
    } else {
        nexthop = same_subnet( dst ) ? dst : if_gw;
        mac = arp_find( nexthop );
        if ( mac == NULL ) { arp_request( nexthop ); return 0; }
    }
    mem_copy( tx_frame, mac, 6 );
    mem_copy( tx_frame + 6, if_mac, 6 );
    put16( tx_frame + 12, ETH_IP );

    ih[0] = 0x45;
    ih[1] = 0;
    put16( ih + 2, (u16)( 20 + plen ) );
    put16( ih + 4, ip_id++ );
    put16( ih + 6, 0x4000 );            /* don't fragment */
    ih[8] = 64;
    ih[9] = proto;
    put16( ih + 10, 0 );
    put32( ih + 12, if_ip );
    put32( ih + 16, dst );
    put16( ih + 10, cksum_done( cksum_add( ih, 20, 0 ) ) );

    return nic_send( tx_frame, ETH_HDR + 20 + plen );
}

/* the checksum over the pseudo-header and the L4 data at +34 */
static u16 l4_cksum( u32 src, u32 dst, u8 proto, u32 plen )
{
    u8 pseudo[12];
    u32 sum;

    put32( pseudo, src );
    put32( pseudo + 4, dst );
    pseudo[8] = 0;
    pseudo[9] = proto;
    put16( pseudo + 10, (u16)plen );
    sum = cksum_add( pseudo, 12, 0 );
    sum = cksum_add( tx_frame + 34, plen, sum );
    return cksum_done( sum );
}

/* ---------------------------------------------------------------- */
/* UDP                                                               */
/* ---------------------------------------------------------------- */

static int udp_send( u32 dst, u16 dport, u16 sport, const u8 *data, u32 dlen )
{
    u8 *uh = tx_frame + 34;
    u16 ck;

    put16( uh, sport );
    put16( uh + 2, dport );
    put16( uh + 4, (u16)( 8 + dlen ) );
    put16( uh + 6, 0 );
    /* a caller with the payload already in place (DHCP, DNS) passes
       data == NULL: leave what is at uh+8 alone */
    if ( dlen && data ) mem_copy( uh + 8, data, dlen );
    ck = l4_cksum( if_ip, dst, IP_UDP, 8 + dlen );
    if ( ck == 0 ) ck = 0xFFFF;
    put16( uh + 6, ck );
    return ip_send( dst, IP_UDP, 8 + dlen );
}

/* forward */
static void dhcp_input( const u8 *bootp, u32 len );
static void dns_input( const u8 *dns, u32 len );
static SOCKET *udp_socket_for( u16 port );
static void udp_queue( SOCKET *s, u32 src, u16 sport, const u8 *data, u32 dlen );

static void udp_input( u32 src, const u8 *uh, u32 len )
{
    u16 sport, dport, ulen;
    const u8 *data;
    u32 dlen;
    SOCKET *s;

    if ( len < 8 ) return;
    sport = get16( uh );
    dport = get16( uh + 2 );
    ulen = get16( uh + 4 );
    if ( ulen < 8 || ulen > len ) ulen = (u16)len;
    data = uh + 8;
    dlen = ulen - 8;

    if ( dport == 68 ) { dhcp_input( uh, len ); return; }
    if ( res.active && dport == res.lport && sport == 53 ) {
        dns_input( data, dlen );
        return;
    }
    s = udp_socket_for( dport );
    if ( s ) udp_queue( s, src, sport, data, dlen );
}

/* ---------------------------------------------------------------- */
/* ICMP                                                              */
/* ---------------------------------------------------------------- */

static void icmp_echo( u32 dst, u16 id, u16 seq )
{
    u8 *ic = tx_frame + 34;
    u32 i, plen = 8 + 32;

    ic[0] = ICMP_ECHOREQ;
    ic[1] = 0;
    put16( ic + 2, 0 );
    put16( ic + 4, id );
    put16( ic + 6, seq );
    for ( i = 0; i < 32; i++ ) ic[8 + i] = (u8)( 'a' + ( i & 15 ) );
    put16( ic + 2, cksum_done( cksum_add( ic, plen, 0 ) ) );
    ip_send( dst, IP_ICMP, plen );
}

static void icmp_input( u32 src, const u8 *ic, u32 len )
{
    if ( len < 8 ) return;
    if ( ic[0] == ICMP_ECHOREQ ) {
        /* answer a ping to us, in place: swap type to a reply */
        u8 *out = tx_frame + 34;
        u32 plen = len;
        if ( plen > sizeof( tx_frame ) - 34 ) plen = sizeof( tx_frame ) - 34;
        mem_copy( out, ic, plen );
        out[0] = ICMP_ECHOREP;
        put16( out + 2, 0 );
        put16( out + 2, cksum_done( cksum_add( out, plen, 0 ) ) );
        ip_send( src, IP_ICMP, plen );
        return;
    }
    if ( ic[0] == ICMP_ECHOREP && ping.active ) {
        if ( get16( ic + 4 ) == ping.id && get16( ic + 6 ) == ping.seq ) {
            u32 now = nds_ticks();
            ping.rtt = ( now - ping.sent_at ) * 55;     /* ~55ms a tick */
            ping.state = NET_DONE;
            ping.active = 0;
        }
    }
}

/* ---------------------------------------------------------------- */
/* IP in                                                             */
/* ---------------------------------------------------------------- */

static void tcp_input( u32 src, u32 dst, const u8 *th, u32 len );

static void ip_input( const u8 *frame, u32 len )
{
    const u8 *ih = frame + ETH_HDR;
    u32 ihl, total, plen, src, dst;
    const u8 *l4;

    if ( len < ETH_HDR + 20 ) return;
    if ( ( ih[0] >> 4 ) != 4 ) return;
    ihl = ( ih[0] & 0x0F ) * 4;
    if ( ihl < 20 || len < ETH_HDR + ihl ) return;
    total = get16( ih + 2 );
    if ( total < ihl || total > len - ETH_HDR ) total = len - ETH_HDR;
    plen = total - ihl;
    src = get32( ih + 12 );
    dst = get32( ih + 16 );
    l4 = ih + ihl;

    switch ( ih[9] ) {
    case IP_ICMP: icmp_input( src, l4, plen ); break;
    case IP_UDP:  udp_input( src, l4, plen ); break;
    case IP_TCP:  tcp_input( src, dst, l4, plen ); break;
    }
}

static void eth_input( const u8 *frame, u32 len )
{
    u16 type;

    if ( len < ETH_HDR ) return;
    type = get16( frame + 12 );
    if ( type == ETH_ARP ) arp_input( frame, len );
    else if ( type == ETH_IP ) ip_input( frame, len );
    else fgn_put( frame, len );         /* NetBEUI etc.: hold for the WfW bridge */
}

/* ---------------------------------------------------------------- */
/* DHCP - done at INIT, where waiting is allowed                     */
/* ---------------------------------------------------------------- */

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

#define DH_OFF        0
#define DH_DISCOVER   1          /* a DISCOVER is out, waiting for an OFFER */
#define DH_REQUEST    2          /* a REQUEST is out, waiting for an ACK */
#define DH_BOUND      3

static u32 dhcp_xid;
static u8  dhcp_state;
static u32 dhcp_next;            /* when to send again */
static u8  dhcp_tries;
static u32 dhcp_offer_ip, dhcp_server_ip;

static u32 bootp_build( u8 type, u32 request_ip, u32 server_ip )
{
    u8 *b = tx_frame + 34 + 8;   /* past the UDP header, filled by udp_send */
    u32 n;

    mem_zero( b, 300 );
    b[0] = 1;                    /* BOOTREQUEST */
    b[1] = 1;                    /* Ethernet */
    b[2] = 6;
    put32( b + 4, dhcp_xid );
    mem_copy( b + 28, if_mac, 6 );      /* chaddr */
    put32( b + 236, 0x63825363L );      /* the magic cookie */
    n = 240;
    b[n++] = 53; b[n++] = 1; b[n++] = type;             /* message type */
    if ( type == DHCP_REQUEST ) {
        b[n++] = 50; b[n++] = 4; put32( b + n, request_ip ); n += 4;
        b[n++] = 54; b[n++] = 4; put32( b + n, server_ip ); n += 4;
    }
    b[n++] = 55; b[n++] = 4;            /* parameter request list */
    b[n++] = 1; b[n++] = 3; b[n++] = 6; b[n++] = 51;    /* mask, gw, dns, lease */
    b[n++] = 255;                       /* end */
    return n;
}

static void dhcp_send( u8 type )
{
    u32 n = bootp_build( type, dhcp_offer_ip, dhcp_server_ip );
    udp_send( IP_BCAST, 67, 68, NULL, n );
}

/* a BOOTP reply arrived: move the state machine along.  Called from the
   pump, so it does not print or wait */
static void dhcp_input( const u8 *uh, u32 len )
{
    const u8 *b = uh + 8;
    u32 blen, n;
    u8  type = 0;

    if ( len < 8 + 240 ) return;
    blen = len - 8;
    if ( b[0] != 2 ) return;            /* BOOTREPLY */
    if ( get32( b + 4 ) != dhcp_xid ) return;
    if ( get32( b + 236 ) != 0x63825363L ) return;

    dhcp_offer_ip = get32( b + 16 );    /* yiaddr */

    for ( n = 240; n + 1 < blen; ) {
        u8 opt = b[n], olen;
        if ( opt == 0 ) { n++; continue; }
        if ( opt == 255 ) break;
        olen = b[n + 1];
        if ( n + 2 + olen > blen ) break;
        switch ( opt ) {
        case 53: type = b[n + 2]; break;
        case 1:  if ( olen >= 4 ) if_mask = get32( b + n + 2 ); break;
        case 3:  if ( olen >= 4 ) if_gw = get32( b + n + 2 ); break;
        case 6:  if ( olen >= 4 ) if_dns = get32( b + n + 2 ); break;
        case 51: if ( olen >= 4 ) if_lease = get32( b + n + 2 ); break;
        case 54: if ( olen >= 4 ) dhcp_server_ip = get32( b + n + 2 ); break;
        }
        n += 2 + olen;
    }

    if ( type == DHCP_OFFER && dhcp_state == DH_DISCOVER ) {
        dhcp_state = DH_REQUEST;
        dhcp_tries = 0;
        dhcp_send( DHCP_REQUEST );
        dhcp_next = nds_ticks() + 18;
    } else if ( type == DHCP_ACK && dhcp_state == DH_REQUEST ) {
        if_ip = dhcp_offer_ip;
        if_up = 1;
        if_dhcp = DHS_BOUND;
        dhcp_state = DH_BOUND;
        arp_request( if_gw );           /* learn the gateway's address early */
    }
}

/* kick DHCP off (at INIT) and keep it going (on every tick until bound) */
static void dhcp_start( void )
{
    dhcp_xid = rnd();
    if_ip = 0;
    dhcp_offer_ip = 0;
    dhcp_server_ip = 0;
    dhcp_tries = 0;
    dhcp_state = DH_DISCOVER;
    dhcp_send( DHCP_DISCOVER );
    dhcp_next = nds_ticks() + 18;
}

static void dhcp_tick( void )
{
    u32 now = nds_ticks();

    if ( dhcp_state == DH_DISCOVER && (long)( now - dhcp_next ) >= 0 ) {
        dhcp_send( DHCP_DISCOVER );
        dhcp_next = now + 36;
    } else if ( dhcp_state == DH_REQUEST && (long)( now - dhcp_next ) >= 0 ) {
        if ( ++dhcp_tries > 3 ) { dhcp_start(); return; }   /* begin again */
        dhcp_send( DHCP_REQUEST );
        dhcp_next = now + 36;
    }
}

/* ---------------------------------------------------------------- */
/* DNS                                                               */
/* ---------------------------------------------------------------- */

static u32 dns_build( const char *name )
{
    u8 *q = tx_frame + 34 + 8;
    u32 n = 12;
    const char *p = name;

    put16( q, res.id );
    put16( q + 2, 0x0100 );              /* recursion desired */
    put16( q + 4, 1 );                   /* one question */
    put16( q + 6, 0 );
    put16( q + 8, 0 );
    put16( q + 10, 0 );
    while ( *p ) {
        const char *start = p;
        u32 label;
        while ( *p && *p != '.' ) p++;
        label = (u32)( p - start );
        if ( label > 63 ) label = 63;
        q[n++] = (u8)label;
        mem_copy( q + n, start, label );
        n += label;
        if ( *p == '.' ) p++;
    }
    q[n++] = 0;                          /* end of the name */
    put16( q + n, 1 ); n += 2;           /* type A */
    put16( q + n, 1 ); n += 2;           /* class IN */
    return n;
}

static u32 dns_skip_name( const u8 *dns, u32 len, u32 at )
{
    while ( at < len ) {
        u8 label = dns[at];
        if ( ( label & 0xC0 ) == 0xC0 ) return at + 2;      /* a pointer */
        if ( label == 0 ) return at + 1;
        at += 1 + label;
    }
    return len;
}

static void dns_input( const u8 *dns, u32 len )
{
    u32 at, answers, i;

    if ( len < 12 ) return;
    if ( get16( dns ) != res.id ) return;
    if ( ( get16( dns + 2 ) & 0x000F ) != 0 ) { res.state = NET_FAIL; res.active = 0; return; }
    answers = get16( dns + 6 );
    at = dns_skip_name( dns, len, 12 ) + 4;         /* past the question */
    for ( i = 0; i < answers && at + 10 <= len; i++ ) {
        u32 rdlen, type;
        at = dns_skip_name( dns, len, at );
        if ( at + 10 > len ) break;
        type = get16( dns + at );
        rdlen = get16( dns + at + 8 );
        if ( type == 1 && rdlen == 4 && at + 10 + 4 <= len ) {
            res.ip = get32( dns + at + 10 );
            res.state = NET_DONE;
            res.active = 0;
            return;
        }
        at += 10 + rdlen;
    }
    res.state = NET_FAIL;
    res.active = 0;
}

static void dns_send( void )
{
    udp_send( if_dns, 53, res.lport, NULL, dns_build( res.name ) );
}

/* ---------------------------------------------------------------- */
/* TCP                                                               */
/* ---------------------------------------------------------------- */

#define SEQ_LT( a, b )   ( (long)( (u32)(a) - (u32)(b) ) < 0 )
#define SEQ_LEQ( a, b )  ( (long)( (u32)(a) - (u32)(b) ) <= 0 )
#define SEQ_GT( a, b )   ( (long)( (u32)(a) - (u32)(b) ) > 0 )
#define SEQ_GEQ( a, b )  ( (long)( (u32)(a) - (u32)(b) ) >= 0 )

static u32 rx_free( SOCKET *s )  { return RXCAP - s->rxlen; }
static u32 tx_free( SOCKET *s )  { return TXCAP - s->txlen; }

/* put one TCP segment on the wire: control flags, and dlen bytes of
   send buffer starting seq bytes from snd_una */
static void tcp_xmit( SOCKET *s, u32 seq, u8 flags, u32 off, u32 dlen )
{
    u8 *th = tx_frame + 34;
    u32 i;

    put16( th, s->lport );
    put16( th + 2, s->rport );
    put32( th + 4, seq );
    put32( th + 8, s->rcv_nxt );
    th[12] = 5 << 4;                     /* a 20-byte header */
    th[13] = flags;
    put16( th + 14, (u16)rx_free( s ) );
    put16( th + 16, 0 );
    put16( th + 18, 0 );
    for ( i = 0; i < dlen; i++ ) {
        th[20 + i] = s->txbuf[( s->txstart + off + i ) % TXCAP];
    }
    put16( th + 16, l4_cksum( if_ip, s->rip, IP_TCP, 20 + dlen ) );
    ip_send( s->rip, IP_TCP, 20 + dlen );
}

static void tcp_rst( u32 dst, u16 lport, u16 rport, u32 seq, u32 ack, u8 have_ack )
{
    u8 *th = tx_frame + 34;

    put16( th, lport );
    put16( th + 2, rport );
    put32( th + 4, seq );
    put32( th + 8, ack );
    th[12] = 5 << 4;
    th[13] = (u8)( TCP_RST | ( have_ack ? TCP_ACK : 0 ) );
    put16( th + 14, 0 );
    put16( th + 16, 0 );
    put16( th + 18, 0 );
    put16( th + 16, l4_cksum( if_ip, dst, IP_TCP, 20 ) );
    ip_send( dst, IP_TCP, 20 );
}

static void tcp_arm( SOCKET *s )
{
    if ( s->rto == 0 ) s->rto = 18;
    s->rto_at = nds_ticks() + s->rto;
}

/* send whatever data the window now allows, and a FIN if it is time */
static void tcp_send_data( SOCKET *s )
{
    u32 usable, avail, off, chunk;

    if ( s->tstate != T_ESTAB && s->tstate != T_CLOSEWAIT ) return;

    for ( ;; ) {
        off = s->snd_nxt - s->snd_una;
        avail = s->txlen - off;                 /* unsent bytes in the ring */
        usable = s->snd_una + s->snd_wnd - s->snd_nxt;      /* the window */
        if ( avail == 0 || (long)usable <= 0 ) break;
        chunk = avail;
        if ( chunk > usable ) chunk = usable;
        if ( chunk > TCP_MSS ) chunk = TCP_MSS;
        tcp_xmit( s, s->snd_nxt, (u8)( TCP_ACK | TCP_PSH ), off, chunk );
        s->snd_nxt += chunk;
        tcp_arm( s );
    }

    /* the app has closed and every byte is out: send the FIN */
    if ( s->closing && !s->finsent && s->snd_nxt == s->snd_una + s->txlen ) {
        s->finseq = s->snd_nxt;
        tcp_xmit( s, s->snd_nxt, (u8)( TCP_ACK | TCP_FIN ), s->txlen, 0 );
        s->snd_nxt++;
        s->finsent = 1;
        if ( s->tstate == T_ESTAB ) s->tstate = T_FINWAIT1;
        else if ( s->tstate == T_CLOSEWAIT ) s->tstate = T_LASTACK;
        tcp_arm( s );
    }
}

static SOCKET *tcp_listener_for( u32 dst, u16 port )
{
    int i;
    (void)dst;
    for ( i = 0; i < SOCK_MAX; i++ ) {
        if ( sock[i].used && sock[i].listening && sock[i].listenport == port ) {
            return &sock[i];
        }
    }
    return NULL;
}

static SOCKET *tcp_match( u32 src, u16 sport, u32 dst, u16 dport )
{
    int i;
    for ( i = 0; i < SOCK_MAX; i++ ) {
        SOCKET *s = &sock[i];
        if ( !s->used || s->type != SOCK_STREAM || s->listening ) continue;
        if ( s->tstate == T_CLOSED || s->tstate == T_LISTEN ) continue;
        if ( s->lport == dport && s->rport == sport &&
             s->lip == dst && s->rip == src ) return s;
    }
    return NULL;
}

static SOCKET *sock_free_slot( void );

static void tcp_input( u32 src, u32 dst, const u8 *th, u32 len )
{
    u16 sport, dport, window;
    u32 seq, ack, off, dlen;
    u8  flags;
    const u8 *data;
    SOCKET *s;

    if ( len < 20 ) return;
    sport = get16( th );
    dport = get16( th + 2 );
    seq = get32( th + 4 );
    ack = get32( th + 8 );
    off = ( th[12] >> 4 ) * 4;
    flags = th[13];
    window = get16( th + 14 );
    if ( off < 20 || off > len ) return;
    data = th + off;
    dlen = len - off;

    s = tcp_match( src, sport, dst, dport );

    if ( s == NULL ) {
        /* a SYN to a listening port starts a new connection */
        SOCKET *lis = tcp_listener_for( dst, dport );
        if ( lis && ( flags & TCP_SYN ) && !( flags & TCP_ACK ) ) {
            SOCKET *c = sock_free_slot();
            if ( c == NULL ) { tcp_rst( src, dport, sport, 0, seq + 1, 1 ); return; }
            c->used = 1;
            c->type = SOCK_STREAM;
            c->lip = dst; c->rip = src;
            c->lport = dport; c->rport = sport;
            c->irs = seq;
            c->rcv_nxt = seq + 1;
            c->iss = rnd();
            c->snd_una = c->iss;
            c->snd_nxt = c->iss;
            c->snd_wnd = window ? window : 1;
            c->tstate = T_SYNRCVD;
            tcp_xmit( c, c->snd_nxt, (u8)( TCP_SYN | TCP_ACK ), 0, 0 );
            c->snd_nxt++;
            tcp_arm( c );
            return;
        }
        if ( !( flags & TCP_RST ) ) {
            tcp_rst( src, dport, sport, ack, seq + dlen, 1 );
        }
        return;
    }

    if ( flags & TCP_RST ) {
        s->tstate = T_CLOSED;
        s->err = NET_FAIL;
        s->rto_at = 0;
        return;
    }

    s->snd_wnd = window;

    if ( s->tstate == T_SYNSENT ) {
        if ( ( flags & TCP_SYN ) && ( flags & TCP_ACK ) ) {
            if ( ack != s->iss + 1 ) { tcp_rst( src, dport, sport, ack, 0, 0 ); return; }
            s->irs = seq;
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_nxt = ack;
            s->tstate = T_ESTAB;
            s->rto_at = 0;
            tcp_xmit( s, s->snd_nxt, TCP_ACK, 0, 0 );
            tcp_send_data( s );
        } else if ( flags & TCP_SYN ) {
            /* simultaneous open: SYN with no ACK */
            s->irs = seq;
            s->rcv_nxt = seq + 1;
            s->tstate = T_SYNRCVD;
            tcp_xmit( s, s->iss, (u8)( TCP_SYN | TCP_ACK ), 0, 0 );
        }
        return;
    }

    /* an acceptable segment acknowledges something; use its ACK */
    if ( flags & TCP_ACK ) {
        if ( SEQ_GT( ack, s->snd_una ) && SEQ_LEQ( ack, s->snd_nxt ) ) {
            u32 acked = ack - s->snd_una;
            u32 databytes = acked;
            if ( s->finsent && SEQ_GT( ack, s->finseq ) ) {
                databytes = acked - 1;          /* the FIN's sequence */
                s->finacked = 1;
            }
            if ( databytes > s->txlen ) databytes = s->txlen;
            s->txstart = ( s->txstart + databytes ) % TXCAP;
            s->txlen -= databytes;
            s->snd_una = ack;
            s->retries = 0;
            if ( s->snd_una == s->snd_nxt ) s->rto_at = 0;
            else tcp_arm( s );
        }
    }

    if ( s->tstate == T_SYNRCVD ) {
        if ( flags & TCP_ACK ) s->tstate = T_ESTAB;
        else return;
    }

    /* data, if it is the next in order */
    if ( dlen && SEQ_LEQ( seq, s->rcv_nxt ) && SEQ_GT( seq + dlen, s->rcv_nxt ) ) {
        u32 skip = s->rcv_nxt - seq;
        u32 take = dlen - skip;
        u32 room = rx_free( s );
        u32 i;
        if ( take > room ) take = room;
        for ( i = 0; i < take; i++ ) {
            s->rxbuf[( s->rxstart + s->rxlen + i ) % RXCAP] = data[skip + i];
        }
        s->rxlen += take;
        s->rcv_nxt += take;
        tcp_xmit( s, s->snd_nxt, TCP_ACK, s->snd_nxt - s->snd_una, 0 );
    } else if ( dlen ) {
        tcp_xmit( s, s->snd_nxt, TCP_ACK, s->snd_nxt - s->snd_una, 0 );      /* keep them in step */
    }

    /* the peer's FIN, once we are in step with it */
    if ( ( flags & TCP_FIN ) && seq + dlen == s->rcv_nxt ) {
        s->rcv_nxt++;
        tcp_xmit( s, s->snd_nxt, TCP_ACK, s->snd_nxt - s->snd_una, 0 );
        switch ( s->tstate ) {
        case T_ESTAB:    s->tstate = T_CLOSEWAIT; break;
        case T_FINWAIT1: s->tstate = s->finacked ? T_TIMEWAIT : T_CLOSING;
                         if ( s->tstate == T_TIMEWAIT ) s->wait_at = nds_ticks() + 36;
                         break;
        case T_FINWAIT2: s->tstate = T_TIMEWAIT; s->wait_at = nds_ticks() + 36; break;
        }
    }

    /* our FIN's acknowledgement moves the close along */
    if ( s->finacked ) {
        if ( s->tstate == T_FINWAIT1 ) s->tstate = T_FINWAIT2;
        else if ( s->tstate == T_CLOSING ) { s->tstate = T_TIMEWAIT; s->wait_at = nds_ticks() + 36; }
        else if ( s->tstate == T_LASTACK ) { s->tstate = T_CLOSED; s->used = 0; }
    }

    tcp_send_data( s );
}

/* the retransmit and close timers, run on every tick */
static void tcp_timers( void )
{
    int i;
    u32 now = nds_ticks();

    for ( i = 0; i < SOCK_MAX; i++ ) {
        SOCKET *s = &sock[i];
        if ( !s->used || s->type != SOCK_STREAM ) continue;

        if ( s->tstate == T_TIMEWAIT && (long)( now - s->wait_at ) >= 0 ) {
            s->tstate = T_CLOSED;
            s->used = 0;
            continue;
        }
        if ( s->rto_at == 0 || (long)( now - s->rto_at ) < 0 ) continue;

        if ( ++s->retries > 6 ) {
            s->tstate = T_CLOSED;
            s->err = NET_FAIL;
            s->used = ( s->tstate == T_CLOSED && s->closing ) ? 0 : s->used;
            s->rto_at = 0;
            continue;
        }
        s->rto *= 2;
        if ( s->rto > 109 ) s->rto = 109;              /* about six seconds */

        if ( s->tstate == T_SYNSENT ) {
            tcp_xmit( s, s->iss, TCP_SYN, 0, 0 );
        } else if ( s->tstate == T_SYNRCVD ) {
            tcp_xmit( s, s->iss, (u8)( TCP_SYN | TCP_ACK ), 0, 0 );
        } else {
            /* go back: resend from the oldest unacknowledged byte */
            s->snd_nxt = s->snd_una;
            if ( s->finsent ) s->finsent = 0;          /* tcp_send_data resends it */
            tcp_send_data( s );
        }
        s->rto_at = now + s->rto;
    }
}

/* ---------------------------------------------------------------- */
/* the socket table                                                  */
/* ---------------------------------------------------------------- */

static SOCKET *sock_free_slot( void )
{
    int i;
    for ( i = 0; i < SOCK_MAX; i++ ) if ( !sock[i].used ) return &sock[i];
    return NULL;
}

static SOCKET *sock_by_handle( u32 handle )
{
    if ( handle < 1 || handle > SOCK_MAX ) return NULL;
    if ( !sock[handle - 1].used ) return NULL;
    return &sock[handle - 1];
}

static u32 sock_handle( SOCKET *s )
{
    return (u32)( s - sock ) + 1;
}

static SOCKET *udp_socket_for( u16 port )
{
    int i;
    for ( i = 0; i < SOCK_MAX; i++ ) {
        if ( sock[i].used && sock[i].type == SOCK_DGRAM && sock[i].lport == port ) {
            return &sock[i];
        }
    }
    return NULL;
}

/* a datagram's frame in the UDP socket's buffer: length, address, port */
static void udp_queue( SOCKET *s, u32 src, u16 sport, const u8 *data, u32 dlen )
{
    u32 need = 8 + dlen;

    if ( s->rxstart == s->rxlen ) { s->rxstart = 0; s->rxlen = 0; }  /* empty: pack */
    if ( s->rxlen + need > RXCAP ) return;                           /* no room: drop */
    put16( s->rxbuf + s->rxlen, (u16)dlen );
    put16( s->rxbuf + s->rxlen + 2, sport );
    put32( s->rxbuf + s->rxlen + 4, src );
    if ( dlen ) mem_copy( s->rxbuf + s->rxlen + 8, data, dlen );
    s->rxlen += need;
}

static void sock_reset( SOCKET *s, u8 type )
{
    u8 *rxbuf = s->rxbuf, *txbuf = s->txbuf;
    tls_release( s );                   /* free any TLS state a prior user left */
    mem_zero( s, sizeof( *s ) );
    s->rxbuf = rxbuf;
    s->txbuf = txbuf;
    s->type = type;
    s->tstate = T_CLOSED;
    s->used = 1;
}

static u32 sock_ss( SOCKET *s )
{
    if ( s->type == SOCK_DGRAM ) return SS_ESTABLISHED;
    if ( s->tls ) {
        /* TLS rides above the TCP state: established means TLS is up, so a
           caller's connect wait does not finish until the handshake does */
        if ( s->tls_state == TLSX_FAIL ) return SS_ERROR;
        if ( s->tls_state == TLSX_UP ) {
            /* up, but report the close once the peer has ended and there is
               no plaintext left, so a read-to-end loop stops */
            u32 left = s->taplen - s->tapstart;
            if ( left == 0 ) {
                if ( s->tls_eof ) return SS_CLOSING;
                if ( s->tstate != T_ESTAB && s->tstate != T_CLOSEWAIT &&
                     s->trxlen == 0 && s->rxlen == 0 ) return SS_CLOSING;
            }
            return SS_ESTABLISHED;
        }
        if ( s->tstate == T_CLOSED ) return s->err ? SS_ERROR : SS_CLOSED;
        return SS_CONNECTING;
    }
    switch ( s->tstate ) {
    case T_CLOSED:   return s->err ? SS_ERROR : SS_CLOSED;
    case T_LISTEN:   return SS_LISTEN;
    case T_SYNSENT:
    case T_SYNRCVD:  return SS_CONNECTING;
    case T_ESTAB:
    case T_CLOSEWAIT: return SS_ESTABLISHED;
    default:         return SS_CLOSING;
    }
}

static u32 sock_rxready( SOCKET *s )
{
    if ( s->tls )
        return ( s->tls_state == TLSX_UP ) ? ( s->taplen - s->tapstart ) : 0;
    if ( s->type == SOCK_DGRAM ) {
        if ( s->rxlen > s->rxstart ) return get16( s->rxbuf + s->rxstart );
        return 0;
    }
    return s->rxlen;
}

/* ---------------------------------------------------------------- */
/* TLS: the handshake and record crypto, kernel-side                 */
/* ---------------------------------------------------------------- */

/* Each TLS-core entry runs on tls_stk through tls_run (TLSSTK.ASM): the
   bridge's scratch stack is far too small for the crypto's call tree.  A
   small job struct carries the arguments across the stack switch. */

typedef struct { tls_conn *c; int ret; } PUMPJOB;
static int __cdecl job_pump( void *p )
{
    PUMPJOB *j = (PUMPJOB *)p;
    j->ret = tls_hs_pump( j->c );
    return j->ret;
}

typedef struct {
    tls_conn *c; const u8 *in; int len; u8 *out; int outcap; int ret;
} ENCJOB;
static int __cdecl job_encrypt( void *p )
{
    ENCJOB *j = (ENCJOB *)p;
    j->ret = tls_app_encrypt( j->c, j->in, j->len, j->out, j->outcap );
    return j->ret;
}

typedef struct {
    tls_conn *c; const u8 *rec; int reclen; u8 *out; int outcap; int ct; int ret;
} DECJOB;
static int __cdecl job_decrypt( void *p )
{
    DECJOB *j = (DECJOB *)p;
    j->ret = tls_app_decrypt( j->c, j->rec, j->reclen, j->out, j->outcap, &j->ct );
    return j->ret;
}

static int run_pump( tls_conn *c )
{
    PUMPJOB j;
    j.c = c; j.ret = TLS_HS_ERROR;
    tls_run( (void *)tls_stktop, (void *)job_pump, &j );
    return j.ret;
}

static int run_encrypt( tls_conn *c, const u8 *in, int len, u8 *out, int outcap )
{
    ENCJOB j;
    j.c = c; j.in = in; j.len = len; j.out = out; j.outcap = outcap; j.ret = -1;
    tls_run( (void *)tls_stktop, (void *)job_encrypt, &j );
    return j.ret;
}

static int run_decrypt( tls_conn *c, const u8 *rec, int reclen,
                        u8 *out, int outcap, int *ct )
{
    DECJOB j;
    j.c = c; j.rec = rec; j.reclen = reclen; j.out = out; j.outcap = outcap;
    j.ct = 0; j.ret = -1;
    tls_run( (void *)tls_stktop, (void *)job_decrypt, &j );
    *ct = j.ct;
    return j.ret;
}

/* free a TLS socket's crypto state (on close, or before a slot is reused) */
static void tls_release( SOCKET *s )
{
    if ( s->tc )   { tls_free( s->tc ); s->tc = NULL; }
    if ( s->rng )  { free( s->rng ); s->rng = NULL; }
    if ( s->trx )  { free( s->trx ); s->trx = NULL; }
    if ( s->tapp ) { free( s->tapp ); s->tapp = NULL; }
    s->trxlen = s->tapstart = s->taplen = 0;
}

/* pop up to 'want' bytes off the TCP receive ring into a linear buffer */
static u32 tcp_rx_pop( SOCKET *s, u8 *dst, u32 want )
{
    u32 got = s->rxlen < want ? s->rxlen : want;
    u32 i;
    for ( i = 0; i < got; i++ ) dst[i] = s->rxbuf[( s->rxstart + i ) % RXCAP];
    s->rxstart = ( s->rxstart + got ) % RXCAP;
    s->rxlen -= got;
    return got;
}

static u8 tls_feedbuf[1024];        /* handshake bytes, off the scratch stack */

/* begin the TLS handshake once the TCP connection is up */
static void tls_begin( SOCKET *s )
{
    const char *host = s->host[0] ? s->host : NULL;

    if ( tls_stk == NULL ) {
        tls_stk = (u8 *)malloc( TLS_STK_SIZE );
        if ( tls_stk == NULL ) { s->tls_state = TLSX_FAIL; s->err = NET_FAIL; return; }
        tls_stktop = ( (u32)tls_stk + TLS_STK_SIZE ) & ~15u;
    }
    s->rng  = (nst_rng *)malloc( sizeof( nst_rng ) );
    s->trx  = (u8 *)malloc( TLS_RECCAP );
    s->tapp = (u8 *)malloc( TLS_REC_MAX );
    if ( s->rng == NULL || s->trx == NULL || s->tapp == NULL ) {
        s->tls_state = TLSX_FAIL; s->err = NET_FAIL; return;
    }
    rng_init( s->rng );
    rng_stir( s->rng, (const u8 *)&s->iss, 4 );     /* a little extra entropy */
    s->tc = tls_new( s->rng );
    if ( s->tc == NULL ) { s->tls_state = TLSX_FAIL; s->err = NET_FAIL; return; }
    if ( tls_ca && tls_catotal > 0 && tls_calen == tls_catotal ) {
        /* chain and host verified against the bundle; no wall clock in a
           driver, so the certificate's dates are not checked */
        tls_set_verify( s->tc, host, "", tls_ca, (int)tls_calen );
    }
    tls_hs_start( s->tc, host );
    s->trxlen = 0;
    s->tapstart = s->taplen = 0;
    s->tls_eof = 0;
    s->tls_state = TLSX_HS;
}

/* assemble and decrypt one inbound record into tapp, if tapp is empty */
static void tls_fill( SOCKET *s )
{
    u32 reclen, extra;
    int ct, plen;

    if ( s->taplen > s->tapstart ) return;          /* app still has plaintext */
    if ( s->tls_eof ) return;

    if ( s->trxlen < 5 )
        s->trxlen += tcp_rx_pop( s, s->trx + s->trxlen, 5 - s->trxlen );
    if ( s->trxlen < 5 ) return;                    /* record header not in yet */

    reclen = 5 + ( ( (u32)s->trx[3] << 8 ) | s->trx[4] );
    if ( reclen > TLS_RECCAP ) { s->tls_state = TLSX_FAIL; s->err = NET_FAIL; return; }
    if ( s->trxlen < reclen )
        s->trxlen += tcp_rx_pop( s, s->trx + s->trxlen, reclen - s->trxlen );
    if ( s->trxlen < reclen ) return;               /* record body not in yet */

    plen = run_decrypt( s->tc, s->trx, (int)reclen, s->tapp, TLS_REC_MAX, &ct );
    extra = s->trxlen - reclen;                     /* a following record's start */
    if ( extra ) mem_copy( s->trx, s->trx + reclen, extra );
    s->trxlen = extra;

    if ( plen < 0 ) { s->tls_state = TLSX_FAIL; s->err = NET_FAIL; return; }
    if ( ct == TLS_CT_APPLICATION_DATA ) { s->tapstart = 0; s->taplen = (u32)plen; }
    else if ( ct == TLS_CT_ALERT ) s->tls_eof = 1;
    /* else a post-handshake handshake message (a session ticket): drop it */
}

/* advance one TLS socket: start it, run the handshake, keep records flowing */
static void tls_drive( SOCKET *s )
{
    if ( !s->tls ) return;

    if ( s->tls_state == TLSX_IDLE ) {
        if ( s->tstate == T_ESTAB ) tls_begin( s );
        else if ( s->tstate == T_CLOSED && s->err ) s->tls_state = TLSX_FAIL;
        return;
    }

    if ( s->tls_state == TLSX_HS ) {
        unsigned char *out;
        int st, m;

        /* feed everything the TCP stream has to the handshake */
        while ( s->rxlen > 0 ) {
            u32 got = tcp_rx_pop( s, tls_feedbuf, sizeof( tls_feedbuf ) );
            if ( got == 0 ) break;
            if ( tls_hs_feed( s->tc, tls_feedbuf, (int)got ) < 0 ) {
                s->tls_state = TLSX_FAIL; s->err = NET_FAIL; return;
            }
        }
        st = run_pump( s->tc );
        /* flush whatever the handshake wants to send into the TCP txbuf */
        while ( ( m = tls_hs_out_ptr( s->tc, &out ) ) > 0 ) {
            u32 room = tx_free( s );
            u32 n = (u32)m < room ? (u32)m : room;
            u32 i;
            if ( n == 0 ) break;                    /* no room; next pass */
            for ( i = 0; i < n; i++ )
                s->txbuf[( s->txstart + s->txlen + i ) % TXCAP] = out[i];
            s->txlen += n;
            tls_hs_out_consume( s->tc, (int)n );
        }
        if ( s->txlen > 0 ) tcp_send_data( s );

        if ( st == TLS_HS_DONE ) {
            unsigned char *lp;
            int lm = tls_hs_leftover( s->tc, &lp );     /* app bytes past Finished */
            if ( lm > 0 ) {
                if ( lm > TLS_RECCAP ) lm = TLS_RECCAP;
                mem_copy( s->trx, lp, (u32)lm );
                s->trxlen = (u32)lm;
            }
            s->tls_state = TLSX_UP;
        } else if ( st == TLS_HS_ERROR ) {
            s->tls_state = TLSX_FAIL;
            s->err = NET_FAIL;
        }
        return;
    }

    if ( s->tls_state == TLSX_UP ) tls_fill( s );       /* keep plaintext ready */
}

static void tls_service( void )
{
    int i;
    for ( i = 0; i < SOCK_MAX; i++ )
        if ( sock[i].used && sock[i].tls ) tls_drive( &sock[i] );
}

/* ---------------------------------------------------------------- */
/* the pump: drain the card, run the timers                          */
/* ---------------------------------------------------------------- */

static void pump( void )
{
    u32 len;
    int budget = 64;

    while ( budget-- > 0 ) {
        len = nic_recv( rx_frame, sizeof( rx_frame ) );
        if ( len == 0 ) break;
        eth_input( rx_frame, len );
    }
    tls_service();      /* advance any TLS socket over the bytes just taken in */
}

static void timers( void )
{
    u32 now = nds_ticks();

    dhcp_tick();
    tcp_timers();

    if ( ping.active && (long)( now - ping.give_up ) >= 0 ) {
        if ( ++ping.tries > 3 ) { ping.state = NET_FAIL; ping.active = 0; }
        else { icmp_echo( ping.ip, ping.id, ping.seq ); ping.give_up = now + 18; }
    }
    if ( res.active && (long)( now - res.give_up ) >= 0 ) {
        if ( ++res.tries > 3 ) { res.state = NET_FAIL; res.active = 0; }
        else { dns_send(); res.give_up = now + 18; }
    }
}

/* ---------------------------------------------------------------- */
/* the socket calls (nd_call)                                        */
/* ---------------------------------------------------------------- */

static int do_ifstat( void *buf, u32 len )
{
    SOCK_IF *info = (SOCK_IF *)buf;
    if ( len < SOCKIF_LEN ) return ERR_BADLEN;
    mem_zero( info, SOCKIF_LEN );
    info->ip = if_ip;
    info->mask = if_mask;
    info->gw = if_gw;
    info->dns = if_dns;
    info->leasesecs = if_lease;
    mem_copy( info->mac, if_mac, 6 );
    info->up = if_up;
    info->dhcp = if_dhcp;
    str_copy( info->nic, if_name, sizeof( info->nic ) );
    return 0;
}

static int do_open( void *buf, u32 len, u32 *answer )
{
    SOCK_OPENREQ *req = (SOCK_OPENREQ *)buf;
    SOCKET *s;
    u32 base;

    if ( len < sizeof( SOCK_OPENREQ ) ) return ERR_BADLEN;
    base = req->type & 0xFF;
    if ( base != SOCK_STREAM && base != SOCK_DGRAM ) return ERR_BADDATA;
    s = sock_free_slot();
    if ( s == NULL ) return ERR_GENERAL;
    sock_reset( s, (u8)base );
    if ( ( req->type & SOCK_TLS ) && base == SOCK_STREAM ) s->tls = 1;
    *answer = sock_handle( s );
    return 0;
}

static int do_close( void *buf, u32 len )
{
    SOCK_HANDLE *req = (SOCK_HANDLE *)buf;
    SOCKET *s;

    if ( len < sizeof( SOCK_HANDLE ) ) return ERR_BADLEN;
    s = sock_by_handle( req->handle );
    if ( s == NULL ) return ERR_BADDATA;

    if ( s->tls ) tls_release( s );     /* drop the crypto; the TCP close runs on */

    if ( s->type == SOCK_DGRAM || s->tstate == T_CLOSED || s->listening ) {
        s->used = 0;
        return 0;
    }
    s->closing = 1;
    if ( s->tstate == T_ESTAB || s->tstate == T_CLOSEWAIT ) tcp_send_data( s );
    return 0;
}

static int do_connect( void *buf, u32 len )
{
    SOCK_CONNREQ *req = (SOCK_CONNREQ *)buf;
    SOCKET *s;

    if ( len < sizeof( SOCK_CONNREQ ) ) return ERR_BADLEN;
    s = sock_by_handle( req->handle );
    if ( s == NULL ) return ERR_BADDATA;
    if ( !if_up ) return ERR_GENERAL;

    s->rip = req->ip;
    s->rport = req->port;
    s->lip = if_ip;
    s->lport = (u16)( 0xC000 + ( rnd() & 0x3FFF ) );

    if ( s->type == SOCK_DGRAM ) return 0;      /* just the default peer */

    s->iss = rnd();
    s->snd_una = s->iss;
    s->snd_nxt = s->iss;
    s->snd_wnd = TCP_MSS;
    s->rcv_nxt = 0;
    s->tstate = T_SYNSENT;
    s->err = 0;
    tcp_xmit( s, s->iss, TCP_SYN, 0, 0 );
    s->snd_nxt = s->iss + 1;
    tcp_arm( s );
    return 0;
}

static int do_listen( void *buf, u32 len )
{
    SOCK_LISTENREQ *req = (SOCK_LISTENREQ *)buf;
    SOCKET *s;

    if ( len < sizeof( SOCK_LISTENREQ ) ) return ERR_BADLEN;
    s = sock_by_handle( req->handle );
    if ( s == NULL || s->type != SOCK_STREAM ) return ERR_BADDATA;
    s->listening = 1;
    s->listenport = req->port;
    s->lport = req->port;
    s->tstate = T_LISTEN;
    return 0;
}

static int do_accept( void *buf, u32 len, u32 *answer )
{
    SOCK_ACCEPTREQ *req = (SOCK_ACCEPTREQ *)buf;
    SOCKET *lis;
    int i;

    if ( len < sizeof( SOCK_ACCEPTREQ ) ) return ERR_BADLEN;
    lis = sock_by_handle( req->handle );
    if ( lis == NULL || !lis->listening ) return ERR_BADDATA;

    req->newh = 0;
    for ( i = 0; i < SOCK_MAX; i++ ) {
        SOCKET *c = &sock[i];
        if ( c->used && !c->listening && c->type == SOCK_STREAM &&
             c->lport == lis->listenport &&
             ( c->tstate == T_ESTAB || c->tstate == T_CLOSEWAIT ) &&
             c->iss != 0 ) {
            /* hand it over once: clear iss so it is not accepted twice */
            req->newh = sock_handle( c );
            req->ip = c->rip;
            req->port = c->rport;
            c->iss = 0;
            break;
        }
    }
    *answer = req->newh;
    return 0;
}

static int do_send( void *buf, u32 len, u32 *answer )
{
    SOCK_IO *io = (SOCK_IO *)buf;
    const u8 *data = (const u8 *)buf + SOCK_IO_HDR;
    SOCKET *s;
    u32 dlen;

    if ( len < SOCK_IO_HDR ) return ERR_BADLEN;
    s = sock_by_handle( io->handle );
    if ( s == NULL ) return ERR_BADDATA;
    dlen = io->len;
    if ( len < SOCK_IO_HDR + dlen ) return ERR_BADLEN;

    if ( s->type == SOCK_DGRAM ) {
        u32 dst = io->ip ? io->ip : s->rip;
        u16 dport = io->port ? io->port : s->rport;
        if ( !if_up || dst == 0 ) return ERR_GENERAL;
        if ( dlen > 1472 ) dlen = 1472;
        if ( s->lport == 0 ) s->lport = (u16)( 0xC000 + ( rnd() & 0x3FFF ) );
        udp_send( dst, dport, s->lport, data, dlen );
        *answer = dlen;
        return 0;
    }

    if ( s->tls ) {
        u32 room, maxp, i;
        int reclen;

        if ( s->tls_state != TLSX_UP || s->tc == NULL ) return ERR_GENERAL;
        room = tx_free( s );
        if ( room < 96 ) { *answer = 0; return 0; }  /* no room for a record: try later */
        maxp = room - 80;                            /* leave space for record overhead */
        if ( maxp > TLS_APP_MAX ) maxp = TLS_APP_MAX;
        if ( dlen > maxp ) dlen = maxp;
        reclen = run_encrypt( s->tc, data, (int)dlen, tls_scratch, (int)sizeof( tls_scratch ) );
        if ( reclen <= 0 ) return ERR_GENERAL;
        for ( i = 0; i < (u32)reclen; i++ )
            s->txbuf[( s->txstart + s->txlen + i ) % TXCAP] = tls_scratch[i];
        s->txlen += (u32)reclen;
        tcp_send_data( s );
        *answer = dlen;
        return 0;
    }

    if ( s->tstate != T_ESTAB && s->tstate != T_CLOSEWAIT ) return ERR_GENERAL;
    if ( dlen > tx_free( s ) ) dlen = tx_free( s );
    {
        u32 i;
        for ( i = 0; i < dlen; i++ ) {
            s->txbuf[( s->txstart + s->txlen + i ) % TXCAP] = data[i];
        }
        s->txlen += dlen;
    }
    tcp_send_data( s );
    *answer = dlen;
    return 0;
}

static int do_recv( void *buf, u32 len, u32 *answer )
{
    SOCK_IO *io = (SOCK_IO *)buf;
    u8 *room = (u8 *)buf + SOCK_IO_HDR;
    SOCKET *s;
    u32 want, got;

    if ( len < SOCK_IO_HDR ) return ERR_BADLEN;
    s = sock_by_handle( io->handle );
    if ( s == NULL ) return ERR_BADDATA;
    want = io->len;
    if ( len < SOCK_IO_HDR + want ) want = len - SOCK_IO_HDR;

    if ( s->type == SOCK_DGRAM ) {
        u32 dlen;
        if ( s->rxstart >= s->rxlen ) { *answer = 0; return 0; }
        dlen = get16( s->rxbuf + s->rxstart );
        io->port = get16( s->rxbuf + s->rxstart + 2 );
        io->ip = get32( s->rxbuf + s->rxstart + 4 );
        got = dlen < want ? dlen : want;
        mem_copy( room, s->rxbuf + s->rxstart + 8, got );
        s->rxstart += 8 + dlen;
        if ( s->rxstart >= s->rxlen ) { s->rxstart = 0; s->rxlen = 0; }
        io->len = got;
        *answer = got;
        return 0;
    }

    if ( s->tls ) {
        u32 avail;
        if ( s->tls_state != TLSX_UP || s->tc == NULL ) { io->len = 0; *answer = 0; return 0; }
        tls_fill( s );                       /* decrypt one record if none is pending */
        avail = s->taplen - s->tapstart;
        got = avail < want ? avail : want;
        mem_copy( room, s->tapp + s->tapstart, got );
        s->tapstart += got;
        io->len = got;
        *answer = got;
        return 0;
    }

    got = s->rxlen < want ? s->rxlen : want;
    {
        u32 i;
        for ( i = 0; i < got; i++ ) room[i] = s->rxbuf[( s->rxstart + i ) % RXCAP];
    }
    s->rxstart = ( s->rxstart + got ) % RXCAP;
    s->rxlen -= got;
    io->len = got;
    *answer = got;
    return 0;
}

static int do_state( void *buf, u32 len, u32 *answer )
{
    SOCK_STAT *st = (SOCK_STAT *)buf;
    SOCKET *s;

    if ( len < SOCKSTAT_LEN ) return ERR_BADLEN;
    s = sock_by_handle( st->handle );
    if ( s == NULL ) return ERR_BADDATA;
    st->state = sock_ss( s );
    st->rxready = sock_rxready( s );
    st->txfree = s->type == SOCK_STREAM ? tx_free( s ) : 1472;
    st->err = s->err;
    *answer = st->state;
    return 0;
}

static int do_resolve( void *buf, u32 len, u32 *answer )
{
    SOCK_RES *req = (SOCK_RES *)buf;

    if ( len < SOCKRES_LEN ) return ERR_BADLEN;
    req->name[sizeof( req->name ) - 1] = 0;
    if ( !if_up || if_dns == 0 ) { req->state = NET_FAIL; *answer = NET_FAIL; return 0; }

    if ( res.state == NET_DONE && res.active == 0 && str_same( res.name, req->name ) ) {
        req->ip = res.ip;
        req->state = NET_DONE;
        *answer = NET_DONE;
        return 0;
    }
    if ( !res.active || !str_same( res.name, req->name ) ) {
        str_copy( res.name, req->name, sizeof( res.name ) );
        res.id = (u16)rnd();
        res.lport = (u16)( 0xC000 + ( rnd() & 0x3FFF ) );
        res.ip = 0;
        res.state = NET_PENDING;
        res.tries = 0;
        res.active = 1;
        res.give_up = nds_ticks() + 18;
        dns_send();
    }
    req->ip = res.ip;
    req->state = res.state;
    *answer = res.state;
    return 0;
}

static int do_ping( void *buf, u32 len, u32 *answer )
{
    SOCK_PINGREQ *req = (SOCK_PINGREQ *)buf;

    if ( len < SOCKPING_LEN ) return ERR_BADLEN;
    if ( !if_up ) { req->state = NET_FAIL; *answer = NET_FAIL; return 0; }

    if ( ping.state != NET_PENDING && ping.active == 0 &&
         ping.ip == req->ip && ping.seq == (u16)req->seq ) {
        req->state = ping.state;
        req->rttms = ping.rtt;
        *answer = ping.state;
        return 0;
    }
    if ( !ping.active || ping.ip != req->ip || ping.seq != (u16)req->seq ) {
        ping.ip = req->ip;
        ping.id = 0x504D;               /* 'PM' */
        ping.seq = (u16)req->seq;
        ping.state = NET_PENDING;
        ping.rtt = 0;
        ping.tries = 0;
        ping.active = 1;
        ping.sent_at = nds_ticks();
        ping.give_up = ping.sent_at + 18;
        icmp_echo( ping.ip, ping.id, ping.seq );
    }
    req->state = ping.state;
    req->rttms = ping.rtt;
    *answer = ping.state;
    return 0;
}

static int do_tlshost( void *buf, u32 len )
{
    SOCK_TLSHOSTREQ *req = (SOCK_TLSHOSTREQ *)buf;
    SOCKET *s;

    if ( len < SOCKTLSHOST_LEN ) return ERR_BADLEN;
    s = sock_by_handle( req->handle );
    if ( s == NULL ) return ERR_BADDATA;
    req->name[sizeof( req->name ) - 1] = 0;
    str_copy( s->host, req->name, sizeof( s->host ) );
    /* naming a server turns a stream socket into a TLS one, so a caller
       that learns it wants TLS only at connect (the WfW bridge's port map)
       can upgrade a plain socket it already opened, before it connects */
    if ( s->type == SOCK_STREAM && s->tstate == T_CLOSED ) s->tls = 1;
    return 0;
}

static int do_setca( void *buf, u32 len )
{
    SOCK_CAREQ *req = (SOCK_CAREQ *)buf;
    const u8 *data = (const u8 *)buf + SOCKCA_HDR;

    if ( len < SOCKCA_HDR ) return ERR_BADLEN;
    if ( len < SOCKCA_HDR + req->len ) return ERR_BADLEN;
    if ( req->total == 0 || req->total > 65536 ) return ERR_BADDATA;
    if ( req->offset + req->len > req->total ) return ERR_BADDATA;

    if ( req->offset == 0 ) {                   /* the first chunk starts fresh */
        if ( tls_ca ) { free( tls_ca ); tls_ca = NULL; }
        tls_ca = (u8 *)malloc( req->total );    /* from the TLS pool, not the tight arena */
        if ( tls_ca == NULL ) return ERR_GENERAL;
        tls_catotal = req->total;
        tls_calen = 0;
    }
    if ( tls_ca == NULL || req->total != tls_catotal ) return ERR_GENERAL;
    if ( req->len ) mem_copy( tls_ca + req->offset, data, req->len );
    if ( req->offset + req->len > tls_calen ) tls_calen = req->offset + req->len;
    return 0;
}

/* ---------------------------------------------------------------- */
/* the four entries the kernel calls                                 */
/* ---------------------------------------------------------------- */

int __cdecl nd_call( u32 fn, void *buf, u32 len, u32 *answer )
{
    int result;

    pumping = 1;
    pump();

    switch ( fn ) {
    case SOCK_IFSTAT:  result = do_ifstat( buf, len ); if ( !result ) *answer = SOCKIF_LEN; break;
    case SOCK_OPEN:    result = do_open( buf, len, answer ); break;
    case SOCK_CLOSE:   result = do_close( buf, len ); break;
    case SOCK_CONNECT: result = do_connect( buf, len ); break;
    case SOCK_LISTEN:  result = do_listen( buf, len ); break;
    case SOCK_ACCEPT:  result = do_accept( buf, len, answer ); break;
    case SOCK_SEND:    result = do_send( buf, len, answer ); break;
    case SOCK_RECV:    result = do_recv( buf, len, answer ); break;
    case SOCK_STATE:   result = do_state( buf, len, answer ); break;
    case SOCK_RESOLVE: result = do_resolve( buf, len, answer ); break;
    case SOCK_PING:    result = do_ping( buf, len, answer ); break;
    case SOCK_TLSHOST: result = do_tlshost( buf, len ); if ( !result ) *answer = 0; break;
    case SOCK_SETCA:   result = do_setca( buf, len ); if ( !result ) *answer = 0; break;
    case SOCK_RXFOREIGN:
        /* the WfW bridge collects a frame the stack does not own (NetBEUI)
           and indicates it to its protocol.  pump() at the top of nd_call
           has already run eth_input, so the ring holds what just arrived */
        *answer = fgn_get( buf, len );
        result = 0;
        break;
    case SOCK_PUMP:
        /* the bridge's one step: take frames off the card, run the
           protocol over them (pump already ran at the top, but the queue
           was empty until nic_service), and run the timers the tick
           otherwise would */
        nic_service();
        pump();
        timers();
        *answer = 0;
        result = 0;
        break;
    case SOCK_NICSTATS:
        /* hand back the card's own frame/error counts, so a program can
           see whether frames are leaving and arriving */
        result = nds_niccall( 0, NIC_STATS, buf, len, answer );
        break;
    default:           result = ERR_FUNC; break;
    }

    pumping = 0;
    return result;
}

int __cdecl nd_init( const char *line )
{
    NIC_INFOBLOCK info;
    u32 answer;
    int i;
    char text[64];

    (void)line;

    if ( nds_niccount() == 0 ) {
        nds_print( "TCP/IP: no network card is loaded - load the card driver first\r\n" );
        return 1;
    }
    if ( nds_niccall( 0, NIC_INFO, &info, sizeof( info ), &answer ) != 0 ) {
        nds_print( "TCP/IP: the card would not say what it is\r\n" );
        return 1;
    }
    mem_copy( if_mac, info.mac, 6 );
    str_copy( if_name, info.name, sizeof( if_name ) );

    rnd_state = nds_ticks() ^ ( (u32)if_mac[5] << 16 ) ^ get32( if_mac + 1 );

    /* the socket buffers, once, from the arena (never freed, never from
       a tick) */
    for ( i = 0; i < SOCK_MAX; i++ ) {
        sock[i].rxbuf = (u8 *)nds_alloc( RXCAP );
        sock[i].txbuf = (u8 *)nds_alloc( TXCAP );
        if ( sock[i].rxbuf == NULL || sock[i].txbuf == NULL ) {
            nds_print( "TCP/IP: not enough memory for the sockets\r\n" );
            return 1;
        }
    }

    /* reserve the TLS heap now, while the arena is still plentiful: a TLS
       socket wants ~200K, and once WfW is up and parks the kernel the arena
       is nearly gone.  Without it, TLS sockets just fail to open. */
    if ( tlsrt_init( TLS_POOL ) == 0 )
        nds_print( "TCP/IP: no memory for TLS; HTTPS sockets will not open\r\n" );

    nds_tickhook();
    running = 1;

    /* DHCP runs from here on, on the clock: it is bound once a program
       finds the interface up (SOCK_IFSTAT) */
    dhcp_start();

    str_copy( text, "TCP/IP on ", sizeof( text ) );
    i = 0; while ( text[i] ) i++;
    str_copy( text + i, if_name, sizeof( text ) - i );
    i = 0; while ( text[i] ) i++;
    str_copy( text + i, ", asking DHCP for an address\r\n", sizeof( text ) - i );
    nds_print( text );
    return 0;
}

void __cdecl nd_park( int back )
{
    if ( !back ) running = 0;
    else running = 1;
}

void __cdecl nd_halt( void )
{
    running = 0;
}

int __cdecl nd_irq( u32 line )
{
    (void)line;
    return 0;                           /* the stack has no interrupt of its own */
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
    NDT_SIGVAL, NDT_VERSION, NDTF_STACK,
    { 'T', 'C', 'P', 'I', 'P', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
