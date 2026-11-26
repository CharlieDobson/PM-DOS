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
 * one sample of every channel: a stereo frame is two int16_t samples.
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
    uint32_t handle;
    uint32_t size;      /* the file's length in bytes */
    uint32_t pos;       /* where the next byte comes from */
    uint8_t *buf;       /* SRC_BUFSIZE bytes */
    uint32_t buf_pos;   /* the file position of buf[0] */
    uint32_t have;      /* bytes in buf from there */
    int      error;     /* a read failed (not the end): sticky */
    int      is_open;
} SRC;

int            src_open( SRC *src, const char *path );            /* 0 or a DOS error */
void           src_close( SRC *src );
unsigned int   src_read( SRC *src, void *dst, unsigned int len ); /* -> bytes copied; fewer only at the end */
int            src_seek( SRC *src, uint32_t pos );                /* 0; -1 past the end (pos is then the end) */
int            src_skip( SRC *src, uint32_t len );                /* 0; -1 if the file ran out first */
int            src_byte( SRC *src );                              /* 0..255, -1 at the end */
uint32_t       src_tell( const SRC *src );
uint32_t       src_left( const SRC *src );                        /* bytes from pos to the end */
const uint8_t *src_peek( SRC *src, unsigned int len );            /* len <= SRC_PEEK bytes at pos, NULL if fewer
                                                                     are left; good until the next src_ call */

/* little- and big-endian numbers in a byte buffer */
#define LE16( p )   ((uint16_t)((p)[0] | ((uint16_t)(p)[1] << 8)))
#define LE32( p )   ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))
#define BE16( p )   ((uint16_t)(((uint16_t)(p)[0] << 8) | (p)[1]))
#define BE32( p )   (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) | ((uint32_t)(p)[2] << 8) | (uint32_t)(p)[3])

/* ------------------------------------------------------------------ */
/* the equalizer                                                       */
/* ------------------------------------------------------------------ */
#define EQ_BANDS        10      /* 31, 62, 125, 250, 500, 1K, 2K, 4K, 8K, 16K Hz */
#define EQ_MAXDB        12      /* each band and the preamp: -EQ_MAXDB..+EQ_MAXDB dB */

typedef struct {
    int8_t   band[EQ_BANDS];    /* half decibels: -24..24 */
    int8_t   preamp;            /* half decibels, applied to every band */
    uint8_t  on;                /* 0: everything at 0 dB, whatever the bands say */
    uint16_t serial;            /* changes every time anything above does */
} EQSET;

extern const uint16_t eq_band_hz[EQ_BANDS];

/* The curve as gains in Q16 (65536 is 0 dB) at "count" frequencies,
   evenly spaced: gain[i] is for (i + 0.5) * (rate / 2) / count Hz -
   the centers of an MDCT's lines, or of the subbands of a filter bank.
   Between band centers the curve is smooth in log frequency; below the
   first and above the last it is flat.  Every gain is 1/4 to 4 (-12 dB
   to +12 dB, the preamp included and then limited).  rate is the
   file's sample rate, not a downsampled one. */
void eq_curve( const EQSET *eq, uint32_t rate, unsigned int count, uint32_t *gain );

/* The gain in Q16 at one frequency. */
uint32_t eq_gain_at( const EQSET *eq, uint32_t hz );

/* Decibels times 256 (limited to -12..+12 dB) as a gain in Q16. */
uint32_t eq_db_gain( int32_t db256 );

/* ------------------------------------------------------------------ */
/* what the player asks for                                            */
/* ------------------------------------------------------------------ */
/* MIDI: where the notes go */
enum { SYN_SOFT,                /* the software synthesizer: samples */
       SYN_OPL2,                /* a Yamaha OPL2 (AdLib, Sound Blaster 1/2): DECEV */
       SYN_OPL3,                /* a Yamaha OPL3 (Sound Blaster Pro 2, 16): DECEV */
       SYN_MPU };               /* an MPU-401 MIDI port: DECEV, the bytes as they are */

typedef struct {
    uint8_t  down;              /* 0, 1 or 2: the rate halved this many times, if the codec can */
    uint8_t  mono;              /* 1: one channel */
    uint8_t  fast;              /* 1: less accuracy for more speed, where the codec has a choice */
    uint8_t  synth;             /* MIDI: SYN_* */
    uint32_t rate;              /* MIDI and other synthesized sound: the rate to make samples at
                                   (a codec with a file rate of its own ignores this) */
    const EQSET *eq;            /* the equalizer, or NULL: never apply one */
} DECOPT;

/* ------------------------------------------------------------------ */
/* what the codec found                                                */
/* ------------------------------------------------------------------ */
#define TAG_LEN         96

typedef struct {
    uint32_t rate;              /* frames a second, as decode gives them */
    uint32_t file_rate;         /* the file's own rate */
    uint8_t  channels;          /* 1 or 2, as decode gives them */
    uint8_t  file_channels;     /* the file's own: 1 and up */
    uint8_t  bits;              /* the file's sample size where it has one (8, 16, 24...), else 0 */
    uint8_t  flags;             /* DIF_* */
    uint32_t kbps;              /* average bits a second / 1000, 0 when not known */
    uint32_t length_ms;         /* the play time, 0 when not known */
    char     format[48];        /* how it is coded, shown to the user: "MPEG-1 layer 3, joint stereo" */
    char     title[TAG_LEN];    /* code page 437 (tag_* below), "" for none */
    char     artist[TAG_LEN];
    char     album[TAG_LEN];
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
    uint32_t frame;
    uint8_t  kind;              /* EV_* */
    uint8_t  reg;               /* EV_OPL: the register */
    uint8_t  val;               /* EV_OPL: the value; EV_MPU: the byte */
    uint8_t  bank;              /* EV_OPL: 0, or 1 for the OPL3's second set */
} DECEV;

#define EV_OPL          1
#define EV_MPU          2

#define LEVEL_BANDS     32      /* DECBUF.levels: low to high, 0..255 */

typedef struct {
    int16_t     *pcm;                  /* in: room for "max" frames */
    unsigned int max;                  /* in: frames wanted, at most (always at least 1152) */
    unsigned int frames;               /* out: frames written; 0 only at the end */
    DECEV       *ev;                   /* in: room for "ev_max" records (DIF_EVENTS only) */
    unsigned int ev_max;
    unsigned int ev_count;             /* out: records written.  A codec that runs out of room stops
                                          the piece at the frame of the first record that did not fit */
    uint8_t      levels[LEVEL_BANDS];  /* out (DIF_LEVELS): what is sounding at the end of the piece */
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
    int  (*detect)( SRC *src, const uint8_t *head, unsigned int len, const char *ext );

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
    int  (*seek)( void *state, uint32_t ms, uint32_t *at_ms );

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
void tag_latin1( char *dst, unsigned int max, const uint8_t *src, unsigned int len );
void tag_utf8( char *dst, unsigned int max, const uint8_t *src, unsigned int len );
void tag_utf16( char *dst, unsigned int max, const uint8_t *src, unsigned int len, int big_endian );
int  tag_437( uint32_t uni );           /* one character: the 437 code, or '?' */

#endif
