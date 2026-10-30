/*
 * NETBIOS.H - NetBIOS over NetBEUI, as NETBEUI.SYS presents it
 *
 * NETBEUI.SYS (DRV\NETBEUI) is a 32-bit driver with NDTF_NETBIOS in its
 * table.  A program reaches it through 21h/F1h AL=25h NDF_CALL on its
 * row (the row whose flags have NDTF_NETBIOS); a redirector inside the
 * kernel through NDS_FLAGCALL with NDTF_NETBIOS.  Either way the
 * function is one of NB_* below, the buffer the matching request block,
 * and the answer a handle, a state or a byte count - or an error
 * (NDRV.H's ERR_*).
 *
 * A NAME is up to 15 characters, ASCIIZ, with the 16th byte - the
 * service - given apart: 0x00 the workstation, 0x20 the file server.
 * A SESSION is a handle, 1 up; it carries MESSAGES, each of which
 * arrives whole and in order.
 *
 * NOTHING BLOCKS.  A call starts a lookup or a session and returns; the
 * caller watches NB_STATE.  NB_RECV may take a message in parts:
 * NB_STATE's rxready is what is left of the message at the head.
 *
 * Needs INC\NDRV.H before it, for u8/u16/u32.
 */
#ifndef NETBIOS_H
#define NETBIOS_H

/* THE FUNCTIONS */
#define NB_IFSTAT    0        /* buf -> NB_IF; answer NBIF_LEN */
#define NB_FIND      1        /* buf -> NB_FINDREQ; answer the state (NBF_*) */
#define NB_CALL      2        /* buf -> NB_NAMEREQ; answer a handle */
#define NB_LISTEN    3        /* buf -> NB_NAMEREQ (suffix only); answer a handle */
#define NB_SEND      4        /* buf -> NB_IO + the message; answer its length
                                 when taken, 0 while one is still going */
#define NB_RECV      5        /* buf -> NB_IO + room; answer the bytes given
                                 (the next part of the head message), 0 none */
#define NB_STATE     6        /* buf -> NB_STAT; answer the state (NBX_*) */
#define NB_HANGUP    7        /* buf -> a u32 handle */
#define NB_SENDP     8        /* buf -> NB_IO + a part; answer its length.  The
                                 part is kept, and the NB_SEND that follows
                                 adds the last of the message and sends it
                                 all.  0 while a message is still going */

#define NB_MSGMAX    34816    /* the longest message either way */

/* a session's state */
#define NBX_FREE     0        /* not a session */
#define NBX_CALLING  1        /* finding the name, joining the link, starting */
#define NBX_LISTEN   2        /* waiting for somebody to call */
#define NBX_UP       3        /* messages may pass */
#define NBX_CLOSED   4        /* the other end hung up, or we did */
#define NBX_FAIL     5        /* no such name, nobody listening, or the link
                                 was lost (NB_STAT.err says which) */

/* NB_STAT.err */
#define NBE_NONE     0
#define NBE_NONAME   1        /* nobody answered for the name */
#define NBE_NOLISTEN 2        /* the name is there, but nothing listens on it */
#define NBE_LINK     3        /* the 802.2 link would not come up, or broke */
#define NBE_TIMEOUT  4        /* the session did not start */

/* a lookup's state, NB_FINDREQ.state */
#define NBF_PENDING  0
#define NBF_DONE     1
#define NBF_FAIL     2

typedef struct {
    char name[16];            /* our name, ASCIIZ */
    u8   mac[6];
    u8   up;                  /* 1: the name is ours on the network */
    u8   sessions;            /* sessions open now */
    char card[32];            /* the card driver's name */
} NB_IF;
#define NBIF_LEN     56

typedef struct {
    char name[16];            /* in: ASCIIZ */
    u8   suffix;              /* in: the 16th byte */
    u8   spare[3];
    u32  state;               /* out: NBF_* */
    u8   mac[6];              /* out: the card that answered */
    u16  spare2;
} NB_FINDREQ;
#define NBFIND_LEN   32

typedef struct {
    char name[16];            /* in: who to call (NB_CALL), ASCIIZ */
    u8   suffix;              /* in: the 16th byte; NB_LISTEN: which of our
                                 names is listened on */
    u8   spare[3];
} NB_NAMEREQ;
#define NBNAME_LEN   20

typedef struct {
    u32 handle;
    u32 len;                  /* bytes of data after this header */
} NB_IO;
#define NBIO_HDR     8

typedef struct {
    u32 handle;               /* in */
    u32 state;                /* out: NBX_* */
    u32 rxready;              /* out: bytes left of the message at the head */
    u32 txbusy;               /* out: 1 while a message is still going */
    u32 err;                  /* out: NBE_* */
} NB_STAT;
#define NBSTAT_LEN   20

#endif /* NETBIOS_H */
