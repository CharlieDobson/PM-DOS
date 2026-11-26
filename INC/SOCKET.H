/*
 * SOCKET.H - the socket API, as the TCP/IP stack presents it
 *
 * The stack is a 32-bit driver (DRV\TCPIP) that rides on a loaded card
 * driver.  A program reaches it through 21h/F1h AL=26h: bits 16-23 of
 * EAX are one of the functions below, EDI points at the matching
 * request block and ECX is its length, and EAX comes back with what the
 * function answers - a handle, a state, a byte count - or CF is set and
 * AX is an error (NDRV.H's ERR_*).  TOOLS\SOCKLIB wraps all of this.
 *
 * Every socket is a handle, a small number from 1 up; 0 is never one.
 * AN ADDRESS IS A uint32_t, most significant octet first: 10.0.2.2 is
 * 0x0A000202L.  A PORT IS A uint16_t in the natural order: 80 is 80.  The
 * stack puts both on the wire big-endian itself.
 *
 * NOTHING BLOCKS.  The stack runs on the clock and whenever a socket
 * call is made; a call does what it can at once and returns.  A connect
 * starts a handshake and returns; the caller watches SOCK_STATE for it
 * to reach SS_ESTABLISHED.  A receive gives what has arrived, or none.
 * SOCKLIB is where the waiting and the timeouts live.
 *
 * Needs INC\NDRV.H before it, for uint8_t/uint16_t/uint32_t and REGS.
 */
#ifndef SOCKET_H
#define SOCKET_H

/* THE FUNCTIONS - bits 16-23 of EAX for a program, EBX for the driver */
#define SOCK_IFSTAT    0      /* buf -> SOCK_IF; answer = SOCKIF_LEN */
#define SOCK_OPEN      1      /* buf -> SOCK_OPENREQ; answer = a handle */
#define SOCK_CLOSE     2      /* buf -> SOCK_HANDLE */
#define SOCK_CONNECT   3      /* buf -> SOCK_CONNREQ; a handshake begun */
#define SOCK_LISTEN    4      /* buf -> SOCK_LISTENREQ */
#define SOCK_ACCEPT    5      /* buf -> SOCK_ACCEPTREQ; answer = new handle, 0 none */
#define SOCK_SEND      6      /* buf -> SOCK_IO + data; answer = bytes taken */
#define SOCK_RECV      7      /* buf -> SOCK_IO + room; answer = bytes given */
#define SOCK_STATE     8      /* buf -> SOCK_STAT; answer = the state (SS_*) */
#define SOCK_RESOLVE   9      /* buf -> SOCK_RES; answer = the state (NET_*) */
#define SOCK_PING      10     /* buf -> SOCK_PINGREQ; answer = the state (NET_*) */
#define SOCK_PUMP      11     /* drain the card + run the stack one step and its
                                 timers; for the WfW bridge, which drives the
                                 parked stack from the forwarded IRQ and tick */
#define SOCK_TLSHOST   12     /* buf -> SOCK_TLSHOSTREQ; name a TLS socket's
                                 server, for SNI and certificate matching */
#define SOCK_SETCA     13     /* buf -> SOCK_CAREQ; hand the stack the CA
                                 bundle (DISK\CACERT.BND) one chunk at a time */
#define SOCK_RXFOREIGN 14     /* buf <- one frame the stack does not own (not IP,
                                 not ARP - NetBEUI); answer = its length, 0 none.
                                 For the WfW bridge to indicate to its protocol */
#define SOCK_NICSTATS  15     /* buf <- the card's NIC_STATBLOCK (INC\NIC.H);
                                 answer = NICS_LEN.  A window on tx/rx counts */
#define SOCK_DHCP      16     /* buf -> a uint32_t, DHCPOP_*; answer = 0.  ERR_BADDATA
                                 for RELEASE or RENEW when the address is
                                 static (TCPIP.SYS /IP=) */
