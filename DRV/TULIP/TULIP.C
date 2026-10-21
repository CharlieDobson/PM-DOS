/*
 * TULIP.C - TULIP.SYS, a driver for the DEC 21x4x "Tulip"
 *
 * Digital's 21040/21140/21143 and the many cards and clones built on
 * them (QEMU's tulip is a 21143; 86Box has the 21040, 21140 and
 * 21143).  A PCI bus-master with two rings of DESCRIPTORS, like the
 * PCnet, but DEC's own register and descriptor layout, and a station
 * address that is not in a port but has to be clocked out of a serial
 * EEPROM a bit at a time.
 *
 * THE RINGS are not chained: each is a run of descriptors in memory,
 * the last marked "end of ring", and the chip wraps to the first.  A
 * descriptor's first dword is its status, with the OWN bit saying
 * whether the chip or the driver has it; the chip walks the ring and
 * stops at a descriptor the driver still owns.
 *
 * THE STATION ADDRESS comes from a 93C46 serial EEPROM read through
 * CSR9, clocked out by hand.  DEC wrote the address into the EEPROM in
 * two different places over the years; this reads both and keeps
 * whichever is a real address.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C); DMA is the driver's own
 * pointers (the identity map).  CSRs are 32-bit and eight bytes apart.
 */
#include "netdrv.h"

#define CSR0   0x00     /* bus mode */
#define CSR1   0x08     /* transmit poll demand */
#define CSR2   0x10     /* receive poll demand */
#define CSR3   0x18     /* receive descriptor list address */
#define CSR4   0x20     /* transmit descriptor list address */
#define CSR5   0x28     /* status */
#define CSR6   0x30     /* operating mode */
#define CSR7   0x38     /* interrupt enable */
#define CSR9   0x48     /* the serial EEPROM and MII */
#define CSR13  0x68     /* SIA connectivity */
#define CSR14  0x70     /* SIA transmit/receive */
#define CSR15  0x78     /* SIA and general purpose */

/* CSR5 status */
#define S_TI    0x00000001L     /* a frame went */
#define S_RI    0x00000040L     /* a frame arrived */
#define S_RU    0x00000080L     /* out of receive descriptors */
#define S_NIS   0x00010000L     /* a normal interrupt is summarised */
#define S_AIS   0x00008000L     /* an abnormal one */
#define S_CLEAR 0x0001FFFFL     /* the write-one-to-clear bits */

/* CSR6 operating mode */
#define M_RXON  0x00000002L
#define M_TXON  0x00002000L
#define M_SF    0x00200000L     /* store and forward */
#define M_BCAST 0x00000100L     /* keep broadcasts (pass-bad off) */
#define M_PAM   0x00000080L     /* pass all multicast */

/* a descriptor's status dword (DES0) */
#define D_OWN   0x80000000L     /* the chip owns it */
#define RD_ES   0x00008000L     /* receive error summary */
#define RD_FS   0x00000200L     /* first descriptor of the frame */
#define RD_LS   0x00000100L     /* last descriptor of the frame */

/* a descriptor's control/length dword (DES1) */
#define TD_IC   0x80000000L     /* interrupt on completion */
#define TD_LS   0x40000000L     /* last segment */
#define TD_FS   0x20000000L     /* first segment */
#define D_TER   0x02000000L     /* end of ring */

/* CSR9 EEPROM bits */
#define EE_SR   0x00000800L     /* select the serial ROM */
#define EE_RD   0x00004000L     /* a read */
#define EE_CS   0x00000001L     /* chip select */
#define EE_CLK  0x00000002L     /* the clock */
#define EE_DO   0x00000004L     /* data to the ROM */
#define EE_DI   0x00000008L     /* data from it */
#define EE_READ_CMD  6          /* 93C46 read opcode, three bits 110 */

#define RXN     16
#define TXN     4
#define BUFSZ   1536

#define SPIN_RESET  1000000L
#define SPIN_TX     400000L

#define TD_SETUP 0x08000000L    /* this transmit is a setup frame */

static volatile u32 *rxr, *txr;
static u8  *rxbuf[RXN];
static u8  *txbuf[TXN];
static u8  *setupbuf;           /* the 192-byte perfect-filter frame */
static int  rx_cur, tx_cur;

static const PCI_ID tulip_ids[] = {
    { 0x1011, 0x0002 },         /* DEC 21040 */
    { 0x1011, 0x0009 },         /* DEC 21140 */
    { 0x1011, 0x0019 },         /* DEC 21143 */
    { 0x1011, 0x0014 },         /* 21041 */
    { 0x1259, 0xa120 },         /* Accton clone */
    { 0x11AD, 0xc115 },         /* Lite-On PNIC clone */
    { 0, 0 }
};

static u32 align16( u32 p ) { return ( p + 15 ) & ~15L; }
static void eedelay( void ) { inl( net_port + CSR9 ); inl( net_port + CSR9 ); }

