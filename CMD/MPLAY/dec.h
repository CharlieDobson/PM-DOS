/*
 * DEC.H - what MPLAY asks of a decoder, and what it gives one.
 *
 * A DECODER TURNS A FILE INTO 16-BIT SAMPLES.  Each format is a module
 * with one CODEC table (wav.c is the worked example).  The player opens
 * the file as a SRC, asks every codec how sure it is that the file is
 * its own (detect), opens it with the surest one, and then asks for
 * samples a piece at a time (decode) for as long as the file lasts.
 * Everything the player does afterwards - the equalizer when the codec
 * cannot do it, the analyzer, the conversion to what the sound card
 * takes - happens to those samples.
 *
 * SAMPLES ARE SIGNED 16-BIT, INTERLEAVED, ONE OR TWO CHANNELS.  A file
 * with more channels is mixed down to two by its codec.  A "frame" is
 * one sample of every channel: a stereo frame is two s16s.
 *
 * THE PLAYER MAY ASK FOR LESS WORK.  On a slow machine DECOPT asks for
 * the rate halved (down = 1) or quartered (down = 2), and for one
 * channel (mono).  A codec that can do these cheaply - MP3 by leaving
 * out the top subbands, an MDCT codec by a smaller inverse transform -
 * does them and says so in DECINFO (rate and channels as decode gives
 * them).  A codec that cannot ignores the request; the player then
 * converts, which costs more.
 *
 * THE EQUALIZER IS FREE IN THE FREQUENCY DOMAIN.  A codec that has the
 * spectrum in hand before its inverse transform (MP3 layer 3, Vorbis,
 * WMA) multiplies each line by the curve eq_curve() makes and sets
 * DIF_EQ; the player then leaves the samples alone.  Layers 1 and 2,
 * PCM and the rest leave it to the player's time-domain filters.  The
 * curve changes while a file plays: compare EQSET.serial with the one
 * the tables were made from, once a frame.
 *
 * MIDI IS EVENTS AS WELL AS SAMPLES.  For an FM chip or a MIDI port,
 * the MIDI codec gives silence and a list of register writes (DECEV)
 * stamped with the frame they belong at; the sound driver sends each
 * one when the card has played that far.  With the software
 * synthesizer it gives samples like any other codec.
 *
 * TRUST NOTHING IN THE FILE.  Every length and offset is checked
 * against what the file holds before it is used.  A damaged stream is
 * skipped and decoding goes on from the next point that makes sense; a
 * codec never reads or writes outside a buffer, never loops without a
 * bound, and never crashes.
 */
#ifndef DEC_H
#define DEC_H

#include "types.h"

/* ------------------------------------------------------------------ */
/* the file                                                            */
/*                                                                     */
/* A SRC is the file read through a buffer, forwards, with seeking.    */
/* src_peek hands out up to SRC_PEEK bytes without using them up, for  */
/* finding a frame's header.                                            */
/* ------------------------------------------------------------------ */
#define SRC_BUFSIZE     32768UL
#define SRC_PEEK        8192UL

typedef struct {
    u32  handle;
    u32  size;          /* the file's length in bytes */
    u32  pos;           /* where the next byte comes from */
    u8  *buf;           /* SRC_BUFSIZE bytes */
    u32  buf_pos;       /* the file position of buf[0] */
    u32  have;          /* bytes in buf from there */
    int  error;         /* a read failed (not the end): sticky */
    int  is_open;
} SRC;

int       src_open( SRC *src, const char *path );   /* 0 or a DOS error */
void      src_close( SRC *src );
u32       src_read( SRC *src, void *dst, u32 len ); /* -> bytes copied; fewer only at the end */
int       src_seek( SRC *src, u32 pos );            /* 0; -1 past the end (pos is then the end) */
int       src_skip( SRC *src, u32 len );            /* 0; -1 if the file ran out first */
int       src_byte( SRC *src );                     /* 0..255, -1 at the end */
u32       src_tell( const SRC *src );
u32       src_left( const SRC *src );               /* bytes from pos to the end */
const u8 *src_peek( SRC *src, u32 len );            /* len <= SRC_PEEK bytes at pos, NULL if fewer
                                                       are left; good until the next src_ call */