#define SOCK_BIND      17     /* buf -> SOCK_BINDREQ; UDP: the port this socket's
                                 datagrams arrive on and leave from; answer =
                                 the port.  ERR_FUNC from a TCPIP.SYS older
                                 than 2026-11-20 */

/* SOCK_DHCP's one word: what to do with the lease */
#define DHCPOP_RELEASE 1      /* give the address back and stay without one */
#define DHCPOP_RENEW   2      /* renew the lease, or ask for a new address */
#define DHCPOP_REGDNS  3      /* send this computer's name to DNS again
                                 (SOCK_IF.regstate says how it went) */

/* a socket's kind, for SOCK_OPEN */
#define SOCK_STREAM    1      /* TCP: a connection */
#define SOCK_DGRAM     2      /* UDP: datagrams */

/* OR this into SOCK_OPEN's type for a TLS stream: the stack runs the TLS (1.3
   or 1.2) handshake itself (kernel-side, so the crypto is 32-bit correct) once the TCP
   connection is up, and SOCK_SEND/SOCK_RECV then carry the plaintext.  The
   caller watches SOCK_STATE: SS_ESTABLISHED means TLS is up, not merely TCP. */
#define SOCK_TLS       0x0100

/* a socket's state, from SOCK_STATE */
#define SS_FREE        0      /* not a socket */
#define SS_CLOSED      1      /* open, idle - a fresh socket or a closed one */
#define SS_CONNECTING  2      /* TCP: the handshake is going */
#define SS_LISTEN      3      /* TCP: waiting for somebody to connect */
#define SS_ESTABLISHED 4      /* data may pass */
#define SS_CLOSING     5      /* TCP: the close is going - ours, or the
                                 peer's once everything it sent is read
                                 (sending is still allowed then) */
#define SS_ERROR       6      /* reset, or the handshake failed */

/* how a lookup or an echo stands (SOCK_RESOLVE, SOCK_PING, and SOCK_STAT's
   err) */
#define NET_PENDING    0
#define NET_DONE       1
#define NET_FAIL       2

/* why a socket is SS_ERROR, in SOCK_STAT's err.  NET_FAIL is any other
   reason, and the only one a TCPIP.SYS older than these gives */
#define NET_REFUSED    3      /* TCP: the server said no (a reset to our SYN) */
#define NET_TIMEOUT    4      /* TCP: the server stopped answering, or never did */
#define NET_NOROUTE    5      /* TCP: the server, or the gateway to it, never
                                 answered ARP - nothing at that address */
#define NET_RESET      6      /* TCP: the server reset a connection that was up */
#define NET_TLSCERT    7      /* TLS: the server's certificate was refused;
                                 SOCK_STAT's why says which check failed */
#define NET_TLSFAIL    8      /* TLS: the handshake failed some other way - an
                                 alert, nothing in common, no memory */

/* SOCK_STAT's why, with NET_TLSCERT */
#define CERT_UNTRUSTED 1      /* no path to a root in the trust bundle */
#define CERT_HOST      2      /* made out to another name */
#define CERT_DATES     3      /* expired, or not valid yet */
#define CERT_BAD       4      /* could not be read */

/* how the interface got its address, in SOCK_IF.dhcp */
#define DHS_DOWN       0      /* no address yet */
#define DHS_BOUND      1      /* DHCP gave one */
#define DHS_STATIC     2      /* set by hand */

/* SOCK_IFSTAT - the interface and its address.  A caller gives SOCKIF_LEN,
   SOCKIF_LEN2 or SOCKIF_LEN3 bytes; the stack fills the longest form that
   fits and answers how many.  A stack older than the form a caller asked
   for answers a shorter one */
