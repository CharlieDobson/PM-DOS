/*
 * NETCORE.C - the shared body of a PM-DOS network card driver
 *
 * The four entries the kernel calls (nd_init, nd_park, nd_halt,
 * nd_call, through NDSTART.ASM's ndv_ veneers), the receive queue the
 * NIC_RECV function reads and a card's interrupt handler fills, the
 * counts, the DEVICE= line, and the PCI bus - everything a card driver
 * shares.  A card's own source (NE2000 is the exception: it was written
 * before this and stands alone) provides card_find, card_setup,
 * card_stop, card_send and card_service, and its own ND_TABLE.
 *
 * See DRV\NETDRV.H for the whole interface.
 */
#include "netdrv.h"

/* the globals a card fills */
u16  net_port;
u32  net_mem;
u8   net_irq = NIC_NOIRQ;
u8   net_bus;
u8   net_flags;
u32  net_pciaddr;
u8   net_mac[6];
char net_name[32];

u32  net_opt_port;
u32  net_opt_mem;
u8   net_opt_irq = NIC_NOIRQ;
u8   net_opt_poll;              /* /POLL: run the card on the clock, no IRQ */

NIC_STATBLOCK net_stats;

volatile u8 net_probing;
volatile u8 net_probe_hit = NIC_NOIRQ;

/* the receive queue: QN frames and a spare the newest goes in when the
   queue is full, so that a card's ISR always has somewhere to write */
#define QN      16
#define QSLOT   1536            /* a length word and the longest frame */

static u8  *queue;
static u32  q_head, q_tail;
static volatile u32 q_count;

static u8   running;
static u8   parked;
static u8   polled;
static u8   tick_seen;          /* the card was asking last tick too */
static u8   bridge_woke;        /* the WfW bridge woke the card (polled) */
static u8   nat_irq;            /* the native line, kept across a bridge wake */
static u8   nat_flags;          /* the native NICF_*, the same */
static u8   probe_hooked[16];

/* ---------------------------------------------------------------- */
/* the services, wrapped                                             */
/* ---------------------------------------------------------------- */

void *net_alloc( u32 bytes )  { return nds_alloc( bytes ); }
void  net_free( void *block ) { nds_free( block ); }
void  net_print( const char *text ) { nds_print( text ); }
u32   net_ticks( void )       { return nds_ticks(); }

/* ---------------------------------------------------------------- */
/* what a library would have supplied                                */
/* ---------------------------------------------------------------- */

void net_memcpy( void *to, const void *from, u32 len )
{
    u8 *t = (u8 *)to;
    const u8 *f = (const u8 *)from;
    while ( len-- ) *t++ = *f++;
}

void net_memzero( void *to, u32 len )
{
    u8 *t = (u8 *)to;
    while ( len-- ) *t++ = 0;
}

void net_strcpy( char *to, const char *from )
{
    while ( ( *to++ = *from++ ) != 0 ) ;
}

int net_mac_ok( const u8 *mac )
{
    int i;
    for ( i = 0; i < 6 && mac[i] == 0; i++ ) ;
    if ( i == 6 ) return 0;                     /* nobody's */
    for ( i = 0; i < 6 && mac[i] == 0xFF; i++ ) ;
    if ( i == 6 ) return 0;                     /* everybody's */
    return !( mac[0] & 1 );                     /* not a group address */
}

/* ---------------------------------------------------------------- */
/* the receive queue                                                 */
/* ---------------------------------------------------------------- */

u8 *net_rx_begin( void )
{
    return queue + ( q_count == QN ? QN : q_tail ) * QSLOT + 2;
}

void net_rx_commit( u32 len )
{
    u8 *slot;

    if ( q_count == QN ) {
        net_stats.rxdropped++;
        return;
    }
    slot = queue + q_tail * QSLOT;
    *(u16 *)slot = (u16)len;
    q_tail = ( q_tail + 1 ) % QN;
    q_count++;
    net_stats.rxframes++;
    net_stats.rxbytes += len;
}

/* ---------------------------------------------------------------- */
/* the PCI bus                                                       */
/* ---------------------------------------------------------------- */

static int bus_call( u32 func, REGS *r )
{
    r->eax = ( r->eax & 0xFFFF ) | ( func << 8 );
    return nds_bus( r );
}

u32 net_pci_cfg_read( u32 addr, int reg, int bytes )
{
    REGS r;

    net_memzero( &r, sizeof( r ) );
    r.ebx = addr;
    r.edx = (u32)reg | ( (u32)bytes << 8 );
    if ( !bus_call( BUS_PCIREAD, &r ) ) return 0xFFFFFFFFL;
    return r.eax;
}

