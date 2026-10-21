/*
 * NE2000.C - NE2000.SYS, a 32-bit driver for the DP8390 family
 *
 * The first card driver of the network stack (INC\NIC.INC), for the
 * card everybody cloned: the National DP8390 behind an NE2000's ports,
 * which is the NE1000 and NE2000 themselves, the Realtek RTL8019AS on
 * ISA and the RTL8029AS on PCI, and the clones of those.  It is the
 * card every emulator has - QEMU's ne2k_isa and ne2k_pci, 86Box's
 * NE1000, NE2000, RTL8019AS and RTL8029AS, DOSBox-X's NE2000 - and
 * the card the first version of everything above it is proved on.
 *
 * HOW THE CARD WORKS.  The 8390 owns 8K (NE1000) or 16K (NE2000) of
 * memory on the card, in 256-byte pages, that the processor reaches
 * only through the chip's "remote DMA": an address and a count are
 * written to its registers and the bytes go through one data port,
 * a byte or a word at a time.  A frame to send is written into the
 * transmit pages and the chip is told to send them.  Frames that
 * arrive go into a ring of the remaining pages, each with a 4-byte
 * header the chip writes - its status, the page after it, its length
 * - and two registers mark the ring: CURR, where the chip will put
 * the next frame, and BNRY, the last page the driver has emptied,
 * which the chip will not write past.  Draining the ring is reading
 * from BNRY+1 up to CURR and moving BNRY along, and that is what the
 * interrupt handler does, into a queue of this driver's own, because
 * the ring is sixty pages and the queue is where NIC_RECV reads from.
 *
 * ONE PORT FOR DATA AND NO BUS MASTERING is why the card is simple
 * and slow: every byte crosses the ISA bus twice, once into the card
 * and once out.  Nothing here waits for the wire except where it
 * must - a frame cannot be loaded while the one before it is being
 * sent, which is 1.2 ms at most - and every wait is bounded by a
 * count, so a card that has died takes a call down, not the machine.
 *
 * FINDING THE CARD.  /P:port names its first port and /I:line its
 * interrupt; without /P: the PCI bus is asked for the 8029 and its
 * clones first, then ISA Plug and Play for an RTL8019AS or anything
 * that calls itself an NE2000 (PNP80D6) - such a card answers at no
 * port until it is given one, and the kernel's Plug and Play
 * services give it 300h, or the first port of its range, and a line
 * it allows that nothing else here has - and then the usual ISA
 * ports are tried: 300h, 280h, 320h, 340h, 360h.  A card is one
 * whose reset is acknowledged and
 * whose address PROM reads back as an 8390's: the station address
 * doubled, byte by byte, as the 16-bit cards present it, or single
 * as an NE1000 does, with the 'W' or 'B' pair the makers put after
 * it.  Which of the two the PROM is also says whether the card takes
 * words or bytes.  Without /I: the line is found by making the card
 * interrupt - a remote DMA of nothing - with a handler on every line
 * a card could be jumpered to, and keeping the one that was called;
 * a card whose line is found by nobody is served from the clock
 * instead, eighteen times a second, which is slow and works.
 *
 * WHAT IS NOT HERE: the card's multicast filter (the stack has no
 * use for it until it speaks IPv6 or mDNS), a second transmit buffer
 * (a frame is loaded only when the last is away), and the shared-
 * memory cousins of the 8390 - the WD8003, the 3C503 - which reach
 * the same chip through a window of memory rather than a port and
 * are a driver of their own.
 *
 * Built by BUILD.CMD: wcc386 -3r -s -zl, behind DRV\NDSTART.ASM,
 * linked twice and put together by D32DRV.  No C library: the few
 * routines one would supply are at the bottom.
 */
#define ND_DRIVER
#include "ndrv.h"
#include "nic.h"

/* ---------------------------------------------------------------- */
/* the processor: ports and the interrupt flag, in ring 0           */
/* ---------------------------------------------------------------- */

u8   inb( u16 port );
#pragma aux inb = "in al,dx" parm [dx] value [al] modify exact [al];
u16  inw( u16 port );
#pragma aux inw = "in ax,dx" parm [dx] value [ax] modify exact [ax];
void outb( u16 port, u8 value );
#pragma aux outb = "out dx,al" parm [dx] [al] modify exact [];
void outw( u16 port, u16 value );
#pragma aux outw = "out dx,ax" parm [dx] [ax] modify exact [];
void insb( u16 port, void *buf, u32 bytes );
#pragma aux insb = "rep insb" parm [dx] [edi] [ecx] modify exact [edi ecx];
void insw( u16 port, void *buf, u32 words );
#pragma aux insw = "rep insw" parm [dx] [edi] [ecx] modify exact [edi ecx];
void outsb( u16 port, const void *buf, u32 bytes );
#pragma aux outsb = "rep outsb" parm [dx] [esi] [ecx] modify exact [esi ecx];
void outsw( u16 port, const void *buf, u32 words );
#pragma aux outsw = "rep outsw" parm [dx] [esi] [ecx] modify exact [esi ecx];
u32  int_off( void );
#pragma aux int_off = "pushfd" "pop eax" "cli" value [eax] modify exact [eax];
void int_back( u32 flags );
#pragma aux int_back = "push eax" "popfd" parm [eax] modify exact [];

/* ---------------------------------------------------------------- */
/* the DP8390                                                        */
/* ---------------------------------------------------------------- */

