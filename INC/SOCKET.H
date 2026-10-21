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
 * AN ADDRESS IS A u32, most significant octet first: 10.0.2.2 is
 * 0x0A000202L.  A PORT IS A u16 in the natural order: 80 is 80.  The
 * stack puts both on the wire big-endian itself.
 *
 * NOTHING BLOCKS.  The stack runs on the clock and whenever a socket
 * call is made; a call does what it can at once and returns.  A connect
 * starts a handshake and returns; the caller watches SOCK_STATE for it
 * to reach SS_ESTABLISHED.  A receive gives what has arrived, or none.
 * SOCKLIB is where the waiting and the timeouts live.
 *
 * Needs INC\NDRV.H before it, for u8/u16/u32 and REGS.
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

/* a socket's kind, for SOCK_OPEN */
#define SOCK_STREAM    1      /* TCP: a connection */
#define SOCK_DGRAM     2      /* UDP: datagrams */

/* OR this into SOCK_OPEN's type for a TLS stream: the stack runs the TLS 1.2
   handshake itself (kernel-side, so the crypto is 32-bit correct) once the TCP
   connection is up, and SOCK_SEND/SOCK_RECV then carry the plaintext.  The
   caller watches SOCK_STATE: SS_ESTABLISHED means TLS is up, not merely TCP. */
#define SOCK_TLS       0x0100

/* a socket's state, from SOCK_STATE */
#define SS_FREE        0      /* not a socket */
#define SS_CLOSED      1      /* open, idle - a fresh socket or a closed one */
#define SS_CONNECTING  2      /* TCP: the handshake is going */
#define SS_LISTEN      3      /* TCP: waiting for somebody to connect */
#define SS_ESTABLISHED 4      /* data may pass */
#define SS_CLOSING     5      /* TCP: the close is going */
#define SS_ERROR       6      /* reset, or the handshake failed */

/* how a lookup or an echo stands (SOCK_RESOLVE, SOCK_PING, and SOCK_STAT's
   err) */
#define NET_PENDING    0
#define NET_DONE       1
#define NET_FAIL       2

/* how the interface got its address, in SOCK_IF.dhcp */
#define DHS_DOWN       0      /* no address yet */
#define DHS_BOUND      1      /* DHCP gave one */
#define DHS_STATIC     2      /* set by hand */

/* SOCK_IFSTAT - the interface and its address */
typedef struct {
    u32  ip;
    u32  mask;
    u32  gw;
    u32  dns;
    u32  leasesecs;           /* the DHCP lease, in seconds */
    u8   mac[6];
    u8   up;                  /* 1: an address is held */
    u8   dhcp;                /* DHS_* */
    char nic[32];             /* the card driver's name */
} SOCK_IF;
#define SOCKIF_LEN   60

/* SOCK_OPEN */
typedef struct {
    u32 type;                 /* SOCK_STREAM or SOCK_DGRAM */
} SOCK_OPENREQ;

/* SOCK_CLOSE */
typedef struct {
    u32 handle;
} SOCK_HANDLE;

/* SOCK_CONNECT - TCP: begin a connection; UDP: fix the default peer */
typedef struct {
    u32 handle;
    u32 ip;
    u16 port;
    u16 spare;
} SOCK_CONNREQ;

/* SOCK_LISTEN - TCP only */
typedef struct {
    u32 handle;
    u16 port;
    u16 spare;
} SOCK_LISTENREQ;

/* SOCK_ACCEPT - the next connection that came in on a listening socket */
typedef struct {
    u32 handle;               /* in: the listening socket */
    u32 newh;                 /* out: the accepted socket, 0 if none */
    u32 ip;                   /* out: the peer's address */
    u16 port;                 /* out: the peer's port */
    u16 spare;
} SOCK_ACCEPTREQ;

/* SOCK_SEND and SOCK_RECV - the header, then the data.  For UDP, ip and
   port are the peer: where a send goes, and where a receive came from */
typedef struct {
    u32 handle;
    u32 ip;
    u16 port;
    u16 spare;
    u32 len;                  /* bytes of data after this header */
} SOCK_IO;
#define SOCK_IO_HDR  16

/* SOCK_STATE */
typedef struct {
    u32 handle;               /* in */
    u32 state;                /* out: SS_* */
    u32 rxready;              /* out: bytes waiting to be read */
    u32 txfree;               /* out: room in the send buffer now */
    u32 err;                  /* out: NET_FAIL and the like when SS_ERROR */
} SOCK_STAT;
#define SOCKSTAT_LEN 20

/* SOCK_RESOLVE - a host name to an address, through the DNS server DHCP
   gave.  Call with the name; poll until the state is not NET_PENDING */
typedef struct {
    u32  ip;                  /* out */
    u32  state;               /* out: NET_* */
    char name[128];           /* in: ASCIIZ */
} SOCK_RES;
#define SOCKRES_LEN  136

/* SOCK_PING - one ICMP echo, and its reply.  Call with the address;
   poll until the state is not NET_PENDING */
typedef struct {
    u32 ip;                   /* in */
    u32 state;                /* out: NET_* */
    u32 rttms;                /* out: the round trip, in milliseconds */
    u32 seq;                  /* in and out */
} SOCK_PINGREQ;
#define SOCKPING_LEN 16

/* SOCK_TLSHOST - the server name of a TLS socket, set after SOCK_OPEN (with
   SOCK_TLS) and before the connection comes up.  It is sent as SNI and the
   name the certificate must match.  An empty name skips both. */
typedef struct {
    u32  handle;
    char name[128];           /* in: ASCIIZ server name */
} SOCK_TLSHOSTREQ;
#define SOCKTLSHOST_LEN 132

/* SOCK_SETCA - hand the stack the trust bundle (concatenated DER roots, as in
   DISK\CACERT.BND) one chunk at a time, because the WfW bridge carries only a
   few hundred bytes per call.  offset 0 starts a fresh bundle of 'total'
   bytes; each call copies 'len' bytes of it at 'offset'.  Once the whole
   bundle is in, TLS sockets verify the server chain against it; with no
   bundle, a TLS socket connects without checking the certificate. */
typedef struct {
    u32 offset;               /* in: where this chunk goes in the bundle */
    u32 total;                /* in: the whole bundle's length */
    u32 len;                  /* in: bytes of data in this chunk */
    /* u8 data[len] follows */
} SOCK_CAREQ;
#define SOCKCA_HDR   12

/* a handy way to write an address out */
#define IP_A( ip )   ((u8)( (ip) >> 24 ))
#define IP_B( ip )   ((u8)( (ip) >> 16 ))
#define IP_C( ip )   ((u8)( (ip) >> 8 ))
#define IP_D( ip )   ((u8)( (ip) ))
#define MAKE_IP( a, b, c, d )  ( ((u32)(a) << 24) | ((u32)(b) << 16) | \
                                 ((u32)(c) << 8) | (u32)(d) )

#endif /* SOCKET_H */
