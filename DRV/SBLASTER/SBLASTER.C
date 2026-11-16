/*
 * SBLASTER.C - SBLASTER.SYS, a 32-bit driver for the Sound Blaster
 * family: the card file behind DRV\AUDCORE.C (DRV\AUDDRV.H).
 *
 * ONE CARD, FIVE GENERATIONS, told apart by the version its DSP
 * reports:
 *   1.xx  the Sound Blaster 1.0/1.5: 8 bits, one channel, up to
 *         22 kHz, and no auto-initializing DMA - every block is
 *         started again from the interrupt, with a tiny gap
 *   2.00  auto-initializing DMA, up to 22 kHz
 *   2.01+ the Sound Blaster 2.0: high-speed mode, up to 44.1 kHz
 *   3.xx  the Sound Blaster Pro: two channels through the mixer, up
 *         to 22 kHz in stereo and 44.1 kHz in mono, 8 bits
 *   4.xx  the Sound Blaster 16, AWE32, AWE64 and the ViBRA chips: 8
 *         or 16 bits, one or two channels, any rate to 44.1 kHz, and
 *         a register that says which part of the card interrupted
 * Clones that answer as one of these (the ESS AudioDrive answers as a
 * Pro) are played as that one.
 *
 * THE DMA BUFFER IS 32K BELOW 16MB inside one 64K page, taken at INIT
 * (the 8237 can reach nothing else).  The 8237 runs round all of the
 * blocks the core asked for, auto-initializing; the DSP interrupts at
 * the end of each block.  Where the card is comes from the 8237's
 * current address.
 *
 * FINDING THE CARD.  The DEVICE= line can say where it is, in the
 * BLASTER variable's own words (A220 I5 D1 H5 P330) or with slashes
 * (/A:220 /I:5 /D:1 /H:5 /P:330 /F:388).  Without that: a Creative
 * Plug and Play card the kernel numbered at boot is used where it is -
 * or, when nothing has switched it on (a machine with no Plug and Play
 * BIOS and no CTCM), it is given the first set of resources it lists,
 * which on every Creative card is 220h, 330h, 388h, IRQ 5 and DMA 1
 * and 5, a line nothing else here has, and switched on.  Then the
 * usual ports are tried, 220h to 280h.  A Sound Blaster 16 says its
 * own line and channels in its mixer; an older card's line is found by
 * making the DSP interrupt (command F2h) with a handler on every line
 * a card could be jumpered to, and its channel is 1 unless the line
 * says otherwise.
 *
 * THE FM CHIP is looked for at the card's port + 8 and at 388h: an
 * OPL2 or an OPL3, told apart by the status register.  An OPL2 needs
 * 23 microseconds after a data write before the next; the waits are
 * reads of its status port, which take a microsecond each on the ISA
 * bus whatever the processor's speed.  THE MIDI PORT is an MPU-401 in
 * UART mode at 330h (or /P:), when it acknowledges a reset.
 *
 * BETWEEN PROGRAMS THE CARD IS LEFT ALONE: no line is claimed, the
 * mixer is put back as it was found at AUD_OPEN, and the DSP is
 * reset, so a DOS game finds the card as it would at boot.
 *
 * Built by BUILD.CMD: wcc386 -3r -s -zl, behind DRV\NDSTART.ASM with
 * DRV\AUDCORE.C, linked twice and put together by D32DRV.
 */
#define ND_DRIVER
#include "ndrv.h"
#include "audio.h"
#include "auddrv.h"

/* the bus calls (PMDOS.INC BUS_*) beyond what NDRV.H carries */
#ifndef BUS_PNPREGS
#define BUS_PNPREGS  0x32
#endif

/* ---------------------------------------------------------------- */
/* the card                                                          */
/* ---------------------------------------------------------------- */
enum { SB_1, SB_15, SB_2, SB_PRO, SB_16 };

static u16 base;                        /* the DSP's ports: base + 6, A, C, E, F */
static u8  dsp_major, dsp_minor;
static u8  kind;
static u8  irq = AUD_NONE, dma8 = AUD_NONE, dma16 = AUD_NONE;
static u16 fm_port, mpu_port;
static u16 fm_port2;                    /* an OPL3's second bank: base+2 when the chip
                                           is reached at base+8, else fm_port+2 */
static u8  is_open;                     /* between card_open and card_close */
static u8  opl3;
static u8  pnp_card;                    /* found through Plug and Play */

/* the DEVICE= line */
static u32 opt_base, opt_irq = AUD_NONE, opt_dma8 = AUD_NONE, opt_dma16 = AUD_NONE;
static u32 opt_mpu, opt_fm;
static u8  opt_nopnp, opt_nompu, opt_nofm;

/* what the card plays now */
static u8  play_bits, play_ch;
static u8  play_dma;                    /* the channel in use */
static u8  high_speed;                  /* DSP 2.01+ high-speed mode: no pause */
static u8  time_const;
static u32 play_rate;
static u8  playing;

/* the mixer as AUD_OPEN found it */
static u8  mix_saved;
static u8  mix_old[8];

/* the IRQ probe */
static u8  probing;
static u8  probe_line;

