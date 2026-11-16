/*
 * ES1371.C - ES1371.SYS, a 32-bit driver for the Ensoniq AudioPCI
 * family: the card file behind DRV\AUDCORE.C (DRV\AUDDRV.H).
 *
 * FIVE CHIPS, TWO DESIGNS.  The ES1370 (PCI 1274:5000, the first
 * AudioPCI and the Sound Blaster PCI64) sets its playback rate with a
 * clock divider and has an AK4531 mixer.  The ES1371 (1274:1371), its
 * successors the ES1373 and the CT5880 (1274:5880, and 1274:1371 at
 * revision 7), and Creative's EV1938 (1102:8938) have a sample-rate
 * converter that plays any rate from 4 to 48 kHz exactly, and an
 * AC'97 codec for a mixer.  86Box offers the ES1371 and the ES1373 -
 * the owner's emulated machine has the ES1373 - and QEMU the ES1370.
 *
 * PLAYBACK IS THE SECOND DAC, "DAC2", which reads memory itself as a
 * PCI bus master: the buffer's physical address and length go to two
 * registers behind the memory page select (page 0Ch), a sample count
 * says how often to interrupt, and the chip loops round the buffer
 * until it is stopped.  Where it has got to is in the high half of the
 * frame size register, in longwords.  There is no FM chip; the MIDI
 * port is the chip's own UART, wired to the game port's MIDI pins.
 *
 * THE SAMPLE-RATE CONVERTER MUST BE PROGRAMMED BEFORE IT IS SWITCHED
 * ON: every one of its 128 registers is written at INIT, its DAC
 * accumulators set as the chip's makers say, and only then is it let
 * run.  A converter switched on with its memory unset is known to
 * lock the chip until the power goes off.  Every write to the AC'97
 * codec goes in a moment the converter is not using the same bus.
 *
 * Built by BUILD.CMD: wcc386 -3r -s -zl, behind DRV\NDSTART.ASM with
 * DRV\AUDCORE.C, linked twice and put together by D32DRV.
 */
#define ND_DRIVER
#include "ndrv.h"
#include "audio.h"
#include "auddrv.h"

/* the registers (offsets from the I/O base) */
#define R_CONTROL       0x00
#define R_STATUS        0x04
#define R_UART_DATA     0x08
#define R_UART_STATUS   0x09            /* read; written, it is the UART's control */
#define R_MEM_PAGE      0x0C
#define R_1370_CODEC    0x10
#define R_1371_SRC      0x10
#define R_1371_CODEC    0x14
#define R_SERIAL        0x20
#define R_DAC2_COUNT    0x28
#define R_DAC2_FRAME    0x38            /* page 0Ch */
#define R_DAC2_SIZE     0x3C            /* page 0Ch */

/* CONTROL */
#define C_1370_CDC_EN   0x00000002UL
#define C_UART_EN       0x00000008UL
#define C_DAC2_EN       0x00000020UL
#define C_BREQ          0x00000080UL
#define C_1371_SYNC_RES 0x00004000UL
#define C_1370_PCLKDIV( div )   (((u32)(div) & 0x1FFF) << 16)

/* STATUS */
#define S_INTR          0x80000000UL
#define S_1370_CSTAT    0x00000400UL
#define S_DAC2          0x00000002UL
#define S_UART          0x00000008UL

/* SERIAL */
#define SR_P2_INT_EN    0x00000200UL
#define SR_P2_PAUSE     0x00001000UL
#define SR_P2_LOOP_SEL  0x00004000UL
#define SR_P2_DAC_SEN   0x00000040UL
#define SR_P2_MODE( m ) ((u32)(m) << 2)
#define SR_P2_MODEMASK  0x0000000CUL
#define SR_P2_END_INC( n )      ((u32)(n) << 19)
#define SR_P2_ST_INC( n )       ((u32)(n) << 16)
#define SR_P2_INCMASK   0x003F0000UL