#define NE_CR       0x00    /* command, every page */
#define NE_PSTART   0x01    /* page 0, written */
#define NE_PSTOP    0x02
#define NE_BNRY     0x03
#define NE_TPSR     0x04
#define NE_TBCR0    0x05
#define NE_TBCR1    0x06
#define NE_ISR      0x07
#define NE_RSAR0    0x08
#define NE_RSAR1    0x09
#define NE_RBCR0    0x0A
#define NE_RBCR1    0x0B
#define NE_RCR      0x0C
#define NE_TCR      0x0D
#define NE_DCR      0x0E
#define NE_IMR      0x0F
#define NE_RSR      0x0C    /* page 0, read */
#define NE_CNTR0    0x0D
#define NE_CNTR1    0x0E
#define NE_CNTR2    0x0F
#define NE_PAR0     0x01    /* page 1 */
#define NE_CURR     0x07
#define NE_MAR0     0x08
#define NE_DATA     0x10    /* the remote DMA port */
#define NE_RESET    0x1F

#define CR_STP      0x01
#define CR_STA      0x02
#define CR_TXP      0x04
#define CR_RD0      0x08    /* remote read */
#define CR_RD1      0x10    /* remote write */
#define CR_RD2      0x20    /* no remote DMA */
#define CR_PAGE0    0x00
#define CR_PAGE1    0x40

#define ISR_PRX     0x01    /* a frame arrived */
#define ISR_PTX     0x02    /* a frame went */
#define ISR_RXE     0x04    /* ...with an error */
#define ISR_TXE     0x08
#define ISR_OVW     0x10    /* the ring ran over */
#define ISR_CNT     0x20    /* a tally counter is half full */
#define ISR_RDC     0x40    /* a remote DMA finished */
#define ISR_RST     0x80    /* the chip is stopped */
#define ISR_ALL     (ISR_PRX | ISR_PTX | ISR_RXE | ISR_TXE | ISR_OVW | ISR_CNT)

#define DCR_WTS     0x01    /* word transfers */
#define DCR_LS      0x08    /* not loopback (the DMA's own) */
#define DCR_FT1     0x40    /* FIFO threshold 8 words */

#define RCR_AB      0x04    /* take broadcasts */
#define RCR_AM      0x08    /* take multicast (filtered by the MAR hash) */
#define RCR_MON     0x20    /* take nothing */

#define TCR_LB      0x02    /* internal loopback */

#define RSR_PRX     0x01    /* the frame in the ring is whole */

#define IMR_WANTED  (ISR_PRX | ISR_PTX | ISR_RXE | ISR_TXE | ISR_OVW)

/* where the chip's memory is, in pages: 8K from 20h on an NE1000, 16K
   from 40h on an NE2000; six pages hold the longest frame to send and
   the rest is the receive ring */
#define TX_PAGES    6

/* how long a bounded wait is - port reads, a microsecond each on real
   hardware and nothing on an emulator */
#define SPIN_RESET  100000L
#define SPIN_TXP    200000L
#define SPIN_RDC    200000L

/* ---------------------------------------------------------------- */
/* the driver's state                                                */
/* ---------------------------------------------------------------- */

#define QN          16                  /* frames the queue holds */
#define QSLOT       1536                /* a slot: a length word and
                                           the longest frame, rounded */
#define PROBE_WAIT  4                   /* ticks a line may take */

static u16  port;                       /* the card's first port */
static u8   irq = NIC_NOIRQ;            /* its line, or polled */
static u8   word;                       /* 16-bit transfers */
static u8   bus;                        /* NICB_* */
static u32  pciaddr;
static u8   mac[6];
static u8   tx_page, rx_start, rx_stop;
static u8   running;                    /* set up and started */
static u8   parked;                     /* Windows has the machine */
static u8   probing;                    /* the line is being looked for */
static volatile u8 probe_line = NIC_NOIRQ; /* ...and this one answered,
                                           written by the handler */
static u8   polled;                     /* the clock reads the card */
static u8   tick_seen;                  /* the clock saw the card asking
                                           last tick too */
static char card_name[32];

static u8  *queue;                      /* QN + 1 slots: the last is
                                           where a frame goes when the
                                           queue is full */
static u32  q_head, q_tail;
static volatile u32 q_count;            /* the handler raises it */
static u8   txbuf[1536];                /* a frame padded, on its way
                                           to the card */

static NIC_STATBLOCK stats;

/* the options on the DEVICE= line */
static u32  opt_port;                   /* /P:, 0 for none */
static u32  opt_irq = NIC_NOIRQ;        /* /I: */

/* the PCI cards that are an NE2000 behind their first base register */
static const struct { u16 vendor, device; const char *name; } pci_cards[] = {
    { 0x10EC, 0x8029, "RTL8029AS" },
    { 0x1050, 0x0940, "Winbond W89C940" },
    { 0x11F6, 0x1401, "Compex RL2000" },
    { 0x8E2E, 0x3000, "KTI ET32P2" },
    { 0x4A14, 0x5000, "NetVin NV5000" },
    { 0x1106, 0x0926, "VIA VT86C926" },
    { 0x10BD, 0x0E34, "SureCom NE34" },
    { 0x12C3, 0x0058, "Holtek HT80232" },
    { 0x12C3, 0x5598, "Holtek HT80229" },
    { 0,      0,      0 }
};

static const u16 isa_ports[] = { 0x300, 0x280, 0x320, 0x340, 0x360, 0 };

/* the lines an NE2000 can be jumpered to, tried in this order */
static const u8 probe_lines[] = { 3, 4, 5, 9, 10, 11, 12, 15, 0 };

