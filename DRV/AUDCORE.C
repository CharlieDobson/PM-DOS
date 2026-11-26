/*
 * AUDCORE.C - what every sound card driver shares: the CALL functions
 * of INC\AUDIO.H, the ring of samples, the queue of timed events, and
 * the interrupt and clock entries (DRV\AUDDRV.H says what a card's
 * own file adds).
 *
 * THREE COUNTS OF BYTES, never reset while the ring is in use, tell
 * where everything is: "written" (what the program has put in),
 * "copied" (what has gone into the card's buffer) and "played" (what
 * the card has played).  The ring holds written - played; the room
 * left is the rest of it.  The ring's size is a power of two, so a
 * count taken modulo the size is a place in it even after the count
 * has gone round 2^32.
 *
 * A BYTE STAYS IN THE RING UNTIL THE CARD HAS PLAYED IT, not merely
 * until it was copied.  That is what makes a pause exact on a card
 * that cannot pause: the card is stopped, the frame it had reached is
 * worked out from its position in the block, and "copied" goes back to
 * that frame - everything after it is still in the ring and is played
 * when the card starts again.
 *
 * BLOCKS ARE DONE WHEN THE CARD HAS LEFT THEM, by its position, not by
 * counting interrupts.  The interrupt, the clock and every call from
 * the program all ask where the card is, and every block it has left
 * since the last time is refilled and counted.  A lost interrupt costs
 * nothing, a stray one finds nothing to do, and a card whose line
 * could not be had is kept going from the clock and the calls alone -
 * with big enough blocks, which card_open chooses when it is told so.
 *
 * EVENTS GO OUT WHEN THEIR FRAME HAS BEEN PLAYED, from the same
 * places.  A MIDI byte the port is not ready for stays at the head of
 * the queue for the next time; nothing waits for the port.
 *
 * IF THE PROGRAM GOES AWAY without closing - it crashed, or was ended
 * with Ctrl+Break - nothing calls any more.  After ten seconds with no
 * call the clock stops the card, so it does not play silence for ever
 * and the next program to open it finds it ready.
 */
#define ND_DRIVER
#include "ndrv.h"
#include "audio.h"
#include "auddrv.h"

/* the bus calls this needs that NDRV.H does not carry (PMDOS.INC BUS_*) */
#ifndef BUS_PHYSOF
#define BUS_PHYSOF   0x11
#endif

AUD_INFOBLOCK aud_info;
uint8_t *dma_buf;
uint32_t dma_phys;
unsigned int dma_size;
unsigned int blk_bytes;
unsigned int blk_count;
unsigned int frame_bytes;

static uint8_t *dma_block;              /* the allocation dma_buf is inside */
static unsigned char opened;            /* a program has the card */
static unsigned char state = AUDST_CLOSED;
static unsigned char hooked;            /* the line is ours */
static unsigned char polled;            /* ...or could not be had */
static uint8_t  silence;                /* the value of a silent byte */
static uint8_t  vol_left = 255, vol_right = 255;

static uint8_t *ring;
static unsigned int ring_size;          /* a power of two */
static uint32_t written, copied, released;
static uint32_t played;                 /* frames, since AUD_OPEN or AUD_STOP */
static unsigned int underruns, irq_count;
static unsigned char was_dry;
static unsigned char had_sound;         /* the card has played something since it started */
static unsigned char pause_kept;        /* the card paused itself: card_resume goes on */

static uint32_t blk_src[AUD_MAXBLK];     /* "copied" at the block's first byte */
static unsigned int blk_len[AUD_MAXBLK]; /* the ring's bytes in it; silence after */
static unsigned int play_blk;            /* the block the card is in */

static AUDEV   *evq;
static unsigned int ev_size;            /* records, a power of two; 0 for none */
static uint32_t ev_in, ev_out;

static uint32_t last_call;              /* the tick of the program's last call */
static unsigned char stale;             /* the watchdog gave the card back; the opening waits */
static unsigned char resume_after;      /* ...and the card was playing then */
static unsigned char park_resume;       /* a park stopped the card while it played */
static AUD_OPENBLOCK open_fmt;          /* the format as opened, to take the card back with */

