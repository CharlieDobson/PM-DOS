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
#define NIC_BIND     7        /* buf -> NICB_LEN bytes: a uint16_t Ethernet type at
                                 +0, or 0 and an 802.2 DSAP at +2.  The frames
                                 that match go to a queue of their own, read by
                                 NIC_RECVB, and not to NIC_RECV's; answer the
                                 binding, 1..NIC_BINDS.  ERR_GENERAL when every
                                 binding is taken */
#define NIC_RECVB    8        /* buf -> a uint32_t binding at +0, room for a frame
                                 at +4 (len counts both); answer the frame's
                                 length copied to +4, 0 for none */

#define NICB_LEN     4
#define NIC_BINDS    2

#define NIC_HDRLEN   14
#define NIC_MINFRAME NIC_HDRLEN
#define NIC_MAXFRAME 1514

/* NIC_INFO's buffer (NICI_*) */
typedef struct {
    uint8_t  mac[6];
    uint8_t  kind;            /* NICK_* */
    uint8_t  bus;             /* NICB_* */
    uint16_t mtu;             /* 1500 */
    uint16_t port;            /* the first I/O port, or 0 */
    uint8_t  irq;             /* the line, or NIC_NOIRQ */
    uint8_t  flags;           /* NICF_* */
    uint16_t spare;
    uint32_t pciaddr;         /* BUS_PCIENUM's address, 0 on ISA */
    char     name[32];        /* ASCIIZ */
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
    uint32_t rxframes, rxbytes;
    uint32_t txframes, txbytes;
    uint32_t rxdropped;           /* lost for a full queue */
    uint32_t rxerrors, txerrors;  /* the card's */
    uint32_t overflows;           /* the card's buffer ran over */
    uint32_t irqs;                /* interrupts that were the card's */
    uint32_t queued;              /* waiting now */
} NIC_STATBLOCK;

#define NICS_LEN     40

#endif /* NIC_H */
