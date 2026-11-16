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
    u8   misc;                  /* 20h: AM, vibrato, sustaining, KSR, multiple */
    u8   level;                 /* 40h: key scale level (2 bits), total level (6) */
    u8   ad;                    /* 60h: attack rate, decay rate */
    u8   sr;                    /* 80h: sustain level, release rate */
    u8   wave;                  /* E0h: waveform 0..3 */
} MDOP;

#define OP_AM           0x80    /* MDOP.misc: tremolo */
#define OP_VIB          0x40    /* vibrato */
#define OP_EGT          0x20    /* the level holds at the sustain level while the key is down */
#define OP_KSR          0x10    /* the envelope is faster for higher notes */

/* A two-operator patch: the modulator, then the carrier. */
typedef struct {
    MDOP op[2];
    u8   fbc;                   /* C0h: feedback << 1, connection (1: the two are added) */
    s8   note;                  /* melodic: semitones added to the note;
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
    u8   program;
    u8   volume;                /* controller 7 */
    u8   expression;            /* controller 11 */
    u8   pan;                   /* controller 10 */
    u8   modulation;            /* controller 1 */
    u8   sustain;               /* controller 64 at 64 and over */
    u8   rpn_msb, rpn_lsb;      /* what data entry goes to: 127 127 for nothing */
    u8   bend_semis;            /* RPN 0: the bend range */
    u8   bend_cents;
    u8   coarse;                /* RPN 2: semitones, 64 for none */
    u8   pressure;              /* channel pressure */
    u16  fine;                  /* RPN 1: 8192 for none */
    u16  bend;                  /* 0..16383, 8192 for none */
    s32  bend64;                /* the bend in 1/64 semitone, worked out from the above */
    s32  tune64;                /* the coarse and fine tuning, the same way */
    u32  vol_att;               /* volume and expression as one attenuation */
    u8   cc[128];               /* every controller's last value, for MPU after a seek */
    u8   cc_set[16];            /* which of them the file has set */
} MDCHAN;

enum { VS_FREE, VS_ON, VS_OFF };        /* MDVOICE.state: free, key down, released */

typedef struct {
    u8   state;
    u8   chan, note, vel;
    u8   held;                  /* the note was let go while the pedal was down */
    u8   drum;
    u8   opl_a0, opl_b0, opl_c0;        /* OPL: what A0h, B0h and C0h were last given */
    u8   opl_40[2];             /* OPL: what the two 40h registers were last given */
    u8   opl_vib;               /* OPL: the vibrato bit controller 1 added to 20h */
    const MDPATCH *patch;
    const MDPATCH *loaded;      /* OPL: the patch the voice's registers hold */
    s32  pitch;                 /* the note with the patch's offset, 1/64 semitone */
    u32  age;                   /* when it was started, as a count */
    u32  on_frame, off_frame;   /* for the analyzer */
} MDVOICE;

/* ------------------------------------------------------------------ */
/* the file and the codec's state                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    const u8 *pos, *end;        /* the next byte, and the end of the chunk */
    const u8 *start;            /* the first event */
    u32  tick;                  /* when the next event is, from the start */
    u8   status;                /* running status */
    u8   done;
} MDTRACK;

#define MD_STAGE        512     /* records one step can make */
#define MD_SYSREC       32      /* SysEx messages kept to send again after a seek */

typedef struct {
    u32  offset, len;           /* in the file */
    u8   escape;                /* an F7 message: sent as it is, without an F0 */
} MDSYSEX;

typedef struct MDSTATE {
    /* the file */
    u8       *file;
    u32       file_len;
    MDTRACK  *track;
    u32       tracks;
    u32       format;           /* 0, 1 or 2 */
    u32       cur_track;        /* format 2: the one playing */
    u32       chan_used;        /* a bit for each channel the file uses */

    /* time.  A position is frames as 32.32 fixed point: the integer part
       is the frame an event lands on, the fraction keeps the tempo
       exact over a long piece. */
    u32       tpq;              /* ticks a quarter note; 0 with SMPTE time */
    u32       smpte_num, smpte_den;     /* SMPTE: seconds a tick, as a fraction */
    u32       uspq;             /* the tempo: microseconds a quarter note */
    u32       seg_tick;         /* the tick of the last tempo change */
    u32       seg_int, seg_frac;        /* and where it fell */
    u32       k_int, k_frac;    /* frames a tick */
    u32       rate;
    u32       frame;            /* the next frame decode gives out */
    u32       song_end;         /* the frame of the last event (the scan at open) */
    u32       length_ms;        /* the same in milliseconds */
    u32       last_frame;       /* the frame of the last event read so far */
    u32       hold;             /* MPU: nothing goes out before this frame (a reset) */
    u32       now;              /* the frame of the event being handled */

    /* the piece being made */
    s16      *pcm;
    u32       piece_start;      /* its first frame */
    u32       out_at;           /* the frame the samples have been made up to */

    /* what is played */
    u8        synth;            /* SYN_* */
    u8        out_ch;           /* channels decode gives */
    u8        fast, mono;
    u8        mode;             /* MM_*: what the event reader is doing */
    u8        ended;            /* the end of the piece: 1 tail, 2 silenced, 3 done */
    u8        job;              /* JOB_*: set-up writes waiting to go out */
    u8        job_step;
    u32       job_frame;
    u32       tail_end;         /* the frame the tail stops at */
    MDCHAN    chan[MD_CHANNELS];
    MDVOICE   voice[MD_MAXVOICES];
    u32       voices;
    u32       age;

    /* SysEx: one being sent a piece at a time, and the ones kept for a seek */
    const u8 *sx_pos;
    u32       sx_left;
    u32       sx_frame;         /* the frame it goes out at */
    u8        sx_reset;         /* it is a reset: hold when it has gone */
    MDSYSEX   sysrec[MD_SYSREC];
    u32       sysrecs;

    /* the names, while the file is scanned at open */
    DECINFO  *info;
    u8        title_prio, artist_prio;  /* 3 RIFF INFO, 2 karaoke "@T", 1 track name */
    u8        kar_titles;

    /* the stage: records for one frame, waiting for room in a DECBUF */
    DECEV     stage[MD_STAGE];
    u32       stage_count, stage_used, stage_frame;

    void     *syn;              /* mdsyn.c's state */
} MDSTATE;

enum { MM_PLAY, MM_SCAN, MM_REPLAY };   /* MDSTATE.mode */

/* ------------------------------------------------------------------ */
/* midi.c, for the outputs                                             */
/* ------------------------------------------------------------------ */
extern const u16 md_vol_att[128];       /* 40 log10( 127 / v ) dB, in attenuation units */
extern const u16 md_vel_att[128];       /* 30 log10( 127 / v ) dB */
extern const u16 md_pan_att[128];       /* a pan's loss on the left; the right's is [128 - pan] */

void md_stage( MDSTATE *md, u8 kind, u8 bank, u8 reg, u8 val );
s32  md_voice_pitch( const MDSTATE *md, const MDVOICE *vc );    /* with bend and tuning */
u32  md_pitch_hz( s32 pitch );                  /* in Hz, 16 bits of fraction */
void md_fnum( u32 hz, u32 *fnum, u32 *block );  /* OPL F-number and block for Hz (Q16) */
u32  md_voice_att( const MDSTATE *md, const MDVOICE *vc );      /* velocity, volume, expression */

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
void syn_render( MDSTATE *md, s16 *pcm, u32 frames );
int  syn_sounding( MDSTATE *md );       /* voices still making sound */

#endif