/* the sample-rate converter's register (ES1371) */
#define SRC_RAM_WE      0x01000000UL
#define SRC_RAM_BUSY    0x00800000UL
#define SRC_DISABLE     0x00400000UL
#define SRC_DIS_P1      0x00200000UL
#define SRC_DIS_P2      0x00100000UL
#define SRC_DIS_R1      0x00080000UL
#define SRC_KEEP        (SRC_DISABLE | SRC_DIS_P1 | SRC_DIS_P2 | SRC_DIS_R1)
#define SRC_ADDR( a )   (((u32)(a) & 0x7F) << 25)

/* ...and its memory */
#define SM_DAC1         0x70
#define SM_DAC2         0x74
#define SM_ADC          0x78
#define SM_VOL_ADC      0x6C
#define SM_VOL_DAC1     0x7C
#define SM_VOL_DAC2     0x7E
#define SM_TRUNC_N      0x00
#define SM_INT_REGS     0x01
#define SM_VFREQ_FRAC   0x03

/* the AC'97 codec's register (ES1371) */
#define CD_RDY          0x80000000UL
#define CD_WIP          0x40000000UL
#define CD_PIRD         0x00800000UL
#define CD_WRITE( r, v )        ((((u32)(r) & 0x7F) << 16) | ((u32)(v) & 0xFFFF))

#define AC97_RESET      0x00
#define AC97_MASTER     0x02
#define AC97_PCM        0x18

#define PAGE_DAC        0x0C
#define POLL            100000UL

enum { CHIP_1370, CHIP_1371 };

static u16 io;
static u8  irq = AUD_NONE;
static u8  chip;
static u32 pciaddr;
static u32 ctrl;                        /* CONTROL as last written */
static u32 sctrl;                       /* SERIAL as last written */
static u8  play_mode;                   /* 0 8-bit mono .. 3 16-bit stereo */
static u32 play_rate;
static u32 play_div;                    /* the ES1370's clock divider for play_rate */
static u8  playing;
static u8  is_open;                     /* between card_open and card_close */
static u8  vol_l = 255, vol_r = 255;    /* the volume as last set */
static u16 old_master = 0xFFFF, old_pcm = 0xFFFF;
static u8  saved_1370[4];
static u8  ak4531[0x20];                /* the ES1370's mixer, as written (it cannot be read) */

/* ---------------------------------------------------------------- */
/* PCI                                                               */
/* ---------------------------------------------------------------- */
static int bus_call( u32 func, REGS *r )
{
    r->eax = ( r->eax & 0xFFFF ) | ( func << 8 );
    return nds_bus( r );
}

static const struct {
    u16 vendor, device;
    u8  chip;
} ids[] = {
    { 0x1274, 0x1371, CHIP_1371 },
    { 0x1274, 0x5880, CHIP_1371 },
    { 0x1102, 0x8938, CHIP_1371 },
    { 0x1274, 0x5000, CHIP_1370 },
    { 0, 0, 0 }
};

/* Configuration space through the kernel's calls, which reach it by
   mechanism 1 or 2, whichever the board has */
static u32 cfg_read( u32 addr, int reg, int bytes )
{
    REGS r;

    mem_zero( &r, sizeof( r ) );
    r.ebx = addr;
    r.edx = (u32)reg | ( (u32)bytes << 8 );
    if ( !bus_call( BUS_PCIREAD, &r ) ) return 0xFFFFFFFFUL;
    return r.eax;
}

static void cfg_write( u32 addr, int reg, int bytes, u32 value )
{
    REGS r;

    mem_zero( &r, sizeof( r ) );
    r.ebx = addr;
    r.edx = (u32)reg | ( (u32)bytes << 8 );
    r.ecx = value;
    bus_call( BUS_PCIWRITE, &r );
}