static void str_cpy( char *to, const char *from );
static void str_cat( char *to, const char *from );
static void str_hex( char *to, u32 value, int digits );
static void str_dec( char *to, u32 value );
static void mem_copy( void *to, const void *from, u32 len );
static void mem_zero( void *to, u32 len );

/* ---------------------------------------------------------------- */
/* the card's memory, through the remote DMA port                    */
/* ---------------------------------------------------------------- */

/* the card is running or in loopback: the chip must be started for
   its DMA to move, which is why set-up puts it in loopback first */
static void remote_read( u16 addr, void *buf, u32 len )
{
    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
    outb( port + NE_RBCR0, (u8)len );
    outb( port + NE_RBCR1, (u8)( len >> 8 ) );
    outb( port + NE_RSAR0, (u8)addr );
    outb( port + NE_RSAR1, (u8)( addr >> 8 ) );
    outb( port + NE_CR, CR_PAGE0 | CR_RD0 | CR_STA );
    if ( word ) {
        insw( port + NE_DATA, buf, ( len + 1 ) / 2 );
    } else {
        insb( port + NE_DATA, buf, len );
    }
    outb( port + NE_ISR, ISR_RDC );
}

/* a write, and the wait for the chip to say it has it all: on a card
   that never says so the frame may still go, so the wait is counted
   and not believed */
static int remote_write( u16 addr, const void *buf, u32 len )
{
    u32 n;

    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
    outb( port + NE_ISR, ISR_RDC );
    outb( port + NE_RBCR0, (u8)len );
    outb( port + NE_RBCR1, (u8)( len >> 8 ) );
    outb( port + NE_RSAR0, (u8)addr );
    outb( port + NE_RSAR1, (u8)( addr >> 8 ) );
    outb( port + NE_CR, CR_PAGE0 | CR_RD1 | CR_STA );
    if ( word ) {
        outsw( port + NE_DATA, buf, len / 2 );
    } else {
        outsb( port + NE_DATA, buf, len );
    }
    for ( n = 0; n < SPIN_RDC; n++ ) {
        if ( inb( port + NE_ISR ) & ISR_RDC ) break;
    }
    outb( port + NE_ISR, ISR_RDC );
    return n < SPIN_RDC;
}

/* ---------------------------------------------------------------- */
/* the receive queue                                                 */
/* ---------------------------------------------------------------- */

static u8 *q_slot( u32 index )
{
    return queue + index * QSLOT;
}

/* the slot the next frame is read into: the tail, or the spare when
   the queue is full (the frame is then dropped, and counted) */
static u8 *q_fill_slot( void )
{
    return q_slot( q_count == QN ? QN : q_tail );
}

static void q_filled( u32 len )
{
    if ( q_count == QN ) {
        stats.rxdropped++;
        return;
    }
    *(u16 *)q_slot( q_tail ) = (u16)len;
    q_tail = ( q_tail + 1 ) % QN;
    q_count++;
    stats.rxframes++;
    stats.rxbytes += len;
}

/* ---------------------------------------------------------------- */
/* the card                                                          */
/* ---------------------------------------------------------------- */

static void card_stop( void )
{
    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    outb( port + NE_IMR, 0 );
    outb( port + NE_ISR, 0xFF );
}

/* the reset port: reading it resets the chip, writing it back is
   what the NE2000's own driver did, and the stopped chip says so in
   the status register.  1 if it did */
static int card_reset( void )
{
    u32 n;

    outb( port + NE_RESET, inb( port + NE_RESET ) );
    for ( n = 0; n < SPIN_RESET; n++ ) {
        if ( inb( port + NE_ISR ) & ISR_RST ) break;
    }
    outb( port + NE_ISR, 0xFF );
    return n < SPIN_RESET;
}

/* the address PROM, 32 bytes at the start of the card's memory, read
   with the chip stopped and in loopback.  1 if it is an 8390's and
   the station address is a usable one; mac[] and word are set */
static int card_prom( int try_word )
{
    u8 prom[32];
    int i, doubled, pair;

    word = (u8)try_word;
    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    outb( port + NE_DCR, (u8)( DCR_FT1 | DCR_LS | ( word ? DCR_WTS : 0 ) ) );
    outb( port + NE_RBCR0, 0 );
    outb( port + NE_RBCR1, 0 );
    outb( port + NE_IMR, 0 );
    outb( port + NE_ISR, 0xFF );
    outb( port + NE_RCR, RCR_MON );
    outb( port + NE_TCR, TCR_LB );
    remote_read( 0, prom, 32 );

    doubled = 1;
    for ( i = 0; i < 32; i += 2 ) {
        if ( prom[i] != prom[i + 1] ) doubled = 0;
    }
    if ( doubled ) {
        pair = prom[28] == prom[30] && ( prom[28] == 'W' || prom[28] == 'B' );
        if ( !pair ) return 0;
        for ( i = 0; i < 6; i++ ) mac[i] = prom[i * 2];
    } else {
        pair = prom[14] == prom[15] && ( prom[14] == 'W' || prom[14] == 'B' );
        if ( !pair ) return 0;
        for ( i = 0; i < 6; i++ ) mac[i] = prom[i];
    }
    /* a station address: not nobody's, not everybody's, not a group's */
    for ( i = 0; i < 6 && mac[i] == 0; i++ ) ;
    if ( i == 6 ) return 0;
    for ( i = 0; i < 6 && mac[i] == 0xFF; i++ ) ;
    if ( i == 6 ) return 0;
    if ( mac[0] & 1 ) return 0;
    return 1;
}

/* is there an 8390 at port?  A reset it acknowledges and a PROM that
   reads as one, in words first and then in bytes */
