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
 * it works.  INIT waits up to five seconds for the first DHCP lease (or
 * takes /IP= from the DEVICE= line); the clock keeps the lease after.
 *
 * NOTHING BLOCKS once it is up.  A tick drains whatever the card has
 * received and runs the timers; a socket call drains too, then does
 * what it can at once and returns.  The waiting - for a handshake to
 * finish, for a byte to arrive - is the caller's, in TOOLS\SOCKLIB.
 *
 * AN ADDRESS IS A uint32_t HERE, host order (10.0.2.2 is 0x0A000202), and a
 * port is a uint16_t in the natural order; the big-endian wire fields are
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
#include "x509.h"             /* ...and its reasons a certificate is refused */

/* TCPSVC.ASM: the two kernel services NDSTART.ASM does not bridge */
int __cdecl nds_readfile( const char *path, void *buf, uint32_t room, uint32_t *got, uint32_t *size );
uint32_t __cdecl nds_stamp( void );
#include "rng.h"

#define NULL ((void *)0)

/* the timer's ports and the interrupt flag, for fine_now - the one
   piece of hardware the stack reads itself */
uint8_t  inb( uint16_t port );
#pragma aux inb = "in al,dx" parm [dx] value [al] modify exact [al];
void     outb( uint16_t port, uint8_t value );
#pragma aux outb = "out dx,al" parm [dx] [al] modify exact [];
uint32_t int_off( void );
#pragma aux int_off = "pushfd" "pop eax" "cli" value [eax] modify exact [eax];
void     int_back( uint32_t flags );
#pragma aux int_back = "push eax" "popfd" parm [eax] modify exact [];

/* A TICK COUNT THAT DOES NOT GO BACK.  nds_ticks() is the BIOS's count
   at 40:6C, and that goes back to 0 at midnight.  Every deadline here is
   a count compared by difference, so a deadline set before midnight for
   after it was not met until the same time the next day: a 24-hour lease
   taken in the afternoon was never renewed, and a TCP retransmission due
   at 23:59:59 waited a day (2026-11-02).  This counts on across midnight
   instead.  A count that goes back by more than an hour's worth is not
   midnight but a program setting the clock, and counts as no time.  Ticks
   and socket calls both ask, so the interrupts are off while it adds. */
#define TICKS_PER_DAY 1573040UL
#define TICKS_HOUR    65543UL

static uint32_t tk_bios, tk_count;

static uint32_t ticks( void )
{
    uint32_t flags = int_off();
    uint32_t bios = nds_ticks(), step;

    if ( bios >= tk_bios ) step = bios - tk_bios;
    else {
        step = TICKS_PER_DAY - tk_bios + bios;
        if ( step > TICKS_HOUR ) step = 0;
    }
    tk_bios = bios;
    tk_count += step;
    bios = tk_count;
    int_back( flags );
    return bios;
}

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
    uint8_t used;
    uint8_t type;               /* SOCK_STREAM or SOCK_DGRAM */
    uint8_t tstate;             /* T_* */
    uint8_t err;                /* why it failed: NET_REFUSED, NET_TLSCERT... */
    uint8_t why;                /* CERT_* with NET_TLSCERT */

    uint32_t lip, rip;          /* local and remote address */
    uint16_t lport, rport;

    /* TCP sequence space */
    uint32_t iss;               /* our initial sequence number */
    uint32_t snd_una;           /* oldest unacked - the first txbuf byte */
    uint32_t snd_nxt;           /* next sequence to send */
    uint32_t snd_wnd;           /* what the peer will take */
    uint32_t irs;               /* the peer's initial sequence number */
    uint32_t rcv_nxt;           /* next sequence expected */

    uint8_t *txbuf;             /* bytes queued to send, from snd_una */
    unsigned int txstart, txlen;
    uint8_t *rxbuf;             /* bytes arrived, waiting to be read */
    unsigned int rxstart, rxlen;
    uint32_t wnd_adv;           /* the receive window the peer last heard */

    uint8_t  finsent;           /* our FIN has gone out */
    uint32_t finseq;            /* ...occupying this sequence */
    uint8_t  finacked;
    uint8_t  closing;           /* the app has asked to close */

    unsigned int rto;           /* the retransmit timeout, in ticks */
    uint32_t rto_at;            /* when to retransmit next (0: no timer) */
    uint8_t  retries;
    uint32_t wait_at;           /* TIME_WAIT / handshake give-up deadline */

    /* a listening socket keeps the port; children carry the connection */
    uint16_t listenport;
    uint8_t  listening;

    /* TLS (SOCK_TLS): the handshake and record crypto run kernel-side over
       the raw TCP stream.  The TLS state rides above tstate: tls_state is
       TLSX_* while tstate is the TCP connection underneath. */
    uint8_t  tls;               /* 1: a TLS socket */
    uint8_t  tls_state;         /* TLSX_* */
    uint8_t  tls_eof;           /* the peer sent close_notify */
    tls_conn *tc;               /* the TLS connection (arena, from tls_new) */
    nst_rng  *rng;              /* its RNG (arena; must outlive tc) */
    uint8_t *trx;               /* one inbound record, being assembled */
    unsigned int trxlen;
    uint8_t *tapp;              /* decrypted plaintext waiting for the app */
    unsigned int tapstart, taplen;
    char     host[128];         /* server name: SNI and the cert to match */
    char     now14[15];         /* the time the certificate is checked
                                   against, YYYYMMDDHHMMSS; "" unchecked */
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
static uint8_t *tls_ca;         /* concatenated DER roots, or NULL */
static unsigned int tls_calen;  /* bytes present so far */
static unsigned int tls_catotal; /* bytes the bundle will be */
static uint8_t *tls_stk;        /* the crypto stack's base */
static uint32_t tls_stktop;     /* ...and its top (ESP starts here) */
static uint8_t  tls_scratch[TLS_RECCAP]; /* one outbound record */

extern uint32_t __cdecl tls_run( void *newtop, void *fn, void *arg );
extern unsigned int tlsrt_init( unsigned int want ); /* grab the TLS heap pool; 0 if none */
extern void *malloc( unsigned bytes );       /* the TLS pool (TLSRT.C), not DOS */
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

static uint8_t  if_mac[6];
static uint32_t if_ip, if_mask, if_gw, if_dns;
static uint32_t if_lease;
static uint8_t  if_up;
static uint8_t  if_dhcp;           /* DHS_* */
static uint8_t  if_static;         /* /IP= was given: DHCP never runs */
static char     if_name[32];
static uint32_t if_wins;           /* the WINS server: /WINS=, or DHCP option 44 */
static uint32_t if_dns2, if_dns3;  /* more DNS servers, asked when if_dns is not */
static char     nb_name[16];       /* our NetBIOS name (/NAME=), upper case */
static char     host_name[64];     /* our DNS host name, lower case: the same name */
static char     dom_name[64];      /* our DNS domain: TCPIP.INI's, else DHCP's; "" none */
static uint8_t  dom_fixed;         /* TCPIP.INI named the domain: DHCP's is not taken */
static uint8_t  reg_dns = 1;       /* RegisterDNS=: send our name to DNS ourselves */
static const uint8_t *rx_srcmac;   /* the station that sent the frame being read */

static uint16_t ip_id = 1;
static uint8_t  running;
static volatile uint8_t pumping; /* a pump is in progress: a tick stands off */

static uint8_t tx_frame[1600];
static uint8_t rx_frame[1600];

/* Frames the stack does not own (not IP, not ARP - NetBEUI and the like)
   are held in a small ring for the WfW bridge to collect through
   SOCK_RXFOREIGN and indicate to its bound protocol.  Without a bridge
   nothing drains the ring; it simply drops the oldest and stays small. */
#define FGN_N    4                      /* a power of two; holds FGN_N-1 frames */
#define FGN_CAP  1518
static uint8_t  fgn_buf[FGN_N][FGN_CAP];
static uint16_t fgn_len[FGN_N];
static uint8_t  fgn_head, fgn_tail;     /* head = next to read, tail = next to fill */

/* the one echo and the one name lookup the stack carries at a time */
static struct {
    uint8_t      active;
    uint32_t     ip;
    uint16_t     id, seq;
    unsigned int state;
    uint32_t     sent_at;
    uint32_t     sent_fine;     /* fine_now() when the echo went */
    unsigned int rtt;
    uint32_t     give_up;
    uint8_t      tries;
} ping;

static struct {
    uint8_t      active;
    uint8_t      step;          /* RS_*: who is being asked */
    uint32_t     ip;
    unsigned int state;
    uint16_t     id;
    uint16_t     lport;
    uint32_t     give_up;
    uint8_t      tries;
    char         name[128];
    char         qname[192];    /* what DNS is asked: name, or name.domain */
} res;

/* a lookup is over, found or not: callers asking for the same name are
   told so - a failure for RES_HOLD ticks, then a new attempt is made */
#define RES_HOLD     55
static uint32_t res_done_at;
static void res_end( void )
{
    res.active = 0;
    res_done_at = ticks();
}

#define RS_WINS      0          /* the WINS server, by NetBIOS name */
#define RS_BCAST     1          /* the network, by NetBIOS broadcast */
#define RS_DNS       2          /* the DNS server */

/* our name sent to DNS (RFC 2136), one record at a time: find the zone
   the name is in and its primary server (SOA), that server's address,
   then send the update.  The address record always; the reverse one too
   when the address is our own (/IP=), since then no DHCP server adds it */
static struct {
    uint8_t  want;              /* send when the chance comes */
    uint8_t  active;
    uint8_t  step;              /* RG_* */
    uint8_t  job;               /* RJ_* */
    uint8_t  tries;
    uint16_t id, lport;
    uint32_t give_up;
    uint32_t asked;             /* the DNS server the zone question went to */
    uint32_t master;            /* the zone's primary server */
    uint32_t ip;                /* the address being sent */
    uint32_t done_ip;           /* the address last taken; 0 none */
    uint32_t done_at;           /* ticks() when it was */
    uint32_t retry_at;          /* after no answer: try again then; 0 not */
    uint16_t state, rcode;      /* DREG_*, and the server's answer */
    char     owner[128];        /* the name the record is for */
    char     zone[128];         /* the zone it is in */
    char     mname[128];        /* the zone's primary server, by name */
} reg;

#define RG_SOA       0          /* asking which zone, and its primary */
#define RG_MASTER    1          /* asking the primary's address */
#define RG_UPDATE    2          /* the update is out */

#define RJ_A         0          /* name -> address */
#define RJ_PTR       1          /* address -> name, in in-addr.arpa */

#define REG_TTL      1200       /* what Windows gives the records it adds */
#define REG_REFRESH  TICKS_PER_DAY      /* sent again once a day */
#define REG_RETRY    ( 10UL * 60 * 18 ) /* ...or ten minutes after no answer */

/* ---------------------------------------------------------------- */
/* what a library would have supplied                                */
/* ---------------------------------------------------------------- */

static void mem_copy( void *to, const void *from, unsigned int len )
{
    uint8_t *dst = (uint8_t *)to;
    const uint8_t *src = (const uint8_t *)from;
    while ( len-- ) *dst++ = *src++;
}

static void mem_zero( void *to, unsigned int len )
{
    uint8_t *dst = (uint8_t *)to;
    while ( len-- ) *dst++ = 0;
}

/* the foreign-frame ring (declared up by rx_frame): hold a frame the stack
   does not own, give the oldest back to the bridge on request */
static void fgn_put( const uint8_t *frame, unsigned int len )
{
    uint8_t next;

    if ( len > FGN_CAP ) return;        /* a foreign frame this large is not ours */
    next = (uint8_t)( ( fgn_tail + 1 ) & ( FGN_N - 1 ) );
    if ( next == fgn_head ) {           /* ring full: drop the oldest */
        fgn_head = (uint8_t)( ( fgn_head + 1 ) & ( FGN_N - 1 ) );
    }
    mem_copy( fgn_buf[fgn_tail], frame, len );
    fgn_len[fgn_tail] = (uint16_t)len;
    fgn_tail = next;
}

static unsigned int fgn_get( uint8_t *buf, unsigned int max )
{
    unsigned int len;

    if ( fgn_head == fgn_tail ) return 0;       /* empty */
    len = fgn_len[fgn_head];
    if ( len > max ) len = max;
    mem_copy( buf, fgn_buf[fgn_head], len );
    fgn_head = (uint8_t)( ( fgn_head + 1 ) & ( FGN_N - 1 ) );
    return len;
}

static void str_copy( char *to, const char *from, unsigned int max )
{
    unsigned int i;
    for ( i = 0; i + 1 < max && from[i]; i++ ) to[i] = from[i];
    to[i] = 0;
}

static int str_same( const char *one, const char *two )
{
    while ( *one && *one == *two ) { one++; two++; }
    return *one == *two;
}

static unsigned int str_len( const char *text )
{
    unsigned int n = 0;
    while ( text[n] ) n++;
    return n;
}

static unsigned char lower( unsigned char ch )
{
    return ( ch >= 'A' && ch <= 'Z' ) ? (unsigned char)( ch + 32 ) : ch;
}

/* the same but for case, as names are */
static int str_samei( const char *one, const char *two )
{
    while ( *one && lower( (unsigned char)*one ) == lower( (unsigned char)*two ) ) { one++; two++; }
    return lower( (unsigned char)*one ) == lower( (unsigned char)*two );
}

static void put16( uint8_t *buf, uint16_t value )
{
    buf[0] = (uint8_t)( value >> 8 );
    buf[1] = (uint8_t)value;
}

static void put32( uint8_t *buf, uint32_t value )
{
    buf[0] = (uint8_t)( value >> 24 );
    buf[1] = (uint8_t)( value >> 16 );
    buf[2] = (uint8_t)( value >> 8 );
    buf[3] = (uint8_t)value;
}

static uint16_t get16( const uint8_t *buf )
{
    return (uint16_t)( ( (uint16_t)buf[0] << 8 ) | buf[1] );
}

static uint32_t get32( const uint8_t *buf )
{
    return ( (uint32_t)buf[0] << 24 ) | ( (uint32_t)buf[1] << 16 ) |
           ( (uint32_t)buf[2] << 8 ) | buf[3];
}

/* the Internet checksum, over data read as big-endian 16-bit words */
static uint32_t cksum_add( const uint8_t *buf, unsigned int len, uint32_t sum )
{
    unsigned int i;
    for ( i = 0; i + 1 < len; i += 2 ) sum += ( (uint32_t)buf[i] << 8 ) | buf[i + 1];
    if ( i < len ) sum += (uint32_t)buf[i] << 8;
    return sum;
}

static uint16_t cksum_done( uint32_t sum )
{
    while ( sum >> 16 ) sum = ( sum & 0xFFFF ) + ( sum >> 16 );
    return (uint16_t)( ~sum & 0xFFFF );
}

/* a pseudo-random seed for sequence numbers and ids, out of the clock */
static uint32_t rnd_state;
static uint32_t rnd( void )
{
    rnd_state = rnd_state * 1103515245L + 12345L;
    return rnd_state;
}

/* ---------------------------------------------------------------- */
/* the card, reached through the kernel                              */
/* ---------------------------------------------------------------- */

static int nic_send( const uint8_t *frame, unsigned int len )
{
    uint32_t answer;
    return nds_niccall( 0, NIC_SEND, (void *)frame, len, &answer ) == 0;
}

static unsigned int nic_recv( uint8_t *buf, unsigned int max )
{
    uint32_t answer;
    if ( nds_niccall( 0, NIC_RECV, buf, max, &answer ) != 0 ) return 0;
    return answer;
}

/* drain the card's hardware ring into its software queue - what the
   IRQ and the clock tick do natively.  The WfW bridge has no IRQ and no
   tick of its own, so SOCK_PUMP asks for this before it pumps */
static void nic_service( void )
{
    uint32_t answer;
    nds_niccall( 0, NIC_SERVICE, (void *)0, 0, &answer );
}

/* ---------------------------------------------------------------- */
/* ARP: a small cache, and who-has for the gateway                   */
/* ---------------------------------------------------------------- */

#define ARP_N   8
#define ARP_RETRY 3             /* ticks: a send that waited on ARP goes again */

static struct { uint32_t ip; uint8_t mac[6]; uint8_t valid; uint32_t at; } arp[ARP_N];

static uint8_t *arp_find( uint32_t ip )
{
    int i;
    for ( i = 0; i < ARP_N; i++ ) {
        if ( arp[i].valid && arp[i].ip == ip ) return arp[i].mac;
    }
    return NULL;
}