static int find_card( void )
{
    REGS r;
    u32 rev, bar;
    int i, found = 0;

    for ( i = 0; ids[i].vendor && !found; i++ ) {
        mem_zero( &r, sizeof( r ) );
        r.ecx = ids[i].vendor;
        r.edx = ids[i].device;
        r.esi = 0;
        if ( bus_call( BUS_PCIID, &r ) ) {
            pciaddr = r.ebx;
            found = 1;
        }
    }
    if ( !found ) return 0;
    i--;
    chip = ids[i].chip;
    /* the registers are in I/O space: 64 bytes of it, and a read
       that failed (all ones) is no address at all */
    bar = cfg_read( pciaddr, 0x10, 4 );
    if ( bar == 0xFFFFFFFFUL || !( bar & 1 ) || ( bar & 0xFFFC ) == 0 || ( bar & 0xFFFC ) > 0xFFC0 ) return 0;
    io = (u16)( bar & 0xFFFC );
    rev = cfg_read( pciaddr, 0x3C, 1 );
    irq = rev != 0 && rev < 16 ? (u8)rev : AUD_NONE;
    mem_zero( &r, sizeof( r ) );
    r.ebx = pciaddr;
    if ( bus_call( BUS_PCIIRQ, &r ) && ( r.eax & 0xFF ) < 16 && ( r.eax & 0xFF ) != 0 ) irq = (u8)( r.eax & 0xFF );
    /* I/O, memory and bus mastering on */
    rev = cfg_read( pciaddr, 0x04, 2 );
    if ( ( rev & 5 ) != 5 ) cfg_write( pciaddr, 0x04, 2, rev | 5 );
    rev = cfg_read( pciaddr, 0x08, 1 ) & 0xFF;
    if ( ids[i].device == 0x5000 ) {
        str_cpy( aud_info.name, "Ensoniq AudioPCI (ES1370)" );
    } else if ( ids[i].device == 0x5880 || ( ids[i].device == 0x1371 && rev == 7 ) ) {
        str_cpy( aud_info.name, "Creative CT5880" );
    } else if ( ids[i].vendor == 0x1102 ) {
        str_cpy( aud_info.name, "Creative Ectiva EV1938" );
    } else if ( rev >= 4 ) {
        str_cpy( aud_info.name, "Ensoniq AudioPCI (ES1373)" );
    } else {
        str_cpy( aud_info.name, "Ensoniq AudioPCI (ES1371)" );
    }
    aud_info.version_hw = (u16)rev;
    return 1;
}

/* ---------------------------------------------------------------- */
/* the sample-rate converter and the codec (ES1371)                  */
/* ---------------------------------------------------------------- */
static u32 src_ready( void )
{
    u32 i, r = 0;

    for ( i = 0; i < POLL; i++ ) {
        r = inl( io + R_1371_SRC );
        if ( !( r & SRC_RAM_BUSY ) ) break;
    }
    return r;
}

static void src_write( u32 reg, u32 data )
{
    u32 r = src_ready() & SRC_KEEP;

    outl( io + R_1371_SRC, r | SRC_ADDR( reg ) | ( data & 0xFFFF ) | SRC_RAM_WE );
}

static u32 src_read( u32 reg )
{
    u32 r, temp, i;

    /* 10000h shows the converter's state bits, so the right moment to
       read can be waited for (as Linux's driver does) */
    r = src_ready() & SRC_KEEP;
    outl( io + R_1371_SRC, r | SRC_ADDR( reg ) | 0x00010000UL );
    temp = src_ready();
    if ( ( temp & 0x00870000UL ) != 0x00010000UL ) {
        for ( i = 0; i < POLL; i++ ) {
            temp = inl( io + R_1371_SRC );
            if ( ( temp & 0x00870000UL ) == 0x00010000UL ) break;
        }
    }
    r = src_ready() & SRC_KEEP;
    outl( io + R_1371_SRC, r | SRC_ADDR( reg ) );
    return temp & 0xFFFF;
}

/* DAC2's rate: frequency * 2^15 / 3000, split into the integer
   register and the fraction */
static void src_dac2_rate( u32 rate )
{
    u32 freq = ( ( rate << 15 ) + 1500 ) / 3000, r;

    r = ( src_ready() & ( SRC_DISABLE | SRC_DIS_P1 | SRC_DIS_R1 ) ) | SRC_DIS_P2;
    outl( io + R_1371_SRC, r );
    src_write( SM_DAC2 + SM_INT_REGS, ( src_read( SM_DAC2 + SM_INT_REGS ) & 0x00FF ) | ( ( freq >> 5 ) & 0xFC00 ) );
    src_write( SM_DAC2 + SM_VFREQ_FRAC, freq & 0x7FFF );
    r = src_ready() & ( SRC_DISABLE | SRC_DIS_P1 | SRC_DIS_R1 );
    outl( io + R_1371_SRC, r );
}