#define DSP_RESET    0x06
#define DSP_READ     0x0A
#define DSP_WRITE    0x0C
#define DSP_STATUS   0x0E               /* read: data waiting; and the 8-bit acknowledge */
#define DSP_ACK16    0x0F
#define MIX_INDEX    0x04
#define MIX_DATA     0x05

static const u8  dma_page[8] = { 0x87, 0x83, 0x81, 0x82, 0x00, 0x8B, 0x89, 0x8A };
static const u16 isa_bases[] = { 0x220, 0x240, 0x260, 0x280, 0 };
static const u8  probe_lines[] = { 5, 7, 2, 10, 3, 9, 0 };

/* ---------------------------------------------------------------- */
/* the DSP                                                           */
/* ---------------------------------------------------------------- */
/* about a microsecond each, on the ISA bus */
static void io_wait( u32 reads )
{
    while ( reads-- ) inb( 0x80 );
}

static int dsp_reset_at( u16 at )
{
    u32 i;

    outb( at + DSP_RESET, 1 );
    io_wait( 10 );
    outb( at + DSP_RESET, 0 );
    for ( i = 0; i < 20000; i++ ) {
        if ( inb( at + DSP_STATUS ) & 0x80 ) {
            if ( inb( at + DSP_READ ) == 0xAA ) return 1;
        }
    }
    return 0;
}

static int dsp_put( u8 value )
{
    u32 i;

    for ( i = 0; i < 100000UL; i++ ) {
        if ( !( inb( base + DSP_WRITE ) & 0x80 ) ) {
            outb( base + DSP_WRITE, value );
            return 1;
        }
    }
    return 0;
}

static int dsp_get( void )
{
    u32 i;

    for ( i = 0; i < 100000UL; i++ ) {
        if ( inb( base + DSP_STATUS ) & 0x80 ) return inb( base + DSP_READ );
    }
    return -1;
}

static u8 mix_get( u8 reg )
{
    outb( base + MIX_INDEX, reg );
    io_wait( 2 );
    return inb( base + MIX_DATA );
}

static void mix_set( u8 reg, u8 value )
{
    outb( base + MIX_INDEX, reg );
    io_wait( 2 );
    outb( base + MIX_DATA, value );
    io_wait( 2 );
}

/* ---------------------------------------------------------------- */
/* the 8237                                                          */
/* ---------------------------------------------------------------- */
static void dma_mask( u8 ch )
{
    if ( ch < 4 ) {
        outb( 0x0A, (u8)( 4 | ch ) );
    } else {
        outb( 0xD4, (u8)( 4 | ( ch - 4 ) ) );
    }
}

/* "bytes" from phys, memory to the card, round and round */
static void dma_program( u8 ch, u32 phys, u32 bytes )
{
    u32 addr, count;
    u8 low;

    if ( ch < 4 ) {
        addr = phys;
        count = bytes - 1;
        outb( 0x0A, (u8)( 4 | ch ) );
        outb( 0x0C, 0 );
        outb( 0x0B, (u8)( 0x58 | ch ) );            /* single, auto-init, read */
        outb( (u16)( ch * 2 ), (u8)addr );
        outb( (u16)( ch * 2 ), (u8)( addr >> 8 ) );
        outb( dma_page[ch], (u8)( phys >> 16 ) );
        outb( (u16)( ch * 2 + 1 ), (u8)count );
        outb( (u16)( ch * 2 + 1 ), (u8)( count >> 8 ) );
        outb( 0x0A, ch );
    } else {
        low = (u8)( ch - 4 );
        addr = phys >> 1;                            /* words, in its 128K page */
        count = bytes / 2 - 1;
        outb( 0xD4, (u8)( 4 | low ) );
        outb( 0xD8, 0 );
        outb( 0xD6, (u8)( 0x58 | low ) );
        outb( (u16)( 0xC0 + low * 4 ), (u8)addr );
        outb( (u16)( 0xC0 + low * 4 ), (u8)( addr >> 8 ) );
        outb( dma_page[ch], (u8)( ( phys >> 16 ) & 0xFE ) );
        outb( (u16)( 0xC2 + low * 4 ), (u8)count );
        outb( (u16)( 0xC2 + low * 4 ), (u8)( count >> 8 ) );
        outb( 0xD4, low );
    }
}

/* The channel's current address.  The two bytes are read one after the
   other while the controller goes on counting, so the low byte can be
   read just before it carries and the high byte just after - a reading
   255 bytes ahead of the truth, which would make a block look finished
   while the card is still playing it.  So the high byte is read either
   side of the second low byte: when both high readings agree, nothing
   carried between them and the second pair is right. */
static u32 dma_now( u8 ch )
{
    u16 port, ff;
    u32 hi1, lo2, hi2, tries;

    if ( ch < 4 ) {
        port = (u16)( ch * 2 );
        ff = 0x0C;
    } else {
        port = (u16)( 0xC0 + ( ch - 4 ) * 4 );
        ff = 0xD8;
    }
    lo2 = hi2 = 0;
    for ( tries = 0; tries < 8; tries++ ) {
        outb( ff, 0 );
        inb( port );
        hi1 = inb( port );
        outb( ff, 0 );
        lo2 = inb( port );
        hi2 = inb( port );
        if ( hi1 == hi2 ) break;
    }
    return lo2 | ( hi2 << 8 );
}