void net_pci_cfg_write( u32 addr, int reg, int bytes, u32 value )
{
    REGS r;

    net_memzero( &r, sizeof( r ) );
    r.ebx = addr;
    r.edx = (u32)reg | ( (u32)bytes << 8 );
    r.ecx = value;
    bus_call( BUS_PCIWRITE, &r );
}

u32 net_pci_find( const PCI_ID *ids, const PCI_ID **which )
{
    REGS r;
    int  i;

    for ( i = 0; ids[i].vendor; i++ ) {
        net_memzero( &r, sizeof( r ) );
        r.ecx = ids[i].vendor;
        r.edx = ids[i].device;
        r.esi = 0;
        if ( !bus_call( BUS_PCIID, &r ) ) continue;
        net_pciaddr = r.ebx;
        if ( which ) *which = &ids[i];
        return r.ebx;
    }
    return 0;
}

/* a base address register: its base, and whether it is in I/O space */
u32 net_pci_bar( u32 addr, int bar, int *is_io )
{
    REGS r;

    net_memzero( &r, sizeof( r ) );
    r.ebx = addr;
    r.edx = bar;
    if ( !bus_call( BUS_PCIBAR, &r ) ) { if ( is_io ) *is_io = 0; return 0; }
    if ( is_io ) *is_io = ( r.edx & BARF_IO ) ? 1 : 0;
    return r.eax;
}

/* device memory made reachable: the identity map means the linear
   address is the physical one, but BUS_MAPPHYS is asked anyway so a
   region above what the boot-time map covers is mapped in */
u32 net_pci_map( u32 phys, u32 bytes )
{
    REGS r;

    net_memzero( &r, sizeof( r ) );
    r.ebx = phys;
    r.ecx = bytes;
    if ( !bus_call( 0x10, &r ) ) return 0;      /* BUS_MAPPHYS */
    return r.eax;
}

int net_pci_irqline( u32 addr )
{
    REGS r;

    net_memzero( &r, sizeof( r ) );
    r.ebx = addr;
    if ( !bus_call( BUS_PCIIRQ, &r ) ) return NIC_NOIRQ;
    if ( ( r.eax & 0xFF ) == 0xFF || ( r.eax & 0xFF ) >= 16 ) return NIC_NOIRQ;
    return (int)( r.eax & 0xFF );
}

/* the command register's I/O, memory and bus-master bits, switched on
   if the BIOS left any off */
void net_pci_enable( u32 addr )
{
    u32 cmd = net_pci_cfg_read( addr, 0x04, 2 );

    if ( ( cmd & 7 ) != 7 ) net_pci_cfg_write( addr, 0x04, 2, cmd | 7 );
}

/* ---------------------------------------------------------------- */
/* the interrupt line, found by making an ISA card interrupt          */
/* ---------------------------------------------------------------- */

static const u8 probe_lines[] = { 3, 4, 5, 9, 10, 11, 12, 15, 0 };

void net_irq_probe_begin( void )
{
    int i;

    net_memzero( probe_hooked, sizeof( probe_hooked ) );
    net_probe_hit = NIC_NOIRQ;
    net_probing = 1;
    for ( i = 0; probe_lines[i]; i++ ) {
        if ( nds_irqhook( probe_lines[i] ) ) probe_hooked[probe_lines[i]] = 1;
    }
}

void net_irq_probe_end( void )
{
    int i;

    net_probing = 0;
    for ( i = 0; probe_lines[i]; i++ ) {
        if ( probe_hooked[probe_lines[i]] && probe_lines[i] != net_probe_hit ) {
            nds_irqunhook( probe_lines[i] );
        }
    }
    net_irq = net_probe_hit;
}

/* ---------------------------------------------------------------- */
/* the DEVICE= line: /P:port (hex), /M:mem (hex), /I:line (decimal)   */
/* ---------------------------------------------------------------- */

static u32 num_after( const char *text, int hex, u32 none )
{
    u32 value = 0;
    int digits = 0, digit;
    char ch;

    for ( ;; text++ ) {
        ch = *text;
        if ( ch >= '0' && ch <= '9' ) digit = ch - '0';
        else if ( hex && ( ch | 0x20 ) >= 'a' && ( ch | 0x20 ) <= 'f' ) digit = ( ch | 0x20 ) - 'a' + 10;
        else break;
        if ( value > 0x0FFFFFFFL ) value = 0xFFFFFFFFL;     /* too long: stays too big */
        else value = value * ( hex ? 16 : 10 ) + digit;
        digits++;
    }
    return digits ? value : none;
}

