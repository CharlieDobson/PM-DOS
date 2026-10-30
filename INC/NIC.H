/*
 * NIC.H - a network card, as a 32-bit driver presents one: INC\NIC.INC
 *         for C.  Every number here is NIC.INC's.
 */
#ifndef NIC_H
#define NIC_H

/* THE FUNCTIONS - the fn of a card driver's CALL entry */
#define NIC_INFO     0        /* buf -> NIC_INFOBLOCK, answer NICI_LEN */
#define NIC_SEND     1        /* buf -> a frame of len bytes, answer len */
#define NIC_RECV     2        /* buf -> len bytes, answer the frame's
                                 length copied there, 0 for none */
#define NIC_STATS    3        /* buf -> NIC_STATBLOCK, answer NICS_LEN */
#define NIC_SERVICE  4        /* drain the card's ring into the queue, as the
                                 IRQ/tick do; for the WfW bridge, which drives
                                 the parked card with interrupts off */
#define NIC_WAKE     5        /* bring the parked card back up to receive,
                                 polled (its interrupt line masked), for the
                                 WfW bridge; answer 1 if it is running */
#define NIC_WAKE_IRQ 6        /* the same, but with the card's interrupt line
                                 LIVE (not polled) so it asserts on receive,
                                 for the WfW bridge to field through VPICD;
                                 answer 1 if it is running */
#define NIC_BIND     7        /* buf -> NICB_LEN bytes: a u16 Ethernet type at
                                 +0, or 0 and an 802.2 DSAP at +2.  The frames
                                 that match go to a queue of their own, read by
                                 NIC_RECVB, and not to NIC_RECV's; answer the
                                 binding, 1..NIC_BINDS.  ERR_GENERAL when every
                                 binding is taken */
#define NIC_RECVB    8        /* buf -> a u32 binding at +0, room for a frame
                                 at +4 (len counts both); answer the frame's
                                 length copied to +4, 0 for none */

#define NICB_LEN     4
#define NIC_BINDS    2

#define NIC_HDRLEN   14
#define NIC_MINFRAME NIC_HDRLEN
#define NIC_MAXFRAME 1514

/* NIC_INFO's buffer (NICI_*) */
typedef struct {
    u8  mac[6];
    u8  kind;                 /* NICK_* */
    u8  bus;                  /* NICB_* */
    u16 mtu;                  /* 1500 */
    u16 port;                 /* the first I/O port, or 0 */
    u8  irq;                  /* the line, or NIC_NOIRQ */
    u8  flags;                /* NICF_* */
    u16 spare;
    u32 pciaddr;              /* BUS_PCIENUM's address, 0 on ISA */
    char name[32];            /* ASCIIZ */
} NIC_INFOBLOCK;

#define NICI_LEN     52

#define NICK_ETHERNET 1

#define NICB_ISA     1
#define NICB_PCI     2

#define NICF_POLLED  0x01     /* no interrupt: the clock asks the card */
#define NICF_WORD    0x02     /* 16-bit transfers */

#define NIC_NOIRQ    0xFF

/* NIC_STATS' buffer (NICS_*) */
typedef struct {
    u32 rxframes, rxbytes;
    u32 txframes, txbytes;
    u32 rxdropped;            /* lost for a full queue */
    u32 rxerrors, txerrors;   /* the card's */
    u32 overflows;            /* the card's buffer ran over */
    u32 irqs;                 /* interrupts that were the card's */
    u32 queued;               /* waiting now */
} NIC_STATBLOCK;

#define NICS_LEN     40

#endif /* NIC_H */