static int card_probe( void )
{
    if ( inb( port + NE_CR ) == 0xFF ) return 0;
    if ( !card_reset() ) return 0;
    if ( card_prom( 1 ) ) return 1;
    if ( !card_reset() ) return 0;
    return card_prom( 0 );
}

/* from a stopped chip to a running one, as the datasheet orders it:
   the registers written in loopback, the station address on page 1,
   then started, and only then taken out of loopback */
static void card_setup( void )
{
    int i;

    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    outb( port + NE_DCR, (u8)( DCR_FT1 | DCR_LS | ( word ? DCR_WTS : 0 ) ) );
    outb( port + NE_RBCR0, 0 );
    outb( port + NE_RBCR1, 0 );
    outb( port + NE_RCR, RCR_MON );
    outb( port + NE_TCR, TCR_LB );
    outb( port + NE_TPSR, tx_page );
    outb( port + NE_PSTART, rx_start );
    outb( port + NE_BNRY, rx_start );
    outb( port + NE_PSTOP, rx_stop );
    outb( port + NE_ISR, 0xFF );
    outb( port + NE_IMR, 0 );
    outb( port + NE_CR, CR_PAGE1 | CR_RD2 | CR_STP );
    for ( i = 0; i < 6; i++ ) outb( port + NE_PAR0 + i, mac[i] );
    /* all-ones multicast hash: take every multicast address.  The NetBEUI
       functional address 03-00-00-00-00-01 is a multicast, so a card that is
       to carry NetBEUI must accept multicast, not only broadcast */
    for ( i = 0; i < 8; i++ ) outb( port + NE_MAR0 + i, 0xFF );
    outb( port + NE_CURR, (u8)( rx_start + 1 ) );
    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
    outb( port + NE_ISR, 0xFF );
    outb( port + NE_IMR, (u8)( polled ? 0 : IMR_WANTED ) );
    outb( port + NE_TCR, 0 );
    outb( port + NE_RCR, (u8)( RCR_AB | RCR_AM ) );
    running = 1;
}

/* the ring, emptied into the queue.  Interrupts are off: the caller
   is the interrupt handler, the clock, or a call that turned them
   off.  Bounded by the ring's size in frames, not by CURR, which the
   chip moves while this runs */
static void card_drain( void )
{
    u8  curr, bnry, next, hdr[4];
    u16 count, addr, first;
    u32 len, n;
    u8 *slot;

    for ( n = 0; n < 64; n++ ) {
        outb( port + NE_CR, CR_PAGE1 | CR_RD2 | CR_STA );
        curr = inb( port + NE_CURR );
        outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
        bnry = inb( port + NE_BNRY );
        next = (u8)( bnry + 1 );
        if ( next >= rx_stop ) next = rx_start;
        if ( next == curr ) break;

        remote_read( (u16)next << 8, hdr, 4 );
        count = (u16)( hdr[2] | ( hdr[3] << 8 ) );
        /* a frame that is whole, of a length a frame can have, whose
           next page is in the ring: anything else is a ring the chip
           and the driver no longer agree about, and it is started
           again from empty */
        if ( !( hdr[0] & RSR_PRX ) || count < 4 + NIC_HDRLEN || count > 4 + NIC_MAXFRAME + 4
                || hdr[1] < rx_start || hdr[1] >= rx_stop ) {
            stats.rxerrors++;
            outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
            outb( port + NE_BNRY, rx_start );
            outb( port + NE_CR, CR_PAGE1 | CR_RD2 | CR_STP );
            outb( port + NE_CURR, (u8)( rx_start + 1 ) );
            outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
            break;
        }
        len = count - 4;
        slot = q_fill_slot() + 2;
        addr = (u16)( ( (u16)next << 8 ) + 4 );
        /* the ring wraps at PSTOP and the chip's DMA may or may not:
           a frame that crosses it is read in two pieces that do not */
        first = (u16)( ( (u16)rx_stop << 8 ) - addr );
        if ( len > first ) {
            remote_read( addr, slot, first );
            remote_read( (u16)rx_start << 8, slot + first, len - first );
        } else {
            remote_read( addr, slot, len );
        }
        q_filled( len );
        /* BNRY trails the page after the frame by one, as the chip
           wants it; the page before the ring's first is its last */
        outb( port + NE_BNRY, (u8)( hdr[1] == rx_start ? rx_stop - 1 : hdr[1] - 1 ) );
    }
}

/* the ring ran over: the datasheet's recovery.  A transmit that was
   in progress is sent again afterwards */
static void card_overflow( void )
{
    u32 n;
    int resend;

    stats.overflows++;
    resend = ( inb( port + NE_CR ) & CR_TXP ) != 0;
    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    for ( n = 0; n < SPIN_RESET; n++ ) {
        if ( inb( port + NE_ISR ) & ISR_RST ) break;
    }
    outb( port + NE_RBCR0, 0 );
    outb( port + NE_RBCR1, 0 );
    if ( resend && ( inb( port + NE_ISR ) & ( ISR_PTX | ISR_TXE ) ) ) resend = 0;
    outb( port + NE_TCR, TCR_LB );
    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
    card_drain();
    outb( port + NE_ISR, ISR_OVW );
    outb( port + NE_TCR, 0 );
    if ( resend ) outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_TXP | CR_STA );
}

/* a frame to the wire.  Interrupts off, so that the handler's reads
   of the ring do not land in the middle of this write.  1 if the
   card took it */
