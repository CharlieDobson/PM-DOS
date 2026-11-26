/*
 * MIDI.H - what the four files of the MIDI codec share (MIDI.C).
 *
 * THE CODEC IS FOUR FILES.  MIDI.C reads the file, keeps the time,
 * keeps each MIDI channel's state and gives notes to voices.  MDOPL.C
 * turns voices into register writes for an OPL2 or an OPL3, MDSYN.C
 * plays them itself as two-operator FM, and MDBANK.C is the instrument
 * bank both of those play from.  The MPU-401 output needs no voices to
 * make sound - the synthesizer at the far end has its own - so it lives
 * in MIDI.C, and keeps voices only for the analyzer.
 *
 * A VOICE IS ONE NOTE BEING PLAYED.  MIDI.C chooses which voice a note
 * gets (a free one, else the one released longest ago, else the
 * oldest) and fills in the note, the channel and the patch; the output
 * makes it sound.  The software synthesizer keeps its own state for
 * each voice in MDSYN.C.
 *
 * LOUDNESS IS COUNTED AS ATTENUATION, in 1/256 of an octave of
 * amplitude: one unit is 0.0235 dB and 4095 is silence.  That is the
 * unit the software synthesizer works in; an OPL's total level is in
 * steps of 0.75 dB, which is 32 of them.
 *
 * PITCH IS COUNTED IN 1/64 OF A SEMITONE from MIDI note 0, so a note
 * is its number times 64 and a bend is a few hundred more or less.
 */
#ifndef MIDI_H
#define MIDI_H

#include "types.h"
#include "dec.h"

/* ------------------------------------------------------------------ */
/* the instrument bank (mdbank.c)                                      */
/* ------------------------------------------------------------------ */
/* One operator, as the OPL's registers hold it. */
typedef struct {
    uint8_t misc;               /* 20h: AM, vibrato, sustaining, KSR, multiple */
    uint8_t level;              /* 40h: key scale level (2 bits), total level (6) */
    uint8_t ad;                 /* 60h: attack rate, decay rate */
    uint8_t sr;                 /* 80h: sustain level, release rate */
    uint8_t wave;               /* E0h: waveform 0..3 */
} MDOP;

#define OP_AM           0x80    /* MDOP.misc: tremolo */
#define OP_VIB          0x40    /* vibrato */
#define OP_EGT          0x20    /* the level holds at the sustain level while the key is down */
#define OP_KSR          0x10    /* the envelope is faster for higher notes */

/* A two-operator patch: the modulator, then the carrier. */
typedef struct {
    MDOP    op[2];
    uint8_t fbc;                /* C0h: feedback << 1, connection (1: the two are added) */
    int8_t  note;               /* melodic: semitones added to the note;
                                   a drum: the note it always plays at */
} MDPATCH;

#define MD_DRUM_FIRST   35      /* the General MIDI drum keys */
#define MD_DRUM_LAST    81
#define MD_DRUMS        (MD_DRUM_LAST - MD_DRUM_FIRST + 1)

extern const MDPATCH md_melodic[128];
extern const MDPATCH md_drums[MD_DRUMS];

/* ------------------------------------------------------------------ */
/* the state of a MIDI channel and of a voice                          */
/* ------------------------------------------------------------------ */
#define MD_CHANNELS     16
#define MD_DRUMCHAN     9       /* MIDI channel 10 */
#define MD_MAXVOICES    32