#define WATCHDOG_TICKS  182             /* ten seconds */
#define RING_DEFAULT    65536UL
#define RING_MAX        (1024UL * 1024UL)
#define EV_MAX          65536UL

/* ---------------------------------------------------------------- */
/* small routines: no C library in a driver                          */
/* ---------------------------------------------------------------- */
void mem_zero( void *dst, unsigned int len )
{
    uint8_t *out = (uint8_t *)dst;

    while ( len-- ) *out++ = 0;
}

void mem_copy( void *dst, const void *src, unsigned int len )
{
    uint8_t *out = (uint8_t *)dst;
    const uint8_t *in = (const uint8_t *)src;

    while ( len >= 4 ) {
        *(uint32_t *)out = *(const uint32_t *)in;
        out += 4;
        in += 4;
        len -= 4;
    }
    while ( len-- ) *out++ = *in++;
}

static void mem_fill( void *dst, uint8_t value, unsigned int len )
{
    uint8_t *out = (uint8_t *)dst;
    uint32_t word = value * 0x01010101UL;

    while ( len >= 4 ) {
        *(uint32_t *)out = word;
        out += 4;
        len -= 4;
    }
    while ( len-- ) *out++ = value;
}

int mem_same( const void *lhs, const void *rhs, unsigned int len )
{
    const uint8_t *one = (const uint8_t *)lhs, *two = (const uint8_t *)rhs;

    while ( len-- ) {
        if ( *one++ != *two++ ) return 0;
    }
    return 1;
}

unsigned int str_len( const char *str )
{
    unsigned int len = 0;

    while ( str[len] ) len++;
    return len;
}

void str_cpy( char *dst, const char *src )
{
    while ( ( *dst++ = *src++ ) != 0 ) ;
}

void str_cat( char *dst, const char *src )
{
    str_cpy( dst + str_len( dst ), src );
}

void str_hex( char *dst, uint32_t value, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    char *at = dst + str_len( dst );
    int i;

    for ( i = digits - 1; i >= 0; i-- ) {
        at[i] = hex[value & 15];
        value >>= 4;
    }
    at[digits] = 0;
}

void str_dec( char *dst, unsigned int value )
{
    char tmp[12];
    int len = 0;
    char *at = dst + str_len( dst );

    do {
        tmp[len++] = (char)( '0' + value % 10 );
        value /= 10;
    } while ( value );
    while ( len ) *at++ = tmp[--len];
    *at = 0;
}

/* a number at text: decimal, or hexadecimal with "0x" or without */
uint32_t num_after( const char *text, int hex, uint32_t none )
{
    uint32_t value = 0;
    int digits = 0, digit;
    char ch;

    if ( hex && text[0] == '0' && ( text[1] | 0x20 ) == 'x' ) text += 2;
    for ( ;; text++ ) {
        ch = *text;
        if ( ch >= '0' && ch <= '9' ) {
            digit = ch - '0';
        } else if ( hex && ( ch | 0x20 ) >= 'a' && ( ch | 0x20 ) <= 'f' ) {
            digit = ( ch | 0x20 ) - 'a' + 10;
        } else {
            break;
        }
        if ( value > 0x0FFFFFFFUL ) {
            value = 0xFFFFFFFFUL;               /* too long: stays too big */
        } else {
            value = value * ( hex ? 16 : 10 ) + digit;
        }
        digits++;
    }
    return digits ? value : none;
}

/* An amplitude 0..255 as an attenuation in half decibels, for a mixer
   that counts in decibels: each step of 0.944 (half a decibel) down
   from 255 is one more.  Off (0) is 255; the most otherwise is 96
   (48 dB, the bottom of 255 to 1).  At CALL time only: it loops. */