static int card_send( const u8 *frame, u32 len )
{
    u32 n, loaded, flags;
    int ok;

    mem_copy( txbuf, frame, len );
    if ( len < 60 ) {
        mem_zero( txbuf + len, 60 - len );
        len = 60;
    }
    loaded = ( len + 1 ) & ~1L;
    flags = int_off();
    for ( n = 0; n < SPIN_TXP; n++ ) {
        if ( !( inb( port + NE_CR ) & CR_TXP ) ) break;
    }
    if ( n == SPIN_TXP ) {
        stats.txerrors++;
        int_back( flags );
        return 0;
    }
    ok = remote_write( (u16)tx_page << 8, txbuf, loaded );
    outb( port + NE_TPSR, tx_page );
    outb( port + NE_TBCR0, (u8)len );
    outb( port + NE_TBCR1, (u8)( len >> 8 ) );
    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_TXP | CR_STA );
    stats.txframes++;
    stats.txbytes += len;
    if ( !ok ) stats.txerrors++;
    int_back( flags );
    return 1;
}

/* what the card has to say, and the answer to it.  Interrupts off.
   1 if any of it was the card's */
static int card_service( void )
{
    u8  isr;
    u32 n;
    int mine = 0;

    for ( n = 0; n < 8; n++ ) {
        outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
        isr = inb( port + NE_ISR );
        if ( !( isr & ISR_ALL ) ) break;
        mine = 1;
        if ( isr & ISR_OVW ) {
            card_overflow();
        } else if ( isr & ( ISR_PRX | ISR_RXE ) ) {
            outb( port + NE_ISR, ISR_PRX | ISR_RXE );
            if ( isr & ISR_RXE ) stats.rxerrors++;
            card_drain();
        }
        if ( isr & ( ISR_PTX | ISR_TXE ) ) {
            outb( port + NE_ISR, ISR_PTX | ISR_TXE );
            if ( isr & ISR_TXE ) stats.txerrors++;
        }
        if ( isr & ISR_CNT ) {
            inb( port + NE_CNTR0 );
            inb( port + NE_CNTR1 );
            inb( port + NE_CNTR2 );
            outb( port + NE_ISR, ISR_CNT );
        }
    }
    return mine;
}

/* ---------------------------------------------------------------- */
/* finding the card                                                  */
/* ---------------------------------------------------------------- */

/* the PCI bus: the first function that is one of the cards above.
   1 with port, pciaddr, irq and card_name set */
static int find_pci( void )
{
    REGS r;
    int  i;
    u32  addr;

    for ( i = 0; pci_cards[i].vendor; i++ ) {
        mem_zero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PCIID << 8;
        r.ecx = pci_cards[i].vendor;
        r.edx = pci_cards[i].device;
        r.esi = 0;
        if ( !nds_bus( &r ) ) continue;
        addr = r.ebx;

        mem_zero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PCIBAR << 8;
        r.ebx = addr;
        r.edx = 0;
        if ( !nds_bus( &r ) || !( r.edx & BARF_IO ) || r.eax == 0 ) continue;
        port = (u16)( r.eax & ~3L );

        /* the function's I/O decoding, switched on if the BIOS left
           it off: bit 0 of the command register */
        mem_zero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PCIREAD << 8;
        r.ebx = addr;
        r.edx = 0x0204;
        if ( nds_bus( &r ) && !( r.eax & 1 ) ) {
            r.ecx = r.eax | 1;
            r.eax = (u32)BUS_PCIWRITE << 8;
            r.ebx = addr;
            r.edx = 0x0204;
            nds_bus( &r );
        }

        mem_zero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PCIIRQ << 8;
        r.ebx = addr;
        irq = NIC_NOIRQ;
        if ( nds_bus( &r ) && ( r.eax & 0xFF ) != 0 && ( r.eax & 0xFF ) < 16 ) irq = (u8)r.eax;

        pciaddr = addr;
        bus = NICB_PCI;
        str_cpy( card_name, pci_cards[i].name );
        str_cat( card_name, " (PCI)" );
        return 1;
    }
    return 0;
}

/* ISA Plug and Play: an RTL8019AS, or any card whose logical device
   says it is an NE2000, switched on at a port and a line of this
   driver's choosing through the kernel's services - a Plug and Play
   card answers at no port at all until something does.  A device a
   Plug and Play BIOS has switched on already is taken where it is.
   1 with port, irq (or NIC_NOIRQ, to be found) and card_name set,
   and the card probed */
static const u8 pnp_rtl8019[4] = { 0x4A, 0x8C, 0x80, 0x19 };   /* RTL8019 */
static const u8 pnp_ne2000[4]  = { 0x41, 0xD0, 0x80, 0xD6 };   /* PNP80D6 */
static const u8 pnp_irq_pref[] = { 10, 11, 5, 9, 3, 15, 12, 4, 0 };

static int mem_same( const void *a, const void *b, u32 len );

static int pnp_id_ours( const u8 *id )
{
    return mem_same( id, pnp_rtl8019, 4 ) || mem_same( id, pnp_ne2000, 4 );
}

