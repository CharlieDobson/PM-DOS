/*
 * PLAYER.H - MPLAY's engine: one file decoded and played through the
 * sound driver, with the equalizer, the volume and the analyzer.  No
 * screen here; MAIN.C and the rest of the program drive it.
 *
 * THE PROGRAM CALLS pl_pump OFTEN - from its loop and from inside
 * every menu and dialog (key_idle in DISP.H) - and pl_pump keeps the
 * driver's ring filled ahead of the card, a slice at a time, so that
 * the screen never waits long for the sound and the sound never runs
 * dry while the screen is busy.
 *
 * WHAT IS HEARD IS WHAT THE ANALYZER SHOWS: the driver says how far
 * the card has played, and pl_levels looks up the samples that were
 * decoded for that moment, not the ones just decoded half a second
 * ahead of it.
 */
#ifndef PLAYER_H
#define PLAYER_H

#include "types.h"
#include "dec.h"

/* what the engine is doing */
enum { PS_EMPTY,                /* no file */
       PS_STOPPED,              /* a file, at its start */
       PS_PLAYING,
       PS_PAUSED,
       PS_ENDED };              /* played to the end */

/* how hard the decoders may work: what the user chose in Options */
enum { PQ_AUTO, PQ_BEST, PQ_HALF, PQ_QUARTER };

typedef struct {
    int  quality;               /* PQ_* */
    int  mono;                  /* 1: one channel whatever the file has */
    int  synth;                 /* MIDI: SYN_* wanted, or -1 for the best the card has */
    int  rate_limit;            /* 0, or the highest rate the card should be asked for */
    int  eq_pcm;                /* 1: time-domain equalizer for codecs that cannot do it */
} PLSET;

/* the card, as the engine sees it */
typedef struct {
    int  present;               /* a sound driver was found */
    char name[40];              /* "Sound Blaster 16", or "No sound card" */
    char line[80];              /* "220h, IRQ 5, DMA 1 and 5" */
    u16  flags;                 /* AUDF_* (INC\AUDIO.H) */
    u32  rate_max, rate_max2;
    u8   bits;
    u8   irq;                   /* the card's line, or 0xFF */
} PLCARD;

/* what the engine is playing now */
typedef struct {
    int  state;                 /* PS_* */
    u32  pos_ms;                /* heard now */
    u32  len_ms;                /* 0 when not known */
    u32  out_rate;              /* the card's */
    u8   out_bits, out_ch;
    int  load;                  /* per cent of the processor the decoding takes */
    int  down, mono;            /* what the codec was asked for */
    int  synth;                 /* MIDI: SYN_* in use */
    u32  underruns;
    u32  irqs;                  /* the card's interrupts so far (for testing) */
    u8   drv_state;             /* the driver's AUDST_* (for testing) */
    char quality[48];           /* "Half rate, stereo" */
} PLSTAT;

int   pl_init( void );                  /* once: the driver, the processor's speed */
void  pl_done( void );
const PLCARD *pl_card( void );
int   pl_cpu_class( void );             /* 0 386, 1 slow 486, 2 fast 486, 3 Pentium and up */
u32   pl_cpu_speed( void );             /* the benchmark: multiplies a millisecond */
const char *pl_cpu_name( void );

void  pl_settings( const PLSET *set );  /* applied at the next pl_open */
const PLSET *pl_get_settings( void );

/* Open a file: DE_OK and its DECINFO, or a DE_* error (DE_UNSUPPORTED
   with info->format saying what it is).  The engine is then
   PS_STOPPED.  A file already open is closed first. */
int   pl_open( const char *path, DECINFO *info );
void  pl_close( void );

void  pl_play( void );
void  pl_pause( void );                 /* and pl_play goes on */
void  pl_stop( void );                  /* back to the start */
int   pl_seek( u32 ms );                /* -> 1 if it moved */

/* Keep the sound going; call it at least ten times a second.  ->
   PS_* now.  It spends at most "budget_ms" decoding (0: the engine's
   own choice). */
int   pl_pump( u32 budget_ms );

void  pl_status( PLSTAT *st );

/* how much decoded sound is waiting for the card, in milliseconds */
u32   pl_ahead_ms( void );

/* the equalizer: the engine keeps its own copy and its serial number */
void  pl_set_eq( const EQSET *eq );

/* 0..100, and -100 (left) .. 100 (right) */
void  pl_set_volume( int volume, int balance );

/* what is being heard: "count" bars from low to high, 0..255 */
void  pl_levels( u8 *bars, int count );

/* ...and the wave itself: "count" samples, mono */
void  pl_wave( s16 *out, int count );

#endif