uint32_t aud_atten( uint8_t level )
{
    uint32_t at = 255UL << 16, want = (uint32_t)level << 16, steps = 0;

    if ( level == 0 ) return 255;
    while ( at > want && steps < 120 ) {
        at = ( at >> 10 ) * 967;            /* 967 / 1024 = 0.9443: half a decibel */
        steps++;
    }
    return steps;
}

void aud_print( const char *text )
{
    nds_print( text );
}

uint32_t aud_ticks( void )
{
    return nds_ticks();
}

void aud_delay_ticks( unsigned int ticks )
{
    uint32_t start = nds_ticks();

    while ( nds_ticks() - start < ticks ) ;
}

/* "NAME.SYS: Sound Blaster 16 at 220h, IRQ 5, DMA 1 and 5, FM at
   388h, MIDI at 330h" */
void aud_say_card( const char *driver )
{
    char text[160];

    str_cpy( text, driver );
    str_cat( text, ": " );
    str_cat( text, aud_info.name );
    if ( aud_info.port ) {
        str_cat( text, " at " );
        str_hex( text, aud_info.port, aud_info.port > 0xFFF ? 4 : 3 );
        str_cat( text, "h" );
    }
    if ( aud_info.irq != AUD_NONE ) {
        str_cat( text, ", IRQ " );
        str_dec( text, aud_info.irq );
    }
    if ( aud_info.dma8 != AUD_NONE ) {
        str_cat( text, ", DMA " );
        str_dec( text, aud_info.dma8 );
        if ( aud_info.dma16 != AUD_NONE && aud_info.dma16 != aud_info.dma8 ) {
            str_cat( text, " and " );
            str_dec( text, aud_info.dma16 );
        }
    }
    if ( aud_info.fm_port ) {
        str_cat( text, aud_info.flags & AUDF_OPL3 ? ", OPL3 at " : ", OPL2 at " );
        str_hex( text, aud_info.fm_port, aud_info.fm_port > 0xFFF ? 4 : 3 );
        str_cat( text, "h" );
    }
    if ( aud_info.mpu_port ) {
        str_cat( text, ", MIDI at " );
        str_hex( text, aud_info.mpu_port, aud_info.mpu_port > 0xFFF ? 4 : 3 );
        str_cat( text, "h" );
    }
    str_cat( text, "\r\n" );
    nds_print( text );
}

/* The buffer the card plays from: "bytes" (a power of two) in one
   piece, below "limit", and - for an ISA card, limit 16MB - inside one
   64K page, because the 8237 cannot carry across one.  Twice the size
   is taken so that a window aligned on its own size is always inside;
   an aligned window of a power of two up to 64K never crosses a 64K
   line.  The arena hands out the lowest memory first at INIT, so a
   driver loaded by DEVICE= gets memory well below 16MB. */
int aud_alloc_dma( unsigned int bytes, uint32_t limit )
{
    REGS r;
    uint32_t lin, phys;

    dma_block = (uint8_t *)nds_alloc( bytes * 2 );
    if ( dma_block == 0 ) return 0;
    lin = ( (uint32_t)dma_block + bytes - 1 ) & ~( bytes - 1 );
    mem_zero( &r, sizeof( r ) );
    r.eax = (uint32_t)BUS_PHYSOF << 8;
    r.ebx = lin;
    r.ecx = bytes;
    if ( !nds_bus( &r ) || r.ecx < bytes ) {
        nds_free( dma_block );
        dma_block = 0;
        return 0;
    }
    phys = r.eax;
    if ( ( limit && ( phys >= limit || phys + bytes > limit ) )
         || ( limit == 0x1000000UL && ( phys >> 16 ) != ( ( phys + bytes - 1 ) >> 16 ) ) ) {
        nds_free( dma_block );
        dma_block = 0;
        return 0;
    }
    dma_buf = (uint8_t *)lin;
    dma_phys = phys;
    dma_size = bytes;
    return 1;
}

int aud_core_init( void )
{
    mem_zero( &aud_info, sizeof( aud_info ) );
    aud_info.sig = AUD_SIGVAL;
    aud_info.version = AUD_VERSION;
    aud_info.irq = aud_info.dma8 = aud_info.dma16 = AUD_NONE;
    aud_info.ring_max = RING_MAX;
    return 1;
}