typedef struct {
    uint32_t ip;
    uint32_t mask;
    uint32_t gw;
    uint32_t dns;
    uint32_t leasesecs;       /* the DHCP lease, in seconds; FFFFFFFF = no end */
    uint8_t  mac[6];
    uint8_t  up;              /* 1: an address is held */
    uint8_t  dhcp;            /* DHS_* */
    char     nic[32];         /* the card driver's name */
    uint32_t dhcpsrv;         /* the DHCP server that gave the lease */
    uint32_t leaseage;        /* seconds since the lease was given or renewed */
    uint16_t dhstate;         /* DHST_*: where DHCP is now */
    uint16_t acks;            /* ACKs taken since TCPIP.SYS loaded (wraps):
                                 a change means a lease was given or renewed */
    uint32_t wins;            /* the WINS server (NetBIOS names), 0 if none */
    char     nbname[16];      /* this computer's NetBIOS name (/NAME=), "" if none */
    /* SOCKIF_LEN3 on */
    char     hostname[64];    /* this computer's DNS host name, lower case */
    char     domain[64];      /* its DNS domain, "" if none: TCPIP.INI's or DHCP's */
    uint32_t dns2, dns3;      /* more DNS servers, asked when the first is not */
    uint32_t cabytes;         /* the trust bundle TLS checks against; 0: none,
                                 and TLS connects without checking */
    uint16_t regstate;        /* DREG_*: the name in DNS */
    uint16_t regrcode;        /* the DNS server's answer to the last update */
} SOCK_IF;
#define SOCKIF_LEN   60
#define SOCKIF_LEN2  92
#define SOCKIF_LEN3  236

/* SOCK_IF.regstate - this computer's name and address sent to DNS (RFC 2136) */
#define DREG_OFF       0      /* not sent: RegisterDNS=No, or no domain */
#define DREG_BUSY      1      /* being sent */
#define DREG_DONE      2      /* the server took it */
#define DREG_REFUSED   3      /* the server said no (regrcode says how) */
#define DREG_NOANSWER  4      /* nobody answered */
#define DREG_NOZONE    5      /* DNS knows no zone of the network's own that
                                 the name could go in: none is sent to */

/* SOCK_IF.dhstate */
#define DHST_OFF       0      /* not asking: a static address, or released */
#define DHST_ASKING    1      /* looking for a server (DISCOVER or REQUEST out) */
#define DHST_BOUND     2      /* holding a lease */
#define DHST_RENEWING  3      /* past half the lease: asking the server again */
#define DHST_REBINDING 4      /* near the end: asking any server */

/* SOCK_OPEN */
typedef struct {
    uint32_t type;            /* SOCK_STREAM or SOCK_DGRAM */
} SOCK_OPENREQ;

/* SOCK_CLOSE */
typedef struct {
    uint32_t handle;
} SOCK_HANDLE;

/* SOCK_CONNECT - TCP: begin a connection; UDP: fix the default peer */
typedef struct {
    uint32_t handle;
    uint32_t ip;
    uint16_t port;
    uint16_t spare;
} SOCK_CONNREQ;

/* SOCK_LISTEN - TCP only */
typedef struct {
    uint32_t handle;
    uint16_t port;
    uint16_t spare;
} SOCK_LISTENREQ;

/* SOCK_BIND - UDP only: give a datagram socket a port of its own.  Until
   then a socket has none, and gets one picked for it by its first send
   or SOCK_CONNECT - good for asking a server, no good for being one.
   Port 0 asks for a picked one now.  ERR_ACCESS when another UDP socket
   has the port, or the stack does (68, 137); ERR_BADDATA for a TCP
   socket.  A bound socket takes every datagram for its port, whoever
   sent it and whether it came to this computer's address or to the
   network's broadcast address; SOCK_CONNECT afterwards sets where
   SOCK_SEND goes and leaves the port alone.  Datagrams wait in a queue
   of 4,096 bytes (8 more than its data for each), and one that does not
   fit is dropped */
typedef struct {
    uint32_t handle;
    uint16_t port;
    uint16_t spare;
} SOCK_BINDREQ;

/* SOCK_ACCEPT - the next connection that came in on a listening socket */
typedef struct {
    uint32_t handle;          /* in: the listening socket */
    uint32_t newh;            /* out: the accepted socket, 0 if none */
    uint32_t ip;              /* out: the peer's address */
    uint16_t port;            /* out: the peer's port */
    uint16_t spare;
} SOCK_ACCEPTREQ;