/* little- and big-endian numbers in a byte buffer */
#define LE16( p )   ((u16)((p)[0] | ((u16)(p)[1] << 8)))
#define LE32( p )   ((u32)(p)[0] | ((u32)(p)[1] << 8) | ((u32)(p)[2] << 16) | ((u32)(p)[3] << 24))
#define BE16( p )   ((u16)(((u16)(p)[0] << 8) | (p)[1]))
#define BE32( p )   (((u32)(p)[0] << 24) | ((u32)(p)[1] << 16) | ((u32)(p)[2] << 8) | (u32)(p)[3])

/* ------------------------------------------------------------------ */
/* the equalizer                                                       */
/* ------------------------------------------------------------------ */
#define EQ_BANDS        10      /* 31, 62, 125, 250, 500, 1K, 2K, 4K, 8K, 16K Hz */
#define EQ_MAXDB        12      /* each band and the preamp: -EQ_MAXDB..+EQ_MAXDB dB */

typedef struct {
    s8   band[EQ_BANDS];        /* half decibels: -24..24 */
    s8   preamp;                /* half decibels, applied to every band */
    u8   on;                    /* 0: everything at 0 dB, whatever the bands say */
    u16  serial;                /* changes every time anything above does */
} EQSET;

extern const u16 eq_band_hz[EQ_BANDS];

/* The curve as gains in Q16 (65536 is 0 dB) at "count" frequencies,
   evenly spaced: gain[i] is for (i + 0.5) * (rate / 2) / count Hz -
   the centers of an MDCT's lines, or of the subbands of a filter bank.
   Between band centers the curve is smooth in log frequency; below the
   first and above the last it is flat.  Every gain is 1/4 to 4 (-12 dB
   to +12 dB, the preamp included and then limited).  rate is the
   file's sample rate, not a downsampled one. */
void eq_curve( const EQSET *eq, u32 rate, u32 count, u32 *gain );

/* The gain in Q16 at one frequency. */
u32  eq_gain_at( const EQSET *eq, u32 hz );

/* Decibels times 256 (limited to -12..+12 dB) as a gain in Q16. */
u32  eq_db_gain( s32 db256 );

/* ------------------------------------------------------------------ */
/* what the player asks for                                            */
/* ------------------------------------------------------------------ */
/* MIDI: where the notes go */
enum { SYN_SOFT,                /* the software synthesizer: samples */
       SYN_OPL2,                /* a Yamaha OPL2 (AdLib, Sound Blaster 1/2): DECEV */
       SYN_OPL3,                /* a Yamaha OPL3 (Sound Blaster Pro 2, 16): DECEV */
       SYN_MPU };               /* an MPU-401 MIDI port: DECEV, the bytes as they are */

typedef struct {
    u8   down;                  /* 0, 1 or 2: the rate halved this many times, if the codec can */
    u8   mono;                  /* 1: one channel */
    u8   fast;                  /* 1: less accuracy for more speed, where the codec has a choice */
    u8   synth;                 /* MIDI: SYN_* */
    u32  rate;                  /* MIDI and other synthesized sound: the rate to make samples at
                                   (a codec with a file rate of its own ignores this) */
    const EQSET *eq;            /* the equalizer, or NULL: never apply one */
} DECOPT;

/* ------------------------------------------------------------------ */
/* what the codec found                                                */
/* ------------------------------------------------------------------ */
#define TAG_LEN         96

typedef struct {
    u32  rate;                  /* frames a second, as decode gives them */
    u32  file_rate;             /* the file's own rate */
    u8   channels;              /* 1 or 2, as decode gives them */
    u8   file_channels;         /* the file's own: 1 and up */
    u8   bits;                  /* the file's sample size where it has one (8, 16, 24...), else 0 */
    u8   flags;                 /* DIF_* */
    u32  kbps;                  /* average bits a second / 1000, 0 when not known */
    u32  length_ms;             /* the play time, 0 when not known */
    char format[48];            /* how it is coded, shown to the user: "MPEG-1 layer 3, joint stereo" */
    char title[TAG_LEN];        /* code page 437 (tag_* below), "" for none */
    char artist[TAG_LEN];
    char album[TAG_LEN];
} DECINFO;

#define DIF_SEEK        0x01    /* seek works */
#define DIF_VBR         0x02    /* the bit rate varies: kbps is an average, length a guess */
#define DIF_EQ          0x04    /* the codec applies the equalizer itself */
#define DIF_EVENTS      0x08    /* decode gives DECEV records */
#define DIF_LEVELS      0x10    /* decode fills DECBUF.levels (MIDI to hardware: the samples are
                                   silence, so the analyzer shows these instead) */