/* ---------------------------------------------------------------- */
/* the ring and the blocks (interrupts off for all of these)         */
/* ---------------------------------------------------------------- */
/* block "b" from the ring, silence after what there is */
static void fill_block( unsigned int b )
{
    uint8_t *dst = dma_buf + b * blk_bytes;
    unsigned int have = written - copied, take, at, first;

    take = have < blk_bytes ? have : blk_bytes;
    blk_src[b] = copied;
    blk_len[b] = take;
    if ( take ) {
        at = copied & ( ring_size - 1 );
        first = ring_size - at;
        if ( first > take ) first = take;
        mem_copy( dst, ring + at, first );
        if ( take > first ) mem_copy( dst + first, ring, take - first );
        copied += take;
    }
    if ( take < blk_bytes ) mem_fill( dst + take, silence, blk_bytes - take );
}

/* block "b" all silence, its bytes to come from the ring later */
static void blank_block( unsigned int b )
{
    mem_fill( dma_buf + b * blk_bytes, silence, blk_bytes );
    blk_src[b] = copied;
    blk_len[b] = 0;
}

/* Blocks the card has not reached yet that hold silence, given what
   the ring has now: the first block after the card's that is not full
   is topped up, and the empty ones after it filled.  Without this a
   card started on a short ring, or one that ran dry, would play every
   block of silence already queued before the new sound - a gap of up
   to the whole buffer.  The block the card is in is never touched.
   Blocks ahead of the card always hold the ring's bytes in order with
   only the last of them partly filled, so appending keeps the order. */
static void top_up( void )
{
    unsigned int n, b, have, take, at, first, used;
    uint8_t *dst;

    if ( state != AUDST_PLAYING && state != AUDST_DRY ) return;
    for ( n = 1; n < blk_count; n++ ) {
        have = written - copied;
        if ( have == 0 ) return;
        b = ( play_blk + n ) % blk_count;
        used = blk_len[b];
        if ( used >= blk_bytes ) continue;
        if ( used == 0 ) blk_src[b] = copied;
        take = have < blk_bytes - used ? have : blk_bytes - used;
        dst = dma_buf + b * blk_bytes + used;
        at = copied & ( ring_size - 1 );
        first = ring_size - at;
        if ( first > take ) first = take;
        mem_copy( dst, ring + at, first );
        if ( take > first ) mem_copy( dst + first, ring, take - first );
        copied += take;
        blk_len[b] = used + take;
    }
}

/* the frames into the block the card is in, by its position */
static unsigned int into_block( void )
{
    unsigned int pos = card_dma_pos(), start = play_blk * blk_bytes;

    if ( pos < start || pos >= start + blk_bytes ) return blk_bytes - frame_bytes;
    return ( pos - start ) / frame_bytes * frame_bytes;
}

static void send_events( uint32_t now );

/* every block the card has left since the last look, counted and
   made the last in line; the blocks ahead topped up from the ring;
   then the events that are due.  Running dry is entering a block that
   has nothing in it, after there was sound - the end of a file is one
   too, and the program knows to leave that one out */
static void catch_up( void )
{
    unsigned int pos, start, guard;

    if ( state != AUDST_PLAYING && state != AUDST_DRY ) return;
    for ( guard = 0; guard < blk_count; guard++ ) {
        pos = card_dma_pos();
        start = play_blk * blk_bytes;
        if ( pos >= start && pos < start + blk_bytes ) break;
        /* the block is done: its bytes are played, and it is free */
        if ( blk_len[play_blk] ) released = blk_src[play_blk] + blk_len[play_blk];
        blank_block( play_blk );
        played += blk_bytes / frame_bytes;
        play_blk = ( play_blk + 1 ) % blk_count;
        if ( blk_len[play_blk] ) {
            had_sound = 1;
            was_dry = 0;
            state = AUDST_PLAYING;
        } else {
            if ( !was_dry && had_sound ) underruns++;
            was_dry = 1;
            state = AUDST_DRY;
        }
    }
    top_up();
    send_events( played + into_block() / frame_bytes );
}