static int find_pnp( void )
{
    REGS r;
    u8  *res, *regs, pairs[12], tag;
    u32  cards, csn, len, pos, size, kind;
    u32  io_min, io_max, io_align, irq_mask;
    int  ours, is_rtl, devices, i, line;
    u16  choice;

    mem_zero( &r, sizeof( r ) );
    r.eax = (u32)BUS_PNPHERE << 8;
    if ( !nds_bus( &r ) || r.eax == 0 ) return 0;
    cards = r.eax;
    res = nds_alloc( 2048 + 256 );
    if ( res == 0 ) return 0;
    regs = res + 2048;

    for ( csn = 1; csn <= cards; csn++ ) {
        mem_zero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PNPRES << 8;
        r.edx = csn;
        r.edi = (u32)res;
        r.ecx = 2048;
        if ( !nds_bus( &r ) || r.ecx < 9 ) continue;
        len = r.ecx;

        /* the card's own ID, then its first logical device's and what
           that is compatible with; the device's first port range and
           first line mask, which are the first set of choices when it
           has sets */
        is_rtl = mem_same( res, pnp_rtl8019, 4 );
        ours = is_rtl || mem_same( res, pnp_ne2000, 4 );
        devices = 0;
        io_min = io_max = io_align = irq_mask = 0;
        pos = 9;
        while ( pos < len ) {
            tag = res[pos++];
            if ( tag & 0x80 ) {                 /* a large item: skipped */
                if ( pos + 2 > len ) break;
                size = res[pos] | ( (u32)res[pos + 1] << 8 );
                pos += 2 + size;
                continue;
            }
            kind = ( tag >> 3 ) & 15;
            size = tag & 7;
            if ( pos + size > len || kind == 15 ) break;
            switch ( kind ) {
            case 2:                             /* a logical device */
                if ( devices++ ) break;
                if ( size >= 4 && pnp_id_ours( res + pos ) ) ours = 1;
                break;
            case 3:                             /* ...compatible with */
                if ( devices == 1 && size >= 4 && pnp_id_ours( res + pos ) ) ours = 1;
                break;
            case 4:                             /* a line */
                if ( devices == 1 && size >= 2 && irq_mask == 0 ) irq_mask = res[pos] | ( (u32)res[pos + 1] << 8 );
                break;
            case 8:                             /* a port range */
                if ( devices == 1 && size >= 7 && io_align == 0 ) {
                    io_min = res[pos + 1] | ( (u32)res[pos + 2] << 8 );
                    io_max = res[pos + 3] | ( (u32)res[pos + 4] << 8 );
                    io_align = res[pos + 5] ? res[pos + 5] : 1;
                }
                break;
            case 9:                             /* a port range of one */
                if ( devices == 1 && size >= 3 && io_align == 0 ) {
                    io_min = io_max = res[pos] | ( (u32)res[pos + 1] << 8 );
                    io_align = 1;
                }
                break;
            }
            if ( devices > 1 ) break;
            pos += size;
        }
        if ( !ours ) continue;

        /* on already, where a BIOS put it? */
        mem_zero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PNPREGS << 8;
        r.edx = csn;                            /* DH = device 0 */
        r.edi = (u32)regs;
        if ( nds_bus( &r ) && ( regs[0x30] & 1 ) && ( regs[0x60] | regs[0x61] ) ) {
            choice = (u16)( ( (u16)regs[0x60] << 8 ) | regs[0x61] );
            line = regs[0x70] & 15;
        } else {
            if ( io_align == 0 ) continue;
            /* 300h when the range allows it, else the range's first;
               the first line the card allows that nothing here has */
            choice = (u16)io_min;
            if ( io_min <= 0x300 && 0x300 <= io_max && ( ( 0x300 - io_min ) % io_align ) == 0 ) choice = 0x300;
            line = 0;
            for ( i = 0; pnp_irq_pref[i]; i++ ) {
                if ( !( irq_mask & ( 1L << pnp_irq_pref[i] ) ) ) continue;
                mem_zero( &r, sizeof( r ) );
                r.eax = (u32)BUS_IRQSTAT << 8;
                r.edx = pnp_irq_pref[i];
                if ( nds_bus( &r ) && ( r.eax & ( IRQF_KERNEL | IRQF_HOOKED | IRQF_OPEN | IRQF_DEAD ) ) ) continue;
                line = pnp_irq_pref[i];
                break;
            }
            /* off, the port and the line, on: a device is moved with
               its decoding off, as the specification says */
            pairs[0] = 0x30;  pairs[1] = 0;
            pairs[2] = 0x60;  pairs[3] = (u8)( choice >> 8 );
            pairs[4] = 0x61;  pairs[5] = (u8)choice;
            pairs[6] = 0x70;  pairs[7] = (u8)line;
            pairs[8] = 0x71;  pairs[9] = 2;
            pairs[10] = 0x30; pairs[11] = 1;
            mem_zero( &r, sizeof( r ) );
            r.eax = (u32)BUS_PNPSET << 8;
            r.edx = csn;
            r.esi = (u32)pairs;
            r.ecx = 6;
            if ( !nds_bus( &r ) ) continue;
        }
        port = choice;
        if ( !card_probe() ) continue;
        irq = line ? (u8)line : NIC_NOIRQ;
        bus = NICB_ISA;
        str_cpy( card_name, is_rtl ? "RTL8019AS (ISA PnP)" : "NE2000 (ISA PnP)" );
        nds_free( res );
        return 1;
    }
    nds_free( res );
    return 0;
}

/* the ISA ports: /P:'s, or the usual ones in turn */
static int find_isa( void )
{
    int i;

    if ( opt_port ) {
        port = (u16)opt_port;
        if ( !card_probe() ) return 0;
    } else {
        for ( i = 0; isa_ports[i]; i++ ) {
            port = isa_ports[i];
            if ( card_probe() ) break;
        }
        if ( !isa_ports[i] ) return 0;
    }
    bus = NICB_ISA;
    str_cpy( card_name, word ? "NE2000 (ISA)" : "NE1000 (ISA)" );
    return 1;
}

/* the line, by making the card interrupt with a handler on every
   line it could be on: the handler that is called knows which.
   Interrupts on; INIT is the one entry that may wait */