/* ------------------------------------------------------------------ */
/* a piece of the decoded stream                                       */
/* ------------------------------------------------------------------ */
/* A register write or a MIDI byte, for the sound driver to send when
   the card has played up to "frame" (counted from the first frame of
   the piece it came with).  The layout is INC\AUDIO.H's AUDEV. */
typedef struct {
    u32  frame;
    u8   kind;                  /* EV_* */
    u8   reg;                   /* EV_OPL: the register */
    u8   val;                   /* EV_OPL: the value; EV_MPU: the byte */
    u8   bank;                  /* EV_OPL: 0, or 1 for the OPL3's second set */
} DECEV;

#define EV_OPL          1
#define EV_MPU          2

#define LEVEL_BANDS     32      /* DECBUF.levels: low to high, 0..255 */

typedef struct {
    s16  *pcm;                  /* in: room for "max" frames */
    u32   max;                  /* in: frames wanted, at most (always at least 1152) */
    u32   frames;               /* out: frames written; 0 only at the end */
    DECEV *ev;                  /* in: room for "ev_max" records (DIF_EVENTS only) */
    u32   ev_max;
    u32   ev_count;             /* out: records written.  A codec that runs out of room stops
                                   the piece at the frame of the first record that did not fit */
    u8    levels[LEVEL_BANDS];  /* out (DIF_LEVELS): what is sounding at the end of the piece */
} DECBUF;

/* ------------------------------------------------------------------ */
/* a codec                                                             */
/* ------------------------------------------------------------------ */
enum { DE_OK,                   /* samples (or the end: frames == 0) */
       DE_FORMAT,               /* open: not this format, or too damaged to start */
       DE_UNSUPPORTED,          /* open: this format, but a kind the codec does not do -
                                   DECINFO.format says what it is */
       DE_NOMEM,
       DE_READ,                 /* the file could not be read (a disk error) */
       DE_CARD };               /* the player's: the sound card refused the format
                                   (no codec returns this) */

typedef struct {
    const char *name;           /* "MP3": short, for a list */
    const char *exts;           /* "MP3 MP2 MP1": what the file box offers, space-separated */

    /* How sure is the codec that the file is its own: 0 (not), 100 (a
       signature nothing else has).  "head" is the file's first bytes
       (up to SRC_PEEK, fewer for a short file), ext its extension in
       capitals without the dot ("" for none).  May look further into
       the file through src (seek back to 0 before returning). */
    int  (*detect)( SRC *src, const u8 *head, u32 len, const char *ext );

    /* Start: read what is needed to fill in info (everything in it),
       make the state and hand it back.  DE_OK or an error with nothing
       left allocated.  src stays open and is the codec's to read until
       close; the player closes it afterwards. */
    int  (*open)( SRC *src, const DECOPT *opt, DECINFO *info, void **state );

    /* The next piece: at most buf->max frames into buf->pcm.  DE_OK
       with frames == 0 is the end.  A damaged part is skipped. */
    int  (*decode)( void *state, DECBUF *buf );

    /* To "ms" from the start, or the nearest point before it the codec
       can start from; *at_ms is where it landed.  DE_OK, or DE_FORMAT
       when it cannot (no DIF_SEEK). */
    int  (*seek)( void *state, u32 ms, u32 *at_ms );

    /* Everything the state holds, freed. */
    void (*close)( void *state );
} CODEC;

/* every codec MPLAY has: dectab.c */
extern const CODEC *const codecs[];
extern const int codec_count;

/* Open "path" with the codec surest of it.  -> DE_OK and the codec,
   its state and info; the SRC is the caller's to close after
   codec->close.  An unknown file is DE_FORMAT. */
int  dec_open( SRC *src, const char *path, const DECOPT *opt,
               const CODEC **codec, void **state, DECINFO *info );

/* ------------------------------------------------------------------ */
/* tags                                                                */
/*                                                                     */
/* Titles and names arrive as Latin-1, UTF-8 or UTF-16; the screen     */
/* shows code page 437.  These turn one into the other (a character    */
/* 437 lacks becomes '?'), cut to "max" bytes with the 0, and drop     */
/* control characters and trailing blanks.                             */
/* ------------------------------------------------------------------ */
void tag_latin1( char *dst, u32 max, const u8 *src, u32 len );
void tag_utf8( char *dst, u32 max, const u8 *src, u32 len );
void tag_utf16( char *dst, u32 max, const u8 *src, u32 len, int big_endian );
int  tag_437( u32 uni );                /* one character: the 437 code, or '?' */

#endif