/* ---------------------------------------------------------------- */
/* events                                                            */
/* ---------------------------------------------------------------- */
static void send_events( uint32_t now )
{
    AUDEV *ev;

    while ( ev_out != ev_in ) {
        ev = &evq[ev_out & ( ev_size - 1 )];
        if ( (long)( now - ev->frame ) < 0 ) break;
        if ( ev->kind == AUDE_OPL ) {
            if ( aud_info.flags & ( AUDF_OPL2 | AUDF_OPL3 ) ) card_opl( ev->bank, ev->reg, ev->val );
        } else if ( ev->kind == AUDE_MPU && ( aud_info.flags & AUDF_MPU ) ) {
            if ( !card_mpu_put( ev->val ) ) break;          /* the port is busy: later */
        }
        ev_out++;
    }
}

/* ---------------------------------------------------------------- */
/* starting and stopping                                             */
/* ---------------------------------------------------------------- */
static void start_card( void )
{
    unsigned int b;

    copied = released;
    for ( b = 0; b < blk_count; b++ ) blank_block( b );
    play_blk = 0;
    fill_block( 0 );
    was_dry = 0;
    had_sound = blk_len[0] != 0;
    state = AUDST_PLAYING;
    top_up();
    card_start();
}

/* the card stopped where it is, and the ring put back to that frame.
   The blocks it finished since the last look are counted first, or a
   block it had moved on from would be measured as nearly all played */
static void stop_exact( void )
{
    unsigned int into;

    catch_up();
    into = into_block();
    card_halt();
    if ( blk_len[play_blk] ) {
        released = blk_src[play_blk] + ( into < blk_len[play_blk] ? into : blk_len[play_blk] );
    }
    copied = released;
    played += into / frame_bytes;
}

/* CALL time only (it frees).  The card is stopped and the opening let
   go with interrupts off, so neither the tick nor the card's interrupt
   can reach the ring, the events or the chip once it is being shut;
   the slow part - the mixer, the FM chip, the MIDI port - and the
   memory come after */
static void close_card( void )
{
    uint32_t flags;
    uint8_t *old_ring;
    AUDEV *old_evq;
    int was_stale;

    if ( !opened ) return;
    flags = int_off();
    if ( !stale && ( state == AUDST_PLAYING || state == AUDST_DRY || state == AUDST_PAUSED ) ) card_halt();
    state = AUDST_CLOSED;
    ev_out = ev_in;
    old_ring = ring;
    old_evq = evq;
    ring = 0;
    evq = 0;
    ev_size = 0;
    was_stale = stale;
    stale = 0;
    resume_after = 0;
    park_resume = 0;
    opened = 0;
    int_back( flags );
    if ( !was_stale ) card_close();          /* given back already: nothing to put back */
    if ( hooked ) nds_irqunhook( aud_info.irq );
    hooked = 0;
    if ( old_ring ) nds_free( old_ring );
    if ( old_evq ) nds_free( old_evq );
    aud_info.flags &= ~AUDF_BUSY;
}

/* The card given back - by the watchdog, when no call has come for
   ten seconds (a program that ended without AUD_CLOSE), and at HALT.
   It is stopped where it is, the mixer and the MIDI port put back and
   the FM chip quiet, and its interrupts are passed on, so DOS programs
   find the card as it was.  The ring, the events and the counts are
   kept: the memory can be freed only at CALL time, and a program that
   was only slow gets the card back at its next call (take_back).
   Interrupts are off */
static void give_back( void )
{
    resume_after = (unsigned char)( state == AUDST_PLAYING || state == AUDST_DRY );
    if ( resume_after || ( state == AUDST_PAUSED && pause_kept ) ) {
        stop_exact();
        pause_kept = 0;
        state = AUDST_PAUSED;
    }
    card_close();
    stale = 1;
    aud_info.flags &= ~AUDF_BUSY;
}