static void find_irq( void )
{
    u8  hooked[16];
    u8  dummy[2];
    u32 t0;
    int i;

    mem_zero( hooked, sizeof( hooked ) );
    probing = 1;
    probe_line = NIC_NOIRQ;
    for ( i = 0; probe_lines[i]; i++ ) {
        if ( nds_irqhook( probe_lines[i] ) ) hooked[probe_lines[i]] = 1;
    }
    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
    outb( port + NE_ISR, 0xFF );
    outb( port + NE_IMR, ISR_RDC );
    /* a remote read of two bytes, whose end is the interrupt: not
       remote_read, which would clear the bit the handler looks for */
    outb( port + NE_RBCR0, 2 );
    outb( port + NE_RBCR1, 0 );
    outb( port + NE_RSAR0, 0 );
    outb( port + NE_RSAR1, 0 );
    outb( port + NE_CR, CR_PAGE0 | CR_RD0 | CR_STA );
    if ( word ) {
        insw( port + NE_DATA, dummy, 1 );
    } else {
        insb( port + NE_DATA, dummy, 2 );
    }
    t0 = nds_ticks();
    while ( probe_line == NIC_NOIRQ && nds_ticks() - t0 < PROBE_WAIT ) ;
    outb( port + NE_IMR, 0 );
    outb( port + NE_ISR, 0xFF );
    probing = 0;
    for ( i = 0; probe_lines[i]; i++ ) {
        if ( hooked[probe_lines[i]] && probe_lines[i] != probe_line ) nds_irqunhook( probe_lines[i] );
    }
    irq = probe_line;
}

/* ---------------------------------------------------------------- */
/* the DEVICE= line: /P:port (hex) and /I:line                       */
/* ---------------------------------------------------------------- */

static u32 num_after( const char *text, int hex, u32 none )
{
    u32 value = 0;
    int digits = 0, digit;
    char ch;

    for ( ;; text++ ) {
        ch = *text;
        if ( ch >= '0' && ch <= '9' ) {
            digit = ch - '0';
        } else if ( hex && ( ch | 0x20 ) >= 'a' && ( ch | 0x20 ) <= 'f' ) {
            digit = ( ch | 0x20 ) - 'a' + 10;
        } else {
            break;
        }
        value = value * ( hex ? 16 : 10 ) + digit;
        digits++;
    }
    /* 300h and 0x300 are both 300h */
    return digits ? value : none;
}

static void parse_line( const char *line )
{
    const char *p = line;

    /* past the path */
    while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' ) p++;
    for ( ; *p && *p != '\r' && *p != '\n'; p++ ) {
        if ( *p != '/' ) continue;
        if ( ( p[1] | 0x20 ) == 'p' && p[2] == ':' ) {
            if ( p[3] == '0' && ( p[4] | 0x20 ) == 'x' ) p += 2;
            opt_port = num_after( p + 3, 1, 0 );
        } else if ( ( p[1] | 0x20 ) == 'i' && p[2] == ':' ) {
            opt_irq = num_after( p + 3, 0, NIC_NOIRQ );
            if ( opt_irq > 15 ) opt_irq = NIC_NOIRQ;
        }
    }
}

/* ---------------------------------------------------------------- */
/* the entries                                                       */
/* ---------------------------------------------------------------- */

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'N', 'E', '2', '0', '0', '0', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};

static void say_card( const char *lead )
{
    char text[128];
    int  i;

    str_cpy( text, lead );
    str_cat( text, card_name );
    str_cat( text, " at " );
    str_hex( text, port, 3 );
    str_cat( text, "h, " );
    if ( irq == NIC_NOIRQ ) {
        str_cat( text, "no interrupt line, polled" );
    } else {
        str_cat( text, "IRQ " );
        str_dec( text, irq );
    }
    str_cat( text, ", address " );
    for ( i = 0; i < 6; i++ ) {
        if ( i ) str_cat( text, "-" );
        str_hex( text, mac[i], 2 );
    }
    str_cat( text, "\r\n" );
    nds_print( text );
}

int __cdecl nd_init( const char *line )
{
    parse_line( line );

    /* the card */
    if ( opt_port ) {
        if ( !find_isa() ) {
            nds_print( "NE2000.SYS: no NE2000 at the port given\r\n" );
            return 1;
        }
    } else if ( find_pci() ) {
        /* the PROM, for the address and for the chip being there */
        if ( !card_reset() || !card_prom( 1 ) ) {
            nds_print( "NE2000.SYS: the PCI card did not answer as an NE2000\r\n" );
            return 1;
        }
    } else if ( find_pnp() ) {
        /* probed and named on the way */
    } else if ( !find_isa() ) {
        nds_print( "NE2000.SYS: no NE2000 found\r\n" );
        return 1;
    }
    if ( word ) {
        tx_page = 0x40;
        rx_start = 0x40 + TX_PAGES;
        rx_stop = 0x80;
    } else {
        tx_page = 0x20;
        rx_start = 0x20 + TX_PAGES;
        rx_stop = 0x40;
    }

    /* the queue */
    queue = nds_alloc( ( QN + 1 ) * QSLOT );
    if ( queue == 0 ) {
        nds_print( "NE2000.SYS: no memory for the receive queue\r\n" );
        return 1;
    }

    /* the line: /I:, the PCI function's own, or found */
    if ( opt_irq != NIC_NOIRQ ) irq = (u8)opt_irq;
    if ( irq != NIC_NOIRQ ) {
        if ( !nds_irqhook( irq ) ) irq = NIC_NOIRQ;
    } else if ( bus == NICB_ISA ) {
        find_irq();
    }
    polled = (u8)( irq == NIC_NOIRQ );
    /* the clock: the whole service when polled, a look at the card
       otherwise, for an interrupt that was lost */
    nds_tickhook();

    card_setup();
    say_card( "NE2000.SYS: " );
    return 0;
}