/* ---------------------------------------------------------------- */
/* the FM chip and the MIDI port                                     */
/* ---------------------------------------------------------------- */
static void opl_raw( u16 port, u8 reg, u8 value, int slow )
{
    int i;

    outb( port, reg );
    for ( i = slow ? 6 : 2; i > 0; i-- ) inb( port );
    outb( port + 1, value );
    for ( i = slow ? 35 : 2; i > 0; i-- ) inb( port );
}

static int opl_at( u16 port, u8 *is_opl3 )
{
    u8 first, second;

    opl_raw( port, 0x04, 0x60, 1 );
    opl_raw( port, 0x04, 0x80, 1 );
    first = inb( port );
    opl_raw( port, 0x02, 0xFF, 1 );
    opl_raw( port, 0x04, 0x21, 1 );
    io_wait( 200 );                     /* the timer runs out after 80 microseconds */
    second = inb( port );
    opl_raw( port, 0x04, 0x60, 1 );
    opl_raw( port, 0x04, 0x80, 1 );
    if ( ( first & 0xE0 ) != 0 || ( second & 0xE0 ) != 0xC0 ) return 0;
    *is_opl3 = (u8)( ( first & 0x06 ) == 0 );
    return 1;
}

/* every voice quiet - every operator turned all the way down, so a
   long release does not ring on, and every key let go - and the OPL3
   back to OPL2 mode, as at boot */
static void opl_quiet( void )
{
    static const u8 op_slot[18] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09, 0x0A,
        0x0B, 0x0C, 0x0D, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15
    };
    u8 ch;

    if ( !fm_port ) return;
    for ( ch = 0; ch < 18; ch++ ) {
        opl_raw( fm_port, (u8)( 0x40 + op_slot[ch] ), 0x3F, !opl3 );
        if ( opl3 ) opl_raw( fm_port2, (u8)( 0x40 + op_slot[ch] ), 0x3F, 0 );
    }
    for ( ch = 0; ch < 9; ch++ ) {
        opl_raw( fm_port, (u8)( 0xB0 + ch ), 0, !opl3 );
        if ( opl3 ) opl_raw( fm_port2, (u8)( 0xB0 + ch ), 0, 0 );
    }
    if ( opl3 ) {
        opl_raw( fm_port2, 0x04, 0, 0 );
        opl_raw( fm_port2, 0x05, 0, 0 );
    }
}

void card_opl( u8 bank, u8 reg, u8 val )
{
    if ( bank && !opl3 ) return;
    opl_raw( bank ? fm_port2 : fm_port, reg, val, !opl3 );
}

void card_mpu_drain( void )
{
    int i;

    if ( !mpu_port ) return;
    for ( i = 0; i < 64 && !( inb( mpu_port + 1 ) & 0x80 ); i++ ) inb( mpu_port );
}

int card_mpu_put( u8 byte )
{
    if ( !mpu_port ) return 1;
    card_mpu_drain();
    if ( inb( mpu_port + 1 ) & 0x40 ) return 0;
    outb( mpu_port, byte );
    return 1;
}

/* a command, and its acknowledgement (FEh) */
static int mpu_command( u16 port, u8 cmd )
{
    u32 i;

    for ( i = 0; i < 20000UL && ( inb( port + 1 ) & 0x40 ); i++ ) ;
    if ( inb( port + 1 ) & 0x40 ) return 0;
    outb( port + 1, cmd );
    for ( i = 0; i < 20000UL; i++ ) {
        if ( !( inb( port + 1 ) & 0x80 ) && inb( port ) == 0xFE ) return 1;
    }
    return 0;
}