static void arp_put( uint32_t ip, const uint8_t *mac )
{
    int i, slot = 0;
    uint32_t oldest = 0xFFFFFFFFL;

    for ( i = 0; i < ARP_N; i++ ) {
        if ( arp[i].valid && arp[i].ip == ip ) { slot = i; break; }
        if ( !arp[i].valid ) { slot = i; break; }
        if ( arp[i].at < oldest ) { oldest = arp[i].at; slot = i; }
    }
    arp[slot].ip = ip;
    mem_copy( arp[slot].mac, mac, 6 );
    arp[slot].valid = 1;
    arp[slot].at = ticks();
}

static void arp_send( uint16_t op, uint32_t target_ip, const uint8_t *target_mac )
{
    uint8_t *frame = tx_frame;
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

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

static void arp_request( uint32_t ip )
{
    arp_send( ARP_REQUEST, ip, NULL );
}

static void arp_input( const uint8_t *frame, unsigned int len )
{
    uint16_t op;
    uint32_t sender_ip, target_ip;

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

static int same_subnet( uint32_t ip )
{
    return if_mask && ( ( ip ^ if_ip ) & if_mask ) == 0;
}

/* where a frame for dst goes first: dst itself, or the gateway */
static uint32_t next_hop( uint32_t dst )
{
    return same_subnet( dst ) ? dst : if_gw;
}

static int ip_send( uint32_t dst, uint8_t proto, unsigned int plen )
{
    uint8_t *mac, *ih = tx_frame + ETH_HDR;
    uint32_t nexthop;
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    if ( dst == IP_BCAST || ( if_mask && same_subnet( dst ) && ( dst | if_mask ) == IP_BCAST ) ) {
        mac = (uint8_t *)bcast;         /* everyone, or everyone on our network */
    } else {
        nexthop = next_hop( dst );
        mac = arp_find( nexthop );
        if ( mac == NULL ) { arp_request( nexthop ); return 0; }
    }
    mem_copy( tx_frame, mac, 6 );
    mem_copy( tx_frame + 6, if_mac, 6 );
    put16( tx_frame + 12, ETH_IP );

    ih[0] = 0x45;
    ih[1] = 0;
    put16( ih + 2, (uint16_t)( 20 + plen ) );
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
static uint16_t l4_cksum( uint32_t src, uint32_t dst, uint8_t proto, unsigned int plen )
{
    uint8_t pseudo[12];
    uint32_t sum;

    put32( pseudo, src );
    put32( pseudo + 4, dst );
    pseudo[8] = 0;
    pseudo[9] = proto;
    put16( pseudo + 10, (uint16_t)plen );
    sum = cksum_add( pseudo, 12, 0 );
    sum = cksum_add( tx_frame + 34, plen, sum );
    return cksum_done( sum );
}

/* ---------------------------------------------------------------- */
/* UDP                                                               */
/* ---------------------------------------------------------------- */

static int udp_send( uint32_t dst, uint16_t dport, uint16_t sport, const uint8_t *data, unsigned int dlen )
{
    uint8_t *uh = tx_frame + 34;
    uint16_t ck;

    put16( uh, sport );
    put16( uh + 2, dport );
    put16( uh + 4, (uint16_t)( 8 + dlen ) );
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
static void dhcp_input( const uint8_t *bootp, unsigned int len );
static void dns_input( const uint8_t *dns, unsigned int len );
static void reg_input( uint32_t src, const uint8_t *dns, unsigned int len );
#define NBNS_PORT 137
static void nbns_input( uint32_t src, uint16_t sport, const uint8_t *data, unsigned int dlen );
static SOCKET *udp_socket_for( uint16_t port );
static void udp_queue( SOCKET *s, uint32_t src, uint16_t sport, const uint8_t *data, unsigned int dlen );

static void udp_input( uint32_t src, const uint8_t *uh, unsigned int len )
{
    uint16_t sport, dport, ulen;
    const uint8_t *data;
    unsigned int dlen;
    SOCKET *s;

    if ( len < 8 ) return;
    sport = get16( uh );
    dport = get16( uh + 2 );
    ulen = get16( uh + 4 );
    if ( ulen < 8 || ulen > len ) ulen = (uint16_t)len;
    data = uh + 8;
    dlen = ulen - 8;

    if ( dport == 68 ) { dhcp_input( uh, len ); return; }
    if ( dport == NBNS_PORT ) { nbns_input( src, sport, data, dlen ); return; }
    if ( res.active && dport == res.lport && sport == 53 ) {
        dns_input( data, dlen );
        return;
    }
    if ( reg.active && dport == reg.lport && sport == 53 ) {
        reg_input( src, data, dlen );
        return;
    }
    s = udp_socket_for( dport );
    if ( s ) udp_queue( s, src, sport, data, dlen );
}

/* ---------------------------------------------------------------- */
/* ICMP                                                              */
/* ---------------------------------------------------------------- */

/* A CLOCK FINER THAN THE TICK, FOR PING.  A reply on a LAN takes well
   under a millisecond, and timed by the 55ms tick every round trip read
   "<55ms" or "55ms" (2026-11-01).  Channel 0 of the timer says how far
   into the tick it is.  The 8254's read-back command latches the count
   and the status together; in the square-wave mode the BIOS sets, the
   count runs down by two twice a tick, the output high for the first
   half.  The answer is in timer counts, 1,193,182 a second, and wraps
   after an hour.  A tick not yet counted (interrupts off) would make the
   clock go back, so it stands still instead. */
#define FINE_PER_MS  1193UL

static uint32_t fine_last;

static uint32_t fine_now( void )
{
    uint32_t flags, tick, into, now, count;
    uint8_t  status, low, high;

    do {
        tick = ticks();
        flags = int_off();
        outb( 0x43, 0xC2 );             /* read-back: count + status, channel 0 */
        status = inb( 0x40 );
        low = inb( 0x40 );
        high = inb( 0x40 );
        int_back( flags );
    } while ( ticks() != tick );
    count = ( (uint32_t)high << 8 ) | low;
    if ( count == 0 ) count = 0x10000UL;
    if ( ( status & 0x06 ) == 0x06 ) {          /* mode 3, the square wave */
        into = ( 0x10000UL - count ) / 2;
        if ( !( status & 0x80 ) ) into += 0x8000UL;     /* output low: second half */
    } else {                                    /* mode 2, the rate generator */
        into = 0x10000UL - count;
    }
    now = tick * 0x10000UL + into;
    if ( (int32_t)( now - fine_last ) < 0 && fine_last - now < 0x10000UL ) now = fine_last;
    fine_last = now;
    return now;
}

static int icmp_echo( uint32_t dst, uint16_t id, uint16_t seq )
{
    uint8_t *ic = tx_frame + 34;
    unsigned int i, plen = 8 + 32;

    ic[0] = ICMP_ECHOREQ;
    ic[1] = 0;
    put16( ic + 2, 0 );
    put16( ic + 4, id );
    put16( ic + 6, seq );
    for ( i = 0; i < 32; i++ ) ic[8 + i] = (uint8_t)( 'a' + ( i & 15 ) );
    put16( ic + 2, cksum_done( cksum_add( ic, plen, 0 ) ) );
    return ip_send( dst, IP_ICMP, plen );
}

static void icmp_input( uint32_t src, const uint8_t *ic, unsigned int len )
{
    if ( len < 8 ) return;
    if ( ic[0] == ICMP_ECHOREQ ) {
        /* answer a ping to us, in place: swap type to a reply */
        uint8_t *out = tx_frame + 34;
        unsigned int plen = len;
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
            ping.rtt = ( fine_now() - ping.sent_fine ) / FINE_PER_MS;
            ping.state = NET_DONE;
            ping.active = 0;
        }
    }
}

/* ---------------------------------------------------------------- */
/* IP in                                                             */
/* ---------------------------------------------------------------- */

static void tcp_input( uint32_t src, uint32_t dst, const uint8_t *th, unsigned int len );

static void ip_input( const uint8_t *frame, unsigned int len )
{
    const uint8_t *ih = frame + ETH_HDR;
    unsigned int ihl, total, plen;
    uint32_t src, dst;
    const uint8_t *l4;

    if ( len < ETH_HDR + 20 ) return;
    if ( ( ih[0] >> 4 ) != 4 ) return;
    ihl = ( ih[0] & 0x0F ) * 4;
    if ( ihl < 20 || len < ETH_HDR + ihl ) return;
    total = get16( ih + 2 );
    if ( total < ihl || total > len - ETH_HDR ) total = len - ETH_HDR;
    plen = total - ihl;
    if ( get16( ih + 6 ) & 0x3FFF ) return;    /* a fragment: not reassembled */
    src = get32( ih + 12 );
    dst = get32( ih + 16 );
    l4 = ih + ihl;

    switch ( ih[9] ) {
    case IP_ICMP: icmp_input( src, l4, plen ); break;
    case IP_UDP:  udp_input( src, l4, plen ); break;
    case IP_TCP:  tcp_input( src, dst, l4, plen ); break;
    }
}

static void eth_input( const uint8_t *frame, unsigned int len )
{
    uint16_t type;

    if ( len < ETH_HDR ) return;
    rx_srcmac = frame + 6;
    type = get16( frame + 12 );
    if ( type == ETH_ARP ) arp_input( frame, len );
    else if ( type == ETH_IP ) ip_input( frame, len );
    else fgn_put( frame, len );         /* NetBEUI etc.: hold for the WfW bridge */
}

/* ---------------------------------------------------------------- */
/* NAMES: this computer's, and its domain's                          */
/* ---------------------------------------------------------------- */

/* One name serves both: NetBIOS's, up to 15 characters in upper case, and
   DNS's host name, up to 63 in lower case.  /NAME= gives the NetBIOS one
   and the host name follows from it; TCPIP.INI's HostName= gives the host
   name and the NetBIOS one is its first 15 characters, as Windows does.
   With neither, the name is made from the card's address, PMDOS-0A1B2C,
   which no other PM-DOS machine on the network has. */

static unsigned char nb_upper( unsigned char ch );

static int is_alnum( unsigned char ch )
{
    return ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) || ( ch >= '0' && ch <= '9' );
}

/* a DNS host name: 1 to 63 letters, digits and hyphens, not starting or
   ending with a hyphen.  0 if it is not one */
static int host_ok( const char *name )
{
    unsigned int n;

    for ( n = 0; name[n]; n++ ) {
        if ( n >= 63 || !( is_alnum( (unsigned char)name[n] ) || name[n] == '-' ) ) return 0;
    }
    return n > 0 && name[0] != '-' && name[n - 1] != '-';
}

/* a DNS domain such as corp.example.com: host names with a dot between,
   63 characters at most, kept in lower case; a dot at the end (or a NUL,
   as some DHCP servers send) is dropped.  0 if it is not one */
static int take_domain( const char *src, char *dst )
{
    char          out[64];
    unsigned int  n = 0, label = 0;
    unsigned char ch;

    for ( ; ( ch = (unsigned char)*src ) != 0; src++ ) {
        if ( n >= sizeof( out ) - 1 ) return 0;
        if ( ch == '.' ) {
            if ( label == 0 || out[n - 1] == '-' ) return 0;
            label = 0;
        } else if ( is_alnum( ch ) || ch == '-' ) {
            if ( label == 0 && ch == '-' ) return 0;
            if ( ++label > 63 ) return 0;
        } else return 0;
        out[n++] = (char)lower( ch );
    }
    if ( n && out[n - 1] == '.' ) n--;
    if ( n == 0 || out[n - 1] == '-' ) return 0;
    out[n] = 0;
    str_copy( dst, out, 64 );
    return 1;
}

/* the host name from the NetBIOS name: lower case, and what DNS cannot
   carry (_, !, $ and the like) a hyphen */
static void host_from_nb( void )
{
    unsigned int n;

    for ( n = 0; nb_name[n]; n++ ) {
        unsigned char ch = (unsigned char)nb_name[n];
        host_name[n] = (char)( is_alnum( ch ) ? lower( ch ) : '-' );
    }
    host_name[n] = 0;
}

/* the NetBIOS name from the host name: its first 15, upper case */
static void nb_from_host( void )
{
    unsigned int n;

    for ( n = 0; n < 15 && host_name[n]; n++ ) nb_name[n] = (char)nb_upper( (unsigned char)host_name[n] );
    nb_name[n] = 0;
}

/* whichever name was given makes the other; with none, PMDOS- and the
   last three bytes of the card's address */
static void names_settle( void )
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned int i;

    if ( nb_name[0] == 0 && host_name[0] == 0 ) {
        str_copy( nb_name, "PMDOS-", sizeof( nb_name ) );
        for ( i = 0; i < 3; i++ ) {
            nb_name[6 + 2 * i] = hex[if_mac[3 + i] >> 4];
            nb_name[7 + 2 * i] = hex[if_mac[3 + i] & 15];
        }
        nb_name[12] = 0;
    }
    if ( nb_name[0] ) host_from_nb();
    else nb_from_host();
}

/* is a name to be looked up this computer's own? */
static int name_is_us( const char *name )
{
    unsigned int n = str_len( host_name ), i;

    if ( str_samei( name, host_name ) || str_samei( name, nb_name ) ) return 1;
    if ( dom_name[0] == 0 || n == 0 ) return 0;
    for ( i = 0; i < n; i++ ) {         /* host.domain: a short name stops here */
        if ( lower( (unsigned char)name[i] ) != (unsigned char)host_name[i] ) return 0;
    }
    return name[n] == '.' && str_samei( name + n + 1, dom_name );
}

/* ---------------------------------------------------------------- */
/* DHCP - the first answer waited for at INIT, the rest on the clock */
/* ---------------------------------------------------------------- */

/* RFC 2131: DISCOVER, an OFFER back, REQUEST, an ACK back - bound.  At
   half the lease (T1) a REQUEST goes to the server that gave it; at
   seven-eighths (T2) to any server, by broadcast; at the end the address
   is let go and the hunt starts again.  A NAK at any point starts it
   again too.  Nothing here prints or waits: it runs from the pump and
   the tick, and nd_init watches dhcp_state to see how it went. */

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6
#define DHCP_RELEASE  7

#define DH_OFF        0          /* not asking: static, or released */
#define DH_DISCOVER   1          /* a DISCOVER is out, waiting for an OFFER */
#define DH_REQUEST    2          /* a REQUEST is out, waiting for an ACK */
#define DH_BOUND      3
#define DH_RENEW      4          /* past T1: REQUESTs to our server */
#define DH_REBIND     5          /* past T2: REQUESTs to anyone */

#define TICKS_SEC     18         /* ~18.2 a second; the .2 is in secs_ticks */
#define DH_FOREVER    0xFFFFFFFFL
#define DH_LONGEST    0x06000000L /* ~3 years: longer is treated as no end,
                                    which keeps every deadline in the
                                    signed range of a tick difference */
#define DH_RETRY_MIN  ( 60 * TICKS_SEC )    /* RFC 2131's floor while renewing */
#define DH_BACKOFF    ( 64 * TICKS_SEC )    /* the longest wait between DISCOVERs */

static uint32_t dhcp_xid;
static volatile uint8_t dhcp_state;  /* nd_init spins on this while the tick runs */
static uint32_t dhcp_next;           /* when to send again */
static unsigned int dhcp_wait;       /* the DISCOVER interval, doubling */
static uint8_t  dhcp_tries;
static uint32_t dhcp_offer_ip, dhcp_server_ip;
static uint32_t dhcp_bound_at;       /* the tick the lease was given or renewed */
static uint32_t dhcp_t1, dhcp_t2, dhcp_end; /* deadlines, in ticks */
static uint8_t  dhcp_timed;          /* 0: the lease has no end */
static uint32_t dns_fixed;           /* /DNS= - used whatever DHCP says */
static uint32_t dns_fixed2, dns_fixed3;
static uint32_t wins_fixed;          /* /WINS= - the same for the WINS server */
static uint16_t dhcp_acks;           /* ACKs taken: IPCONFIG /RENEW watches it move */

/* seconds to ticks, 18.2 a second, for a span under DH_LONGEST */
static uint32_t secs_ticks( uint32_t secs )
{
    return secs * TICKS_SEC + secs / 5;
}

/* ticks to seconds, without the multiply overflowing */
static uint32_t ticks_secs( uint32_t ticks )
{
    return ( ticks / 91 ) * 5 + ( ( ticks % 91 ) * 5 ) / 91;
}

static unsigned int dns_put_name( uint8_t *q, unsigned int n, const char *name );

/* Our name, for the DHCP server to put in DNS and in its list of leases:
   option 12, the host name, which home routers take; and option 81 (RFC
   4702), the full name, which a Windows DHCP server takes.  Option 81's
   flags say who adds what to DNS: with RegisterDNS= on and a domain
   known, we add the address record ourselves (reg_*) and the server adds
   only the reverse one; otherwise the server is asked to add both.  With
   no domain known the name is a partial one, and the server adds its own
   domain to it */
static unsigned int dhcp_names( uint8_t *b, unsigned int n )
{
    unsigned int len = str_len( host_name ), at;

    if ( len == 0 ) return n;
    b[n++] = 12; b[n++] = (uint8_t)len;
    mem_copy( b + n, host_name, len );
    n += len;

    b[n++] = 81;
    at = n++;                           /* the length, when it is known */
    b[n++] = (uint8_t)( ( reg_dns && dom_name[0] ) ? 0x04 : 0x05 ); /* E, and S or not */
    b[n++] = 0;                         /* RCODE1 and RCODE2: not used */
    b[n++] = 0;
    if ( dom_name[0] ) {
        char full[128];
        str_copy( full, host_name, sizeof( full ) );
        str_copy( full + len, ".", sizeof( full ) - len );
        str_copy( full + len + 1, dom_name, sizeof( full ) - len - 1 );
        n = dns_put_name( b, n, full );
    } else {
        n = dns_put_name( b, n, host_name ) - 1;        /* partial: no root */
    }
    b[at] = (uint8_t)( n - at - 1 );
    return n;
}

static unsigned int bootp_build( uint8_t type, uint32_t client_ip, uint32_t request_ip, uint32_t server_ip )
{
    uint8_t *b = tx_frame + 34 + 8; /* past the UDP header, filled by udp_send */
    unsigned int n;

    mem_zero( b, 548 );          /* the most a DHCP message need be */
    b[0] = 1;                    /* BOOTREQUEST */
    b[1] = 1;                    /* Ethernet */
    b[2] = 6;
    put32( b + 4, dhcp_xid );
    put32( b + 12, client_ip );         /* ciaddr: ours, when renewing */
    mem_copy( b + 28, if_mac, 6 );      /* chaddr */
    put32( b + 236, 0x63825363L );      /* the magic cookie */
    n = 240;
    b[n++] = 53; b[n++] = 1; b[n++] = type;             /* message type */
    if ( request_ip ) {
        b[n++] = 50; b[n++] = 4; put32( b + n, request_ip ); n += 4;
    }
    if ( server_ip ) {
        b[n++] = 54; b[n++] = 4; put32( b + n, server_ip ); n += 4;
    }
    if ( type != DHCP_RELEASE ) {
        b[n++] = 55; b[n++] = 6;        /* parameter request list */
        b[n++] = 1; b[n++] = 3; b[n++] = 6; b[n++] = 51;    /* mask, gw, dns, lease */
        b[n++] = 44;                                        /* WINS */
        b[n++] = 15;                                        /* the domain */
        n = dhcp_names( b, n );
    }
    b[n++] = 255;                       /* end */
    return n;
}

static void dhcp_send( uint8_t type )
{
    unsigned int n;

    switch ( dhcp_state ) {
    case DH_RENEW:              /* to our server, from our address */
        n = bootp_build( type, if_ip, 0, 0 );
        udp_send( dhcp_server_ip, 67, 68, NULL, n );
        break;
    case DH_REBIND:             /* to anyone, from our address */
        n = bootp_build( type, if_ip, 0, 0 );
        udp_send( IP_BCAST, 67, 68, NULL, n );
        break;
    default:                    /* SELECTING: from nowhere, naming the offer */
        if ( type == DHCP_REQUEST ) n = bootp_build( type, 0, dhcp_offer_ip, dhcp_server_ip );
        else n = bootp_build( type, 0, 0, 0 );
        udp_send( IP_BCAST, 67, 68, NULL, n );
        break;
    }
}

/* let the address go: nothing is sent from it after this */
static void dhcp_drop( void )
{
    if_ip = 0;
    if_mask = 0;
    if_gw = 0;
    if_dns = dns_fixed;
    if_dns2 = dns_fixed2;
    if_dns3 = dns_fixed3;
    if_wins = wins_fixed;
    if_up = 0;
    if_dhcp = DHS_DOWN;
}

/* start looking for a server: at INIT, after a NAK, at the lease's end,
   or for IPCONFIG /RENEW with no address */
static void dhcp_start( void )
{
    dhcp_drop();
    dhcp_xid = rnd();
    dhcp_offer_ip = 0;
    dhcp_server_ip = 0;
    dhcp_tries = 0;
    dhcp_state = DH_DISCOVER;
    dhcp_send( DHCP_DISCOVER );
    dhcp_wait = TICKS_SEC;              /* 1s, 2s, 4s ... up to DH_BACKOFF */
    dhcp_next = ticks() + dhcp_wait;
}

/* the lease's three deadlines, from the ACK's lease, T1 and T2 */
static void dhcp_schedule( uint32_t lease, uint32_t t1, uint32_t t2 )
{
    uint32_t now = ticks();

    dhcp_bound_at = now;
    if ( lease == DH_FOREVER || lease >= DH_LONGEST ) {
        dhcp_timed = 0;
        return;
    }
    if ( t1 == 0 || t1 >= lease ) t1 = lease / 2;
    if ( t2 == 0 || t2 >= lease || t2 <= t1 ) t2 = lease - lease / 8;
    if ( t2 <= t1 ) t2 = t1 + 1;        /* a lease of a second or two */
    dhcp_timed = 1;
    dhcp_t1 = now + secs_ticks( t1 );
    dhcp_t2 = now + secs_ticks( t2 );
    dhcp_end = now + secs_ticks( lease );
}

/* the next try while renewing or rebinding: half the time left to the
   deadline, but not under a minute, and never past the deadline */
static uint32_t dhcp_halfway( uint32_t now, uint32_t deadline )
{
    uint32_t left = deadline - now;
    uint32_t step = left / 2;

    if ( (int32_t)left <= 0 ) return now;
    if ( step < DH_RETRY_MIN ) step = DH_RETRY_MIN;
    if ( step > left ) step = left;
    return now + step;
}

/* a BOOTP reply arrived: move the state machine along.  Called from the
   pump, so it does not print or wait */
static void dhcp_input( const uint8_t *uh, unsigned int len )
{
    const uint8_t *b = uh + 8;
    unsigned int blen, n;
    uint8_t  type = 0;
    uint32_t yiaddr, server = 0, mask = 0, gw = 0, dns = 0, wins = 0;
    uint32_t dns2 = 0, dns3 = 0;
    char     dom[64];
    uint32_t lease = 0, t1 = 0, t2 = 0;
    uint8_t  have_lease = 0;

    dom[0] = 0;
    if ( dhcp_state == DH_OFF || dhcp_state == DH_BOUND ) return;
    if ( len < 8 + 240 ) return;
    blen = len - 8;
    if ( b[0] != 2 ) return;            /* BOOTREPLY */
    if ( get32( b + 4 ) != dhcp_xid ) return;
    if ( get32( b + 236 ) != 0x63825363L ) return;
    if ( !( b[28] == if_mac[0] && b[29] == if_mac[1] && b[30] == if_mac[2] &&
            b[31] == if_mac[3] && b[32] == if_mac[4] && b[33] == if_mac[5] ) ) return;

    yiaddr = get32( b + 16 );

    for ( n = 240; n + 1 < blen; ) {
        uint8_t opt = b[n], olen;
        if ( opt == 0 ) { n++; continue; }
        if ( opt == 255 ) break;
        olen = b[n + 1];
        if ( n + 2 + olen > blen ) break;
        switch ( opt ) {
        case 53: if ( olen >= 1 ) type = b[n + 2]; break;
        case 1:  if ( olen >= 4 ) mask = get32( b + n + 2 ); break;
        case 3:  if ( olen >= 4 ) gw = get32( b + n + 2 ); break;
        case 6:                         /* up to three DNS servers */
            if ( olen >= 4 ) dns = get32( b + n + 2 );
            if ( olen >= 8 ) dns2 = get32( b + n + 6 );
            if ( olen >= 12 ) dns3 = get32( b + n + 10 );
            break;
        case 15:                        /* the domain, maybe with a NUL */
            if ( olen > 0 && olen < sizeof( dom ) ) {
                mem_copy( dom, b + n + 2, olen );
                dom[olen] = 0;
            }
            break;
        case 44: if ( olen >= 4 ) wins = get32( b + n + 2 ); break;
        case 51: if ( olen >= 4 ) { lease = get32( b + n + 2 ); have_lease = 1; } break;
        case 54: if ( olen >= 4 ) server = get32( b + n + 2 ); break;
        case 58: if ( olen >= 4 ) t1 = get32( b + n + 2 ); break;
        case 59: if ( olen >= 4 ) t2 = get32( b + n + 2 ); break;
        }
        n += 2 + olen;
    }

    if ( type == DHCP_OFFER && dhcp_state == DH_DISCOVER ) {
        if ( yiaddr == 0 || server == 0 ) return;
        dhcp_offer_ip = yiaddr;
        dhcp_server_ip = server;
        dhcp_state = DH_REQUEST;
        dhcp_tries = 0;
        dhcp_send( DHCP_REQUEST );
        dhcp_next = ticks() + 2 * TICKS_SEC;
    } else if ( type == DHCP_ACK && dhcp_state != DH_DISCOVER ) {
        if ( yiaddr == 0 ) return;
        if_ip = yiaddr;
        if ( mask ) if_mask = mask;
        if ( gw ) if_gw = gw;
        if ( !dns_fixed && dns ) {
            if_dns = dns;
            if_dns2 = dns2;
            if_dns3 = dns3;
        }
        if_wins = wins_fixed ? wins_fixed : ( wins ? wins : if_wins );
        if ( !dom_fixed && dom[0] ) take_domain( dom, dom_name );
        if ( server ) dhcp_server_ip = server;
        if ( !have_lease ) lease = DH_FOREVER;
        if_lease = lease;
        dhcp_schedule( lease, t1, t2 );
        if_up = 1;
        if_dhcp = DHS_BOUND;
        dhcp_state = DH_BOUND;
        dhcp_acks++;
        arp_request( if_gw );           /* learn the gateway's address early */
        if ( if_ip != reg.done_ip ) reg.want = 1;   /* a new address for DNS */
    } else if ( type == DHCP_NAK && dhcp_state != DH_DISCOVER ) {
        dhcp_start();                   /* the server says no: begin again */
    }
}

/* every tick: resend what is unanswered, and keep the lease */
static void dhcp_tick( void )
{
    uint32_t now = ticks();

    switch ( dhcp_state ) {
    case DH_DISCOVER:
        if ( (int32_t)( now - dhcp_next ) < 0 ) break;
        dhcp_send( DHCP_DISCOVER );
        if ( dhcp_wait < DH_BACKOFF ) dhcp_wait *= 2;
        dhcp_next = now + dhcp_wait;
        break;
    case DH_REQUEST:
        if ( (int32_t)( now - dhcp_next ) < 0 ) break;
        if ( ++dhcp_tries > 3 ) { dhcp_start(); break; }    /* begin again */
        dhcp_send( DHCP_REQUEST );
        dhcp_next = now + 2 * TICKS_SEC;
        break;
    case DH_BOUND:
        if ( !dhcp_timed || (int32_t)( now - dhcp_t1 ) < 0 ) break;
        dhcp_state = DH_RENEW;
        dhcp_xid = rnd();
        dhcp_send( DHCP_REQUEST );
        dhcp_next = dhcp_halfway( now, dhcp_t2 );
        break;
    case DH_RENEW:
        if ( !dhcp_timed ) {            /* a /RENEW of a lease with no end */
            if ( (int32_t)( now - dhcp_next ) >= 0 ) dhcp_state = DH_BOUND;
        } else if ( (int32_t)( now - dhcp_t2 ) >= 0 ) {
            dhcp_state = DH_REBIND;
            dhcp_send( DHCP_REQUEST );
            dhcp_next = dhcp_halfway( now, dhcp_end );
        } else if ( (int32_t)( now - dhcp_next ) >= 0 ) {
            dhcp_send( DHCP_REQUEST );
            dhcp_next = dhcp_halfway( now, dhcp_t2 );
        }
        break;
    case DH_REBIND:
        if ( (int32_t)( now - dhcp_end ) >= 0 ) {
            dhcp_start();               /* the lease ran out */
        } else if ( (int32_t)( now - dhcp_next ) >= 0 ) {
            dhcp_send( DHCP_REQUEST );
            dhcp_next = dhcp_halfway( now, dhcp_end );
        }
        break;
    }
}

/* IPCONFIG /RELEASE: tell the server, then hold no address until a renew */
static void dhcp_release( void )
{
    if ( if_up && dhcp_server_ip ) {
        unsigned int n;
        dhcp_xid = rnd();
        n = bootp_build( DHCP_RELEASE, if_ip, 0, dhcp_server_ip );
        udp_send( dhcp_server_ip, 67, 68, NULL, n );
    }
    dhcp_drop();
    dhcp_state = DH_OFF;
}

/* IPCONFIG /RENEW: with a lease, ask at once; without one, look */
static void dhcp_renew( void )
{
    if ( dhcp_state == DH_BOUND || dhcp_state == DH_RENEW || dhcp_state == DH_REBIND ) {
        if ( dhcp_state == DH_BOUND ) dhcp_state = DH_RENEW;
        dhcp_xid = rnd();
        dhcp_send( DHCP_REQUEST );
        dhcp_next = ticks() + 4 * TICKS_SEC;    /* one more try soon */
    } else {
        dhcp_start();
    }
}

/* ---------------------------------------------------------------- */
/* DNS                                                               */
/* ---------------------------------------------------------------- */

/* a name as DNS carries it: each label after its length, 0 at the end.
   An empty label (a.b. or a..b) is left out rather than ending the name */
static unsigned int dns_put_name( uint8_t *q, unsigned int n, const char *name )
{
    const char *p = name;

    while ( *p ) {
        const char *start = p;
        unsigned int label;
        while ( *p && *p != '.' ) p++;
        label = (unsigned int)( p - start );
        if ( label > 63 ) label = 63;
        if ( label ) {
            q[n++] = (uint8_t)label;
            mem_copy( q + n, start, label );
            n += label;
        }
        if ( *p == '.' ) p++;
    }
    q[n++] = 0;                          /* the root */
    return n;
}

/* one question - a name and a record type - with recursion asked for,
   placed where udp_send expects it */
static unsigned int dns_query( uint16_t id, const char *name, uint16_t type )
{
    uint8_t *q = tx_frame + 34 + 8;
    unsigned int n;

    put16( q, id );
    put16( q + 2, 0x0100 );              /* recursion desired */
    put16( q + 4, 1 );                   /* one question */
    put16( q + 6, 0 );
    put16( q + 8, 0 );
    put16( q + 10, 0 );
    n = dns_put_name( q, 12, name );
    put16( q + n, type ); n += 2;
    put16( q + n, 1 ); n += 2;           /* class IN */
    return n;
}

static unsigned int dns_skip_name( const uint8_t *dns, unsigned int len, unsigned int at )
{
    while ( at < len ) {
        uint8_t label = dns[at];
        if ( ( label & 0xC0 ) == 0xC0 ) return at + 2;      /* a pointer */
        if ( label == 0 ) return at + 1;
        at += 1 + label;
    }
    return len;
}

/* a name in a reply as dotted text, its pointers followed; the offset
   just past it in the message, or 0 if it is not a name or is longer
   than max */
static unsigned int dns_get_name( const uint8_t *dns, unsigned int len, unsigned int at, char *out, unsigned int max )
{
    unsigned int o = 0, after = 0, hops = 0;
    uint8_t      label;

    for ( ;; ) {
        if ( at >= len ) return 0;
        label = dns[at];
        if ( ( label & 0xC0 ) == 0xC0 ) {
            if ( at + 1 >= len || ++hops > 16 ) return 0;   /* a loop of pointers */
            if ( after == 0 ) after = at + 2;
            at = ( (unsigned int)( label & 0x3F ) << 8 ) | dns[at + 1];
            continue;
        }
        if ( label & 0xC0 ) return 0;
        if ( label == 0 ) break;
        if ( at + 1 + label > len ) return 0;
        if ( o ) {
            if ( o + 1 >= max ) return 0;
            out[o++] = '.';
        }
        if ( o + label >= max ) return 0;
        mem_copy( out + o, dns + at + 1, label );
        o += label;
        at += 1 + label;
    }
    out[o] = 0;
    return after ? after : at + 1;
}

/* the DNS servers we have, the n'th of them, round and round */
static uint32_t dns_server( unsigned int n )
{
    uint32_t     list[3];
    unsigned int count = 0;

    if ( if_dns ) list[count++] = if_dns;
    if ( if_dns2 ) list[count++] = if_dns2;
    if ( if_dns3 ) list[count++] = if_dns3;
    return count ? list[n % count] : 0;
}

/* what a lookup asks DNS for: the name as given, or a short one (no
   dot) with our domain after it, as Windows adds its DNS suffix */
static void dns_qname( void )
{
    unsigned int i, n;

    str_copy( res.qname, res.name, sizeof( res.qname ) );
    for ( i = 0; res.name[i] && res.name[i] != '.'; i++ ) ;
    if ( res.name[i] == 0 && dom_name[0] ) {
        n = str_len( res.qname );
        str_copy( res.qname + n, ".", sizeof( res.qname ) - n );
        str_copy( res.qname + n + 1, dom_name, sizeof( res.qname ) - n - 1 );
    }
}

static int dns_send( void )
{
    return udp_send( dns_server( res.tries ), 53, res.lport, NULL,
                     dns_query( res.id, res.qname, 1 ) );
}

static void dns_input( const uint8_t *dns, unsigned int len )
{
    unsigned int at, answers, i, rcode;

    if ( len < 12 ) return;
    if ( get16( dns ) != res.id ) return;
    rcode = get16( dns + 2 ) & 0x000F;
    if ( rcode == 2 || rcode == 5 ) {
        /* this server could not, or would not: the next one has a turn */
        if ( ++res.tries > 3 ) { res.state = NET_FAIL; res_end(); }
        else res.give_up = ticks() + ( dns_send() ? 18 : ARP_RETRY );
        return;
    }
    if ( rcode != 0 ) { res.state = NET_FAIL; res_end(); return; }
    answers = get16( dns + 6 );
    at = dns_skip_name( dns, len, 12 ) + 4;         /* past the question */
    for ( i = 0; i < answers && at + 10 <= len; i++ ) {
        unsigned int rdlen, type;
        at = dns_skip_name( dns, len, at );
        if ( at + 10 > len ) break;
        type = get16( dns + at );
        rdlen = get16( dns + at + 8 );
        if ( type == 1 && rdlen == 4 && at + 10 + 4 <= len ) {
            res.ip = get32( dns + at + 10 );
            res.state = NET_DONE;
            res_end();
            return;
        }
        at += 10 + rdlen;
    }
    res.state = NET_FAIL;
    res_end();
}

/* ---------------------------------------------------------------- */
/* DNS UPDATE: our name and address sent to DNS (RFC 2136)           */
/* ---------------------------------------------------------------- */

/* A DHCP server that adds names to DNS takes option 81 and does; many
   do not, and an address set by /IP= has no DHCP server at all.  So, as
   Windows does, the computer sends its own address record to its zone's
   primary server: when an address is first held or changes, once a day
   after that, and for IPCONFIG /REGISTERDNS.  The update is not signed:
   a zone that takes only secure updates (an Active Directory zone, as it
   comes) answers REFUSED, and IPCONFIG /ALL says so.  The record put in
   replaces whatever address the name had.

   NOT A ZONE OUT ON THE INTERNET.  A name in no zone of the network's
   own gets the SOA of the root or a top-level domain back, and early
   Windows clients sent their updates there; a zone without a dot in it
   is never sent to.  Nor is a primary called localhost, which is what a
   resolver's own empty zones for private reverse addresses (RFC 6303)
   name. */

/* host.domain */
static void reg_fqdn( char *out, unsigned int max )
{
    unsigned int n;

    str_copy( out, host_name, max );
    n = str_len( out );
    str_copy( out + n, ".", max - n );
    str_copy( out + n + 1, dom_name, max - n - 1 );
}

static char *put_dec( char *p, unsigned int value )
{
    char digits[10];
    unsigned int n = 0;

    do {
        digits[n++] = (char)( '0' + value % 10 );
        value /= 10;
    } while ( value );
    while ( n ) *p++ = digits[--n];
    return p;
}

/* the question or the update for the step reg is at, sent */
static void reg_send( void )
{
    uint8_t *q = tx_frame + 34 + 8;
    unsigned int n;
    uint32_t dst;
    unsigned int at;
    uint16_t type = reg.job == RJ_A ? 1 : 12;
    char full[128];

    reg.id = (uint16_t)rnd();
    if ( reg.step == RG_UPDATE ) {
        dst = reg.master;
        put16( q, reg.id );
        put16( q + 2, 0x2800 );         /* opcode 5: UPDATE */
        put16( q + 4, 1 );              /* the zone */
        put16( q + 6, 0 );              /* nothing that must hold first */
        put16( q + 8, 2 );              /* two changes */
        put16( q + 10, 0 );
        n = dns_put_name( q, 12, reg.zone );
        put16( q + n, 6 );              /* SOA, IN */
        put16( q + n + 2, 1 );
        n += 4;
        /* every record of the type the name has goes out... */
        n = dns_put_name( q, n, reg.owner );
        put16( q + n, type );
        put16( q + n + 2, 255 );        /* class ANY: delete the set */
        put32( q + n + 4, 0 );
        put16( q + n + 8, 0 );
        n += 10;
        /* ...and ours goes in */
        n = dns_put_name( q, n, reg.owner );
        put16( q + n, type );
        put16( q + n + 2, 1 );
        put32( q + n + 4, REG_TTL );
        n += 10;
        if ( reg.job == RJ_A ) {
            put16( q + n - 2, 4 );
            put32( q + n, reg.ip );
            n += 4;
        } else {
            at = n;
            reg_fqdn( full, sizeof( full ) );
            n = dns_put_name( q, n, full );
            put16( q + at - 2, (uint16_t)( n - at ) );
        }
    } else if ( reg.step == RG_MASTER ) {
        dst = reg.asked;
        n = dns_query( reg.id, reg.mname, 1 );
    } else {
        dst = dns_server( reg.tries );
        reg.asked = dst;
        n = dns_query( reg.id, reg.owner, 6 );
    }
    reg.give_up = ticks() + ( !udp_send( dst, 53, reg.lport, NULL, n ) ? ARP_RETRY :
                              reg.step == RG_UPDATE ? 36 : 18 );
}

/* begin one record: the address one (RJ_A) or the reverse one (RJ_PTR) */
static void reg_begin( uint8_t job )
{
    char *p;
    unsigned int shift;

    reg.job = job;
    reg.ip = if_ip;
    if ( job == RJ_A ) reg_fqdn( reg.owner, sizeof( reg.owner ) );
    else {                              /* d.c.b.a.in-addr.arpa */
        p = reg.owner;
        for ( shift = 0; shift < 32; shift += 8 ) {
            p = put_dec( p, ( reg.ip >> shift ) & 0xFF );
            *p++ = '.';
        }
        str_copy( p, "in-addr.arpa", 16 );
    }
    reg.zone[0] = 0;
    reg.mname[0] = 0;
    reg.master = 0;
    reg.step = RG_SOA;
    reg.tries = 0;
    reg.lport = (uint16_t)( 0xC000 + ( rnd() & 0x3FFF ) );
    reg.active = 1;
    reg_send();
}

/* one record is done with.  The address record's fate is what IPCONFIG
   shows; the reverse one, sent after it for an address of our own, is
   taken or not without a word */
static void reg_end( uint16_t state, uint16_t rcode )
{
    reg.active = 0;
    if ( reg.job != RJ_A ) return;
    reg.state = state;
    reg.rcode = rcode;
    reg.retry_at = 0;
    if ( state == DREG_DONE ) {
        reg.done_ip = reg.ip;
        reg.done_at = ticks();
        if ( if_static ) reg_begin( RJ_PTR );
    } else if ( state == DREG_NOANSWER ) {
        reg.retry_at = ticks() + REG_RETRY;
    }
}

/* a zone we will send to: one with a dot in it, whose primary is not a
   resolver's own empty zone */
static int reg_zone_ok( void )
{
    unsigned int i;

    for ( i = 0; reg.zone[i] && reg.zone[i] != '.'; i++ ) ;
    if ( reg.zone[i] == 0 ) return 0;
    return !str_samei( reg.mname, "localhost" );
}

/* the primary's address, or the server that answered when it has none
   (or one no update should go to) */
static void reg_to_master( uint32_t ip )
{
    if ( ip == 0 || ( ip >> 24 ) == 127 || ip == IP_BCAST ) ip = reg.asked;
    reg.master = ip;
    reg.step = RG_UPDATE;
    reg.tries = 0;
    reg_send();
}

static void reg_input( uint32_t src, const uint8_t *dns, unsigned int len )
{
    unsigned int at, i, count, rcode, type, rdlen, name_at;

    if ( len < 12 || get16( dns ) != reg.id || !( get16( dns + 2 ) & 0x8000 ) ) return;
    rcode = get16( dns + 2 ) & 0x000F;

    if ( reg.step == RG_UPDATE ) {
        reg_end( (uint16_t)( rcode == 0 ? DREG_DONE : DREG_REFUSED ), (uint16_t)rcode );
        return;
    }
    if ( rcode != 0 && rcode != 3 ) {   /* not this server: the next, when time is up */
        reg.give_up = ticks();
        return;
    }

    at = 12;
    for ( i = get16( dns + 4 ); i > 0 && at < len; i-- ) at = dns_skip_name( dns, len, at ) + 4;
    count = get16( dns + 6 );           /* the answers ... */
    if ( reg.step == RG_SOA ) count += get16( dns + 8 );    /* ... and who is in charge */
    for ( i = 0; i < count; i++ ) {
        name_at = at;
        at = dns_skip_name( dns, len, at );
        if ( at + 10 > len ) break;
        type = get16( dns + at );
        rdlen = get16( dns + at + 8 );
        if ( at + 10 + rdlen > len ) break;
        if ( reg.step == RG_SOA && type == 6 ) {
            if ( dns_get_name( dns, len, name_at, reg.zone, sizeof( reg.zone ) ) == 0 ||
                 dns_get_name( dns, len, at + 10, reg.mname, sizeof( reg.mname ) ) == 0 ) break;
            if ( !reg_zone_ok() ) { reg_end( DREG_NOZONE, 0 ); return; }
            reg.asked = src;
            reg.step = RG_MASTER;
            reg.tries = 0;
            reg_send();
            return;
        }
        if ( reg.step == RG_MASTER && type == 1 && rdlen == 4 ) {
            reg_to_master( get32( dns + at + 10 ) );
            return;
        }
        at += 10 + rdlen;
    }
    if ( reg.step == RG_MASTER ) reg_to_master( 0 );
    else reg_end( DREG_NOZONE, 0 );
}

/* a step's time is up: ask again (the zone question round the servers),
   or move on, or give up */
static void reg_timeout( void )
{
    if ( ++reg.tries < ( reg.step == RG_MASTER ? 2 : 3 ) ) { reg_send(); return; }
    if ( reg.step == RG_MASTER ) { reg_to_master( 0 ); return; }
    reg_end( DREG_NOANSWER, 0 );
}

/* on the clock: a step's deadline, the daily refresh, a retry after no
   answer, and a send that was asked for */
static void reg_poll( uint32_t now )
{
    if ( reg.active ) {
        if ( (int32_t)( now - reg.give_up ) >= 0 ) reg_timeout();
        return;
    }
    if ( reg.done_ip && reg.done_ip == if_ip && now - reg.done_at >= REG_REFRESH ) reg.want = 1;
    if ( reg.retry_at && (int32_t)( now - reg.retry_at ) >= 0 ) {
        reg.retry_at = 0;
        reg.want = 1;
    }
    if ( !reg.want || !if_up ) return;
    reg.want = 0;
    if ( !reg_dns || dom_name[0] == 0 || if_dns == 0 ) {
        reg.state = DREG_OFF;
        return;
    }
    reg.state = DREG_BUSY;
    reg.rcode = 0;
    reg_begin( RJ_A );
}

/* ---------------------------------------------------------------- */
/* NetBIOS names over TCP/IP: the name service on UDP 137            */
/* ---------------------------------------------------------------- */

/* RFC 1001/1002.  A name is 15 characters, upper case, padded with
   spaces, and a 16th byte saying which service: 00 the workstation, 20
   the file server.  On the wire each byte is two letters 'A' + nibble,
   32 in all, in one label.  A lookup asks the WINS server if there is
   one, then the network by broadcast; the owner of the name answers.
   With /NAME= we answer for our own name, so a Windows machine finds
   PM-DOS's SMB server as \\NAME. */

#define NB_TTL       300000L    /* what Windows gives its own names, seconds */

/* 1 to 15 characters, no dot: a name NetBIOS can look up */
static int nb_able( const char *name )
{
    unsigned int n;

    for ( n = 0; name[n]; n++ ) {
        if ( name[n] == '.' || n >= 15 ) return 0;
    }
    return n > 0;
}

static unsigned char nb_upper( unsigned char ch )
{
    return ( ch >= 'a' && ch <= 'z' ) ? (unsigned char)( ch - 32 ) : ch;
}

/* the 34 bytes of an encoded name: the length 32, the letters, the root */
static void nb_encode( uint8_t *out, const char *name, uint8_t suffix )
{
    uint8_t      raw[16];
    unsigned int i;
    int          ended = 0;

    for ( i = 0; i < 15; i++ ) {
        if ( !ended && name[i] == 0 ) ended = 1;
        raw[i] = ended ? (uint8_t)' ' : nb_upper( (unsigned char)name[i] );
    }
    raw[15] = suffix;
    out[0] = 32;
    for ( i = 0; i < 16; i++ ) {
        out[1 + 2 * i] = (uint8_t)( 'A' + ( raw[i] >> 4 ) );
        out[2 + 2 * i] = (uint8_t)( 'A' + ( raw[i] & 15 ) );
    }
    out[33] = 0;
}

/* a query for res.name: to the WINS server, or to everyone on the network */
static int nbns_send( void )
{
    uint8_t *q = tx_frame + 34 + 8;
    uint32_t dst;

    put16( q, res.id );
    put16( q + 2, res.step == RS_BCAST ? 0x0110 : 0x0100 );    /* query, RD (+ B) */
    put16( q + 4, 1 );
    put16( q + 6, 0 );
    put16( q + 8, 0 );
    put16( q + 10, 0 );
    nb_encode( q + 12, res.name, 0x00 );
    put16( q + 46, 0x0020 );            /* NB */
    put16( q + 48, 1 );                 /* IN */
    if ( res.step == RS_WINS ) dst = if_wins;
    else dst = if_mask ? ( if_ip | ~if_mask ) : IP_BCAST;
    return udp_send( dst, NBNS_PORT, NBNS_PORT, NULL, 50 );
}

/* is the query's name (34 bytes at q) ours, for the workstation or the
   server service? */
static int nb_ours( const uint8_t *q )
{
    uint8_t      raw[16];
    unsigned int i;

    if ( nb_name[0] == 0 || q[0] != 32 || q[33] != 0 ) return 0;
    for ( i = 0; i < 16; i++ ) {
        uint8_t hi = (uint8_t)( q[1 + 2 * i] - 'A' ), lo = (uint8_t)( q[2 + 2 * i] - 'A' );
        if ( hi > 15 || lo > 15 ) return 0;
        raw[i] = (uint8_t)( ( hi << 4 ) | lo );
    }
    if ( raw[15] != 0x00 && raw[15] != 0x20 ) return 0;
    for ( i = 0; i < 15; i++ ) {
        uint8_t want = nb_name[i] ? (uint8_t)nb_name[i] : (uint8_t)' ';
        if ( nb_name[i] == 0 ) {
            for ( ; i < 15; i++ ) if ( raw[i] != ' ' ) return 0;
            return 1;
        }
        if ( nb_upper( raw[i] ) != want ) return 0;
    }
    return 1;
}

static void res_next( uint8_t step );

static void nbns_input( uint32_t src, uint16_t sport, const uint8_t *d, unsigned int dlen )
{
    uint16_t     flags;
    unsigned int at, rdlen;

    if ( dlen < 12 ) return;
    flags = get16( d + 2 );

    if ( flags & 0x8000 ) {
        /* an answer: to our lookup, if it is the one going */
        if ( !res.active || res.step == RS_DNS || get16( d ) != res.id ) return;
        if ( ( flags & 0x000F ) != 0 ) {        /* "no such name" */
            if ( res.step == RS_WINS ) res_next( RS_BCAST );
            return;
        }
        if ( get16( d + 6 ) == 0 || dlen < 13 ) return;
        at = 12;
        if ( ( d[at] & 0xC0 ) == 0xC0 ) at += 2;
        else if ( d[at] == 32 && at + 34 <= dlen ) at += 34;
        else return;
        if ( at + 10 > dlen || get16( d + at ) != 0x0020 ) return;
        rdlen = get16( d + at + 8 );
        if ( rdlen < 6 || at + 10 + 6 > dlen ) return;
        res.ip = get32( d + at + 12 );          /* past the NB flags */
        res.state = NET_DONE;
        res_end();
        return;
    }

    /* a question: answer one for our name, if we have an address */
    if ( ( ( flags >> 11 ) & 0x0F ) != 0 ) return;      /* a query, not a registration */
    if ( !if_up || dlen < 50 || get16( d + 4 ) != 1 ) return;
    if ( get16( d + 46 ) != 0x0020 || !nb_ours( d + 12 ) ) return;
    {
        uint8_t *r = tx_frame + 34 + 8;

        put16( r, get16( d ) );
        put16( r + 2, 0x8500 );         /* answer, authoritative, RD */
        put16( r + 4, 0 );
        put16( r + 6, 1 );
        put16( r + 8, 0 );
        put16( r + 10, 0 );
        mem_copy( r + 12, d + 12, 34 ); /* the name as it was asked */
        put16( r + 46, 0x0020 );
        put16( r + 48, 1 );
        put32( r + 50, NB_TTL );
        put16( r + 54, 6 );
        put16( r + 56, 0x0000 );        /* a unique name, B node */
        put32( r + 58, if_ip );
        /* the asker is on our network and waiting: take its card address
           from this frame, so the answer need not wait for an ARP */
        if ( same_subnet( src ) && arp_find( src ) == NULL && rx_srcmac ) arp_put( src, rx_srcmac );
        udp_send( src, sport, NBNS_PORT, NULL, 62 );
    }
}

/* the lookup's next step: WINS (if there is one), broadcast, DNS (if
   there is one), or give up */
static void res_next( uint8_t step )
{
    int sent;

    if ( step == RS_WINS && if_wins == 0 ) step = RS_BCAST;
    if ( step == RS_DNS && if_dns == 0 ) {
        res.state = NET_FAIL;
        res_end();
        return;
    }
    res.step = step;
    res.tries = 0;
    res.id = (uint16_t)rnd();
    if ( step == RS_DNS ) {
        dns_qname();
        sent = dns_send();
    }
    else sent = nbns_send();
    /* a send that is waiting on ARP goes again as soon as the answer
       can be in */
    res.give_up = ticks() + ( !sent ? ARP_RETRY : step == RS_BCAST ? 14 : 18 );
}

/* a lookup's time is up: ask again, or move on */
static void res_timeout( uint32_t now )
{
    switch ( res.step ) {
    case RS_WINS:                       /* two tries, a second each */
        if ( ++res.tries >= 2 ) res_next( RS_BCAST );
        else res.give_up = now + ( nbns_send() ? 18 : ARP_RETRY );
        break;
    case RS_BCAST:                      /* three, three-quarters of a second */
        if ( ++res.tries >= 3 ) res_next( RS_DNS );
        else res.give_up = now + ( nbns_send() ? 14 : ARP_RETRY );
        break;
    default:                            /* DNS: four, a second each */
        if ( ++res.tries > 3 ) { res.state = NET_FAIL; res_end(); }
        else res.give_up = now + ( dns_send() ? 18 : ARP_RETRY );
        break;
    }
}

/* ---------------------------------------------------------------- */
/* TCP                                                               */
/* ---------------------------------------------------------------- */

#define SEQ_LT( a, b )   ( (int32_t)( (uint32_t)(a) - (uint32_t)(b) ) < 0 )
#define SEQ_LEQ( a, b )  ( (int32_t)( (uint32_t)(a) - (uint32_t)(b) ) <= 0 )
#define SEQ_GT( a, b )   ( (int32_t)( (uint32_t)(a) - (uint32_t)(b) ) > 0 )
#define SEQ_GEQ( a, b )  ( (int32_t)( (uint32_t)(a) - (uint32_t)(b) ) >= 0 )

static unsigned int rx_free( SOCKET *s )  { return RXCAP - s->rxlen; }
static unsigned int tx_free( SOCKET *s )  { return TXCAP - s->txlen; }

/* put one TCP segment on the wire: control flags, and dlen bytes of
   send buffer starting seq bytes from snd_una */
static void tcp_xmit( SOCKET *s, uint32_t seq, uint8_t flags, unsigned int off, unsigned int dlen )
{
    uint8_t *th = tx_frame + 34;
    unsigned int i;

    put16( th, s->lport );
    put16( th + 2, s->rport );
    put32( th + 4, seq );
    put32( th + 8, s->rcv_nxt );
    th[12] = 5 << 4;                     /* a 20-byte header */
    th[13] = flags;
    s->wnd_adv = rx_free( s );
    put16( th + 14, (uint16_t)s->wnd_adv );
    put16( th + 16, 0 );
    put16( th + 18, 0 );
    for ( i = 0; i < dlen; i++ ) {
        th[20 + i] = s->txbuf[( s->txstart + off + i ) % TXCAP];
    }
    put16( th + 16, l4_cksum( if_ip, s->rip, IP_TCP, 20 + dlen ) );
    ip_send( s->rip, IP_TCP, 20 + dlen );
}

static void tcp_rst( uint32_t dst, uint16_t lport, uint16_t rport, uint32_t seq, uint32_t ack, uint8_t have_ack )
{
    uint8_t *th = tx_frame + 34;

    put16( th, lport );
    put16( th + 2, rport );
    put32( th + 4, seq );
    put32( th + 8, ack );
    th[12] = 5 << 4;
    th[13] = (uint8_t)( TCP_RST | ( have_ack ? TCP_ACK : 0 ) );
    put16( th + 14, 0 );
    put16( th + 16, 0 );
    put16( th + 18, 0 );
    put16( th + 16, l4_cksum( if_ip, dst, IP_TCP, 20 ) );
    ip_send( dst, IP_TCP, 20 );
}

static void tcp_arm( SOCKET *s )
{
    if ( s->rto == 0 ) s->rto = 18;
    s->rto_at = ticks() + s->rto;
}

/* send whatever data the window now allows, and a FIN if it is time */
static void tcp_send_data( SOCKET *s )
{
    uint32_t usable, avail, off, chunk;

    if ( s->tstate != T_ESTAB && s->tstate != T_CLOSEWAIT ) return;

    for ( ;; ) {
        off = s->snd_nxt - s->snd_una;
        avail = s->txlen - off;                 /* unsent bytes in the ring */
        usable = s->snd_una + s->snd_wnd - s->snd_nxt;      /* the window */
        if ( avail == 0 || (int32_t)usable <= 0 ) break;
        chunk = avail;
        if ( chunk > usable ) chunk = usable;
        if ( chunk > TCP_MSS ) chunk = TCP_MSS;
        tcp_xmit( s, s->snd_nxt, (uint8_t)( TCP_ACK | TCP_PSH ), off, chunk );
        s->snd_nxt += chunk;
        tcp_arm( s );
    }

    /* the app has closed and every byte is out: send the FIN */
    if ( s->closing && !s->finsent && s->snd_nxt == s->snd_una + s->txlen ) {
        s->finseq = s->snd_nxt;
        tcp_xmit( s, s->snd_nxt, (uint8_t)( TCP_ACK | TCP_FIN ), s->txlen, 0 );
        s->snd_nxt++;
        s->finsent = 1;
        if ( s->tstate == T_ESTAB ) s->tstate = T_FINWAIT1;
        else if ( s->tstate == T_CLOSEWAIT ) s->tstate = T_LASTACK;
        tcp_arm( s );
    }
}

static SOCKET *tcp_listener_for( uint32_t dst, uint16_t port )
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

static SOCKET *tcp_match( uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport )
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
static void sock_reset( SOCKET *s, uint8_t type );

static void tcp_input( uint32_t src, uint32_t dst, const uint8_t *th, uint32_t len )
{
    uint16_t sport, dport, window;
    uint32_t seq, ack;
    unsigned int off, dlen;
    uint8_t  flags;
    const uint8_t *data;
    SOCKET  *s;

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
            sock_reset( c, SOCK_STREAM );
            c->lip = dst; c->rip = src;
            c->lport = dport; c->rport = sport;
            c->irs = seq;
            c->rcv_nxt = seq + 1;
            c->iss = rnd();
            c->snd_una = c->iss;
            c->snd_nxt = c->iss;
            c->snd_wnd = window ? window : 1;
            c->tstate = T_SYNRCVD;
            tcp_xmit( c, c->snd_nxt, (uint8_t)( TCP_SYN | TCP_ACK ), 0, 0 );
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
        /* a reset to our SYN is "nobody is listening there"; to a
           connection that was up, the server dropping it */
        s->err = s->tstate == T_SYNSENT ? NET_REFUSED : NET_RESET;
        s->tstate = T_CLOSED;
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
            tcp_xmit( s, s->iss, (uint8_t)( TCP_SYN | TCP_ACK ), 0, 0 );
        }
        return;
    }

    /* an acceptable segment acknowledges something; use its ACK */
    if ( flags & TCP_ACK ) {
        if ( SEQ_GT( ack, s->snd_una ) && SEQ_LEQ( ack, s->snd_nxt ) ) {
            uint32_t acked = ack - s->snd_una;
            uint32_t databytes = acked;
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
        uint32_t skip = s->rcv_nxt - seq;
        unsigned int take = dlen - skip;
        unsigned int room = rx_free( s );
        unsigned int i;
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
                         if ( s->tstate == T_TIMEWAIT ) s->wait_at = ticks() + 36;
                         break;
        case T_FINWAIT2: s->tstate = T_TIMEWAIT; s->wait_at = ticks() + 36; break;
        }
    }

    /* our FIN's acknowledgement moves the close along */
    if ( s->finacked ) {
        if ( s->tstate == T_FINWAIT1 ) s->tstate = T_FINWAIT2;
        else if ( s->tstate == T_CLOSING ) { s->tstate = T_TIMEWAIT; s->wait_at = ticks() + 36; }
        else if ( s->tstate == T_LASTACK ) { s->tstate = T_CLOSED; s->used = 0; }
    }

    tcp_send_data( s );
}