void __cdecl nd_park( int back )
{
    u32 flags;

    flags = int_off();
    if ( !back ) {
        parked = 1;
        if ( running ) card_stop();
        running = 0;
    } else {
        parked = 0;
        card_reset();
        card_prom( word );
        card_setup();
    }
    int_back( flags );
}

void __cdecl nd_halt( void )
{
    if ( running ) card_stop();
    running = 0;
}

int __cdecl nd_irq( u32 line )
{
    u8 isr;

    if ( probing ) {
        outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
        isr = inb( port + NE_ISR );
        if ( !( isr & ISR_RDC ) ) return 0;
        outb( port + NE_IMR, 0 );
        outb( port + NE_ISR, 0xFF );
        probe_line = (u8)line;
        return 1;
    }
    if ( !running ) return 0;
    if ( card_service() ) {
        stats.irqs++;
        return 1;
    }
    return 0;
}

/* the clock: the whole service when polled.  With a line, only a
   card that has been asking since the last tick as well is served
   here - an interrupt that was lost, not one that is on its way,
   which a service from here would turn into a stray at the handler */
void __cdecl nd_tick( void )
{
    u8 isr;

    if ( !running || probing ) return;
    if ( polled ) {
        card_service();
        return;
    }
    outb( port + NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
    isr = inb( port + NE_ISR );
    if ( !( isr & ISR_ALL ) ) {
        tick_seen = 0;
        return;
    }
    if ( tick_seen ) {
        card_service();
        tick_seen = 0;
    } else {
        tick_seen = 1;
    }
}

int __cdecl nd_call( u32 fn, void *buf, u32 len, u32 *answer )
{
    NIC_INFOBLOCK *info;
    NIC_STATBLOCK *st;
    u32 flags, got;
    u8 *slot;

    switch ( fn ) {
    case NIC_INFO:
        if ( len < NICI_LEN ) return ERR_BADLEN;
        info = (NIC_INFOBLOCK *)buf;
        mem_zero( info, NICI_LEN );
        mem_copy( info->mac, mac, 6 );
        info->kind = NICK_ETHERNET;
        info->bus = bus;
        info->mtu = 1500;
        info->port = port;
        info->irq = irq;
        info->flags = (u8)( ( polled ? NICF_POLLED : 0 ) | ( word ? NICF_WORD : 0 ) );
        info->pciaddr = pciaddr;
        str_cpy( info->name, card_name );
        *answer = NICI_LEN;
        return 0;

    case NIC_SEND:
        if ( len < NIC_MINFRAME || len > NIC_MAXFRAME ) return ERR_BADLEN;
        if ( !running ) return ERR_GENERAL;
        if ( !card_send( (const u8 *)buf, len ) ) return ERR_GENERAL;
        *answer = len;
        return 0;

    case NIC_RECV:
        /* the head slot is nobody else's while it is counted: the
           handler writes the tail or the spare.  So it is copied with
           interrupts on and let go with them off */
        if ( q_count == 0 ) {
            *answer = 0;
            return 0;
        }
        slot = q_slot( q_head );
        got = *(u16 *)slot;
        mem_copy( buf, slot + 2, got < len ? got : len );
        flags = int_off();
        q_head = ( q_head + 1 ) % QN;
        q_count--;
        int_back( flags );
        *answer = got;
        return 0;

    case NIC_STATS:
        if ( len < NICS_LEN ) return ERR_BADLEN;
        st = (NIC_STATBLOCK *)buf;
        flags = int_off();
        mem_copy( st, &stats, NICS_LEN );
        st->queued = q_count;
        int_back( flags );
        *answer = NICS_LEN;
        return 0;
    }
    return ERR_FUNC;
}

/* ---------------------------------------------------------------- */
/* what a library would have supplied                                */
/* ---------------------------------------------------------------- */

static void str_cpy( char *to, const char *from )
{
    while ( ( *to++ = *from++ ) != 0 ) ;
}

static void str_cat( char *to, const char *from )
{
    while ( *to ) to++;
    str_cpy( to, from );
}

static void str_hex( char *to, u32 value, int digits )
{
    int shift;

    while ( *to ) to++;
    for ( shift = ( digits - 1 ) * 4; shift >= 0; shift -= 4 ) {
        *to++ = "0123456789ABCDEF"[( value >> shift ) & 15];
    }
    *to = 0;
}

static void str_dec( char *to, u32 value )
{
    char digits[12];
    int  n = 0;

    while ( *to ) to++;
    do {
        digits[n++] = (char)( '0' + value % 10 );
        value /= 10;
    } while ( value );
    while ( n ) *to++ = digits[--n];
    *to = 0;
}

static void mem_copy( void *to, const void *from, u32 len )
{
    u8 *t = (u8 *)to;
    const u8 *f = (const u8 *)from;

    while ( len-- ) *t++ = *f++;
}

static void mem_zero( void *to, u32 len )
{
    u8 *t = (u8 *)to;

    while ( len-- ) *t++ = 0;
}

static int mem_same( const void *a, const void *b, u32 len )
{
    const u8 *p = (const u8 *)a;
    const u8 *q = (const u8 *)b;

    while ( len-- ) {
        if ( *p++ != *q++ ) return 0;
    }
    return 1;
}