/* SOCK_SEND and SOCK_RECV - the header, then the data.  For UDP, ip and
   port are the peer: where a send goes, and where a receive came from */
typedef struct {
    uint32_t handle;
    uint32_t ip;
    uint16_t port;
    uint16_t spare;
    uint32_t len;             /* bytes of data after this header */
} SOCK_IO;
#define SOCK_IO_HDR  16

/* SOCK_STATE */
typedef struct {
    uint32_t handle;          /* in */
    uint32_t state;           /* out: SS_* */
    uint32_t rxready;         /* out: bytes waiting to be read */
    uint32_t txfree;          /* out: room in the send buffer now */
    uint32_t err;             /* out: NET_FAIL and the like when SS_ERROR */
    uint32_t why;             /* out: CERT_* with NET_TLSCERT, else 0.  Only
                                 when SOCKSTAT_LEN2 bytes were given */
} SOCK_STAT;
#define SOCKSTAT_LEN  20
#define SOCKSTAT_LEN2 24

/* SOCK_RESOLVE - a host name to an address.  A name of 1 to 15 characters
   with no dot is a NetBIOS name first: the WINS server is asked, then the
   network by broadcast, and DNS last.  Any other name goes to DNS.  Call
   with the name; poll until the state is not NET_PENDING (up to about 8
   seconds when nothing answers) */
typedef struct {
    uint32_t ip;              /* out */
    uint32_t state;           /* out: NET_* */
    char     name[128];       /* in: ASCIIZ */
} SOCK_RES;
#define SOCKRES_LEN  136

/* SOCK_PING - one ICMP echo, and its reply.  Call with the address;
   poll until the state is not NET_PENDING */
typedef struct {
    uint32_t ip;              /* in */
    uint32_t state;           /* out: NET_* */
    uint32_t rttms;           /* out: the round trip, in milliseconds */
    uint32_t seq;             /* in and out */
} SOCK_PINGREQ;
#define SOCKPING_LEN 16

/* SOCK_TLSHOST - the server name of a TLS socket, set after SOCK_OPEN (with
   SOCK_TLS) and before the connection comes up.  It is sent as SNI and the
   name the certificate must match.  An empty name skips both. */
typedef struct {
    uint32_t handle;
    char     name[128];       /* in: ASCIIZ server name */
} SOCK_TLSHOSTREQ;
#define SOCKTLSHOST_LEN 132

/* SOCK_SETCA - hand the stack the trust bundle (concatenated DER roots, as in
   DISK\CACERT.BND) one chunk at a time, because the WfW bridge carries only a
   few hundred bytes per call.  offset 0 starts a fresh bundle of 'total'
   bytes; each call copies 'len' bytes of it at 'offset'.  Once the whole
   bundle is in, TLS sockets verify the server chain against it; with no
   bundle, a TLS socket connects without checking the certificate.
   TCPIP.SYS loads a bundle of its own at INIT (TCPIP.INI's CACerts=, or
   CACERT.BND beside TCPIP.SYS), so a program need not; one sent here
   replaces it for every socket opened after. */
typedef struct {
    uint32_t offset;          /* in: where this chunk goes in the bundle */
    uint32_t total;           /* in: the whole bundle's length */
    uint32_t len;             /* in: bytes of data in this chunk */
    /* uint8_t data[len] follows */
} SOCK_CAREQ;
#define SOCKCA_HDR   12

/* a handy way to write an address out */
#define IP_A( ip )   ((uint8_t)( (ip) >> 24 ))
#define IP_B( ip )   ((uint8_t)( (ip) >> 16 ))
#define IP_C( ip )   ((uint8_t)( (ip) >> 8 ))
#define IP_D( ip )   ((uint8_t)( (ip) ))
#define MAKE_IP( a, b, c, d )  ( ((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                                 ((uint32_t)(c) << 8) | (uint32_t)(d) )

#endif /* SOCKET_H */