/* the retransmit and close timers, run on every tick */
static void tcp_timers( void )
{
    int i;
    uint32_t now = ticks();

    for ( i = 0; i < SOCK_MAX; i++ ) {
        SOCKET *s = &sock[i];
        if ( !s->used || s->type != SOCK_STREAM ) continue;

        if ( s->tstate == T_TIMEWAIT && (int32_t)( now - s->wait_at ) >= 0 ) {
            s->tstate = T_CLOSED;
            s->used = 0;
            continue;
        }
        if ( s->rto_at == 0 || (int32_t)( now - s->rto_at ) < 0 ) continue;

        if ( ++s->retries > 6 ) {
            /* nothing came back.  A SYN that never left, because the
               server or the gateway never answered ARP, is no route */
            s->err = ( s->tstate == T_SYNSENT && arp_find( next_hop( s->rip ) ) == NULL )
                     ? NET_NOROUTE : NET_TIMEOUT;
            s->tstate = T_CLOSED;
            s->used = ( s->tstate == T_CLOSED && s->closing ) ? 0 : s->used;
            s->rto_at = 0;
            continue;
        }
        s->rto *= 2;
        if ( s->rto > 109 ) s->rto = 109;              /* about six seconds */

        if ( s->tstate == T_SYNSENT ) {
            tcp_xmit( s, s->iss, TCP_SYN, 0, 0 );
        } else if ( s->tstate == T_SYNRCVD ) {
            tcp_xmit( s, s->iss, (uint8_t)( TCP_SYN | TCP_ACK ), 0, 0 );
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

static SOCKET *sock_by_handle( unsigned int handle )
{
    if ( handle < 1 || handle > SOCK_MAX ) return NULL;
    if ( !sock[handle - 1].used ) return NULL;
    return &sock[handle - 1];
}

static unsigned int sock_handle( SOCKET *s )
{
    return (unsigned int)( s - sock ) + 1;
}

static SOCKET *udp_socket_for( uint16_t port )
{
    int i;

    /* no socket's port: a UDP socket that has none yet says 0 too, and a
       datagram sent to port 0 was queued on the first of those */
    if ( port == 0 ) return NULL;
    for ( i = 0; i < SOCK_MAX; i++ ) {
        if ( sock[i].used && sock[i].type == SOCK_DGRAM && sock[i].lport == port ) {
            return &sock[i];
        }
    }
    return NULL;
}

/* a port for a UDP socket that has not asked for one: one no other UDP
   socket has, or that socket's datagrams would go to this one */
static uint16_t udp_port_pick( void )
{
    uint16_t port;

    do {
        port = (uint16_t)( 0xC000 + ( rnd() & 0x3FFF ) );
    } while ( udp_socket_for( port ) != NULL );
    return port;
}

/* a datagram's frame in the UDP socket's buffer: length, address, port */
static void udp_queue( SOCKET *s, uint32_t src, uint16_t sport, const uint8_t *data, unsigned int dlen )
{
    unsigned int need = 8 + dlen;

    if ( s->rxstart == s->rxlen ) { s->rxstart = 0; s->rxlen = 0; }  /* empty: pack */
    if ( s->rxlen + need > RXCAP ) return;                           /* no room: drop */
    put16( s->rxbuf + s->rxlen, (uint16_t)dlen );
    put16( s->rxbuf + s->rxlen + 2, sport );
    put32( s->rxbuf + s->rxlen + 4, src );
    if ( dlen ) mem_copy( s->rxbuf + s->rxlen + 8, data, dlen );
    s->rxlen += need;
}

static void sock_reset( SOCKET *s, uint8_t type )
{
    uint8_t *rxbuf = s->rxbuf, *txbuf = s->txbuf;
    tls_release( s );                   /* free any TLS state a prior user left */
    mem_zero( s, sizeof( *s ) );
    s->rxbuf = rxbuf;
    s->txbuf = txbuf;
    s->type = type;
    s->tstate = T_CLOSED;
    s->used = 1;
}

static unsigned int sock_ss( SOCKET *s )
{
    if ( s->type == SOCK_DGRAM ) return SS_ESTABLISHED;
    if ( s->tls ) {
        /* TLS rides above the TCP state: established means TLS is up, so a
           caller's connect wait does not finish until the handshake does */
        if ( s->tls_state == TLSX_FAIL ) return SS_ERROR;
        if ( s->tls_state == TLSX_UP ) {
            /* up, but report the close once the peer has ended and there is
               no plaintext left, so a read-to-end loop stops */
            unsigned int left = s->taplen - s->tapstart;
            if ( left == 0 ) {
                if ( s->tls_eof ) return SS_CLOSING;
                /* a server that sends its FIN without close_notify has
                   ended too (CLOSE_WAIT counts, as for plain TCP below) */
                if ( s->tstate != T_ESTAB &&
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
    case T_ESTAB:    return SS_ESTABLISHED;
    /* THE PEER HAS ENDED: SAY SO ONCE ITS DATA IS READ.  CLOSE_WAIT was
       "established" whatever was left, so a program reading to the end
       could not tell a finished server from a slow one - SOCKLIB's
       sl_closed never fired and WGET waited out its 15 s on every plain
       fetch; WINSOCK.DLL's recv never returned 0 nor posted FD_CLOSE
       (2026-11-03).  Sending is still allowed here; do_send says so */
    case T_CLOSEWAIT: return s->rxlen ? SS_ESTABLISHED : SS_CLOSING;
    default:         return SS_CLOSING;
    }
}

static unsigned int sock_rxready( SOCKET *s )
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
/* the date and time, for a certificate's dates                      */
/* ---------------------------------------------------------------- */

/* NDS_STAMP gives the date and the time - the clock chip's, through the
   BIOS - but only while the kernel is not parked: under Windows the WfW
   bridge makes the socket calls, and the BIOS is Windows's then.  So the
   stamp is taken whenever it safely can be, with the tick count beside
   it, and the time is that stamp and the ticks counted since.  The clock
   is the machine's own local time and is taken as UTC, which is what a
   certificate's dates are in: a few hours out at the very edges of a
   certificate's life, which are weeks or years apart. */
static uint32_t clk_stamp;      /* NDS_STAMP: the date << 16 | the time; 0 none */
static uint32_t clk_at;         /* ticks() when it was taken */

static void clock_take( void )
{
    clk_stamp = nds_stamp();
    clk_at = ticks();
}

/* days from 1970-01-01 to a date, and back - H. Hinnant's civil calendar
   arithmetic, here for the years a DOS date holds (1980 to 2107) */
static unsigned int days_of( unsigned int y, unsigned int m, unsigned int d )
{
    unsigned int era, yoe, doy, doe;

    if ( m <= 2 ) y--;
    era = y / 400;
    yoe = y - era * 400;
    doy = ( 153 * ( m > 2 ? m - 3 : m + 9 ) + 2 ) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097UL + doe - 719468UL;
}

static void date_of( unsigned int days, unsigned int *y, unsigned int *m, unsigned int *d )
{
    unsigned int z = days + 719468UL, era, doe, yoe, doy, mp;

    era = z / 146097UL;
    doe = z - era * 146097UL;
    yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
    doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
    mp = ( 5 * doy + 2 ) / 153;
    *d = doy - ( 153 * mp + 2 ) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + ( *m <= 2 );
}

static char *put_num( char *p, unsigned int value, unsigned int width )
{
    unsigned int i;

    for ( i = width; i-- > 0; ) {
        p[i] = (char)( '0' + value % 10 );
        value /= 10;
    }
    return p + width;
}

/* now, as YYYYMMDDHHMMSS for the TLS core; "" if the kernel gave no date */
static void clock_now14( char *out )
{
    uint32_t date = clk_stamp >> 16, time = clk_stamp & 0xFFFF;
    unsigned int secs, days, y, m, d;
    char *p = out;

    out[0] = 0;
    if ( ( ( date >> 5 ) & 15 ) == 0 || ( ( date >> 5 ) & 15 ) > 12 || ( date & 31 ) == 0 ) return;
    secs = ( time >> 11 ) * 3600UL + ( ( time >> 5 ) & 63 ) * 60 + ( time & 31 ) * 2;
    secs += ticks_secs( ticks() - clk_at );
    days = days_of( 1980 + ( date >> 9 ), ( date >> 5 ) & 15, date & 31 ) + secs / 86400UL;
    secs %= 86400UL;
    date_of( days, &y, &m, &d );
    p = put_num( p, y, 4 );
    p = put_num( p, m, 2 );
    p = put_num( p, d, 2 );
    p = put_num( p, secs / 3600, 2 );
    p = put_num( p, secs / 60 % 60, 2 );
    p = put_num( p, secs % 60, 2 );
    *p = 0;
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
    tls_conn *c; const uint8_t *in; int len; uint8_t *out; int outcap; int ret;
} ENCJOB;
static int __cdecl job_encrypt( void *p )
{
    ENCJOB *j = (ENCJOB *)p;
    j->ret = tls_app_encrypt( j->c, j->in, j->len, j->out, j->outcap );
    return j->ret;
}

typedef struct {
    tls_conn *c; const uint8_t *rec; int reclen; uint8_t *out; int outcap; int ct; int ret;
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

static int run_encrypt( tls_conn *c, const uint8_t *in, int len, uint8_t *out, int outcap )
{
    ENCJOB j;
    j.c = c; j.in = in; j.len = len; j.out = out; j.outcap = outcap; j.ret = -1;
    tls_run( (void *)tls_stktop, (void *)job_encrypt, &j );
    return j.ret;
}

static int run_decrypt( tls_conn *c, const uint8_t *rec, int reclen,
                        uint8_t *out, int outcap, int *ct )
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

/* pop up to 'want' bytes off the TCP receive ring into a linear buffer.
   SAY SO WHEN THE WINDOW OPENS AGAIN.  The window went out only on an ACK
   for arriving data, so once the ring had filled, the peer never heard it
   had been read: it waited for its persist timer, sent a probe, got the
   new window, sent 4K, and waited again.  Under QEMU that was 5.5 s a
   round - 0.57 KB/s, the same with TLS or without (2026-11-03).  Now the
   window goes out as soon as it has grown by a segment, or by half the
   ring if that is less (RFC 1122 4.2.3.3), which also keeps the peer from
   being told of every few bytes */
static unsigned int tcp_rx_pop( SOCKET *s, uint8_t *dst, unsigned int want )
{
    unsigned int got = s->rxlen < want ? s->rxlen : want;
    unsigned int i, step = TCP_MSS < RXCAP / 2 ? TCP_MSS : RXCAP / 2;
    for ( i = 0; i < got; i++ ) dst[i] = s->rxbuf[( s->rxstart + i ) % RXCAP];
    s->rxstart = ( s->rxstart + got ) % RXCAP;
    s->rxlen -= got;
    if ( got && ( s->tstate == T_ESTAB || s->tstate == T_FINWAIT1 || s->tstate == T_FINWAIT2 ) &&
         rx_free( s ) >= s->wnd_adv + step ) {
        tcp_xmit( s, s->snd_nxt, TCP_ACK, s->snd_nxt - s->snd_una, 0 );
    }
    return got;
}

static uint8_t tls_feedbuf[1024];   /* handshake bytes, off the scratch stack */

/* the TLS core's reason a certificate was refused, as SOCK_STAT's CERT_*;
   0 if it was not refused */
static uint8_t cert_why( int verr )
{
    switch ( verr ) {
    case 0:                return 0;
    case X509_ERR_SIG:     return CERT_UNTRUSTED;
    case X509_ERR_HOST:    return CERT_HOST;
    case X509_ERR_EXPIRED: return CERT_DATES;
    default:               return CERT_BAD;
    }
}

/* begin the TLS handshake once the TCP connection is up */
static void tls_begin( SOCKET *s )
{
    const char *host = s->host[0] ? s->host : NULL;

    if ( tls_stk == NULL ) {
        tls_stk = (uint8_t *)malloc( TLS_STK_SIZE );
        if ( tls_stk == NULL ) { s->tls_state = TLSX_FAIL; s->err = NET_TLSFAIL; return; }
        tls_stktop = ( (uint32_t)tls_stk + TLS_STK_SIZE ) & ~15u;
    }
    s->rng  = (nst_rng *)malloc( sizeof( nst_rng ) );
    s->trx  = (uint8_t *)malloc( TLS_RECCAP );
    s->tapp = (uint8_t *)malloc( TLS_REC_MAX );
    if ( s->rng == NULL || s->trx == NULL || s->tapp == NULL ) {
        s->tls_state = TLSX_FAIL; s->err = NET_TLSFAIL; return;
    }
    rng_init( s->rng );
    rng_stir( s->rng, (const uint8_t *)&s->iss, 4 ); /* a little extra entropy */
    s->tc = tls_new( s->rng );
    if ( s->tc == NULL ) { s->tls_state = TLSX_FAIL; s->err = NET_TLSFAIL; return; }
    if ( tls_ca && tls_catotal > 0 && tls_calen == tls_catotal ) {
        /* chain, host and dates verified against the bundle; the time was
           taken at the connect (clock_now14), "" if the kernel had none */
        tls_set_verify( s->tc, host, s->now14, tls_ca, (int)tls_calen );
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
    unsigned int reclen, extra;
    int ct, plen;

    if ( s->taplen > s->tapstart ) return;          /* app still has plaintext */
    if ( s->tls_eof ) return;

    if ( s->trxlen < 5 )
        s->trxlen += tcp_rx_pop( s, s->trx + s->trxlen, 5 - s->trxlen );
    if ( s->trxlen < 5 ) return;                    /* record header not in yet */

    reclen = 5 + ( ( (unsigned int)s->trx[3] << 8 ) | s->trx[4] );
    if ( reclen > TLS_RECCAP ) { s->tls_state = TLSX_FAIL; s->err = NET_TLSFAIL; return; }
    if ( s->trxlen < reclen )
        s->trxlen += tcp_rx_pop( s, s->trx + s->trxlen, reclen - s->trxlen );
    if ( s->trxlen < reclen ) return;               /* record body not in yet */

    plen = run_decrypt( s->tc, s->trx, (int)reclen, s->tapp, TLS_REC_MAX, &ct );
    extra = s->trxlen - reclen;                     /* a following record's start */
    if ( extra ) mem_copy( s->trx, s->trx + reclen, extra );
    s->trxlen = extra;

    if ( plen < 0 ) { s->tls_state = TLSX_FAIL; s->err = NET_TLSFAIL; return; }
    if ( ct == TLS_CT_APPLICATION_DATA ) { s->tapstart = 0; s->taplen = (unsigned int)plen; }
    else if ( ct == TLS_CT_ALERT ) s->tls_eof = 1;
    /* else a post-handshake handshake message (a session ticket): drop it */
}

/* advance one TLS socket: start it, run the handshake, keep records flowing */
static void tls_drive( SOCKET *s )
{
    if ( !s->tls || s->tls_state == TLSX_FAIL ) return;     /* over: err says why */
    if ( s->tls_state != TLSX_IDLE && ( s->tc == NULL || s->trx == NULL || s->tapp == NULL ) ) {
        s->tls_state = TLSX_FAIL; s->err = NET_TLSFAIL; return;
    }

    if ( s->tls_state == TLSX_IDLE ) {
        if ( s->tstate == T_ESTAB ) tls_begin( s );
        else if ( s->tstate == T_CLOSED && s->err ) s->tls_state = TLSX_FAIL;
        return;
    }

    if ( s->tls_state == TLSX_HS ) {
        uint8_t *out;
        int st, m;

        /* feed everything the TCP stream has to the handshake */
        while ( s->rxlen > 0 ) {
            unsigned int got = tcp_rx_pop( s, tls_feedbuf, sizeof( tls_feedbuf ) );
            if ( got == 0 ) break;
            if ( tls_hs_feed( s->tc, tls_feedbuf, (int)got ) < 0 ) {
                s->tls_state = TLSX_FAIL; s->err = NET_TLSFAIL; return;
            }
        }
        st = run_pump( s->tc );
        /* flush whatever the handshake wants to send into the TCP txbuf */
        while ( ( m = tls_hs_out_ptr( s->tc, &out ) ) > 0 ) {
            unsigned int room = tx_free( s );
            unsigned int n = (unsigned int)m < room ? (unsigned int)m : room;
            unsigned int i;
            if ( n == 0 ) break;                    /* no room; next pass */
            for ( i = 0; i < n; i++ )
                s->txbuf[( s->txstart + s->txlen + i ) % TXCAP] = out[i];
            s->txlen += n;
            tls_hs_out_consume( s->tc, (int)n );
        }
        if ( s->txlen > 0 ) tcp_send_data( s );

        if ( st == TLS_HS_DONE ) {
            uint8_t *lp;
            int lm = tls_hs_leftover( s->tc, &lp );     /* app bytes past Finished */
            if ( lm > 0 ) {
                if ( lm > TLS_RECCAP ) lm = TLS_RECCAP;
                mem_copy( s->trx, lp, (unsigned int)lm );
                s->trxlen = (unsigned int)lm;
            }
            s->tls_state = TLSX_UP;
        } else if ( st == TLS_HS_ERROR ) {
            /* the certificate refused, or the handshake itself gone
               wrong: the caller tells the user which */
            s->tls_state = TLSX_FAIL;
            s->why = cert_why( tls_verify_error( s->tc ) );
            s->err = s->why ? NET_TLSCERT : NET_TLSFAIL;
        }
        return;
    }

    if ( s->tls_state == TLSX_UP ) tls_fill( s );       /* keep plaintext ready */
}

static void tls_service( void )
{
    int i;
    for ( i = 0; i < SOCK_MAX; i++ ) {
        if ( !sock[i].used || !sock[i].tls ) continue;
        tls_drive( &sock[i] );
        /* A FAILED HANDSHAKE KEEPS ITS REASON, NOT ITS MEMORY.  The crypto
           state is most of the 320K TLS pool, and it was let go only at
           SOCK_CLOSE: a program that ended without closing a socket that
           had failed (TLSTEST did) left the next TLS socket no memory, and
           its failure looked like the certificate's (2026-11-02) */
        if ( sock[i].tls_state == TLSX_FAIL ) tls_release( &sock[i] );
    }
}

/* ---------------------------------------------------------------- */
/* the pump: drain the card, run the timers                          */
/* ---------------------------------------------------------------- */

static void pump( void )
{
    unsigned int len;
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
    uint32_t now = ticks();

    dhcp_tick();
    tcp_timers();

    if ( ping.active && (int32_t)( now - ping.give_up ) >= 0 ) {
        if ( ++ping.tries > 3 ) { ping.state = NET_FAIL; ping.active = 0; }
        else {
            ping.sent_fine = fine_now();        /* the round trip of the echo now sent */
            ping.give_up = now + ( icmp_echo( ping.ip, ping.id, ping.seq ) ? 18 : ARP_RETRY );
        }
    }
    if ( res.active && (int32_t)( now - res.give_up ) >= 0 ) res_timeout( now );
    reg_poll( now );
}

/* ---------------------------------------------------------------- */
/* the socket calls (nd_call)                                        */
/* ---------------------------------------------------------------- */

static int do_ifstat( void *buf, uint32_t len, uint32_t *answer )
{
    SOCK_IF *info = (SOCK_IF *)buf;
    static const uint8_t dhst[6] = { DHST_OFF, DHST_ASKING, DHST_ASKING,
                                DHST_BOUND, DHST_RENEWING, DHST_REBINDING };

    if ( len < SOCKIF_LEN ) return ERR_BADLEN;
    len = len >= SOCKIF_LEN3 ? SOCKIF_LEN3 : len >= SOCKIF_LEN2 ? SOCKIF_LEN2 : SOCKIF_LEN;
    mem_zero( info, len );
    info->ip = if_ip;
    info->mask = if_mask;
    info->gw = if_gw;
    info->dns = if_dns;
    info->leasesecs = if_lease;
    mem_copy( info->mac, if_mac, 6 );
    info->up = if_up;
    info->dhcp = if_dhcp;
    str_copy( info->nic, if_name, sizeof( info->nic ) );
    if ( len >= SOCKIF_LEN3 ) {
        str_copy( info->hostname, host_name, sizeof( info->hostname ) );
        str_copy( info->domain, dom_name, sizeof( info->domain ) );
        info->dns2 = if_dns2;
        info->dns3 = if_dns3;
        info->cabytes = ( tls_ca && tls_calen == tls_catotal ) ? tls_calen : 0;
        info->regstate = reg.state;
        info->regrcode = reg.rcode;
    }
    if ( len >= SOCKIF_LEN2 ) {
        if ( if_up && if_dhcp == DHS_BOUND ) {
            info->dhcpsrv = dhcp_server_ip;
            info->leaseage = ticks_secs( ticks() - dhcp_bound_at );
        }
        info->dhstate = dhcp_state < 6 ? dhst[dhcp_state] : DHST_OFF;
        info->acks = dhcp_acks;
        info->wins = if_wins;
        str_copy( info->nbname, nb_name, sizeof( info->nbname ) );
    }
    *answer = len;
    return 0;
}

/* SOCK_DHCP: IPCONFIG's /RELEASE and /RENEW */
static int do_dhcp( void *buf, uint32_t len, uint32_t *answer )
{
    uint32_t op;

    if ( len < 4 ) return ERR_BADLEN;
    op = *(uint32_t *)buf;
    if ( op == DHCPOP_REGDNS ) {            /* IPCONFIG /REGISTERDNS: the tick sends it */
        reg.want = 1;
        reg.retry_at = 0;
        if ( !reg.active && if_up && reg_dns && dom_name[0] && if_dns ) reg.state = DREG_BUSY;
        *answer = 0;
        return 0;
    }
    if ( if_static ) return ERR_BADDATA;    /* /IP= on the DEVICE= line: no lease */
    if ( op == DHCPOP_RELEASE ) dhcp_release();
    else if ( op == DHCPOP_RENEW ) dhcp_renew();
    else return ERR_FUNC;
    *answer = 0;
    return 0;
}

static int do_open( void *buf, uint32_t len, uint32_t *answer )
{
    SOCK_OPENREQ *req = (SOCK_OPENREQ *)buf;
    SOCKET *s;
    unsigned int base;

    if ( len < sizeof( SOCK_OPENREQ ) ) return ERR_BADLEN;
    base = req->type & 0xFF;
    if ( base != SOCK_STREAM && base != SOCK_DGRAM ) return ERR_BADDATA;
    s = sock_free_slot();
    if ( s == NULL ) return ERR_GENERAL;
    sock_reset( s, (uint8_t)base );
    if ( ( req->type & SOCK_TLS ) && base == SOCK_STREAM ) s->tls = 1;
    *answer = sock_handle( s );
    return 0;
}

static int do_close( void *buf, uint32_t len )
{
    SOCK_HANDLE *req = (SOCK_HANDLE *)buf;
    SOCKET *s;

    if ( len < sizeof( SOCK_HANDLE ) ) return ERR_BADLEN;
    s = sock_by_handle( req->handle );
    if ( s == NULL ) return ERR_BADDATA;

    if ( s->tls ) {                     /* drop the crypto; the TCP close runs on */
        tls_release( s );
        s->tls = 0; s->tls_state = TLSX_IDLE; s->tls_eof = 0;
    }

    if ( s->type == SOCK_DGRAM || s->tstate == T_CLOSED || s->listening ) {
        s->used = 0;
        return 0;
    }
    s->closing = 1;
    if ( s->tstate == T_ESTAB || s->tstate == T_CLOSEWAIT ) tcp_send_data( s );
    return 0;
}

static int do_connect( void *buf, uint32_t len )
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

    if ( s->type == SOCK_DGRAM ) {
        /* just the default peer.  A port SOCK_BIND gave stays: it was
           thrown away for a picked one here, so a socket could not both
           have a port and a peer (2026-11-20) */
        if ( s->lport == 0 ) s->lport = udp_port_pick();
        return 0;
    }
    s->lport = (uint16_t)( 0xC000 + ( rnd() & 0x3FFF ) );

    s->iss = rnd();
    s->snd_una = s->iss;
    s->snd_nxt = s->iss;
    s->snd_wnd = TCP_MSS;
    s->rcv_nxt = 0;
    s->tstate = T_SYNSENT;
    s->err = 0;
    s->why = 0;
    if ( s->tls ) {                     /* the time its certificate must be good at */
        if ( running ) clock_take();    /* parked: counted on from the last stamp */
        clock_now14( s->now14 );
    }
    tcp_xmit( s, s->iss, TCP_SYN, 0, 0 );
    s->snd_nxt = s->iss + 1;
    tcp_arm( s );
    return 0;
}

static int do_listen( void *buf, uint32_t len )
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

/* SOCK_BIND: A UDP SOCKET'S OWN PORT.  There was no way to have one - a
   UDP socket's port was picked at its first send - so a program could
   ask a server (SNTP) and could not be one, and a game could not be
   reached by the others in it (DOOM, 2026-11-20).  The ports the stack
   takes itself in udp_input, before any socket is looked for, are
   refused: a socket bound to one would never be given a datagram */
static int do_bind( void *buf, uint32_t len, uint32_t *answer )
{
    SOCK_BINDREQ *req = (SOCK_BINDREQ *)buf;
    SOCKET *s, *holder;

    if ( len < sizeof( SOCK_BINDREQ ) ) return ERR_BADLEN;
    s = sock_by_handle( req->handle );
    if ( s == NULL || s->type != SOCK_DGRAM ) return ERR_BADDATA;
    if ( req->port == 0 ) {
        if ( s->lport == 0 ) s->lport = udp_port_pick();
    } else {
        if ( req->port == 68 || req->port == NBNS_PORT ) return ERR_ACCESS;
        holder = udp_socket_for( req->port );
        if ( holder != NULL && holder != s ) return ERR_ACCESS;
        s->lport = req->port;
    }
    *answer = s->lport;
    return 0;
}

static int do_accept( void *buf, uint32_t len, uint32_t *answer )
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

static int do_send( void *buf, uint32_t len, uint32_t *answer )
{
    SOCK_IO *io = (SOCK_IO *)buf;
    const uint8_t *data = (const uint8_t *)buf + SOCK_IO_HDR;
    SOCKET *s;
    unsigned int dlen;

    if ( len < SOCK_IO_HDR ) return ERR_BADLEN;
    s = sock_by_handle( io->handle );
    if ( s == NULL ) return ERR_BADDATA;
    dlen = io->len;
    if ( dlen > len - SOCK_IO_HDR ) return ERR_BADLEN;

    if ( s->type == SOCK_DGRAM ) {
        uint32_t dst = io->ip ? io->ip : s->rip;
        uint16_t dport = io->port ? io->port : s->rport;
        if ( !if_up || dst == 0 ) return ERR_GENERAL;
        if ( dlen > 1472 ) dlen = 1472;
        if ( s->lport == 0 ) s->lport = udp_port_pick();
        udp_send( dst, dport, s->lport, data, dlen );
        *answer = dlen;
        return 0;
    }

    if ( s->tls ) {
        unsigned int room, maxp, i;
        int reclen;

        if ( s->tls_state != TLSX_UP || s->tc == NULL ) return ERR_GENERAL;
        room = tx_free( s );
        if ( room < 96 ) { *answer = 0; return 0; }  /* no room for a record: try later */
        maxp = room - 80;                            /* leave space for record overhead */
        if ( maxp > TLS_APP_MAX ) maxp = TLS_APP_MAX;
        if ( dlen > maxp ) dlen = maxp;
        reclen = run_encrypt( s->tc, data, (int)dlen, tls_scratch, (int)sizeof( tls_scratch ) );
        if ( reclen <= 0 ) return ERR_GENERAL;
        for ( i = 0; i < (unsigned int)reclen; i++ )
            s->txbuf[( s->txstart + s->txlen + i ) % TXCAP] = tls_scratch[i];
        s->txlen += (unsigned int)reclen;
        tcp_send_data( s );
        *answer = dlen;
        return 0;
    }

    if ( s->tstate != T_ESTAB && s->tstate != T_CLOSEWAIT ) return ERR_GENERAL;
    if ( dlen > tx_free( s ) ) dlen = tx_free( s );
    {
        unsigned int i;
        for ( i = 0; i < dlen; i++ ) {
            s->txbuf[( s->txstart + s->txlen + i ) % TXCAP] = data[i];
        }
        s->txlen += dlen;
    }
    tcp_send_data( s );
    *answer = dlen;
    return 0;
}

static int do_recv( void *buf, uint32_t len, uint32_t *answer )
{
    SOCK_IO *io = (SOCK_IO *)buf;
    uint8_t *room = (uint8_t *)buf + SOCK_IO_HDR;
    SOCKET *s;
    unsigned int want, got;

    if ( len < SOCK_IO_HDR ) return ERR_BADLEN;
    s = sock_by_handle( io->handle );
    if ( s == NULL ) return ERR_BADDATA;
    want = io->len;
    if ( want > len - SOCK_IO_HDR ) want = len - SOCK_IO_HDR;

    if ( s->type == SOCK_DGRAM ) {
        unsigned int dlen;
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
        unsigned int avail;
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

    got = tcp_rx_pop( s, room, want );
    io->len = got;
    *answer = got;
    return 0;
}

static int do_state( void *buf, uint32_t len, uint32_t *answer )
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
    if ( len >= SOCKSTAT_LEN2 ) st->why = s->why;
    *answer = st->state;
    return 0;
}

static int do_resolve( void *buf, uint32_t len, uint32_t *answer )
{
    SOCK_RES *req = (SOCK_RES *)buf;

    if ( len < SOCKRES_LEN ) return ERR_BADLEN;
    req->name[sizeof( req->name ) - 1] = 0;
    if ( if_up && name_is_us( req->name ) ) {   /* our own name: no need to ask */
        req->ip = if_ip;
        req->state = NET_DONE;
        *answer = NET_DONE;
        return 0;
    }
    if ( !if_up || ( if_dns == 0 && !nb_able( req->name ) ) ) {
        req->state = NET_FAIL;
        *answer = NET_FAIL;
        return 0;
    }

    if ( res.active == 0 && str_same( res.name, req->name ) &&
         ( res.state == NET_DONE ||
           ( res.state == NET_FAIL && ticks() - res_done_at < RES_HOLD ) ) ) {
        req->ip = res.ip;
        req->state = res.state;
        *answer = res.state;
        return 0;
    }
    if ( !res.active || !str_same( res.name, req->name ) ) {
        str_copy( res.name, req->name, sizeof( res.name ) );
        res.lport = (uint16_t)( 0xC000 + ( rnd() & 0x3FFF ) );
        res.ip = 0;
        res.state = NET_PENDING;
        res.active = 1;
        res_next( nb_able( res.name ) ? RS_WINS : RS_DNS );
    }
    req->ip = res.ip;
    req->state = res.state;
    *answer = res.state;
    return 0;
}

static int do_ping( void *buf, uint32_t len, uint32_t *answer )
{
    SOCK_PINGREQ *req = (SOCK_PINGREQ *)buf;

    if ( len < SOCKPING_LEN ) return ERR_BADLEN;
    if ( !if_up ) { req->state = NET_FAIL; *answer = NET_FAIL; return 0; }
    if ( req->ip == if_ip || ( req->ip >> 24 ) == 127 ) {
        /* ourselves - PING of our own name, now that it resolves: no wire
           to cross, and an ARP for our own address is never answered */
        req->state = NET_DONE;
        req->rttms = 0;
        *answer = NET_DONE;
        return 0;
    }

    if ( ping.state != NET_PENDING && ping.active == 0 &&
         ping.ip == req->ip && ping.seq == (uint16_t)req->seq ) {
        req->state = ping.state;
        req->rttms = ping.rtt;
        *answer = ping.state;
        return 0;
    }
    if ( !ping.active || ping.ip != req->ip || ping.seq != (uint16_t)req->seq ) {
        ping.ip = req->ip;
        ping.id = 0x504D;               /* 'PM' */
        ping.seq = (uint16_t)req->seq;
        ping.state = NET_PENDING;
        ping.rtt = 0;
        ping.tries = 0;
        ping.active = 1;
        ping.sent_at = ticks();
        ping.sent_fine = fine_now();
        ping.give_up = ping.sent_at + ( icmp_echo( ping.ip, ping.id, ping.seq ) ? 18 : ARP_RETRY );
    }
    req->state = ping.state;
    req->rttms = ping.rtt;
    *answer = ping.state;
    return 0;
}

static int do_tlshost( void *buf, uint32_t len )
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

static int do_setca( void *buf, uint32_t len )
{
    SOCK_CAREQ *req = (SOCK_CAREQ *)buf;
    const uint8_t *data = (const uint8_t *)buf + SOCKCA_HDR;
    int i;

    if ( len < SOCKCA_HDR ) return ERR_BADLEN;
    if ( req->len > len - SOCKCA_HDR ) return ERR_BADLEN;
    if ( req->total == 0 || req->total > 65536 ) return ERR_BADDATA;
    if ( req->offset > req->total || req->len > req->total - req->offset ) return ERR_BADDATA;

    /* a handshake under way reads the whole bundle: leave it be till done */
    if ( tls_ca && tls_calen == tls_catotal ) {
        for ( i = 0; i < SOCK_MAX; i++ ) {
            if ( sock[i].used && sock[i].tls && sock[i].tls_state == TLSX_HS ) return ERR_GENERAL;
        }
    }

    if ( req->offset == 0 ) {                      /* the first chunk starts fresh */
        if ( tls_ca ) { free( tls_ca ); tls_ca = NULL; }
        tls_ca = (uint8_t *)malloc( req->total );  /* from the TLS pool, not the tight arena */
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
/* the DEVICE= line: /IP=, /MASK=, /GW=, /DNS=, /INI:, /CA:          */
/* ---------------------------------------------------------------- */

static uint32_t opt_ip, opt_mask, opt_gw, opt_dns, opt_wins;
static uint32_t opt_dns2, opt_dns3;
static uint8_t  opt_bad;
static char     drv_dir[128];   /* where TCPIP.SYS was loaded from, \ and all */
static char     ini_path[128];  /* /INI: - the settings file, if not beside it */
static char     ca_path[128];   /* /CA: or CACerts= - the trust bundle */

/* drv_dir: the DEVICE= line's path up to its last \ or : ("" for none) */
static void drv_dir_from( const char *line )
{
    unsigned int n = 0, cut = 0;

    while ( line[n] && line[n] != ' ' && line[n] != '\t' && line[n] != '\r' &&
            line[n] != '\n' && n < sizeof( drv_dir ) - 1 ) {
        drv_dir[n] = line[n];
        n++;
        if ( line[n - 1] == '\\' || line[n - 1] == ':' ) cut = n;
    }
    drv_dir[cut] = 0;
}

/* a file beside TCPIP.SYS */
static void path_beside( char *path, const char *name )
{
    unsigned int n;

    str_copy( path, drv_dir, 128 );
    n = str_len( path );
    str_copy( path + n, name, 128 - n );
}

/* "TCP/IP: " and up to three pieces, to the console */
static void say3( const char *one, const char *two, const char *three )
{
    char text[200];
    unsigned int n;

    str_copy( text, "TCP/IP: ", sizeof( text ) );
    n = str_len( text );
    str_copy( text + n, one, sizeof( text ) - n );
    n = str_len( text );
    str_copy( text + n, two, sizeof( text ) - n );
    n = str_len( text );
    str_copy( text + n, three, sizeof( text ) - n );
    nds_print( text );
}

/* a path after /INI: or /CA:, to the next space or switch; 0 if there is
   none or it is too long */
static int take_path( const char *p, char *out, unsigned int max, const char **after )
{
    unsigned int n = 0;

    while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '/' ) {
        if ( n + 1 >= max ) return 0;
        out[n++] = *p++;
    }
    out[n] = 0;
    *after = p;
    return n > 0;
}

/* /NAME=: 1 to 15 of the characters a NetBIOS name may safely have,
   kept in upper case; 0 if the name is not one */
static int take_name( const char *p, const char **after )
{
    unsigned int  n = 0;
    unsigned char ch;

    for ( ;; p++ ) {
        ch = (unsigned char)*p;
        if ( ch == 0 || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '/' ) break;
        if ( !( ( ch >= 'A' && ch <= 'Z' ) || ( ch >= 'a' && ch <= 'z' ) || ( ch >= '0' && ch <= '9' ) ||
                ch == '-' || ch == '_' || ch == '!' || ch == '#' || ch == '$' || ch == '%' ||
                ch == '&' || ch == '\'' || ch == '(' || ch == ')' || ch == '^' || ch == '{' ||
                ch == '}' || ch == '~' ) ) return 0;
        if ( n >= 15 ) return 0;
        nb_name[n++] = (char)nb_upper( ch );
    }
    nb_name[n] = 0;
    *after = p;
    return n > 0;
}

/* a dotted quad, a.b.c.d, ending at a space, a switch or the line's end;
   0 if it is not one */
static uint32_t parse_ip( const char *p, const char **after )
{
    uint32_t ip = 0, part;
    int i, digits;

    for ( i = 0; i < 4; i++ ) {
        part = 0;
        digits = 0;
        while ( *p >= '0' && *p <= '9' && digits < 4 ) {
            part = part * 10 + (uint32_t)( *p - '0' );
            p++;
            digits++;
        }
        if ( digits == 0 || digits > 3 || part > 255 ) return 0;
        ip = ( ip << 8 ) | part;
        if ( i < 3 ) {
            if ( *p != '.' ) return 0;
            p++;
        }
    }
    if ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '/' && *p != ',' ) return 0;
    *after = p;
    return ip;
}

/* one to three addresses with commas between (spaces after a comma are
   let by), for /DNS= and DNSServers=; how many, or 0 if one is wrong */
static unsigned int parse_ips( const char *p, uint32_t *list, const char **after )
{
    unsigned int n = 0;

    for ( ;; ) {
        if ( n == 3 ) return 0;
        list[n] = parse_ip( p, &p );
        if ( list[n++] == 0 ) return 0;
        if ( *p != ',' ) break;
        p++;
        while ( *p == ' ' || *p == '\t' ) p++;
    }
    *after = p;
    return n;
}

/* does the switch at p (just past the '/') read 'name' then '=' or ':'? */
static const char *switch_is( const char *p, const char *name )
{
    while ( *name ) {
        if ( ( *p | 0x20 ) != *name ) return NULL;
        p++;
        name++;
    }
    return ( *p == '=' || *p == ':' ) ? p + 1 : NULL;
}

static void say_switch( const char *what, const char *p )
{
    char text[100];
    unsigned int i = 0;

    str_copy( text, "TCP/IP: ", sizeof( text ) );
    while ( text[i] ) i++;
    while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && i < 40 ) text[i++] = *p++;
    text[i] = 0;
    str_copy( text + i, what, sizeof( text ) - i );
    nds_print( text );
}

static void parse_line( const char *line )
{
    const char *p = line, *value, *after;
    uint32_t *slot;

    while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' ) p++;
    for ( ; *p && *p != '\r' && *p != '\n'; p++ ) {
        if ( *p != '/' ) continue;
        slot = NULL;
        if ( ( value = switch_is( p + 1, "name" ) ) != NULL ) {
            if ( !take_name( value, &after ) ) {
                nb_name[0] = 0;
                say_switch( " is not a name of 1 to 15 letters and digits\r\n", p );
                opt_bad = 1;
                continue;
            }
            p = after - 1;
            continue;
        }
        if ( ( value = switch_is( p + 1, "ini" ) ) != NULL ||
             ( value = switch_is( p + 1, "ca" ) ) != NULL ) {
            char *path = ( p[1] | 0x20 ) == 'i' ? ini_path : ca_path;
            if ( !take_path( value, path, sizeof( ini_path ), &after ) ) {
                path[0] = 0;
                say_switch( " is not a file name\r\n", p );
                opt_bad = 1;
                continue;
            }
            p = after - 1;
            continue;
        }
        if ( ( value = switch_is( p + 1, "dns" ) ) != NULL ) {
            uint32_t list[3];
            unsigned int n = parse_ips( value, list, &after );
            if ( n == 0 ) {
                say_switch( " is not up to three addresses such as 192.168.1.1,192.168.1.2\r\n", p );
                opt_bad = 1;
                continue;
            }
            opt_dns = list[0];
            opt_dns2 = n > 1 ? list[1] : 0;
            opt_dns3 = n > 2 ? list[2] : 0;
            p = after - 1;
            continue;
        }
        if ( ( value = switch_is( p + 1, "ip" ) ) != NULL ) slot = &opt_ip;
        else if ( ( value = switch_is( p + 1, "mask" ) ) != NULL ) slot = &opt_mask;
        else if ( ( value = switch_is( p + 1, "gw" ) ) != NULL ) slot = &opt_gw;
        else if ( ( value = switch_is( p + 1, "wins" ) ) != NULL ) slot = &opt_wins;
        if ( slot == NULL ) {
            say_switch( " is not a TCPIP.SYS switch\r\n", p );
            continue;
        }
        *slot = parse_ip( value, &after );
        if ( *slot == 0 ) {
            say_switch( " is not an address such as 192.168.1.20\r\n", p );
            opt_bad = 1;
            continue;
        }
        p = after - 1;
    }
}

/* ---------------------------------------------------------------- */
/* TCPIP.INI: the same settings, kept in a file                      */
/* ---------------------------------------------------------------- */

/* TCPIP.INI is read at INIT from beside TCPIP.SYS, or from /INI:file.
   One setting a line, Name=value, in a [TCPIP] section or before any
   section; a ; starts a comment.  A setting left out, or left empty,
   takes its default, the same as a switch left off the DEVICE= line:
   an address from DHCP, the name made from the card's address, and so
   on.  A switch on the DEVICE= line wins over the file.  A wrong address
   stops the load, as a wrong switch does; any other wrong line is said
   and passed over. */

#define INI_MAX   4096

static const char *ini_name;    /* the file, for what is said about it */
static unsigned int ini_line;

/* "TCP/IP: C:\DOS\TCPIP.INI line 4: Gateway= is not ..." */
static void ini_say( const char *key, const char *what )
{
    char text[200], num[12];
    unsigned int n;

    str_copy( text, ini_name, sizeof( text ) );
    n = str_len( text );
    str_copy( text + n, " line ", sizeof( text ) - n );
    n = str_len( text );
    *put_dec( num, ini_line ) = 0;
    str_copy( text + n, num, sizeof( text ) - n );
    n = str_len( text );
    str_copy( text + n, ": ", sizeof( text ) - n );
    n = str_len( text );
    str_copy( text + n, key, sizeof( text ) - n );
    n = str_len( text );
    str_copy( text + n, "= ", sizeof( text ) - n );
    say3( text, what, "\r\n" );
}

/* an address setting, into a slot a switch has not filled */
static void ini_ip( const char *key, const char *val, uint32_t *slot, const char *like )
{
    const char *after;
    uint32_t ip;

    if ( *slot ) return;
    ip = parse_ip( val, &after );
    if ( ip == 0 || *after ) {
        ini_say( key, like );
        opt_bad = 1;
        return;
    }
    *slot = ip;
}

static void ini_set( const char *key, const char *val )
{
    unsigned int i, n;
    uint32_t list[3];
    const char *after;

    if ( val[0] == 0 ) return;                  /* empty: the default */
    if ( str_samei( key, "HostName" ) ) {
        if ( nb_name[0] ) return;               /* /NAME= wins */
        if ( !host_ok( val ) ) {
            ini_say( key, "is not a name of 1 to 63 letters, digits and hyphens" );
            return;
        }
        for ( i = 0; val[i]; i++ ) host_name[i] = (char)lower( (unsigned char)val[i] );
        host_name[i] = 0;
    } else if ( str_samei( key, "Domain" ) ) {
        if ( take_domain( val, dom_name ) ) dom_fixed = 1;
        else ini_say( key, "is not a domain such as corp.example.com" );
    } else if ( str_samei( key, "IPAddress" ) ) {
        ini_ip( key, val, &opt_ip, "is not an address such as 192.168.1.20" );
    } else if ( str_samei( key, "SubnetMask" ) ) {
        ini_ip( key, val, &opt_mask, "is not a network mask such as 255.255.255.0" );
    } else if ( str_samei( key, "Gateway" ) ) {
        ini_ip( key, val, &opt_gw, "is not an address such as 192.168.1.1" );
    } else if ( str_samei( key, "WINSServer" ) ) {
        ini_ip( key, val, &opt_wins, "is not an address such as 192.168.1.1" );
    } else if ( str_samei( key, "DNSServers" ) ) {
        if ( opt_dns ) return;
        n = parse_ips( val, list, &after );
        if ( n == 0 || *after ) {
            ini_say( key, "is not up to three addresses such as 192.168.1.1, 192.168.1.2" );
            opt_bad = 1;
            return;
        }
        opt_dns = list[0];
        opt_dns2 = n > 1 ? list[1] : 0;
        opt_dns3 = n > 2 ? list[2] : 0;
    } else if ( str_samei( key, "CACerts" ) ) {
        if ( ca_path[0] ) return;               /* /CA: wins */
        if ( str_len( val ) >= sizeof( ca_path ) ) ini_say( key, "is too long a file name" );
        else str_copy( ca_path, val, sizeof( ca_path ) );
    } else if ( str_samei( key, "RegisterDNS" ) ) {
        if ( str_samei( val, "yes" ) || str_samei( val, "on" ) || str_samei( val, "1" ) ) reg_dns = 1;
        else if ( str_samei( val, "no" ) || str_samei( val, "off" ) || str_samei( val, "0" ) ) reg_dns = 0;
        else ini_say( key, "is not Yes or No" );
    } else {
        ini_say( key, "is not a TCPIP.INI setting" );
    }
}

/* spaces and tabs off both ends, in place */
static char *trim( char *text )
{
    unsigned int n;

    while ( *text == ' ' || *text == '\t' ) text++;
    n = str_len( text );
    while ( n && ( text[n - 1] == ' ' || text[n - 1] == '\t' ) ) text[--n] = 0;
    return text;
}

/* the file's text, NUL at the end: one line at a time */
static void ini_parse( char *text )
{
    char *line, *eq, *close;
    int ours = 1;                       /* before any section: [TCPIP]'s */

    ini_line = 0;
    while ( *text ) {
        line = text;
        while ( *text && *text != '\n' && *text != 0x1A ) text++;
        if ( *text == 0x1A ) *text = 0;         /* ^Z: the end of a DOS text */
        else if ( *text ) *text++ = 0;
        ini_line++;
        for ( eq = line; *eq && *eq != ';' && *eq != '\r'; eq++ ) ;
        *eq = 0;                                /* the comment off */
        line = trim( line );
        if ( *line == 0 ) continue;
        if ( *line == '[' ) {
            for ( close = line + 1; *close && *close != ']'; close++ ) ;
            *close = 0;
            ours = str_samei( trim( line + 1 ), "TCPIP" );
            continue;
        }
        if ( !ours ) continue;
        for ( eq = line; *eq && *eq != '='; eq++ ) ;
        if ( *eq == 0 ) {
            ini_say( line, "is not Name=value" );
            continue;
        }
        *eq = 0;
        ini_set( trim( line ), trim( eq + 1 ) );
    }
}

/* TCPIP.INI, if there is one.  The one beside TCPIP.SYS may well not be
   there, and that is no matter; one named by /INI: has to be */
static void ini_read( void )
{
    char path[128];
    char *text;
    uint32_t got, size;

    if ( ini_path[0] ) str_copy( path, ini_path, sizeof( path ) );
    else path_beside( path, "TCPIP.INI" );
    ini_name = path;
    text = (char *)nds_alloc( INI_MAX + 1 );
    if ( text == NULL ) return;
    if ( nds_readfile( path, text, INI_MAX, &got, &size ) != 0 ) {
        if ( ini_path[0] ) say3( "cannot read ", path, "\r\n" );
    } else {
        if ( size > INI_MAX ) say3( path, " is longer than 4096 bytes: the rest is not read", "\r\n" );
        text[got] = 0;
        ini_parse( text );
    }
    nds_free( text );
    ini_name = "TCPIP.INI";
}

/* THE TRUST BUNDLE, read once here so that every TLS socket checks the
   server's certificate - before this, a socket checked only when the
   program (WINSOCK.DLL) had sent the bundle itself, and the rest went
   unchecked without a word.  CACerts= or /CA: names it; otherwise it is
   CACERT.BND beside TCPIP.SYS.  Kept in the TLS pool, as SOCK_SETCA's is.
   Without one, TLS still connects, unchecked, and that is said. */
static void ca_load( void )
{
    char     path[128];
    uint8_t  probe[1];
    uint8_t *bundle;
    uint32_t got, size;

    if ( ca_path[0] ) str_copy( path, ca_path, sizeof( path ) );
    else path_beside( path, "CACERT.BND" );
    if ( nds_readfile( path, probe, 0, &got, &size ) != 0 ) {
        say3( "no ", path, " - HTTPS certificates will not be checked\r\n" );
        return;
    }
    if ( size == 0 || size > 65536UL ) {
        say3( path, " is not a trust bundle - HTTPS certificates will not be checked", "\r\n" );
        return;
    }
    bundle = (uint8_t *)malloc( size );
    if ( bundle == NULL ) {
        say3( "no memory for ", path, " - HTTPS certificates will not be checked\r\n" );
        return;
    }
    if ( nds_readfile( path, bundle, size, &got, &size ) != 0 || got != size || bundle[0] != 0x30 ) {
        free( bundle );
        say3( path, " is not a trust bundle - HTTPS certificates will not be checked", "\r\n" );
        return;
    }
    tls_ca = bundle;
    tls_calen = size;
    tls_catotal = size;
}

/* a mask is ones then zeros */
static int mask_ok( uint32_t mask )
{
    uint32_t low = ~mask + 1;       /* the lowest one bit, or 0 for 0.0.0.0 */
    return mask != 0 && ( low & ( low - 1 ) ) == 0;
}

/* the mask an address's class gives, for /IP= without /MASK= */
static uint32_t class_mask( uint32_t ip )
{
    if ( ( ip >> 24 ) < 128 ) return 0xFF000000L;
    if ( ( ip >> 24 ) < 192 ) return 0xFFFF0000L;
    return 0xFFFFFF00L;
}

/* check the switches and make them the interface's; 0 if they are wrong */
static int take_static( void )
{
    if ( opt_bad ) return 0;
    if ( opt_ip == 0 ) {
        if ( opt_mask || opt_gw ) nds_print( "TCP/IP: /MASK= and /GW= are used only with /IP=\r\n" );
        dns_fixed = opt_dns;
        dns_fixed2 = opt_dns2;
        dns_fixed3 = opt_dns3;
        wins_fixed = opt_wins;
        if_wins = opt_wins;
        return 1;
    }
    if ( opt_ip == IP_BCAST ) {
        nds_print( "TCP/IP: /IP= cannot be 255.255.255.255\r\n" );
        return 0;
    }
    if ( opt_mask == 0 ) opt_mask = class_mask( opt_ip );
    if ( !mask_ok( opt_mask ) ) {
        nds_print( "TCP/IP: /MASK= is not a network mask such as 255.255.255.0\r\n" );
        return 0;
    }
    if ( opt_gw && ( ( opt_gw ^ opt_ip ) & opt_mask ) != 0 ) {
        nds_print( "TCP/IP: /GW= is not on the same network as /IP=\r\n" );
        return 0;
    }
    if_static = 1;
    if_ip = opt_ip;
    if_mask = opt_mask;
    if_gw = opt_gw;
    if_dns = opt_dns;
    if_dns2 = opt_dns2;
    if_dns3 = opt_dns3;
    if_lease = 0;
    dns_fixed = opt_dns;
    dns_fixed2 = opt_dns2;
    dns_fixed3 = opt_dns3;
    wins_fixed = opt_wins;
    if_wins = opt_wins;
    return 1;
}

/* "IP Address: 10.0.2.15" */
static void say_address( void )
{
    char text[40];
    unsigned int i, shift, part;

    str_copy( text, "IP Address: ", sizeof( text ) );
    i = 0; while ( text[i] ) i++;
    for ( shift = 24; ; shift -= 8 ) {
        part = ( if_ip >> shift ) & 0xFF;
        if ( part >= 100 ) text[i++] = (char)( '0' + part / 100 );
        if ( part >= 10 ) text[i++] = (char)( '0' + part / 10 % 10 );
        text[i++] = (char)( '0' + part % 10 );
        if ( shift == 0 ) break;
        text[i++] = '.';
    }
    text[i++] = '\r';
    text[i++] = '\n';
    text[i] = 0;
    nds_print( text );
}

/* ---------------------------------------------------------------- */
/* the four entries the kernel calls                                 */
/* ---------------------------------------------------------------- */

int __cdecl nd_call( uint32_t fn, void *buf, uint32_t len, uint32_t *answer )
{
    int result;

    pumping = 1;
    pump();

    switch ( fn ) {
    case SOCK_IFSTAT:  result = do_ifstat( buf, len, answer ); break;
    case SOCK_DHCP:    result = do_dhcp( buf, len, answer ); break;
    case SOCK_OPEN:    result = do_open( buf, len, answer ); break;
    case SOCK_CLOSE:   result = do_close( buf, len ); break;
    case SOCK_CONNECT: result = do_connect( buf, len ); break;
    case SOCK_LISTEN:  result = do_listen( buf, len ); break;
    case SOCK_BIND:    result = do_bind( buf, len, answer ); break;
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
    uint32_t answer, start;
    int i;

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

    /* the switches first, then TCPIP.INI for what they left out: a wrong
       address in either stops the load before any memory is kept */
    drv_dir_from( line );
    parse_line( line );
    ini_read();
    names_settle();
    if ( !take_static() ) return 1;

    rnd_state = ticks() ^ ( (uint32_t)if_mac[5] << 16 ) ^ get32( if_mac + 1 );

    /* the socket buffers, once, from the arena (never freed, never from
       a tick) */
    for ( i = 0; i < SOCK_MAX; i++ ) {
        sock[i].rxbuf = (uint8_t *)nds_alloc( RXCAP );
        sock[i].txbuf = (uint8_t *)nds_alloc( TXCAP );
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
    else ca_load();
    clock_take();

    nds_tickhook();
    running = 1;

    if ( if_static ) {
        if_up = 1;
        if_dhcp = DHS_STATIC;
        dhcp_state = DH_OFF;
        reg.want = 1;                       /* our name to DNS, on the clock */
        if ( if_gw ) arp_request( if_gw );  /* learn the gateway's address early */
        say_address();
        return 0;
    }

    /* DHCP runs on the clock.  Wait up to five seconds here for the first
       lease, so the address can be shown; without one the clock keeps
       asking, more slowly, and IPCONFIG shows when one arrives */
    dhcp_start();
    start = ticks();
    while ( dhcp_state != DH_BOUND && ticks() - start < 5 * TICKS_SEC + 1 ) ;
    if ( dhcp_state == DH_BOUND ) say_address();
    else nds_print( "Failed to get IP address\r\n" );
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

int __cdecl nd_irq( uint32_t line )
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