/* the card opened again as it was, for a program that is still there;
   playing again if it was playing.  -> 1, or 0 and the opening closed */
static int take_back( void )
{
    AUD_OPENBLOCK fmt;
    unsigned int old_bytes = blk_bytes, old_count = blk_count;
    uint32_t flags;
    int rc;

    mem_copy( &fmt, &open_fmt, sizeof( fmt ) );
    rc = card_open( &fmt );
    if ( rc == 0 ) stale = 0;                /* open again: close_card puts it back */
    if ( rc != 0 || blk_bytes != old_bytes || blk_count != old_count ) {
        close_card();
        return 0;
    }
    flags = int_off();
    aud_info.flags |= AUDF_BUSY;
    card_volume( vol_left, vol_right );
    if ( resume_after ) start_card();
    resume_after = 0;
    int_back( flags );
    return 1;
}

static unsigned int pow2_at_least( unsigned int want, unsigned int most )
{
    unsigned int size = 1;

    while ( size < want && size < most ) size <<= 1;
    return size;
}

static int open_card( AUD_OPENBLOCK *fmt )
{
    unsigned int want_ring, want_ev;
    uint32_t flags;
    int rc;

    if ( opened ) close_card();              /* a program that ended without closing */
    if ( !( aud_info.flags & AUDF_FOUND ) && !card_find() ) return ERR_GENERAL;
    if ( fmt->bits != 8 && fmt->bits != 16 ) fmt->bits = 16;
    if ( fmt->channels != 1 && fmt->channels != 2 ) fmt->channels = 2;
    want_ring = fmt->ring ? fmt->ring : RING_DEFAULT;
    want_ev = fmt->events;
    rc = card_open( fmt );                   /* the format given, and the blocks */
    if ( rc ) return rc;
    frame_bytes = (unsigned int)( fmt->bits / 8 ) * fmt->channels;
    silence = (uint8_t)( fmt->bits == 8 ? 0x80 : 0x00 );
    if ( blk_count < 2 || blk_count > AUD_MAXBLK || blk_bytes * blk_count > dma_size
         || blk_bytes % frame_bytes ) {
        card_close();
        return ERR_GENERAL;
    }
    ring_size = pow2_at_least( want_ring, RING_MAX );
    if ( ring_size < blk_bytes * blk_count * 2 ) ring_size = pow2_at_least( blk_bytes * blk_count * 2, RING_MAX );
    ring = (uint8_t *)nds_alloc( ring_size );
    while ( ring == 0 && ring_size > blk_bytes * blk_count * 2 && ring_size > 4096 ) {
        ring_size >>= 1;                     /* less, if that is all there is */
        ring = (uint8_t *)nds_alloc( ring_size );
    }
    if ( ring == 0 ) {
        card_close();
        return ERR_GENERAL;
    }
    ev_size = 0;
    if ( want_ev ) {
        ev_size = pow2_at_least( want_ev, EV_MAX );
        evq = (AUDEV *)nds_alloc( ev_size * sizeof( AUDEV ) );
        while ( evq == 0 && ev_size > 256 ) {
            ev_size >>= 1;
            evq = (AUDEV *)nds_alloc( ev_size * sizeof( AUDEV ) );
        }
        if ( evq == 0 ) ev_size = 0;
    }
    written = copied = released = 0;
    played = 0;
    underruns = 0;                           /* counted for this opening only */
    ev_in = ev_out = 0;
    play_blk = 0;
    polled = 1;
    if ( aud_info.irq != AUD_NONE && nds_irqhook( aud_info.irq ) ) {
        hooked = 1;
        polled = 0;
    }
    stale = 0;
    resume_after = 0;
    park_resume = 0;
    opened = 1;
    state = AUDST_IDLE;
    aud_info.flags |= AUDF_BUSY;
    flags = int_off();                       /* the SB16's interrupt moves the mixer's index */
    card_volume( vol_left, vol_right );
    int_back( flags );
    fmt->ring = ring_size;
    fmt->block = blk_bytes;
    fmt->events = ev_size;
    mem_copy( &open_fmt, fmt, sizeof( open_fmt ) );
    return 0;
}