static void src_dac1_rate( u32 rate )
{
    u32 freq = ( ( rate << 15 ) + 1500 ) / 3000, r;

    r = ( src_ready() & ( SRC_DISABLE | SRC_DIS_P2 | SRC_DIS_R1 ) ) | SRC_DIS_P1;
    outl( io + R_1371_SRC, r );
    src_write( SM_DAC1 + SM_INT_REGS, ( src_read( SM_DAC1 + SM_INT_REGS ) & 0x00FF ) | ( ( freq >> 5 ) & 0xFC00 ) );
    src_write( SM_DAC1 + SM_VFREQ_FRAC, freq & 0x7FFF );
    r = src_ready() & ( SRC_DISABLE | SRC_DIS_P2 | SRC_DIS_R1 );
    outl( io + R_1371_SRC, r );
}

static void src_init( void )
{
    u32 i;

    src_ready();
    outl( io + R_1371_SRC, SRC_DISABLE );
    for ( i = 0; i < 0x80; i++ ) src_write( i, 0 );
    src_write( SM_DAC1 + SM_TRUNC_N, 16 << 4 );
    src_write( SM_DAC1 + SM_INT_REGS, 16 << 10 );
    src_write( SM_DAC2 + SM_TRUNC_N, 16 << 4 );
    src_write( SM_DAC2 + SM_INT_REGS, 16 << 10 );
    src_write( SM_VOL_ADC, 1 << 12 );
    src_write( SM_VOL_ADC + 1, 1 << 12 );
    src_write( SM_VOL_DAC1, 1 << 12 );
    src_write( SM_VOL_DAC1 + 1, 1 << 12 );
    src_write( SM_VOL_DAC2, 1 << 12 );
    src_write( SM_VOL_DAC2 + 1, 1 << 12 );
    src_dac1_rate( 22050 );
    src_dac2_rate( 22050 );
    src_ready();
    outl( io + R_1371_SRC, 0 );
}

/* a codec write, in a moment the converter is not using the bus */
static void ac97_write( u32 reg, u32 value )
{
    u32 t, saved, i;

    for ( t = 0; t < POLL; t++ ) {
        if ( inl( io + R_1371_CODEC ) & CD_WIP ) continue;
        saved = src_ready();
        outl( io + R_1371_SRC, ( saved & SRC_KEEP ) | 0x00010000UL );
        for ( i = 0; i < POLL && ( inl( io + R_1371_SRC ) & 0x00870000UL ) != 0; i++ ) ;
        for ( i = 0; i < POLL && ( inl( io + R_1371_SRC ) & 0x00870000UL ) != 0x00010000UL; i++ ) ;
        outl( io + R_1371_CODEC, CD_WRITE( reg, value ) );
        src_ready();
        outl( io + R_1371_SRC, saved );
        return;
    }
}

static u32 ac97_read( u32 reg )
{
    u32 t, saved, i, value;

    for ( t = 0; t < POLL; t++ ) {
        if ( inl( io + R_1371_CODEC ) & CD_WIP ) continue;
        saved = src_ready();
        outl( io + R_1371_SRC, ( saved & SRC_KEEP ) | 0x00010000UL );
        for ( i = 0; i < POLL && ( inl( io + R_1371_SRC ) & 0x00870000UL ) != 0; i++ ) ;
        for ( i = 0; i < POLL && ( inl( io + R_1371_SRC ) & 0x00870000UL ) != 0x00010000UL; i++ ) ;
        outl( io + R_1371_CODEC, CD_WRITE( reg, 0 ) | CD_PIRD );
        src_ready();
        outl( io + R_1371_SRC, saved );
        for ( i = 0; i < POLL; i++ ) {
            value = inl( io + R_1371_CODEC );
            if ( value & CD_RDY ) return value & 0xFFFF;
        }
        return 0xFFFF;
    }
    return 0xFFFF;
}