static int mpu_at( u16 port )
{
    if ( !mpu_command( port, 0xFF ) ) {
        /* a port already in UART mode does not acknowledge a reset */
        if ( !mpu_command( port, 0xFF ) ) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* Plug and Play                                                     */
/* ---------------------------------------------------------------- */
/* "CTL" as an EISA ID's first two bytes */
#define CTL_HI  0x0E
#define CTL_LO  0x8C

static u16 pick_port( u32 lo, u32 hi, u32 align, u16 want )
{
    if ( align == 0 ) align = 1;
    if ( lo <= want && want <= hi && ( ( want - lo ) % align ) == 0 ) return want;
    return (u16)lo;
}

static int line_free( u32 line )
{
    REGS r;

    mem_zero( &r, sizeof( r ) );
    r.eax = (u32)BUS_IRQSTAT << 8;
    r.edx = line;
    if ( nds_bus( &r ) && ( r.eax & ( IRQF_KERNEL | IRQF_HOOKED | IRQF_OPEN | IRQF_DEAD ) ) ) return 0;
    return 1;
}

static int find_pnp( void )
{
    static const u8 irq_pref[] = { 5, 7, 10, 9, 11, 0 };
    REGS r;
    u8  *res, *regs, pairs[24], tag;
    u32  cards, csn, len, pos, size, kindtag, mask, i;
    u32  io_lo[3], io_hi[3], io_align[3];
    u32  irq_mask, ch_mask[2];
    int  devices, nio, ndma, n;
    u16  io[3];
    u8   line, ch8, ch16;

    mem_zero( &r, sizeof( r ) );
    r.eax = (u32)BUS_PNPHERE << 8;
    if ( !nds_bus( &r ) || r.eax == 0 ) return 0;
    cards = r.eax;
    res = (u8 *)nds_alloc( 4096 + 256 );
    if ( res == 0 ) return 0;
    regs = res + 4096;
    for ( csn = 1; csn <= cards && csn < 256; csn++ ) {
        mem_zero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PNPRES << 8;
        r.edx = csn;
        r.edi = (u32)res;
        r.ecx = 4096;
        if ( !nds_bus( &r ) || r.ecx < 9 || r.ecx > 4096 ) continue;
        len = r.ecx;
        if ( res[0] != CTL_HI || res[1] != CTL_LO ) continue;

        /* logical device 0, the audio: its first ports, line and
           channels, which are the first dependent set's */
        devices = 0;
        nio = ndma = 0;
        irq_mask = 0;
        ch_mask[0] = ch_mask[1] = 0;
        pos = 9;
        while ( pos < len ) {
            tag = res[pos++];
            if ( tag & 0x80 ) {
                if ( pos + 2 > len ) break;
                size = res[pos] | ( (u32)res[pos + 1] << 8 );
                if ( size > len - pos - 2 ) break;
                pos += 2 + size;
                continue;
            }
            kindtag = ( tag >> 3 ) & 15;
            size = tag & 7;
            if ( pos + size > len || kindtag == 15 ) break;
            if ( kindtag == 2 && devices++ ) break;          /* the next device */
            if ( devices == 1 ) {
                switch ( kindtag ) {
                case 4:                                 /* a line */
                    if ( size >= 2 && irq_mask == 0 ) irq_mask = res[pos] | ( (u32)res[pos + 1] << 8 );
                    break;
                case 5:                                 /* a channel */
                    if ( size >= 1 && ndma < 2 ) ch_mask[ndma++] = res[pos];
                    break;
                case 8:                                 /* a port range */
                    if ( size >= 7 && nio < 3 ) {
                        io_lo[nio] = res[pos + 1] | ( (u32)res[pos + 2] << 8 );
                        io_hi[nio] = res[pos + 3] | ( (u32)res[pos + 4] << 8 );
                        io_align[nio] = res[pos + 5];
                        nio++;
                    }
                    break;
                case 9:                                 /* a fixed port */
                    if ( size >= 3 && nio < 3 ) {
                        io_lo[nio] = io_hi[nio] = res[pos] | ( (u32)( res[pos + 1] & 3 ) << 8 );
                        io_align[nio] = 1;
                        nio++;
                    }
                    break;
                case 7:                                 /* the end of the first set */
                    if ( nio ) pos = len;
                    break;
                }
            }
            pos += size;
        }
        if ( nio == 0 ) continue;

        /* on already, where a BIOS or ISAPNP put it? */
        mem_zero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PNPREGS << 8;
        r.edx = csn;                                    /* DH = device 0 */
        r.edi = (u32)regs;
        if ( !nds_bus( &r ) ) continue;
        if ( !( regs[0x30] & 1 ) || !( regs[0x60] | regs[0x61] ) ) {
            if ( opt_nopnp ) continue;
            io[0] = pick_port( io_lo[0], io_hi[0], io_align[0], 0x220 );
            io[1] = nio > 1 ? pick_port( io_lo[1], io_hi[1], io_align[1], 0x330 ) : 0;
            io[2] = nio > 2 ? pick_port( io_lo[2], io_hi[2], io_align[2], 0x388 ) : 0;
            line = 0;
            for ( i = 0; irq_pref[i]; i++ ) {
                if ( ( irq_mask & ( 1UL << irq_pref[i] ) ) && line_free( irq_pref[i] ) ) {
                    line = irq_pref[i];
                    break;
                }
            }
            /* channel 1 and channel 5 when allowed, else the lowest; 4 is none */
            ch8 = ch16 = 4;
            mask = ch_mask[0];
            for ( i = 0; i < 4 && ch8 == 4; i++ ) {
                if ( mask & ( 1UL << ( i ^ 1 ) ) ) ch8 = (u8)( i ^ 1 );
            }
            mask = ch_mask[1];
            for ( i = 5; i < 8 && ch16 == 4; i++ ) {
                if ( mask & ( 1UL << i ) ) ch16 = (u8)i;
            }
            n = 0;
            pairs[n++] = 0x30; pairs[n++] = 0;
            pairs[n++] = 0x60; pairs[n++] = (u8)( io[0] >> 8 );
            pairs[n++] = 0x61; pairs[n++] = (u8)io[0];
            if ( io[1] ) { pairs[n++] = 0x62; pairs[n++] = (u8)( io[1] >> 8 ); pairs[n++] = 0x63; pairs[n++] = (u8)io[1]; }
            if ( io[2] ) { pairs[n++] = 0x64; pairs[n++] = (u8)( io[2] >> 8 ); pairs[n++] = 0x65; pairs[n++] = (u8)io[2]; }
            pairs[n++] = 0x70; pairs[n++] = line;
            pairs[n++] = 0x71; pairs[n++] = 2;
            pairs[n++] = 0x74; pairs[n++] = ch8;
            pairs[n++] = 0x75; pairs[n++] = ch16;
            pairs[n++] = 0x30; pairs[n++] = 1;
            mem_zero( &r, sizeof( r ) );
            r.eax = (u32)BUS_PNPSET << 8;
            r.edx = csn;
            r.esi = (u32)pairs;
            r.ecx = (u32)( n / 2 );
            if ( !nds_bus( &r ) ) continue;
            mem_zero( &r, sizeof( r ) );
            r.eax = (u32)BUS_PNPREGS << 8;
            r.edx = csn;
            r.edi = (u32)regs;
            if ( !nds_bus( &r ) ) continue;
        }
        /* a port the card did not take reads 0: resetting a DSP there
           would write the 8237's registers.  The card's other settings
           are used only once its DSP has answered */
        base = (u16)( ( (u16)regs[0x60] << 8 ) | regs[0x61] );
        if ( base < 0x100 || !dsp_reset_at( base ) ) {
            base = 0;
            continue;
        }
        if ( nio > 1 && !opt_mpu ) opt_mpu = ( (u32)regs[0x62] << 8 ) | regs[0x63];
        if ( nio > 2 && !opt_fm ) opt_fm = ( (u32)regs[0x64] << 8 ) | regs[0x65];
        if ( opt_irq == AUD_NONE && ( regs[0x70] & 15 ) ) opt_irq = regs[0x70] & 15;
        if ( opt_dma8 == AUD_NONE && regs[0x74] < 4 ) opt_dma8 = regs[0x74];
        if ( opt_dma16 == AUD_NONE && regs[0x75] >= 5 && regs[0x75] < 8 ) opt_dma16 = regs[0x75];
        pnp_card = 1;
        nds_free( res );
        return 1;
    }
    nds_free( res );
    return 0;
}

/* ---------------------------------------------------------------- */
/* finding the card                                                  */
/* ---------------------------------------------------------------- */
static void find_irq( void )
{
    u8 hooked[16];
    u32 start;
    int i;

    mem_zero( hooked, sizeof( hooked ) );
    probing = 1;
    probe_line = AUD_NONE;
    for ( i = 0; probe_lines[i]; i++ ) {
        if ( line_free( probe_lines[i] ) && nds_irqhook( probe_lines[i] ) ) hooked[probe_lines[i]] = 1;
    }
    dsp_put( 0xF2 );
    start = aud_ticks();
    while ( probe_line == AUD_NONE && aud_ticks() - start < 3 ) ;
    probing = 0;
    inb( base + DSP_STATUS );
    for ( i = 0; probe_lines[i]; i++ ) {
        if ( hooked[probe_lines[i]] ) nds_irqunhook( probe_lines[i] );
    }
    irq = probe_line;
}

/* the probe's handler (the core sends every interrupt here while no
   program has the card open): the first line called is the card's */
int card_probe_irq( u32 line )
{
    if ( !probing || probe_line != AUD_NONE ) return 0;
    inb( base + DSP_STATUS );
    probe_line = (u8)line;
    return 1;
}

int card_find( void )
{
    u32 i;
    u8 sel, is3;
    int ver;

    if ( base == 0 ) {
        if ( opt_base ) {
            if ( !dsp_reset_at( (u16)opt_base ) ) return 0;
            base = (u16)opt_base;
        } else if ( !find_pnp() ) {
            for ( i = 0; isa_bases[i]; i++ ) {
                if ( dsp_reset_at( isa_bases[i] ) ) break;
            }
            if ( !isa_bases[i] ) return 0;
            base = isa_bases[i];
        }
    }
    if ( !dsp_reset_at( base ) || !dsp_put( 0xE1 ) ) return 0;
    ver = dsp_get();
    if ( ver < 0 ) return 0;
    dsp_major = (u8)ver;
    ver = dsp_get();
    dsp_minor = (u8)( ver < 0 ? 0 : ver );
    if ( dsp_major >= 4 ) {
        kind = SB_16;
    } else if ( dsp_major == 3 ) {
        kind = SB_PRO;
    } else if ( dsp_major == 2 && dsp_minor > 0 ) {
        kind = SB_2;
    } else if ( dsp_major == 2 ) {
        kind = SB_15;
    } else {
        kind = SB_1;
    }

    /* the line and the channels */
    irq = (u8)opt_irq;
    dma8 = (u8)opt_dma8;
    dma16 = (u8)opt_dma16;
    if ( kind == SB_16 ) {
        sel = mix_get( 0x80 );
        if ( irq == AUD_NONE ) {
            irq = sel & 1 ? 2 : sel & 2 ? 5 : sel & 4 ? 7 : sel & 8 ? 10 : AUD_NONE;
            if ( irq == 2 ) irq = 9;                     /* line 2 arrives as 9 on an AT */
        }
        sel = mix_get( 0x81 );
        if ( dma8 == AUD_NONE ) dma8 = sel & 1 ? 0 : sel & 2 ? 1 : sel & 8 ? 3 : AUD_NONE;
        if ( dma16 == AUD_NONE ) dma16 = sel & 0x20 ? 5 : sel & 0x40 ? 6 : sel & 0x80 ? 7 : AUD_NONE;
    } else {
        if ( dma8 == AUD_NONE ) dma8 = 1;
        dma16 = AUD_NONE;
        if ( irq == AUD_NONE ) find_irq();
    }
    if ( dma8 == AUD_NONE && dma16 == AUD_NONE ) return 0;

    /* the FM chip and the MIDI port */
    fm_port = 0;
    opl3 = 0;
    if ( !opt_nofm ) {
        if ( opt_fm && opl_at( (u16)opt_fm, &is3 ) ) {
            fm_port = (u16)opt_fm;
        } else if ( opl_at( (u16)( base + 8 ), &is3 ) ) {
            fm_port = 0x388;                             /* the same chip, the usual name for it */
            if ( !opl_at( 0x388, &is3 ) ) fm_port = (u16)( base + 8 );
        } else if ( opl_at( 0x388, &is3 ) ) {
            fm_port = 0x388;
        }
        if ( fm_port ) opl3 = is3;
        /* the second bank: at base+2 on a card whose chip is at base+8
           (base+0Ah is the DSP's read port), after the first anywhere else */
        fm_port2 = (u16)( fm_port == base + 8 ? base + 2 : fm_port + 2 );
    }
    mpu_port = 0;
    if ( !opt_nompu ) {
        if ( opt_mpu && mpu_at( (u16)opt_mpu ) ) {
            mpu_port = (u16)opt_mpu;
        } else if ( !opt_mpu && kind == SB_16 && mpu_at( 0x330 ) ) {
            mpu_port = 0x330;
        }
        if ( mpu_port ) mpu_command( mpu_port, 0xFF );   /* left in reset until a program opens */
    }

    /* what to call it and what it plays */
    aud_info.flags = AUDF_FOUND | AUDF_PCM;
    aud_info.bits = AUDB_8;
    aud_info.channels = 1;
    aud_info.rate_min = 4000;
    switch ( kind ) {
    case SB_16:
        str_cpy( aud_info.name, "Sound Blaster 16" );
        aud_info.bits = AUDB_8 | AUDB_16;
        aud_info.channels = 2;
        aud_info.rate_min = 5000;
        aud_info.rate_max = aud_info.rate_max2 = 44100;
        aud_info.flags |= AUDF_MIXER | AUDF_ANYRATE;
        if ( dma16 == AUD_NONE ) aud_info.bits = AUDB_8;
        if ( dma8 == AUD_NONE ) aud_info.bits = AUDB_16;
        break;
    case SB_PRO:
        str_cpy( aud_info.name, "Sound Blaster Pro" );
        aud_info.channels = 2;
        aud_info.rate_max = 44100;
        aud_info.rate_max2 = 22050;
        aud_info.flags |= AUDF_MIXER;
        break;
    case SB_2:
        str_cpy( aud_info.name, "Sound Blaster 2.0" );
        aud_info.rate_max = aud_info.rate_max2 = 44100;
        break;
    case SB_15:
        str_cpy( aud_info.name, "Sound Blaster 1.5" );
        aud_info.rate_max = aud_info.rate_max2 = 22050;
        break;
    default:
        str_cpy( aud_info.name, "Sound Blaster" );
        aud_info.rate_max = aud_info.rate_max2 = 22050;
        break;
    }
    if ( fm_port ) aud_info.flags |= opl3 ? AUDF_OPL3 : AUDF_OPL2;
    if ( mpu_port ) aud_info.flags |= AUDF_MPU;
    aud_info.port = base;
    aud_info.irq = irq;
    aud_info.dma8 = dma8;
    aud_info.dma16 = dma16;
    aud_info.fm_port = fm_port;
    aud_info.mpu_port = mpu_port;
    aud_info.version_hw = (u16)( ( (u16)dsp_major << 8 ) | dsp_minor );
    return 1;
}

/* ---------------------------------------------------------------- */
/* playing                                                           */
/* ---------------------------------------------------------------- */
int card_open( AUD_OPENBLOCK *fmt )
{
    u32 rate = fmt->rate, want, frame, count;

    if ( dma_buf == 0 ) return ERR_GENERAL;
    /* a 1.x card plays one block a command, and the next command is
       given from its interrupt: with no line, it would stop after one */
    if ( kind == SB_1 && irq == AUD_NONE ) return ERR_GENERAL;
    /* what this card can do with what was asked */
    if ( kind == SB_16 ) {
        if ( fmt->bits == 16 && dma16 == AUD_NONE ) fmt->bits = 8;
        if ( fmt->bits == 8 && dma8 == AUD_NONE ) fmt->bits = 16;
        if ( rate < 5000 ) rate = 5000;
        if ( rate > 44100 ) rate = 44100;
        high_speed = 0;
    } else {
        fmt->bits = 8;
        if ( kind != SB_PRO ) fmt->channels = 1;
        if ( rate < 4000 ) rate = 4000;
        if ( fmt->channels == 2 && rate > 22050 ) rate = 22050;
        if ( rate > ( kind >= SB_2 ? 44100UL : 22050UL ) ) rate = kind >= SB_2 ? 44100UL : 22050UL;
        /* the time constant: 256 - 1,000,000 / (rate x channels), and
           the rate it really gives */
        time_const = (u8)( 256 - ( 1000000UL + rate * fmt->channels / 2 ) / ( rate * fmt->channels ) );
        rate = 1000000UL / ( 256 - time_const ) / fmt->channels;
        high_speed = (u8)( kind >= SB_2 && rate * fmt->channels > 22050 );
    }
    play_bits = fmt->bits;
    play_ch = fmt->channels;
    play_rate = rate;
    play_dma = play_bits == 16 ? dma16 : dma8;
    fmt->rate = rate;

    /* blocks: what was asked for, else about 20 ms; in whole frames,
       at least 2 and at most AUD_MAXBLK of them in the buffer */
    frame = (u32)( play_bits / 8 ) * play_ch;
    want = fmt->block ? fmt->block : rate * frame / 50;
    if ( want < 32 ) want = 32;
    if ( want > dma_size / 2 ) want = dma_size / 2;
    want -= want % ( frame * 2 );               /* even, for the 16-bit count */
    if ( want == 0 ) want = frame * 2;
    count = dma_size / want;
    if ( count > AUD_MAXBLK ) count = AUD_MAXBLK;
    blk_bytes = want;
    blk_count = count;

    /* the mixer as it is, to put back at close */
    mix_saved = 0;
    if ( kind == SB_16 ) {
        mix_old[0] = mix_get( 0x32 );
        mix_old[1] = mix_get( 0x33 );
        mix_old[2] = mix_get( 0x34 );
        mix_old[3] = mix_get( 0x35 );
        mix_saved = 1;
    } else if ( kind == SB_PRO ) {
        mix_old[0] = mix_get( 0x04 );
        mix_old[1] = mix_get( 0x26 );
        mix_old[2] = mix_get( 0x0E );
        mix_saved = 1;
    }
    if ( mpu_port ) mpu_command( mpu_port, 0x3F );      /* UART mode */
    is_open = 1;
    return 0;
}

void card_start( void )
{
    u32 count;

    dsp_reset_at( base );
    dma_program( play_dma, dma_phys, blk_bytes * blk_count );
    if ( kind == SB_16 ) {
        dsp_put( 0x41 );
        dsp_put( (u8)( play_rate >> 8 ) );
        dsp_put( (u8)play_rate );
        if ( play_bits == 16 ) {
            count = blk_bytes / 2 - 1;
            dsp_put( 0xB6 );                         /* 16 bits, out, auto-init, FIFO */
            dsp_put( (u8)( 0x10 | ( play_ch == 2 ? 0x20 : 0 ) ) );
        } else {
            count = blk_bytes - 1;
            dsp_put( 0xC6 );
            dsp_put( (u8)( play_ch == 2 ? 0x20 : 0 ) );
        }
        dsp_put( (u8)count );
        dsp_put( (u8)( count >> 8 ) );
    } else {
        if ( kind == SB_PRO ) {
            /* stereo, and the output filter off, which dulls 22 kHz */
            mix_set( 0x0E, (u8)( ( mix_old[2] & ~0x22 ) | ( play_ch == 2 ? 0x22 : 0x20 ) ) );
        }
        dsp_put( 0xD1 );                             /* the speaker on */
        dsp_put( 0x40 );
        dsp_put( time_const );
        count = blk_bytes - 1;
        if ( kind == SB_1 ) {
            dsp_put( 0x14 );                         /* one block; the next from the interrupt */
            dsp_put( (u8)count );
            dsp_put( (u8)( count >> 8 ) );
        } else {
            dsp_put( 0x48 );
            dsp_put( (u8)count );
            dsp_put( (u8)( count >> 8 ) );
            dsp_put( (u8)( high_speed ? 0x90 : 0x1C ) );
        }
    }
    playing = 1;
}

void card_halt( void )
{
    if ( !playing ) return;
    if ( kind == SB_16 ) {
        dsp_put( (u8)( play_bits == 16 ? 0xD5 : 0xD0 ) );
    } else if ( !high_speed ) {
        dsp_put( 0xD0 );
    }
    dma_mask( play_dma );
    dsp_reset_at( base );                            /* the only way out of high speed */
    inb( base + DSP_STATUS );
    if ( kind == SB_16 ) inb( base + DSP_ACK16 );
    playing = 0;
}

int card_pause( void )
{
    if ( !playing || high_speed || kind == SB_1 ) return 0;
    dsp_put( (u8)( kind == SB_16 && play_bits == 16 ? 0xD5 : 0xD0 ) );
    return 1;
}

void card_resume( void )
{
    dsp_put( (u8)( kind == SB_16 && play_bits == 16 ? 0xD6 : 0xD4 ) );
}

void card_close( void )
{
    card_halt();
    if ( kind != SB_16 ) dsp_put( 0xD3 );            /* the speaker off */
    if ( mix_saved ) {
        if ( kind == SB_16 ) {
            mix_set( 0x32, mix_old[0] );
            mix_set( 0x33, mix_old[1] );
            mix_set( 0x34, mix_old[2] );
            mix_set( 0x35, mix_old[3] );
        } else {
            mix_set( 0x04, mix_old[0] );
            mix_set( 0x26, mix_old[1] );
            mix_set( 0x0E, mix_old[2] );
        }
        mix_saved = 0;
    }
    opl_quiet();
    if ( mpu_port ) mpu_command( mpu_port, 0xFF );   /* out of UART mode */
    is_open = 0;
}

u32 card_dma_pos( void )
{
    u32 now = dma_now( play_dma ), start, pos;

    if ( play_dma < 4 ) {
        start = dma_phys & 0xFFFF;
        pos = ( now - start ) & 0xFFFF;
    } else {
        start = ( dma_phys >> 1 ) & 0xFFFF;
        pos = ( ( now - start ) & 0xFFFF ) * 2;
    }
    if ( pos >= blk_bytes * blk_count ) pos = 0;
    return pos;
}

int card_irq( void )
{
    u8 status;
    u32 count;

    if ( kind == SB_16 ) {
        status = mix_get( 0x82 );
        if ( status & 4 ) card_mpu_drain();
        if ( status & 2 ) {
            inb( base + DSP_ACK16 );
            return CARD_BLOCK;
        }
        if ( status & 1 ) {
            inb( base + DSP_STATUS );
            return CARD_BLOCK;
        }
        return status & 4 ? CARD_OTHER : CARD_NOTMINE;
    }
    inb( base + DSP_STATUS );
    if ( kind == SB_1 && playing ) {
        count = blk_bytes - 1;
        dsp_put( 0x14 );
        dsp_put( (u8)count );
        dsp_put( (u8)( count >> 8 ) );
    }
    return CARD_BLOCK;
}

/* an attenuation in half decibels onto "top" steps of "half" half
   decibels each: the step from the top, 0 being the quietest */
static u32 mixer_step( u8 level, u32 top, u32 half )
{
    u32 att = aud_atten( level ), down = att / half;

    if ( level == 0 || down >= top ) return 0;
    return top - down;
}

/* the amplitude onto the card's own steps: the Sound Blaster 16's
   voice and FM volumes go in 2 dB steps from 31, the Pro's in about
   4 dB steps from 7 */
void card_volume( u8 left, u8 right )
{
    u32 l, r;

    if ( kind == SB_16 ) {
        l = mixer_step( left, 31, 4 ) << 3;
        r = mixer_step( right, 31, 4 ) << 3;
        mix_set( 0x32, (u8)l );
        mix_set( 0x33, (u8)r );
        mix_set( 0x34, (u8)l );
        mix_set( 0x35, (u8)r );
    } else if ( kind == SB_PRO ) {
        l = mixer_step( left, 7, 8 );
        r = mixer_step( right, 7, 8 );
        mix_set( 0x04, (u8)( ( l << 5 ) | ( r << 1 ) ) );
        mix_set( 0x26, (u8)( ( l << 5 ) | ( r << 1 ) ) );
    }
}

void card_park( int back )
{
    if ( !back ) {
        card_halt();
        opl_quiet();
    } else if ( is_open && mpu_port ) {
        mpu_command( mpu_port, 0xFF );               /* whatever real mode left it in, */
        mpu_command( mpu_port, 0x3F );               /* UART mode again */
    }
}

/* ---------------------------------------------------------------- */
/* the DEVICE= line                                                  */
/* ---------------------------------------------------------------- */
/* "A220 I5 D1 H5 P330", "/A:220 /I:5", "/NOPNP", "/NOFM", "/NOMIDI" */
static void parse_line( const char *line )
{
    const char *at = line;
    char letter;

    while ( *at && *at != ' ' && *at != '\t' && *at != '\r' && *at != '\n' ) at++;
    while ( *at && *at != '\r' && *at != '\n' ) {
        if ( *at == ' ' || *at == '\t' || *at == '/' ) {
            at++;
            continue;
        }
        letter = (char)( *at | 0x20 );
        if ( letter == 'n' && ( at[1] | 0x20 ) == 'o' ) {
            if ( ( at[2] | 0x20 ) == 'p' ) opt_nopnp = 1;
            if ( ( at[2] | 0x20 ) == 'f' ) opt_nofm = 1;
            if ( ( at[2] | 0x20 ) == 'm' ) opt_nompu = 1;
        } else {
            at++;
            if ( *at == ':' || *at == '=' ) at++;
            switch ( letter ) {
            case 'a': opt_base = num_after( at, 1, 0 ); break;
            case 'p': opt_mpu = num_after( at, 1, 0 ); break;
            case 'f': opt_fm = num_after( at, 1, 0 ); break;
            case 'i': opt_irq = num_after( at, 0, AUD_NONE ); if ( opt_irq > 15 ) opt_irq = AUD_NONE; break;
            case 'd': opt_dma8 = num_after( at, 0, AUD_NONE ); if ( opt_dma8 > 3 ) opt_dma8 = AUD_NONE; break;
            case 'h': opt_dma16 = num_after( at, 0, AUD_NONE ); if ( opt_dma16 < 5 || opt_dma16 > 7 ) opt_dma16 = AUD_NONE; break;
            }
        }
        while ( *at && *at != ' ' && *at != '\t' && *at != '/' && *at != '\r' && *at != '\n' ) at++;
    }
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_AUDIO,
    { 'S', 'B', 'L', 'A', 'S', 'T', 'E', 'R' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};

int __cdecl nd_init( const char *line )
{
    aud_core_init();
    parse_line( line );
    if ( !card_find() ) {
        aud_print( "SBLASTER.SYS: no Sound Blaster was found\r\n" );
        return 1;
    }
    if ( !aud_alloc_dma( 32768UL, 0x1000000UL ) ) {
        aud_print( "SBLASTER.SYS: no memory below 16MB for the card's buffer\r\n" );
        return 1;
    }
    dsp_reset_at( base );
    nds_tickhook();
    aud_say_card( "SBLASTER.SYS" );
    return 0;
}