/* ---------------------------------------------------------------- */
/* the entries                                                       */
/* ---------------------------------------------------------------- */
int __cdecl nd_irq( uint32_t line )
{
    int rc;

    if ( !opened ) return card_probe_irq( line );
    if ( stale || !hooked || line != aud_info.irq ) return 0;     /* given back: for DOS programs */
    rc = card_irq();
    if ( rc == CARD_NOTMINE ) return 0;
    if ( rc == CARD_BLOCK ) irq_count++;
    catch_up();
    return 1;
}

void __cdecl nd_tick( void )
{
    uint32_t flags;

    if ( !opened || stale ) return;
    flags = int_off();
    if ( state == AUDST_PLAYING || state == AUDST_DRY ) catch_up();
    if ( nds_ticks() - last_call > WATCHDOG_TICKS ) {
        give_back();                         /* nobody has called: the card goes back */
    } else if ( aud_info.flags & AUDF_MPU ) {
        card_mpu_drain();
    }
    int_back( flags );
}

/* to real mode and back: a card that was playing is stopped where it
   is - one paused by the card too, since the card's park stops it -
   and started again on the way back */
void __cdecl nd_park( int back )
{
    uint32_t flags = int_off();

    if ( !back && opened && !stale ) {
        park_resume = (unsigned char)( state == AUDST_PLAYING || state == AUDST_DRY );
        if ( park_resume || ( state == AUDST_PAUSED && pause_kept ) ) {
            stop_exact();
            pause_kept = 0;
            state = AUDST_PAUSED;
        }
    }
    card_park( back );
    if ( back && opened && !stale && park_resume ) {
        park_resume = 0;
        start_card();
    }
    int_back( flags );
}

/* HALT may come with interrupts off and may not free: the card is
   stopped and given back, and that is all */
void __cdecl nd_halt( void )
{
    uint32_t flags = int_off();

    if ( opened && !stale ) give_back();
    int_back( flags );
}