static void parse_line( const char *line )
{
    const char *p = line;

    while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' ) p++;
    for ( ; *p && *p != '\r' && *p != '\n'; p++ ) {
        if ( *p != '/' ) continue;
        if ( ( p[1] | 0x20 ) == 'p' && p[2] == ':' ) {
            if ( p[3] == '0' && ( p[4] | 0x20 ) == 'x' ) p += 2;
            net_opt_port = num_after( p + 3, 1, 0 );
        } else if ( ( p[1] | 0x20 ) == 'm' && p[2] == ':' ) {
            if ( p[3] == '0' && ( p[4] | 0x20 ) == 'x' ) p += 2;
            net_opt_mem = num_after( p + 3, 1, 0 );
        } else if ( ( p[1] | 0x20 ) == 'i' && p[2] == ':' ) {
            u32 value = num_after( p + 3, 0, NIC_NOIRQ );
            net_opt_irq = (u8)( value > 15 ? NIC_NOIRQ : value );
        } else if ( ( p[1] | 0x20 ) == 'p' && ( p[2] | 0x20 ) == 'o' ) {
            net_opt_poll = 1;           /* /POLL - no interrupt, clock only */
        }
    }
}

/* ---------------------------------------------------------------- */
/* saying what was found                                             */
/* ---------------------------------------------------------------- */

static void add_str( char *to, const char *from )
{
    while ( *to ) to++;
    net_strcpy( to, from );
}

static void add_hex( char *to, u32 value, int digits )
{
    int shift;
    while ( *to ) to++;
    for ( shift = ( digits - 1 ) * 4; shift >= 0; shift -= 4 ) *to++ = "0123456789ABCDEF"[( value >> shift ) & 15];
    *to = 0;
}

static void add_dec( char *to, u32 value )
{
    char d[12];
    int  n = 0;
    while ( *to ) to++;
    do { d[n++] = (char)( '0' + value % 10 ); value /= 10; } while ( value );
    while ( n ) *to++ = d[--n];
    *to = 0;
}

static void say_found( void )
{
    char text[128];
    int  i;

    text[0] = 0;
    add_str( text, net_name );
    add_str( text, " at " );
    if ( net_port ) { add_hex( text, net_port, 3 ); add_str( text, "h" ); }
    else { add_str( text, "mem " ); add_hex( text, net_mem, 8 ); add_str( text, "h" ); }
    if ( net_irq == NIC_NOIRQ ) add_str( text, ", polled" );
    else { add_str( text, ", IRQ " ); add_dec( text, net_irq ); }
    add_str( text, ", " );
    for ( i = 0; i < 6; i++ ) { if ( i ) add_str( text, "-" ); add_hex( text, net_mac[i], 2 ); }
    add_str( text, "\r\n" );
    net_print( text );
}

/* ---------------------------------------------------------------- */
/* the entries NDSTART.ASM calls                                     */
/* ---------------------------------------------------------------- */

int __cdecl nd_init( const char *line )
{
    char msg[64];

    parse_line( line );
    if ( card_find() ) {
        msg[0] = 0;
        add_str( msg, net_name[0] ? net_name : "This card" );
        add_str( msg, ": not found\r\n" );
        net_print( msg );
        return 1;
    }
    queue = net_alloc( ( QN + 1 ) * QSLOT );
    if ( queue == 0 ) {
        net_print( "no memory for the receive queue\r\n" );
        return 1;
    }
    /* the line a card already knows (PCI, or an ISA card that found it
       in card_find) is hooked; one it does not, on ISA, is found by
       making the card interrupt, which is card_setup's to do while
       net_probing - a card that cannot is left polled */
    if ( net_opt_irq != NIC_NOIRQ ) net_irq = net_opt_irq;
    if ( net_opt_poll ) net_irq = NIC_NOIRQ;    /* /POLL: force the clock path */
    if ( net_irq != NIC_NOIRQ && !net_probing ) {
        if ( !nds_irqhook( net_irq ) ) net_irq = NIC_NOIRQ;
    }
    polled = (u8)( net_irq == NIC_NOIRQ );
    if ( polled ) net_flags |= NICF_POLLED;
    nds_tickhook();
    if ( card_setup() ) {
        net_print( "the card would not start\r\n" );
        /* the kernel frees this driver: let go of the line, the clock
           and the queue first */
        nds_tickunhook();
        if ( net_irq != NIC_NOIRQ ) nds_irqunhook( net_irq );
        nds_free( queue );
        queue = 0;
        return 1;
    }
    running = 1;
    say_found();
    return 0;
}