/* a word of the 93C46 at a word address (six address bits) */
static u16 srom_word( int addr )
{
    u32 base = EE_SR | EE_RD;
    u32 cmd = ( (u32)EE_READ_CMD << 6 ) | ( addr & 0x3F );  /* 110 aaaaaa, 9 bits */
    u16 val = 0;
    int i;

    outl( net_port + CSR9, base );
    outl( net_port + CSR9, base | EE_CS );
    for ( i = 8; i >= 0; i-- ) {
        u32 d = base | EE_CS | ( ( cmd >> i ) & 1 ? EE_DO : 0 );
        outl( net_port + CSR9, d );
        eedelay();
        outl( net_port + CSR9, d | EE_CLK );
        eedelay();
        outl( net_port + CSR9, d );
        eedelay();
    }
    for ( i = 0; i < 16; i++ ) {
        outl( net_port + CSR9, base | EE_CS | EE_CLK );
        eedelay();
        val = (u16)( ( val << 1 ) | ( ( inl( net_port + CSR9 ) & EE_DI ) ? 1 : 0 ) );
        outl( net_port + CSR9, base | EE_CS );
        eedelay();
    }
    outl( net_port + CSR9, base );
    return val;
}

/* the MAC lives at EEPROM byte 20 in DEC's later format and byte 0 in
   the oldest; read enough words to cover both and keep the real one */
static int read_mac( void )
{
    u8 rom[32];
    int i;

    for ( i = 0; i < 16; i++ ) {
        u16 w = srom_word( i );
        rom[i * 2] = (u8)w;
        rom[i * 2 + 1] = (u8)( w >> 8 );
    }
    net_memcpy( net_mac, rom + 20, 6 );
    if ( net_mac_ok( net_mac ) ) return 1;
    net_memcpy( net_mac, rom, 6 );
    return net_mac_ok( net_mac );
}

int card_find( void )
{
    const PCI_ID *which;
    int isio, i;
    u32 at, need;
    u8 *pool;

    if ( !net_pci_find( tulip_ids, &which ) ) return 1;
    net_pci_enable( net_pciaddr );
    net_port = (u16)net_pci_bar( net_pciaddr, 0, &isio );   /* BAR0 is I/O */
    if ( net_port == 0 || !isio ) return 1;
    net_irq = (u8)net_pci_irqline( net_pciaddr );
    net_bus = NICB_PCI;
    net_flags = NICF_WORD;

    outl( net_port + CSR0, 0x00000001L );       /* software reset */
    for ( i = 0; i < 1000; i++ ) eedelay();
    outl( net_port + CSR0, 0x00000000L );

    if ( !read_mac() ) return 1;

    if ( rxr == 0 ) {
        need = RXN * 16 + TXN * 16 + ( RXN + TXN ) * BUFSZ + 192 + 16;
        pool = net_alloc( need );
        if ( pool == 0 ) return 1;
        at = align16( (u32)pool );
        rxr = (volatile u32 *)at;   at += RXN * 16;
        txr = (volatile u32 *)at;   at += TXN * 16;
        setupbuf = (u8 *)at;        at += 192;
        for ( i = 0; i < RXN; i++ ) { rxbuf[i] = (u8 *)at; at += BUFSZ; }
        for ( i = 0; i < TXN; i++ ) { txbuf[i] = (u8 *)at; at += BUFSZ; }
    }
    net_strcpy( net_name, "DEC 21x4x Tulip" );
    return 0;
}