typedef struct {
    uint8_t  program;
    uint8_t  volume;            /* controller 7 */
    uint8_t  expression;        /* controller 11 */
    uint8_t  pan;               /* controller 10 */
    uint8_t  modulation;        /* controller 1 */
    uint8_t  sustain;           /* controller 64 at 64 and over */
    uint8_t  rpn_msb, rpn_lsb;  /* what data entry goes to: 127 127 for nothing */
    uint8_t  bend_semis;        /* RPN 0: the bend range */
    uint8_t  bend_cents;
    uint8_t  coarse;            /* RPN 2: semitones, 64 for none */
    uint8_t  pressure;          /* channel pressure */
    uint16_t fine;              /* RPN 1: 8192 for none */
    uint16_t bend;              /* 0..16383, 8192 for none */
    int32_t  bend64;            /* the bend in 1/64 semitone, worked out from the above */
    int32_t  tune64;            /* the coarse and fine tuning, the same way */
    uint32_t vol_att;           /* volume and expression as one attenuation */
    uint8_t  cc[128];           /* every controller's last value, for MPU after a seek */
    uint8_t  cc_set[16];        /* which of them the file has set */
} MDCHAN;

enum { VS_FREE, VS_ON, VS_OFF };        /* MDVOICE.state: free, key down, released */

typedef struct {
    uint8_t  state;
    uint8_t  chan, note, vel;
    uint8_t  held;                 /* the note was let go while the pedal was down */
    uint8_t  drum;
    uint8_t  opl_a0, opl_b0, opl_c0;    /* OPL: what A0h, B0h and C0h were last given */
    uint8_t  opl_40[2];            /* OPL: what the two 40h registers were last given */
    uint8_t  opl_vib;              /* OPL: the vibrato bit controller 1 added to 20h */
    const MDPATCH *patch;
    const MDPATCH *loaded;         /* OPL: the patch the voice's registers hold */
    int32_t  pitch;                /* the note with the patch's offset, 1/64 semitone */
    uint32_t age;                  /* when it was started, as a count */
    uint32_t on_frame, off_frame;  /* for the analyzer */
} MDVOICE;

/* ------------------------------------------------------------------ */
/* the file and the codec's state                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    const uint8_t *pos, *end;   /* the next byte, and the end of the chunk */
    const uint8_t *start;       /* the first event */
    uint32_t tick;              /* when the next event is, from the start */
    uint8_t  status;            /* running status */
    uint8_t  done;
} MDTRACK;

#define MD_STAGE        512     /* records one step can make */
#define MD_SYSREC       32      /* SysEx messages kept to send again after a seek */

typedef struct {
    uint32_t offset, len;       /* in the file */
    uint8_t  escape;            /* an F7 message: sent as it is, without an F0 */
} MDSYSEX;

typedef struct MDSTATE {
    /* the file */
    uint8_t  *file;
    uint32_t  file_len;
    MDTRACK  *track;
    uint32_t  tracks;
    uint32_t  format;           /* 0, 1 or 2 */
    uint32_t  cur_track;        /* format 2: the one playing */
    uint32_t  chan_used;        /* a bit for each channel the file uses */

    /* time.  A position is frames as 32.32 fixed point: the integer part
       is the frame an event lands on, the fraction keeps the tempo
       exact over a long piece. */
    uint32_t  tpq;              /* ticks a quarter note; 0 with SMPTE time */
    uint32_t  smpte_num, smpte_den;     /* SMPTE: seconds a tick, as a fraction */
    uint32_t  uspq;             /* the tempo: microseconds a quarter note */
    uint32_t  seg_tick;         /* the tick of the last tempo change */
    uint32_t  seg_int, seg_frac;        /* and where it fell */
    uint32_t  k_int, k_frac;    /* frames a tick */
    uint32_t  rate;
    uint32_t  frame;            /* the next frame decode gives out */
    uint32_t  song_end;         /* the frame of the last event (the scan at open) */
    uint32_t  length_ms;        /* the same in milliseconds */
    uint32_t  last_frame;       /* the frame of the last event read so far */
    uint32_t  hold;             /* MPU: nothing goes out before this frame (a reset) */
    uint32_t  now;              /* the frame of the event being handled */

    /* the piece being made */
    int16_t  *pcm;
    uint32_t  piece_start;      /* its first frame */
    uint32_t  out_at;           /* the frame the samples have been made up to */

    /* what is played */
    uint8_t   synth;            /* SYN_* */
    uint8_t   out_ch;           /* channels decode gives */
    uint8_t   fast, mono;
    uint8_t   mode;             /* MM_*: what the event reader is doing */
    uint8_t   ended;            /* the end of the piece: 1 tail, 2 silenced, 3 done */
    uint8_t   job;              /* JOB_*: set-up writes waiting to go out */
    uint8_t   job_step;
    uint32_t  job_frame;
    uint32_t  tail_end;         /* the frame the tail stops at */
    MDCHAN    chan[MD_CHANNELS];
    MDVOICE   voice[MD_MAXVOICES];
    uint32_t  voices;
    uint32_t  age;

    /* SysEx: one being sent a piece at a time, and the ones kept for a seek */
    const uint8_t *sx_pos;
    uint32_t       sx_left;
    uint32_t       sx_frame;    /* the frame it goes out at */
    uint8_t        sx_reset;    /* it is a reset: hold when it has gone */
    MDSYSEX        sysrec[MD_SYSREC];
    uint32_t       sysrecs;

    /* the names, while the file is scanned at open */
    DECINFO  *info;
    uint8_t   title_prio, artist_prio;  /* 3 RIFF INFO, 2 karaoke "@T", 1 track name */
    uint8_t   kar_titles;

    /* the stage: records for one frame, waiting for room in a DECBUF */
    DECEV     stage[MD_STAGE];
    uint32_t  stage_count, stage_used, stage_frame;

    void     *syn;              /* mdsyn.c's state */
} MDSTATE;