int __cdecl nd_call( uint32_t fn, void *buf, uint32_t len, uint32_t *answer )
{
    AUD_STATBLOCK *st;
    AUDEV *ev;
    uint32_t flags;
    unsigned int room, take, at, first, count, i;

    last_call = nds_ticks();
    *answer = 0;
    /* a card the watchdog gave back is taken again for the program that
       is still there - but not to be closed, opened anew or described */
    if ( opened && stale && fn != AUD_OPEN && fn != AUD_CLOSE && fn != AUD_INFO ) take_back();
    if ( opened && !stale ) {
        flags = int_off();
        catch_up();
        int_back( flags );
    }
    switch ( fn ) {
    case AUD_INFO:
        if ( len < AUDI_LEN ) return ERR_BADLEN;
        if ( !( aud_info.flags & AUDF_FOUND ) ) card_find();
        mem_copy( buf, &aud_info, AUDI_LEN );
        *answer = AUDI_LEN;
        return 0;

    case AUD_OPEN:
        if ( len < AUDO_LEN ) return ERR_BADLEN;
        return open_card( (AUD_OPENBLOCK *)buf );

    case AUD_WRITE:
        if ( !opened ) return ERR_GENERAL;
        room = ring_size - ( written - released );
        take = len < room ? len : room;
        take -= take % frame_bytes;
        if ( take ) {
            at = written & ( ring_size - 1 );
            first = ring_size - at;
            if ( first > take ) first = take;
            mem_copy( ring + at, buf, first );
            if ( take > first ) mem_copy( ring, (const uint8_t *)buf + first, take - first );
            written += take;                 /* after the copy: the interrupt reads it */
            flags = int_off();
            top_up();
            int_back( flags );
        }
        *answer = take;
        return 0;

    case AUD_STATUS:
        if ( len < AUDS_LEN ) return ERR_BADLEN;
        st = (AUD_STATBLOCK *)buf;
        mem_zero( st, AUDS_LEN );
        flags = int_off();
        if ( opened ) {
            st->played = played;
            if ( state == AUDST_PLAYING || state == AUDST_DRY ) st->played += into_block() / frame_bytes;
            st->queued = written - released;
            st->room = ring_size - ( written - released );
            st->room -= st->room % frame_bytes;
            st->ev_queued = ev_in - ev_out;
            st->ev_room = ev_size - ( ev_in - ev_out );
        }
        st->underruns = underruns;
        st->irqs = irq_count;
        st->state = state;
        int_back( flags );
        *answer = AUDS_LEN;
        return 0;

    case AUD_START:
        if ( !opened ) return ERR_GENERAL;
        flags = int_off();
        if ( state == AUDST_IDLE ) {
            start_card();
        } else if ( state == AUDST_PAUSED ) {
            if ( pause_kept ) {
                card_resume();
                state = AUDST_PLAYING;
            } else {
                start_card();
            }
        }
        int_back( flags );
        return 0;

    case AUD_PAUSE:
        if ( !opened ) return ERR_GENERAL;
        flags = int_off();
        if ( state == AUDST_PLAYING || state == AUDST_DRY ) {
            if ( card_pause() ) {
                pause_kept = 1;
            } else {
                pause_kept = 0;
                stop_exact();
            }
            state = AUDST_PAUSED;
        }
        int_back( flags );
        return 0;

    case AUD_STOP:
        if ( !opened ) return ERR_GENERAL;
        flags = int_off();
        if ( state != AUDST_IDLE ) card_halt();
        written = copied = released = 0;
        played = 0;
        ev_in = ev_out = 0;
        play_blk = 0;
        state = AUDST_IDLE;
        int_back( flags );
        return 0;

    case AUD_CLOSE:
        close_card();
        return 0;

    case AUD_VOLUME:
        if ( len < 2 ) return ERR_BADLEN;
        vol_left = ( (const uint8_t *)buf )[0];
        vol_right = ( (const uint8_t *)buf )[1];
        if ( opened && !stale ) {
            flags = int_off();               /* the SB16's interrupt moves the mixer's index */
            card_volume( vol_left, vol_right );
            int_back( flags );
        }
        return 0;

    case AUD_EVENTS:
        if ( !opened || ev_size == 0 ) return ERR_GENERAL;
        ev = (AUDEV *)buf;
        count = len / sizeof( AUDEV );
        room = ev_size - ( ev_in - ev_out );
        if ( count > room ) count = room;
        for ( i = 0; i < count; i++ ) evq[( ev_in + i ) & ( ev_size - 1 )] = ev[i];
        ev_in += count;                      /* after the copy */
        *answer = count;
        return 0;

    case AUD_SENDNOW:
        /* each write with interrupts off: the queued events go out from
           the interrupt, and an OPL index and its value, or a MIDI
           message's bytes, must not be split by them */
        if ( !opened || stale ) return ERR_GENERAL;
        ev = (AUDEV *)buf;
        count = len / sizeof( AUDEV );
        for ( i = 0; i < count; i++ ) {
            if ( ev[i].kind == AUDE_OPL && ( aud_info.flags & ( AUDF_OPL2 | AUDF_OPL3 ) ) ) {
                flags = int_off();
                card_opl( ev[i].bank, ev[i].reg, ev[i].val );
                int_back( flags );
            } else if ( ev[i].kind == AUDE_MPU && ( aud_info.flags & AUDF_MPU ) ) {
                uint32_t start = nds_ticks();
                unsigned int sent;

                for ( ;; ) {
                    flags = int_off();
                    sent = (unsigned int)card_mpu_put( ev[i].val );
                    int_back( flags );
                    if ( sent ) break;
                    if ( nds_ticks() - start > 2 ) return ERR_GENERAL;  /* the port is stuck */
                }
            }
        }
        *answer = count;
        return 0;

    case AUD_DROPEV:
        flags = int_off();
        ev_out = ev_in;
        int_back( flags );
        return 0;
    }
    return ERR_FUNC;
}