/* ---------------------------------------------------------------- */
/* the AK4531 mixer (ES1370)                                         */
/* ---------------------------------------------------------------- */
static void ak4531_write( u32 reg, u32 value )
{
    u32 i;

    for ( i = 0; i < POLL; i++ ) {
        if ( !( inl( io + R_STATUS ) & S_1370_CSTAT ) ) break;
    }
    outw( io + R_1370_CODEC, (u16)( ( ( reg & 0xFF ) << 8 ) | ( value & 0xFF ) ) );
    if ( reg < sizeof( ak4531 ) ) ak4531[reg] = (u8)value;
}

/* ---------------------------------------------------------------- */
/* the card                                                          */
/* ---------------------------------------------------------------- */
static void chip_init( void )
{
    u32 i;

    if ( chip == CHIP_1371 ) {
        ctrl = 0;
        outl( io + R_CONTROL, ctrl );
        sctrl = 0;
        outl( io + R_SERIAL, sctrl );
        outl( io + 0x18, 0 );                   /* the legacy (Sound Blaster) traps off */
        /* a warm reset of the codec link */
        outl( io + R_CONTROL, ctrl | C_1371_SYNC_RES );
        for ( i = 0; i < 40; i++ ) inl( io + R_STATUS );
        outl( io + R_CONTROL, ctrl );
        src_init();
        ac97_write( AC97_RESET, 0 );
        for ( i = 0; i < 2000; i++ ) inl( io + R_STATUS );
    } else {
        ctrl = C_1370_CDC_EN | C_1370_PCLKDIV( 1411200UL / 22050 - 2 );
        outl( io + R_CONTROL, ctrl );
        sctrl = 0;
        outl( io + R_SERIAL, sctrl );
        ak4531_write( 0x16, 0x03 );             /* out of reset and power-down */
        for ( i = 0; i < 2000; i++ ) inl( io + R_STATUS );
        ak4531_write( 0x17, 0x00 );             /* the clocks */
        for ( i = 0; i < 0x10; i++ ) ak4531[i] = 0x9F;      /* the chip's own starting state */
        ak4531[0x10] = ak4531[0x11] = 0;
    }
}

int card_find( void )
{
    if ( !find_card() ) return 0;
    chip_init();
    aud_info.flags = AUDF_FOUND | AUDF_PCM | AUDF_MIXER | AUDF_MPU;
    aud_info.bits = AUDB_8 | AUDB_16;
    aud_info.channels = 2;
    aud_info.port = io;
    aud_info.irq = irq;
    aud_info.pciaddr = pciaddr;
    aud_info.mpu_port = (u16)( io + R_UART_DATA );
    if ( chip == CHIP_1371 ) {
        aud_info.rate_min = 4000;
        aud_info.rate_max = aud_info.rate_max2 = 48000;
        aud_info.flags |= AUDF_ANYRATE;
    } else {
        aud_info.rate_min = 5512;
        aud_info.rate_max = aud_info.rate_max2 = 44100;
    }
    return 1;
}

/* what an opening sets on the chip - the rate, the output mix, the
   MIDI UART - put on it; again after a park, whose chip_init undoes it */
static void apply_open( void )
{
    if ( chip == CHIP_1370 ) {
        ctrl = ( ctrl & ~C_1370_PCLKDIV( 0x1FFF ) ) | C_1370_PCLKDIV( play_div );
        outl( io + R_CONTROL, ctrl );
        ak4531_write( 0x00, 0x00 );
        ak4531_write( 0x01, 0x00 );
        ak4531_write( 0x11, ak4531[0x11] | 0x0C );  /* the voice into the output */
    } else {
        src_dac2_rate( play_rate );
        ac97_write( AC97_MASTER, 0x0000 );      /* 0 dB, not muted */
    }
    outb( io + R_UART_STATUS, 3 );
    outb( io + R_UART_STATUS, 0 );
    ctrl |= C_UART_EN;
    outl( io + R_CONTROL, ctrl );
}