enum { MM_PLAY, MM_SCAN, MM_REPLAY };   /* MDSTATE.mode */

/* ------------------------------------------------------------------ */
/* midi.c, for the outputs                                             */
/* ------------------------------------------------------------------ */
extern const uint16_t md_vol_att[128];  /* 40 log10( 127 / v ) dB, in attenuation units */
extern const uint16_t md_vel_att[128];  /* 30 log10( 127 / v ) dB */
extern const uint16_t md_pan_att[128];  /* a pan's loss on the left; the right's is [128 - pan] */

void     md_stage( MDSTATE *md, uint8_t kind, uint8_t bank, uint8_t reg, uint8_t val );
int32_t  md_voice_pitch( const MDSTATE *md, const MDVOICE *vc );  /* with bend and tuning */
uint32_t md_pitch_hz( int32_t pitch );                             /* in Hz, 16 bits of fraction */
void     md_fnum( uint32_t hz, uint32_t *fnum, uint32_t *block );  /* OPL F-number and block for Hz (Q16) */
uint32_t md_voice_att( const MDSTATE *md, const MDVOICE *vc );    /* velocity, volume, expression */

/* ------------------------------------------------------------------ */
/* mdopl.c                                                             */
/* ------------------------------------------------------------------ */
void opl_setup( MDSTATE *md );          /* the chip set up and every voice silent */
void opl_silence( MDSTATE *md );        /* every voice silent */
void opl_start( MDSTATE *md, int voice );
void opl_stop( MDSTATE *md, int voice );
void opl_kill( MDSTATE *md, int voice );
void opl_pitch( MDSTATE *md, int voice );
void opl_level( MDSTATE *md, int voice );
void opl_pan( MDSTATE *md, int voice );
void opl_vibrato( MDSTATE *md, int voice );

/* ------------------------------------------------------------------ */
/* mdsyn.c                                                             */
/* ------------------------------------------------------------------ */
int  syn_open( MDSTATE *md );           /* DE_OK or DE_NOMEM */
void syn_close( MDSTATE *md );
void syn_reset( MDSTATE *md );          /* every voice silent at once */
void syn_start( MDSTATE *md, int voice );
void syn_stop( MDSTATE *md, int voice );
void syn_kill( MDSTATE *md, int voice );
void syn_pitch( MDSTATE *md, int voice );
void syn_level( MDSTATE *md, int voice );
void syn_pan( MDSTATE *md, int voice );
void syn_render( MDSTATE *md, int16_t *pcm, unsigned int frames );
int  syn_sounding( MDSTATE *md );       /* voices still making sound */

#endif