int card_setup( void )
{
    u32 n;
    int i;

    outl( net_port + CSR0, 0x00000001L );
    for ( n = 0; n < 1000; n++ ) eedelay();
    outl( net_port + CSR0, 0x00000000L );

    for ( i = 0; i < RXN; i++ ) {
        rxr[i * 4 + 0] = D_OWN;
        rxr[i * 4 + 1] = ( i == RXN - 1 ? D_TER : 0 ) | BUFSZ;   /* buffer 1 size */
        rxr[i * 4 + 2] = (u32)rxbuf[i];
        rxr[i * 4 + 3] = 0;
    }
    for ( i = 0; i < TXN; i++ ) {
        txr[i * 4 + 0] = 0;                      /* the driver owns it */
        txr[i * 4 + 1] = ( i == TXN - 1 ? D_TER : 0 );
        txr[i * 4 + 2] = (u32)txbuf[i];
        txr[i * 4 + 3] = 0;
    }
    rx_cur = tx_cur = 0;

    outl( net_port + CSR3, (u32)rxr );
    outl( net_port + CSR4, (u32)txr );
    /* the SIA, for the 21040/21143's 10baseT port: reset it, then the
       usual 10baseT values.  The 21140 and QEMU's 21143 ignore what
       they do not use */
    outl( net_port + CSR13, 0x00000000L );
    outl( net_port + CSR14, 0x00007F3FL );
    outl( net_port + CSR13, 0x00000001L );
    /* M_PAM passes all multicast: NetBEUI's functional address
       03-00-00-00-00-01 is a multicast, not only broadcast */
    outl( net_port + CSR6, M_SF | M_BCAST | M_PAM | M_RXON | M_TXON );
    outl( net_port + CSR5, S_CLEAR );
    outl( net_port + CSR7, (u32)( net_flags & NICF_POLLED ? 0 : ( S_NIS | S_AIS | S_RI | S_RU | S_TI ) ) );
    outl( net_port + CSR2, 0 );                  /* poll the receive ring */

    /* tell the chip its address: a setup frame of 16 perfect-filter
       slots.  Without it the chip takes broadcasts but drops unicast
       to our own address.  Slot 0 is us, slot 1 is broadcast, the rest
       are us again.  Each slot is three longwords, two address bytes
       in the low word of each */
    {
        u32 *sf = (u32 *)setupbuf;
        u32 me0 = (u32)( net_mac[0] | ( net_mac[1] << 8 ) );
        u32 me1 = (u32)( net_mac[2] | ( net_mac[3] << 8 ) );
        u32 me2 = (u32)( net_mac[4] | ( net_mac[5] << 8 ) );
        for ( i = 0; i < 16; i++ ) {
            if ( i == 1 ) { sf[i * 3] = 0xFFFF; sf[i * 3 + 1] = 0xFFFF; sf[i * 3 + 2] = 0xFFFF; }
            else { sf[i * 3] = me0; sf[i * 3 + 1] = me1; sf[i * 3 + 2] = me2; }
        }
        txr[0 * 4 + 0] = D_OWN;
        txr[0 * 4 + 1] = ( txr[0 * 4 + 1] & D_TER ) | TD_SETUP | 192;
        txr[0 * 4 + 2] = (u32)setupbuf;
        outl( net_port + CSR1, 1 );              /* poll the transmit ring */
        for ( n = 0; n < SPIN_TX; n++ ) {
            if ( !( txr[0 * 4 + 0] & D_OWN ) ) break;
        }
        /* leave descriptor 0 free and the ring ready for real frames */
        txr[0 * 4 + 0] = 0;
        txr[0 * 4 + 1] = ( txr[0 * 4 + 1] & D_TER );
        tx_cur = 0;
    }
    return 0;
}

void card_stop( void )
{
    outl( net_port + CSR7, 0 );
    outl( net_port + CSR6, 0 );                  /* receive and transmit off */
    outl( net_port + CSR5, S_CLEAR );
}

int card_send( const u8 *frame, u32 len )
{
    static u8 pad[64];
    int entry = tx_cur;
    volatile u32 *d = txr + entry * 4;
    u32 n;

    if ( d[0] & D_OWN ) {
        for ( n = 0; n < SPIN_TX; n++ ) {
            if ( !( d[0] & D_OWN ) ) break;
        }
        if ( n == SPIN_TX ) { net_stats.txerrors++; return 0; }
    }
    if ( len < 60 ) {
        net_memzero( pad, sizeof( pad ) );
        net_memcpy( pad, frame, len );
        net_memcpy( txbuf[entry], pad, 60 );
        len = 60;
    } else {
        net_memcpy( txbuf[entry], frame, len );
    }
    d[2] = (u32)txbuf[entry];
    d[1] = ( d[1] & D_TER ) | TD_FS | TD_LS | len;   /* one descriptor, whole frame */
    d[0] = D_OWN;
    tx_cur = ( entry + 1 ) % TXN;
    outl( net_port + CSR1, 1 );                  /* poll the transmit ring */
    return 1;
}

static void rx_drain( void )
{
    volatile u32 *d;
    u32 status, len, n;
    u8 *slot;

    for ( n = 0; n < RXN * 2; n++ ) {
        d = rxr + rx_cur * 4;
        status = d[0];
        if ( status & D_OWN ) break;             /* the chip still has it */
        if ( ( status & RD_ES ) || ( status & ( RD_FS | RD_LS ) ) != ( RD_FS | RD_LS ) ) {
            net_stats.rxerrors++;
        } else {
            len = ( ( status >> 16 ) & 0x3FFF );  /* the frame and its CRC */
            if ( len >= NIC_HDRLEN + 4 && len <= NIC_MAXFRAME + 4 ) {
                slot = net_rx_begin();
                net_memcpy( slot, rxbuf[rx_cur], len - 4 );
                net_rx_commit( len - 4 );
            } else {
                net_stats.rxerrors++;
            }
        }
        d[0] = D_OWN;                            /* give it back */
        rx_cur = ( rx_cur + 1 ) % RXN;
    }
}

int card_service( void )
{
    u32 csr;
    u32 n;
    int mine = 0;

    for ( n = 0; n < 8; n++ ) {
        csr = inl( net_port + CSR5 );
        if ( !( csr & ( S_RI | S_TI | S_RU | S_AIS ) ) ) break;
        mine = 1;
        outl( net_port + CSR5, csr & S_CLEAR );  /* dismiss */
        if ( csr & ( S_RI | S_RU ) ) rx_drain();
        outl( net_port + CSR2, 0 );              /* keep the receive unit polling */
    }
    return mine;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'T', 'u', 'l', 'i', 'p', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