int card_open( AUD_OPENBLOCK *fmt )
{
    u32 rate = fmt->rate, frame, want, count, div;

    if ( dma_buf == 0 ) return ERR_GENERAL;
    if ( rate < aud_info.rate_min ) rate = aud_info.rate_min;
    if ( rate > aud_info.rate_max ) rate = aud_info.rate_max;
    if ( chip == CHIP_1370 ) {
        /* 1411200 / (div + 2), the nearest */
        div = ( 1411200UL + rate / 2 ) / rate;
        div = div < 2 ? 0 : div - 2;
        rate = 1411200UL / ( div + 2 );
        play_div = div;
    }
    play_rate = rate;
    fmt->rate = rate;
    play_mode = (u8)( ( fmt->bits == 16 ? 2 : 0 ) | ( fmt->channels == 2 ? 1 : 0 ) );
    frame = (u32)( fmt->bits / 8 ) * fmt->channels;
    want = fmt->block ? fmt->block : rate * frame / 50;
    if ( want < 64 ) want = 64;
    if ( want > dma_size / 2 ) want = dma_size / 2;
    want -= want % ( frame * 4 > 4 ? frame * 4 : 4 );   /* whole longwords and frames */
    if ( want == 0 ) want = 16;
    count = dma_size / want;
    if ( count > AUD_MAXBLK ) count = AUD_MAXBLK;
    blk_bytes = want;
    blk_count = count;
    /* the mixer as it is, to put back at close */
    if ( chip == CHIP_1371 ) {
        old_master = (u16)ac97_read( AC97_MASTER );
        old_pcm = (u16)ac97_read( AC97_PCM );
    } else {
        saved_1370[0] = ak4531[0x00];
        saved_1370[1] = ak4531[0x01];
        saved_1370[2] = ak4531[0x02];
        saved_1370[3] = ak4531[0x03];
    }
    apply_open();
    is_open = 1;
    return 0;
}

void card_start( void )
{
    static const u8 shift[4] = { 0, 1, 1, 2 };
    u32 frames;

    ctrl &= ~C_DAC2_EN;
    outl( io + R_CONTROL, ctrl );
    outl( io + R_MEM_PAGE, PAGE_DAC );
    outl( io + R_DAC2_FRAME, dma_phys );
    outl( io + R_DAC2_SIZE, ( blk_bytes * blk_count >> 2 ) - 1 );
    sctrl &= ~( SR_P2_LOOP_SEL | SR_P2_PAUSE | SR_P2_DAC_SEN | SR_P2_INCMASK | SR_P2_MODEMASK );
    sctrl |= SR_P2_INT_EN | SR_P2_MODE( play_mode ) | SR_P2_END_INC( play_mode & 2 ? 2 : 1 ) | SR_P2_ST_INC( 0 );
    outl( io + R_SERIAL, sctrl );
    frames = blk_bytes >> shift[play_mode];
    outl( io + R_DAC2_COUNT, frames - 1 );
    ctrl |= C_DAC2_EN;
    outl( io + R_CONTROL, ctrl );
    playing = 1;
}

void card_halt( void )
{
    ctrl &= ~C_DAC2_EN;
    outl( io + R_CONTROL, ctrl );
    sctrl &= ~SR_P2_INT_EN;
    outl( io + R_SERIAL, sctrl );
    playing = 0;
}

int card_pause( void )
{
    if ( !playing ) return 0;
    sctrl |= SR_P2_PAUSE;
    outl( io + R_SERIAL, sctrl );
    return 1;
}

void card_resume( void )
{
    sctrl &= ~SR_P2_PAUSE;
    outl( io + R_SERIAL, sctrl );
}

void card_close( void )
{
    card_halt();
    ctrl &= ~C_UART_EN;
    outl( io + R_CONTROL, ctrl );
    if ( chip == CHIP_1371 ) {
        if ( old_master != 0xFFFF ) ac97_write( AC97_MASTER, old_master );
        if ( old_pcm != 0xFFFF ) ac97_write( AC97_PCM, old_pcm );
        old_master = old_pcm = 0xFFFF;
    } else {
        ak4531_write( 0x00, saved_1370[0] );
        ak4531_write( 0x01, saved_1370[1] );
        ak4531_write( 0x02, saved_1370[2] );
        ak4531_write( 0x03, saved_1370[3] );
    }
    is_open = 0;
}