void __cdecl nd_park( int back )
{
    u32 flags = int_off();

    if ( !back ) {
        parked = 1;
        if ( running ) card_stop();
        running = 0;
    } else {
        parked = 0;
        if ( bridge_woke ) {
            /* undo a bridge wake: the native line and flags come back, so
               the kernel runs the card on its interrupt again, not polled */
            net_irq = nat_irq;
            net_flags = nat_flags;
            polled = (u8)( net_irq == NIC_NOIRQ );
            bridge_woke = 0;
        }
        if ( !card_find() && !card_setup() ) running = 1;
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
    if ( net_probing ) return 0;                /* a card that probes its
                                                   line handles this itself */
    if ( !running ) return 0;
    if ( card_service() ) { net_stats.irqs++; return 1; }
    return 0;
}

/* the clock drains the card as a backstop to its interrupt line, every
   tick, whether the card is polled or its line simply did not deliver
   this one.  card_service is safe to call when nothing is pending (it
   returns at once) and the clock is a kernel interrupt like the card's,
   so the two never run at once.  This keeps receive latency to a tick
   even on an emulated card whose interrupt is unreliable */
void __cdecl nd_tick( void )
{
    if ( !running || net_probing ) return;
    card_service();
    (void)tick_seen;
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
        net_memzero( info, NICI_LEN );
        net_memcpy( info->mac, net_mac, 6 );
        info->kind = NICK_ETHERNET;
        info->bus = net_bus;
        info->mtu = 1500;
        info->port = net_port;
        info->irq = net_irq;
        info->flags = net_flags;
        info->pciaddr = net_pciaddr;
        net_strcpy( info->name, net_name );
        *answer = NICI_LEN;
        return 0;

    case NIC_SEND:
        if ( len < NIC_MINFRAME || len > NIC_MAXFRAME ) return ERR_BADLEN;
        if ( !running ) return ERR_GENERAL;
        if ( !card_send( (const u8 *)buf, len ) ) return ERR_GENERAL;
        net_stats.txframes++;
        net_stats.txbytes += len;
        *answer = len;
        return 0;

    case NIC_RECV:
        if ( q_count == 0 ) { *answer = 0; return 0; }
        slot = queue + q_head * QSLOT;
        got = *(u16 *)slot;
        net_memcpy( buf, slot + 2, got < len ? got : len );
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
        net_memcpy( st, &net_stats, NICS_LEN );
        st->queued = q_count;
        int_back( flags );
        *answer = NICS_LEN;
        return 0;

    case NIC_SERVICE:
        /* take whatever has arrived off the card's ring into the queue,
           as nd_irq/nd_tick do - the WfW bridge drives this with
           interrupts off, so NIC_RECV then finds the frames */
        *answer = running ? (u32)card_service() : 0;
        return 0;

    case NIC_WAKE:
        /* the WfW bridge brings the parked card back up to receive while
           Windows owns the machine.  It comes up polled - its own
           interrupt line masked (card_setup reads NICF_POLLED) - so no
           interrupt is left unserviced under Windows; the bridge's tick
           drains the ring.  The native line is kept for the un-park. */
        flags = int_off();
        if ( !bridge_woke ) {
            nat_irq = net_irq;
            nat_flags = net_flags;
            bridge_woke = 1;
        }
        net_flags |= NICF_POLLED;
        net_irq = NIC_NOIRQ;
        polled = 1;
        if ( !card_setup() ) running = 1;
        int_back( flags );
        *answer = running;
        return 0;

    case NIC_WAKE_IRQ:
        /* the WfW bridge brings the parked card back up with its interrupt
           line LIVE, for the bridge's VPICD handler to field: the card
           comes up exactly as the native kernel runs it (IMR enabled,
           card_setup reads NICF_POLLED), so it asserts its line on
           receive.  The native line and flags are kept for the un-park;
           if a polled wake ran first, it is undone here. */
        flags = int_off();
        if ( !bridge_woke ) {
            nat_irq = net_irq;
            nat_flags = net_flags;
            bridge_woke = 1;
        } else {
            net_irq = nat_irq;
            net_flags = nat_flags;
        }
        net_flags = (u8)( net_flags & ~NICF_POLLED );
        polled = 0;
        if ( !card_setup() ) running = 1;
        int_back( flags );
        *answer = running;
        return 0;
    }
    return ERR_FUNC;
}