u32 card_dma_pos( void )
{
    u32 pos;

    outl( io + R_MEM_PAGE, PAGE_DAC );
    pos = ( ( inl( io + R_DAC2_SIZE ) >> 16 ) & 0xFFFF ) * 4;
    if ( pos >= blk_bytes * blk_count ) pos = 0;
    return pos;
}

int card_irq( void )
{
    u32 status = inl( io + R_STATUS );

    if ( !( status & S_INTR ) ) return CARD_NOTMINE;
    if ( status & S_UART ) card_mpu_drain();
    if ( status & S_DAC2 ) {
        /* acknowledged by switching its interrupt off and on */
        outl( io + R_SERIAL, sctrl & ~SR_P2_INT_EN );
        outl( io + R_SERIAL, sctrl );
        return CARD_BLOCK;
    }
    return CARD_OTHER;
}

/* the amplitude onto the codec's attenuation, at most 31: AC'97 PCM
   Out in 1.5 dB steps from 8 (0 is +12 dB, 8 is 0 dB), the AK4531's
   voice in 2 dB steps from 6 (0 is +12 dB there too); nothing at all
   is muted */
void card_volume( u8 left, u8 right )
{
    u32 al = aud_atten( left ), ar = aud_atten( right );

    vol_l = left;
    vol_r = right;
    if ( chip == CHIP_1371 ) {
        al = 8 + al / 3 > 31 ? 31 : 8 + al / 3;
        ar = 8 + ar / 3 > 31 ? 31 : 8 + ar / 3;
        ac97_write( AC97_PCM, ( left || right ? 0 : 0x8000 ) | ( al << 8 ) | ar );
    } else {
        al = 6 + al / 4 > 31 ? 31 : 6 + al / 4;
        ar = 6 + ar / 4 > 31 ? 31 : 6 + ar / 4;
        ak4531_write( 0x02, left ? al : 0x9F );
        ak4531_write( 0x03, right ? ar : 0x9F );
    }
}

void card_opl( u8 bank, u8 reg, u8 val )
{
    (void)bank;
    (void)reg;
    (void)val;
}

void card_mpu_drain( void )
{
    int i;

    for ( i = 0; i < 64 && ( inb( io + R_UART_STATUS ) & 1 ); i++ ) inb( io + R_UART_DATA );
}

int card_mpu_put( u8 byte )
{
    if ( !( inb( io + R_UART_STATUS ) & 2 ) ) return 0;
    outb( io + R_UART_DATA, byte );
    return 1;
}

/* back from real mode the chip is set up from the start, as whatever
   ran there may have left it anyhow; a card that is open gets its
   rate, mix, UART and volume back (AUDCORE starts it again) */
void card_park( int back )
{
    if ( !back ) {
        card_halt();
    } else {
        chip_init();
        if ( is_open ) {
            apply_open();
            card_volume( vol_l, vol_r );
        }
    }
}

int card_probe_irq( u32 line )
{
    (void)line;
    return 0;
}

/* ---------------------------------------------------------------- */
/* the entries                                                       */
/* ---------------------------------------------------------------- */
const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_AUDIO,
    { 'E', 'S', '1', '3', '7', '1', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};

int __cdecl nd_init( const char *line )
{
    (void)line;
    aud_core_init();
    if ( !card_find() ) {
        aud_print( "ES1371.SYS: no Ensoniq AudioPCI or Creative CT5880 was found\r\n" );
        return 1;
    }
    /* a bus master reaches all memory: the buffer need only be in one piece */
    if ( !aud_alloc_dma( 32768UL, 0 ) ) {
        aud_print( "ES1371.SYS: no memory for the card's buffer\r\n" );
        return 1;
    }
    nds_tickhook();
    aud_say_card( "ES1371.SYS" );
    return 0;
}
