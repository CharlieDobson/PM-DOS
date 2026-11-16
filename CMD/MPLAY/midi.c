/*
 * MIDI.C - Standard MIDI Files, played on an FM chip, through a MIDI
 * port, or by the software synthesizer (DEC.H, MIDI.H).
 *
 * A MIDI FILE IS A LIST OF EVENTS, NOT A SOUND.  "MThd" says how many
 * tracks there are and how long a tick is; each "MTrk" is a list of
 * events, each after a wait counted in ticks.  Format 0 has one track,
 * format 1 several played together, format 2 several played one after
 * another.  A RIFF "RMID" file is an SMF inside a "data" chunk, and a
 * .KAR file is an SMF with the words of a song in its text events.
 *
 * THE WHOLE FILE IS READ INTO MEMORY.  The tracks of a format 1 file
 * are read side by side, an event from one and then from another, and
 * through SRC's buffer that would go to the disk again at every turn.
 * MIDI files are small; one bigger than MAX_FILE is played as far as
 * that goes.
 *
 * TIME IS KEPT IN FRAMES OF OUTPUT, as 32.32 fixed point.  A tempo
 * change works out how many frames a tick is (k_int, k_frac) and the
 * frame it happened at; an event's frame is that point plus its ticks
 * since then times the frames a tick.  Nothing is added up event by
 * event, so a long piece does not drift, and the fraction keeps a tempo
 * exact.  SMPTE time is the same with a fixed length of tick.
 *
 * THE PLAYER GETS EVENTS AS WELL AS SAMPLES.  For an OPL or an MPU-401
 * the samples are silence, there only as a clock, and the music is a
 * list of register writes or MIDI bytes (DECEV), each stamped with its
 * frame.  One MIDI event can make a few dozen writes.  They are staged
 * first, then copied into the player's buffer as room allows; when the
 * room runs out, the piece ends at that frame and the rest go out at
 * the start of the next one.
 *
 * SEEKING READS THE FILE AGAIN FROM THE START, without a sound, keeping
 * the programs, the controllers and the bends.  Then the chip is made
 * silent, or the MIDI port's synthesizer is reset and given that state
 * (with the SysEx messages that set it up), and the piece goes on from
 * there.  No note is left sounding.
 *
 * A GM RESET NEEDS TIME.  A synthesizer on the MIDI port can take tens
 * of milliseconds over a reset and miss what comes during it, so after
 * one nothing more is sent until MPU_GAP has passed (MDSTATE.hold).
 * The events of that time go out together at its end.
 *
 * A DAMAGED FILE IS PLAYED AS FAR AS IT MAKES SENSE.  Every length read
 * is checked against the end of its chunk, a delta time is at most four
 * bytes, a track without an end-of-track event ends at the end of its
 * chunk, and every event read uses up at least one byte, so a pass over
 * the file always ends.
 */
#include "rt.h"
#include "fix.h"
#include "dec.h"
#include "midi.h"

#define MAX_FILE        0x800000UL      /* 8 MB: more than any real MIDI file */
#define MAX_TRACKS      256
#define MAX_TICK        0xF0000000UL    /* a track whose ticks pass this has ended */
#define MD_MAXFRAME     0x7FFFFFFFUL    /* and a piece whose frames do */
#define MD_ENDFRAME     0xF0000000UL    /* past the end and its tail: a piece never reaches it */
#define TEMPO_DEFAULT   500000UL        /* 120 beats a minute */
#define MPU_VOICES      32              /* notes the analyzer follows for the MIDI port */
#define TAIL_SECONDS    1               /* the time a released note is given to die away */
#define SOFT_TAIL       3               /* at most, after the last event (software) */

enum { JOB_NONE, JOB_START };           /* MDSTATE.job */

/* (a * b) >> 32, unsigned */
u32 md_umulhi( u32 a, u32 b );
#pragma aux md_umulhi = \
    "mul edx" \
    parm [eax] [edx] value [edx] modify exact [eax edx] nomemory;

/* (hi:lo) / div, and the remainder; hi must be less than div */
u32 md_div64( u32 lo, u32 hi, u32 div );
#pragma aux md_div64 = \
    "div ecx" \
    parm [eax] [edx] [ecx] value [eax] modify exact [eax edx] nomemory;

u32 md_mod64( u32 lo, u32 hi, u32 div );
#pragma aux md_mod64 = \
    "div ecx" \
    parm [eax] [edx] [ecx] value [edx] modify exact [eax edx] nomemory;

/* ------------------------------------------------------------------ */
/* tables (made by TOOLS\MKTAB.PL in the MIDI notes: see the report)   */
/* ------------------------------------------------------------------ */
/* 40 log10( 127 / v ) dB as attenuation units (0.0235 dB): the General
   MIDI curve for volume and expression */
const u16 md_vol_att[128] = {
    4095, 3578, 3066, 2767, 2554, 2389, 2255, 2141, 2042, 1955, 1877, 1807,
    1743, 1684, 1629, 1578, 1530, 1485, 1443, 1403, 1365, 1329, 1295, 1262,
    1231, 1201, 1172, 1144, 1117, 1091, 1066, 1042, 1018,  995,  973,  952,
     931,  911,  891,  872,  853,  835,  817,  800,  783,  766,  750,  734,
     719,  703,  689,  674,  660,  646,  632,  618,  605,  592,  579,  566,
     554,  542,  530,  518,  506,  495,  483,  472,  461,  451,  440,  430,
     419,  409,  399,  389,  379,  370,  360,  351,  341,  332,  323,  314,
     305,  297,  288,  279,  271,  263,  254,  246,  238,  230,  222,  214,
     207,  199,  191,  184,  177,  169,  162,  155,  148,  141,  134,  127,
     120,  113,  106,   99,   93,   86,   80,   73,   67,   61,   54,   48,
      42,   36,   30,   24,   18,   12,    6,    0
};

/* 30 log10( 127 / v ) dB: velocity, a little gentler than volume */
const u16 md_vel_att[128] = {
    4095, 2684, 2300, 2075, 1916, 1792, 1691, 1606, 1532, 1466, 1408, 1355,
    1307, 1263, 1222, 1183, 1148, 1114, 1082, 1052, 1024,  997,  971,  947,
     923,  900,  879,  858,  838,  818,  799,  781,  764,  747,  730,  714,
     698,  683,  668,  654,  640,  626,  613,  600,  587,  575,  563,  551,
     539,  528,  516,  505,  495,  484,  474,  464,  454,  444,  434,  425,
     415,  406,  397,  388,  380,  371,  363,  354,  346,  338,  330,  322,
     314,  307,  299,  292,  284,  277,  270,  263,  256,  249,  242,  236,
     229,  222,  216,  210,  203,  197,  191,  185,  179,  173,  167,  161,
     155,  149,  144,  138,  132,  127,  121,  116,  111,  105,  100,   95,
      90,   85,   80,   75,   70,   65,   60,   55,   50,   45,   41,   36,
      31,   27,   22,   18,   13,    9,    4,    0
};

/* the left side's loss for a pan setting: -log2( cos( theta ) ) * 256,
   theta = (pan - 1) / 126 * 90 degrees, 0 counted as 1, so that 64 is
   the middle (3 dB down each side).  The right side's is [128 - pan]. */
const u16 md_pan_att[128] = {
       0,    0,    0,    0,    0,    0,    1,    1,    1,    2,    2,    3,
       3,    4,    5,    6,    6,    7,    8,    9,   10,   12,   13,   14,
      15,   17,   18,   20,   21,   23,   25,   26,   28,   30,   32,   34,
      36,   39,   41,   43,   45,   48,   51,   53,   56,   59,   61,   64,
      67,   70,   74,   77,   80,   84,   87,   91,   95,   98,  102,  106,
     110,  115,  119,  123,  128,  133,  137,  142,  147,  153,  158,  163,
     169,  174,  180,  186,  192,  199,  205,  212,  219,  226,  233,  240,
     248,  256,  264,  272,  281,  290,  299,  308,  318,  328,  339,  349,
     360,  372,  384,  396,  409,  423,  437,  451,  466,  482,  499,  517,
     535,  555,  576,  598,  621,  647,  674,  703,  735,  770,  809,  852,
     901,  958, 1025, 1108, 1214, 1363, 1619, 4095
};

/* MIDI notes 120..131 in Hz with 16 bits of fraction; a lower octave
   is a shift */
static const u32 md_semi[12] = {
     548668578UL,  581294109UL,  615859655UL,  652480576UL,
     691279090UL,  732384684UL,  775934544UL,  822074013UL,
     870957077UL,  922746880UL,  977616265UL, 1035748353UL
};

/* 2 ^ (k / 768), Q30: the 64 steps inside a semitone */
static const u32 md_fine[64] = {
    1073741824UL, 1074711351UL, 1075681754UL, 1076653033UL, 1077625190UL, 1078598223UL,
    1079572136UL, 1080546928UL, 1081522600UL, 1082499153UL, 1083476588UL, 1084454905UL,
    1085434106UL, 1086414191UL, 1087395161UL, 1088377016UL, 1089359758UL, 1090343388UL,
    1091327906UL, 1092313312UL, 1093299609UL, 1094286796UL, 1095274874UL, 1096263845UL,
    1097253708UL, 1098244466UL, 1099236118UL, 1100228665UL, 1101222108UL, 1102216449UL,
    1103211687UL, 1104207825UL, 1105204861UL, 1106202798UL, 1107201636UL, 1108201375UL,
    1109202018UL, 1110203564UL, 1111206014UL, 1112209370UL, 1113213631UL, 1114218799UL,
    1115224875UL, 1116231859UL, 1117239753UL, 1118248556UL, 1119258271UL, 1120268897UL,
    1121280436UL, 1122292888UL, 1123306254UL, 1124320536UL, 1125335733UL, 1126351846UL,
    1127368878UL, 1128386827UL, 1129405696UL, 1130425485UL, 1131446194UL, 1132467826UL,
    1133490379UL, 1134513856UL, 1135538257UL, 1136563583UL
};

/* the analyzer's band for each drum, 35..81 (0 = 31 Hz, 31 = 16 kHz) */
static const u8 md_drum_band[MD_DRUMS] = {
     1,  1, 14, 11, 16, 12,  4, 28,  5, 27,  6, 28,  7,  8, 29,  9,
    27, 29, 24, 27, 30, 18, 29, 20, 27, 15, 13, 13, 12, 11, 14, 12,
    19, 18, 28, 29, 22, 21, 23, 22, 21, 19, 18, 15, 14, 26, 26
};

static const u8 gm_on[6] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */
/* Frames a tick, as 32.32: rate * mul / div1 / div2.  rate * mul / div1
   must be under 2^32, which it is for any tempo (24 bits of
   microseconds) and rate under 2^24.  When it does not come out exact
   it is rounded up, by 2^-32 of a frame: a tick that falls exactly on a
   frame (they often do) then lands on it and not one before, and the
   error over a whole piece stays under a frame. */
static void md_set_k( MDSTATE *md, u32 mul, u32 div1, u32 div2 )
{
    u32 lo, hi, whole, rem, frac, inexact;

    lo = md->rate * mul;
    hi = md_umulhi( md->rate, mul );
    if ( div2 == 0 ) {
        div2 = 1;
    }
    if ( hi >= div1 ) {
        md->k_int = MD_MAXFRAME;        /* cannot happen with the limits above */
        md->k_frac = 0;
        return;
    }
    whole = md_div64( lo, hi, div1 );
    rem = md_mod64( lo, hi, div1 );
    frac = md_div64( 0, rem, div1 );
    inexact = md_mod64( 0, rem, div1 );
    md->k_int = whole / div2;
    rem = whole % div2;
    inexact |= md_mod64( frac, rem, div2 );
    md->k_frac = md_div64( frac, rem, div2 );
    if ( inexact != 0 ) {
        if ( ++md->k_frac == 0 ) {
            md->k_int++;
        }
    }
}

static void md_tempo_k( MDSTATE *md )
{
    if ( md->tpq != 0 ) {
        md_set_k( md, md->uspq, 1000000UL, md->tpq );
    } else {
        md_set_k( md, md->smpte_num, md->smpte_den, 1 );
    }
}

/* The frame tick "tick" falls on (it is not before the last tempo
   change), and its fraction in *frac.  A piece longer than MD_MAXFRAME
   stops there. */
static u32 md_tick_frame( const MDSTATE *md, u32 tick, u32 *frac )
{
    u32 dt = tick - md->seg_tick, lo, hi, fr, whole;

    lo = dt * md->k_frac;
    hi = md_umulhi( dt, md->k_frac );
    fr = md->seg_frac + lo;
    whole = hi + (fr < lo ? 1 : 0);
    *frac = fr;
    if ( whole > MD_MAXFRAME ) {
        return MD_MAXFRAME;
    }
    if ( md->k_int != 0 ) {
        if ( dt > (MD_MAXFRAME - whole) / md->k_int ) {
            return MD_MAXFRAME;
        }
        whole += dt * md->k_int;
    }
    if ( whole > MD_MAXFRAME - md->seg_int ) {
        return MD_MAXFRAME;
    }
    return whole + md->seg_int;
}

static void md_new_tempo( MDSTATE *md, u32 tick, u32 uspq )
{
    u32 frac;

    md->seg_int = md_tick_frame( md, tick, &frac );
    md->seg_frac = frac;
    md->seg_tick = tick;
    md->uspq = uspq;
    md_tempo_k( md );
}

/* ------------------------------------------------------------------ */
/* reading the tracks                                                  */
/* ------------------------------------------------------------------ */
/* A variable-length number: at most four bytes; a fifth is not looked
   for, the number is cut there.  0 if the chunk ended first. */
static int md_vlq( const u8 **pos, const u8 *end, u32 *val )
{
    const u8 *at = *pos;
    u32 num = 0;
    int i;

    for ( i = 0; i < 4; i++ ) {
        if ( at >= end ) {
            *pos = at;
            return 0;
        }
        num = (num << 7) | (*at & 0x7F);
        if ( !(*at++ & 0x80) ) {
            break;
        }
    }
    *pos = at;
    *val = num;
    return 1;
}

/* The wait before a track's next event */
static void md_next_delta( MDTRACK *trk )
{
    u32 delta;

    if ( trk->done ) {
        return;
    }
    if ( !md_vlq( &trk->pos, trk->end, &delta ) || trk->pos >= trk->end
         || delta > MAX_TICK - trk->tick ) {
        trk->done = 1;
        return;
    }
    trk->tick += delta;
}

/* The track whose event comes next, or NULL at the end of them all */
static MDTRACK *md_next_track( MDSTATE *md )
{
    MDTRACK *best = NULL, *trk;
    u32 i, at;

    if ( md->format == 2 ) {
        while ( md->cur_track < md->tracks ) {
            trk = &md->track[md->cur_track];
            if ( !trk->done ) {
                return trk;
            }
            at = trk->tick;             /* the next one starts where this one ended */
            md->cur_track++;
            if ( md->cur_track < md->tracks ) {
                trk = &md->track[md->cur_track];
                trk->tick = at;
                md_next_delta( trk );
            }
        }
        return NULL;
    }
    for ( i = 0; i < md->tracks; i++ ) {
        trk = &md->track[i];
        if ( !trk->done && (best == NULL || trk->tick < best->tick) ) {
            best = trk;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* channels                                                            */
/* ------------------------------------------------------------------ */
static void md_chan_derive( MDCHAN *ch )
{
    s32 range;
    u32 att;

    range = (s32)ch->bend_semis * 64 + (s32)ch->bend_cents * 64 / 100;
    ch->bend64 = ((s32)ch->bend - 8192) * range / 8192;
    ch->tune64 = ((s32)ch->coarse - 64) * 64 + ((s32)ch->fine - 8192) / 128;
    att = (u32)md_vol_att[ch->volume] + md_vol_att[ch->expression];
    ch->vol_att = att > 4095 ? 4095 : att;
}

/* what controller 121 resets (General MIDI's list: not volume or pan) */
static void md_chan_controllers( MDCHAN *ch )
{
    ch->modulation = 0;
    ch->expression = 127;
    ch->sustain = 0;
    ch->rpn_msb = ch->rpn_lsb = 127;
    ch->bend = 8192;
    ch->pressure = 0;
    md_chan_derive( ch );
}

static void md_reset_channels( MDSTATE *md )
{
    MDCHAN *ch;
    int i;

    for ( i = 0; i < MD_CHANNELS; i++ ) {
        ch = &md->chan[i];
        mem_set( ch, 0, sizeof( MDCHAN ) );
        ch->volume = 100;
        ch->pan = 64;
        ch->bend_semis = 2;
        ch->coarse = 64;
        ch->fine = 8192;
        md_chan_controllers( ch );
    }
}

/* ------------------------------------------------------------------ */
/* what the outputs share                                              */
/* ------------------------------------------------------------------ */
void md_stage( MDSTATE *md, u8 kind, u8 bank, u8 reg, u8 val )
{
    DECEV *ev;

    if ( md->mode != MM_PLAY || md->stage_count >= MD_STAGE ) {
        return;
    }
    ev = &md->stage[md->stage_count++];
    ev->frame = 0;
    ev->kind = kind;
    ev->reg = reg;
    ev->val = val;
    ev->bank = bank;
}

static void md_mpu( MDSTATE *md, u8 byte )
{
    md_stage( md, EV_MPU, 0, 0, byte );
}

s32 md_voice_pitch( const MDSTATE *md, const MDVOICE *vc )
{
    const MDCHAN *ch = &md->chan[vc->chan];
    s32 pitch = vc->pitch;

    if ( !vc->drum ) {
        pitch += ch->bend64 + ch->tune64;
    }
    if ( pitch < 0 ) {
        pitch = 0;
    } else if ( pitch > 132 * 64 - 1 ) {
        pitch = 132 * 64 - 1;
    }
    return pitch;
}

u32 md_pitch_hz( s32 pitch )
{
    u32 oct, idx;

    if ( pitch < 0 ) {
        pitch = 0;
    } else if ( pitch > 132 * 64 - 1 ) {
        pitch = 132 * 64 - 1;
    }
    oct = (u32)pitch / 768;
    idx = (u32)pitch % 768;
    return (u32)fx_mul30( (s32)md_semi[idx / 64], (s32)md_fine[idx % 64] ) >> (10 - oct);
}

/* The OPL's frequency is fnum * 49716 / 2^(20 - block) Hz: the smallest
   block that keeps the F-number under 1024 keeps the most of it. */
void md_fnum( u32 hz, u32 *fnum, u32 *block )
{
    u32 num = fx_umuldiv( hz, 16, 49716 ), blk = 0;

    while ( num >= 1024 && blk < 7 ) {
        num = (num + 1) >> 1;
        blk++;
    }
    *fnum = num > 1023 ? 1023 : num;
    *block = blk;
}

u32 md_voice_att( const MDSTATE *md, const MDVOICE *vc )
{
    u32 att = (u32)md_vel_att[vc->vel & 0x7F] + md->chan[vc->chan].vol_att;

    return att > 4095 ? 4095 : att;
}

/* ------------------------------------------------------------------ */
/* the voices                                                          */
/* ------------------------------------------------------------------ */
static int md_is_opl( const MDSTATE *md )
{
    return md->synth == SYN_OPL2 || md->synth == SYN_OPL3;
}

static void out_start( MDSTATE *md, int v )
{
    if ( md_is_opl( md ) ) {
        opl_start( md, v );
    } else if ( md->synth == SYN_SOFT ) {
        syn_start( md, v );
    }
}

static void out_stop( MDSTATE *md, int v )
{
    if ( md_is_opl( md ) ) {
        opl_stop( md, v );
    } else if ( md->synth == SYN_SOFT ) {
        syn_stop( md, v );
    }
}

static void out_kill( MDSTATE *md, int v )
{
    if ( md_is_opl( md ) ) {
        opl_kill( md, v );
    } else if ( md->synth == SYN_SOFT ) {
        syn_kill( md, v );
    }
}

/* what = 0 pitch, 1 level, 2 pan, 3 vibrato, for every voice of a channel */
static void out_channel( MDSTATE *md, int chan, int what )
{
    u32 i;

    if ( md->mode != MM_PLAY ) {
        return;
    }
    for ( i = 0; i < md->voices; i++ ) {
        if ( md->voice[i].state == VS_FREE || md->voice[i].chan != chan ) {
            continue;
        }
        if ( md_is_opl( md ) ) {
            switch ( what ) {
            case 0:
                opl_pitch( md, (int)i );
                break;
            case 1:
                opl_level( md, (int)i );
                break;
            case 2:
                opl_pan( md, (int)i );
                break;
            default:
                opl_vibrato( md, (int)i );
                break;
            }
        } else if ( md->synth == SYN_SOFT ) {
            switch ( what ) {
            case 0:
            case 3:
                syn_pitch( md, (int)i );
                break;
            case 1:
                syn_level( md, (int)i );
                break;
            default:
                syn_pan( md, (int)i );
                break;
            }
        }
    }
}

/* A voice for a new note: a free one, else the one released longest
   ago, else the oldest */
static int md_find_voice( MDSTATE *md )
{
    int best = -1;
    u32 i;
    MDVOICE *vc;

    for ( i = 0; i < md->voices; i++ ) {
        if ( md->voice[i].state == VS_FREE ) {
            return (int)i;
        }
    }
    for ( i = 0; i < md->voices; i++ ) {
        vc = &md->voice[i];
        if ( vc->state == VS_OFF && (best < 0 || vc->off_frame < md->voice[best].off_frame
                                     || (vc->off_frame == md->voice[best].off_frame
                                         && vc->age < md->voice[best].age)) ) {
            best = (int)i;
        }
    }
    if ( best >= 0 ) {
        return best;
    }
    best = 0;
    for ( i = 1; i < md->voices; i++ ) {
        if ( md->voice[i].age < md->voice[best].age ) {
            best = (int)i;
        }
    }
    return best;
}

static void md_release( MDSTATE *md, int v )
{
    MDVOICE *vc = &md->voice[v];

    vc->state = VS_OFF;
    vc->held = 0;
    vc->off_frame = md->now;
    out_stop( md, v );
}

static void md_note_on( MDSTATE *md, int chan, int note, int vel )
{
    const MDPATCH *patch;
    MDVOICE *vc;
    s32 pitch;
    int v = -1;
    u32 i;

    if ( chan == MD_DRUMCHAN ) {
        if ( note < MD_DRUM_FIRST || note > MD_DRUM_LAST ) {
            return;
        }
        patch = &md_drums[note - MD_DRUM_FIRST];
        pitch = (s32)patch->note * 64;
    } else {
        patch = &md_melodic[md->chan[chan].program & 0x7F];
        pitch = ((s32)note + patch->note) * 64;
    }
    /* the same note again on the same channel takes the voice it had */
    for ( i = 0; i < md->voices; i++ ) {
        vc = &md->voice[i];
        if ( vc->state != VS_FREE && vc->chan == chan && vc->note == note ) {
            v = (int)i;
            break;
        }
    }
    if ( v < 0 ) {
        v = md_find_voice( md );
    }
    vc = &md->voice[v];
    vc->state = VS_ON;
    vc->chan = (u8)chan;
    vc->note = (u8)note;
    vc->vel = (u8)vel;
    vc->held = 0;
    vc->drum = (u8)(chan == MD_DRUMCHAN);
    vc->patch = patch;
    vc->pitch = pitch;
    vc->age = ++md->age;
    vc->on_frame = vc->off_frame = md->now;
    out_start( md, v );
}

static void md_note_off( MDSTATE *md, int chan, int note )
{
    MDVOICE *vc;
    u32 i;

    for ( i = 0; i < md->voices; i++ ) {
        vc = &md->voice[i];
        if ( vc->state == VS_ON && vc->chan == chan && vc->note == note ) {
            if ( md->chan[chan].sustain ) {
                vc->held = 1;
            } else {
                md_release( md, (int)i );
            }
        }
    }
}

/* all notes off: as if each had been let go (the pedal still holds) */
static void md_all_off( MDSTATE *md, int chan )
{
    MDVOICE *vc;
    u32 i;

    for ( i = 0; i < md->voices; i++ ) {
        vc = &md->voice[i];
        if ( vc->state == VS_ON && vc->chan == chan ) {
            if ( md->chan[chan].sustain ) {
                vc->held = 1;
            } else {
                md_release( md, (int)i );
            }
        }
    }
}

/* all sound off: silent at once */
static void md_kill_channel( MDSTATE *md, int chan )
{
    u32 i;

    for ( i = 0; i < md->voices; i++ ) {
        if ( md->voice[i].state != VS_FREE && (chan < 0 || md->voice[i].chan == chan) ) {
            md->voice[i].state = VS_FREE;
            md->voice[i].held = 0;
            out_kill( md, (int)i );
        }
    }
}

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */
static void md_rpn_data( MDSTATE *md, int chan )
{
    MDCHAN *ch = &md->chan[chan];
    u8 msb = ch->cc[6], lsb = ch->cc[38];

    if ( ch->rpn_msb != 0 ) {
        return;
    }
    switch ( ch->rpn_lsb ) {
    case 0:                             /* the bend range */
        ch->bend_semis = (u8)(msb > 24 ? 24 : msb);
        ch->bend_cents = (u8)(lsb > 99 ? 99 : lsb);
        break;
    case 1:                             /* fine tuning */
        ch->fine = (u16)(((u16)msb << 7) | lsb);
        break;
    case 2:                             /* coarse tuning */
        ch->coarse = msb;
        break;
    default:
        return;
    }
    md_chan_derive( ch );
    out_channel( md, chan, 0 );
}

/* the notes the pedal was holding, let go */
static void md_release_held( MDSTATE *md, int chan )
{
    MDVOICE *vc;
    u32 i;

    if ( md->mode != MM_PLAY ) {
        return;
    }
    for ( i = 0; i < md->voices; i++ ) {
        vc = &md->voice[i];
        if ( vc->state == VS_ON && vc->chan == chan && vc->held ) {
            md_release( md, (int)i );
        }
    }
}

static void md_controller( MDSTATE *md, int chan, int num, int val )
{
    MDCHAN *ch = &md->chan[chan];

    ch->cc[num] = (u8)val;
    ch->cc_set[num >> 3] |= (u8)(1 << (num & 7));
    switch ( num ) {
    case 1:
        ch->modulation = (u8)val;
        out_channel( md, chan, 3 );
        break;
    case 6:
        ch->cc[38] = 0;                 /* a new MSB starts the value again */
        md_rpn_data( md, chan );
        break;
    case 38:
        md_rpn_data( md, chan );
        break;
    case 7:
        ch->volume = (u8)val;
        md_chan_derive( ch );
        out_channel( md, chan, 1 );
        break;
    case 10:
        ch->pan = (u8)val;
        out_channel( md, chan, 2 );
        break;
    case 11:
        ch->expression = (u8)val;
        md_chan_derive( ch );
        out_channel( md, chan, 1 );
        break;
    case 64:
        ch->sustain = (u8)(val >= 64);
        if ( !ch->sustain ) {
            md_release_held( md, chan );
        }
        break;
    case 98:
    case 99:                            /* an NRPN: data entry now goes nowhere we know */
        ch->rpn_msb = ch->rpn_lsb = 127;
        break;
    case 100:
        ch->rpn_lsb = (u8)val;
        break;
    case 101:
        ch->rpn_msb = (u8)val;
        break;
    case 120:
        if ( md->mode == MM_PLAY ) {
            md_kill_channel( md, chan );
        }
        break;
    case 121:
        md_chan_controllers( ch );
        out_channel( md, chan, 0 );
        out_channel( md, chan, 1 );
        out_channel( md, chan, 3 );
        md_release_held( md, chan );
        break;
    case 123:
    case 124:
    case 125:
    case 126:
    case 127:
        if ( md->mode == MM_PLAY ) {
            md_all_off( md, chan );
        }
        break;
    }
}

static void md_channel_msg( MDSTATE *md, u8 status, u8 d1, u8 d2 )
{
    int chan = status & 0x0F;
    MDCHAN *ch = &md->chan[chan];

    md->chan_used |= 1UL << chan;
    if ( md->mode == MM_SCAN ) {
        return;
    }
    if ( md->synth == SYN_MPU ) {
        md_mpu( md, status );
        md_mpu( md, d1 );
        if ( (status & 0xE0) != 0xC0 ) {
            md_mpu( md, d2 );
        }
    }
    switch ( status & 0xF0 ) {
    case 0x80:
        if ( md->mode == MM_PLAY ) {
            md_note_off( md, chan, d1 );
        }
        break;
    case 0x90:
        if ( md->mode == MM_PLAY ) {
            if ( d2 != 0 ) {
                md_note_on( md, chan, d1, d2 );
            } else {
                md_note_off( md, chan, d1 );
            }
        }
        break;
    case 0xB0:
        md_controller( md, chan, d1, d2 );
        break;
    case 0xC0:
        ch->program = d1;
        break;
    case 0xD0:
        ch->pressure = d1;
        break;
    case 0xE0:
        ch->bend = (u16)(d1 | ((u16)d2 << 7));
        md_chan_derive( ch );
        out_channel( md, chan, 0 );
        break;
    }
}

/* GM System On, GM2 On, the GS reset and XG System On: everything back
   to its default.  "msg" is what follows the F0. */
static int md_is_reset( const u8 *msg, u32 len )
{
    if ( len >= 4 && msg[0] == 0x7E && msg[2] == 0x09 && (msg[3] == 0x01 || msg[3] == 0x03) ) {
        return 1;
    }
    if ( len >= 7 && msg[0] == 0x41 && msg[2] == 0x42 && msg[3] == 0x12 && msg[4] == 0x40
         && msg[5] == 0x00 && msg[6] == 0x7F ) {
        return 1;
    }
    if ( len >= 6 && msg[0] == 0x43 && (msg[1] & 0xF0) == 0x10 && msg[2] == 0x4C && msg[3] == 0
         && msg[4] == 0 && msg[5] == 0x7E ) {
        return 1;
    }
    return 0;
}

/* The rest of a SysEx message being sent: as much as the stage holds */
static void md_sysex_more( MDSTATE *md )
{
    u32 room;

    md->stage_count = md->stage_used = 0;
    md->stage_frame = md->sx_frame;
    room = MD_STAGE;
    while ( md->sx_left > 0 && room > 0 ) {
        md_mpu( md, *md->sx_pos++ );
        md->sx_left--;
        room--;
    }
    if ( md->sx_left == 0 && md->sx_reset ) {
        md->hold = md->sx_frame + md->rate / 16;
        md->sx_reset = 0;
    }
}

static void md_sysex( MDSTATE *md, const u8 *msg, u32 len, int escape )
{
    int reset;

    if ( md->mode == MM_SCAN ) {
        return;
    }
    reset = !escape && md_is_reset( msg, len );
    if ( reset ) {
        md_reset_channels( md );
        if ( md->mode == MM_PLAY && md->synth != SYN_MPU ) {
            md_kill_channel( md, -1 );
        }
    }
    if ( md->synth != SYN_MPU ) {
        return;
    }
    if ( md->mode == MM_REPLAY ) {
        if ( reset ) {
            md->sysrecs = 0;            /* a reset undoes what came before it */
        }
        if ( md->sysrecs < MD_SYSREC ) {
            md->sysrec[md->sysrecs].offset = (u32)(msg - md->file);
            md->sysrec[md->sysrecs].len = len;
            md->sysrec[md->sysrecs].escape = (u8)escape;
            md->sysrecs++;
        }
        return;
    }
    if ( !escape ) {
        md_mpu( md, 0xF0 );
    }
    md->sx_pos = msg;
    md->sx_left = len;
    md->sx_frame = md->stage_frame;
    md->sx_reset = (u8)reset;
    /* what fits now goes now; the rest after the stage has gone out */
    while ( md->sx_left > 0 && md->stage_count < MD_STAGE ) {
        md_mpu( md, *md->sx_pos++ );
        md->sx_left--;
    }
    if ( md->sx_left == 0 && reset ) {
        md->hold = md->sx_frame + md->rate / 16;
        md->sx_reset = 0;
    }
}

/* The text of an event as a tag: UTF-8 if it is that, else Latin-1 */
static void md_text( char *dst, const u8 *src, u32 len )
{
    u32 i, k, need;
    int multi = 0;

    for ( i = 0; i < len; i++ ) {
        if ( src[i] < 0x80 ) {
            continue;
        }
        if ( (src[i] & 0xE0) == 0xC0 ) {
            need = 1;
        } else if ( (src[i] & 0xF0) == 0xE0 ) {
            need = 2;
        } else if ( (src[i] & 0xF8) == 0xF0 ) {
            need = 3;
        } else {
            break;
        }
        if ( need > len - i - 1 ) {
            break;
        }
        for ( k = 1; k <= need; k++ ) {
            if ( (src[i + k] & 0xC0) != 0x80 ) {
                break;
            }
        }
        if ( k <= need ) {
            break;
        }
        multi = 1;
        i += need;
    }
    if ( multi && i >= len ) {
        tag_utf8( dst, TAG_LEN, src, len );
    } else {
        tag_latin1( dst, TAG_LEN, src, len );
    }
}

static void md_meta( MDSTATE *md, MDTRACK *trk, u8 type, const u8 *data, u32 len )
{
    u32 uspq;

    switch ( type ) {
    case 0x51:                          /* tempo; SMPTE time has none */
        if ( len >= 3 && md->tpq != 0 ) {
            uspq = ((u32)data[0] << 16) | ((u32)data[1] << 8) | data[2];
            if ( uspq != 0 ) {
                md_new_tempo( md, trk->tick, uspq );
            }
        }
        break;
    case 0x01:                          /* text: a karaoke file's "@T" lines name the song */
        if ( md->mode == MM_SCAN && md->info != NULL && len > 2 && data[0] == '@'
             && data[1] == 'T' ) {
            if ( md->kar_titles == 0 && md->title_prio < 2 ) {
                md_text( md->info->title, data + 2, len - 2 );
                md->title_prio = 2;
            } else if ( md->kar_titles == 1 && md->artist_prio < 2 ) {
                md_text( md->info->artist, data + 2, len - 2 );
                md->artist_prio = 2;
            }
            md->kar_titles++;
        }
        break;
    case 0x02:                          /* copyright: shown as the artist when nothing else is */
        if ( md->mode == MM_SCAN && md->info != NULL && md->artist_prio < 1 && len > 0 ) {
            md_text( md->info->artist, data, len );
            md->artist_prio = 1;
        }
        break;
    case 0x03:                          /* the first track's name is the piece's */
        if ( md->mode == MM_SCAN && md->info != NULL && md->title_prio < 1 && len > 0
             && trk == &md->track[0] ) {
            md_text( md->info->title, data, len );
            md->title_prio = 1;
            /* what some programs call every first track is no title */
            if ( str_icmp( md->info->title, "control track" ) == 0
                 || str_icmp( md->info->title, "untitled" ) == 0 ) {
                md->info->title[0] = 0;
                md->title_prio = 0;
            }
        }
        break;
    }
}

/* One event of a track, at frame md->now */
static void md_event( MDSTATE *md, MDTRACK *trk )
{
    const u8 *at = trk->pos, *end = trk->end;
    u8 status, type, d1 = 0, d2 = 0;
    u32 len;

    if ( at >= end ) {
        trk->done = 1;
        return;
    }
    if ( *at & 0x80 ) {
        status = *at++;
    } else {
        status = trk->status;
        if ( status == 0 ) {            /* a data byte with nothing to belong to */
            trk->pos = at + 1;
            md_next_delta( trk );
            return;
        }
    }
    if ( status < 0xF0 ) {
        trk->status = status;
        /* a data byte that is not one ends the message, which is dropped */
        if ( at >= end || (*at & 0x80) ) {
            trk->pos = at;
            md_next_delta( trk );
            return;
        }
        d1 = *at++;
        if ( (status & 0xE0) != 0xC0 ) {
            if ( at >= end || (*at & 0x80) ) {
                trk->pos = at;
                md_next_delta( trk );
                return;
            }
            d2 = *at++;
        }
        trk->pos = at;
        md_channel_msg( md, status, d1, d2 );
    } else if ( status == 0xF0 || status == 0xF7 ) {
        if ( !md_vlq( &at, end, &len ) ) {
            trk->pos = at;
            trk->done = 1;
            return;
        }
        if ( len > (u32)(end - at) ) {
            len = (u32)(end - at);
        }
        md_sysex( md, at, len, status == 0xF7 );
        trk->pos = at + len;
    } else if ( status == 0xFF ) {
        if ( at >= end ) {
            trk->pos = at;
            trk->done = 1;
            return;
        }
        type = *at++;
        if ( !md_vlq( &at, end, &len ) ) {
            trk->pos = at;
            trk->done = 1;
            return;
        }
        if ( len > (u32)(end - at) ) {
            len = (u32)(end - at);
        }
        md_meta( md, trk, type, at, len );
        trk->pos = at + len;
        if ( type == 0x2F ) {
            trk->done = 1;
            return;
        }
    } else {
        trk->pos = at;                  /* F1..FE have no place in a file: stepped over */
    }
    md_next_delta( trk );
}

/* ------------------------------------------------------------------ */
/* the start of a piece, a seek, and the end                           */
/* ------------------------------------------------------------------ */
static void md_rewind( MDSTATE *md )
{
    MDTRACK *trk;
    u32 i;

    for ( i = 0; i < md->tracks; i++ ) {
        trk = &md->track[i];
        trk->pos = trk->start;
        trk->tick = 0;
        trk->status = 0;
        trk->done = 0;
        if ( md->format != 2 || i == 0 ) {
            md_next_delta( trk );
        }
    }
    md->cur_track = 0;
    md->uspq = TEMPO_DEFAULT;
    md->seg_tick = md->seg_int = md->seg_frac = 0;
    md_tempo_k( md );
    md->hold = 0;
    md_reset_channels( md );
    for ( i = 0; i < MD_MAXVOICES; i++ ) {
        md->voice[i].state = VS_FREE;
        md->voice[i].held = 0;
    }
    md->stage_count = md->stage_used = 0;
    md->sx_left = 0;
    md->sx_reset = 0;
    md->sysrecs = 0;
    md->ended = 0;
    md->job = JOB_NONE;
    md->job_step = 0;
    if ( md->synth == SYN_SOFT && md->syn != NULL ) {
        syn_reset( md );
    }
}

/* the stage emptied and placed for an event at "frame" */
static void md_begin( MDSTATE *md, u32 frame )
{
    md->now = frame;
    md->stage_count = md->stage_used = 0;
    md->stage_frame = frame > md->hold ? frame : md->hold;
}

/* MPU: one channel's state after a seek - what the file set, and only
   that */
static void md_mpu_state( MDSTATE *md, int chan )
{
    MDCHAN *ch = &md->chan[chan];
    u8 status = (u8)(0xB0 | chan);
    int num;

    if ( ch->cc_set[0] & 1 ) {
        md_mpu( md, status );
        md_mpu( md, 0 );
        md_mpu( md, ch->cc[0] );
    }
    if ( ch->cc_set[4] & 1 ) {
        md_mpu( md, status );
        md_mpu( md, 32 );
        md_mpu( md, ch->cc[32] );
    }
    if ( ch->program != 0 || (ch->cc_set[0] & 1) || (ch->cc_set[4] & 1) ) {
        md_mpu( md, (u8)(0xC0 | chan) );
        md_mpu( md, ch->program );
    }
    for ( num = 1; num < 120; num++ ) {
        if ( num == 6 || num == 32 || num == 38 || (num >= 96 && num <= 101)
             || !(ch->cc_set[num >> 3] & (1 << (num & 7))) ) {
            continue;
        }
        md_mpu( md, status );
        md_mpu( md, (u8)num );
        md_mpu( md, ch->cc[num] );
    }
    if ( ch->bend_semis != 2 || ch->bend_cents != 0 || ch->fine != 8192 || ch->coarse != 64 ) {
        static const u8 rpn_lsb[3] = { 0, 1, 2 };
        int k;
        u8 msb, lsb;

        for ( k = 0; k < 3; k++ ) {
            if ( k == 0 ) {
                msb = ch->bend_semis;
                lsb = ch->bend_cents;
            } else if ( k == 1 ) {
                msb = (u8)(ch->fine >> 7);
                lsb = (u8)(ch->fine & 0x7F);
            } else {
                msb = ch->coarse;
                lsb = 0;
            }
            md_mpu( md, status );
            md_mpu( md, 101 );
            md_mpu( md, 0 );
            md_mpu( md, status );
            md_mpu( md, 100 );
            md_mpu( md, rpn_lsb[k] );
            md_mpu( md, status );
            md_mpu( md, 6 );
            md_mpu( md, msb );
            md_mpu( md, status );
            md_mpu( md, 38 );
            md_mpu( md, lsb );
        }
        md_mpu( md, status );
        md_mpu( md, 101 );
        md_mpu( md, 127 );
        md_mpu( md, status );
        md_mpu( md, 100 );
        md_mpu( md, 127 );
    }
    if ( ch->bend != 8192 ) {
        md_mpu( md, (u8)(0xE0 | chan) );
        md_mpu( md, (u8)(ch->bend & 0x7F) );
        md_mpu( md, (u8)(ch->bend >> 7) );
    }
    if ( ch->pressure != 0 ) {
        md_mpu( md, (u8)(0xD0 | chan) );
        md_mpu( md, ch->pressure );
    }
}

/* MPU: every channel silent, notes and sound */
static void md_mpu_quiet( MDSTATE *md, int reset )
{
    int chan;

    for ( chan = 0; chan < MD_CHANNELS; chan++ ) {
        md_mpu( md, (u8)(0xB0 | chan) );
        md_mpu( md, 120 );
        md_mpu( md, 0 );
        md_mpu( md, (u8)(0xB0 | chan) );
        md_mpu( md, 123 );
        md_mpu( md, 0 );
        if ( reset ) {
            md_mpu( md, (u8)(0xB0 | chan) );
            md_mpu( md, 121 );
            md_mpu( md, 0 );
        }
    }
}

/* The next step of the writes that start a piece or follow a seek:
   the chip set up and silent, or the MIDI port's synthesizer reset and
   given the state the file had reached */
static void md_job_step( MDSTATE *md )
{
    u32 step = md->job_step++, k;
    const MDSYSEX *rec;

    md_begin( md, md->job_frame );
    if ( md_is_opl( md ) ) {
        opl_setup( md );
        md->job = JOB_NONE;
        return;
    }
    if ( md->synth != SYN_MPU ) {
        md->job = JOB_NONE;
        return;
    }
    if ( step == 0 ) {
        for ( k = 0; k < sizeof( gm_on ); k++ ) {
            md_mpu( md, gm_on[k] );
        }
        md->hold = md->stage_frame + md->rate / 16;
        return;
    }
    if ( step == 1 ) {
        md_mpu_quiet( md, 1 );
        return;
    }
    step -= 2;
    if ( step < md->sysrecs ) {
        rec = &md->sysrec[step];
        if ( !rec->escape ) {
            md_mpu( md, 0xF0 );
        }
        md->sx_pos = md->file + rec->offset;
        md->sx_left = rec->len;
        md->sx_frame = md->stage_frame;
        md->sx_reset = (u8)(!rec->escape && md_is_reset( md->sx_pos, rec->len ));
        while ( md->sx_left > 0 && md->stage_count < MD_STAGE ) {
            md_mpu( md, *md->sx_pos++ );
            md->sx_left--;
        }
        if ( md->sx_left == 0 && md->sx_reset ) {
            md->hold = md->sx_frame + md->rate / 16;
            md->sx_reset = 0;
        }
        return;
    }
    step -= md->sysrecs;
    if ( step < MD_CHANNELS ) {
        md_mpu_state( md, (int)step );
        if ( step == MD_CHANNELS - 1 ) {
            md->job = JOB_NONE;
        }
        return;
    }
    md->job = JOB_NONE;
}

/* The last event has been read: every note let go, and the time they
   are given to die away worked out */
static void md_song_over( MDSTATE *md )
{
    u32 i, at = md->out_at, end, tail;
    MDVOICE *vc;
    int chan;

    md_begin( md, at );
    for ( i = 0; i < md->voices; i++ ) {
        if ( md->voice[i].state == VS_ON ) {
            md_release( md, (int)i );
        }
    }
    if ( md->synth == SYN_MPU ) {
        for ( chan = 0; chan < MD_CHANNELS; chan++ ) {
            md_mpu( md, (u8)(0xB0 | chan) );
            md_mpu( md, 123 );
            md_mpu( md, 0 );
        }
    }
    if ( md->synth == SYN_SOFT ) {
        md->tail_end = at + SOFT_TAIL * md->rate;
    } else {
        tail = at;
        for ( i = 0; i < md->voices; i++ ) {
            vc = &md->voice[i];
            if ( vc->state != VS_FREE ) {
                end = vc->off_frame + TAIL_SECONDS * md->rate;
                if ( end > tail ) {
                    tail = end;
                }
            }
        }
        md->tail_end = tail;
    }
    md->ended = 1;
}

static void md_end_silence( MDSTATE *md )
{
    md_begin( md, md->tail_end );
    if ( md_is_opl( md ) ) {
        opl_silence( md );
    } else if ( md->synth == SYN_MPU ) {
        md_mpu_quiet( md, 0 );
    } else if ( md->syn != NULL ) {
        syn_reset( md );
    }
    md_kill_channel( md, -1 );
}

/* ------------------------------------------------------------------ */
/* decoding                                                            */
/* ------------------------------------------------------------------ */
/* the output (silence, or the synthesizer) up to frame "to" */
static void md_output_to( MDSTATE *md, u32 to )
{
    u32 count;
    s16 *at;

    if ( to <= md->out_at ) {
        return;
    }
    count = to - md->out_at;
    at = md->pcm + (md->out_at - md->piece_start) * md->out_ch;
    if ( md->synth == SYN_SOFT ) {
        syn_render( md, at, count );
    } else {
        mem_set( at, 0, count * md->out_ch * sizeof( s16 ) );
    }
    md->out_at = to;
}

/* The stage into the player's buffer.  0 when the room ran out first. */
static int md_flush( MDSTATE *md, DECBUF *buf )
{
    DECEV *ev;

    if ( buf->ev == NULL || buf->ev_max == 0 ) {
        md->stage_count = md->stage_used = 0;   /* nowhere to put them */
        return 1;
    }
    while ( md->stage_used < md->stage_count ) {
        if ( buf->ev_count >= buf->ev_max ) {
            return 0;
        }
        ev = &buf->ev[buf->ev_count++];
        *ev = md->stage[md->stage_used++];
        ev->frame = md->stage_frame - md->piece_start;
    }
    md->stage_count = md->stage_used = 0;
    return 1;
}

/* What is sounding, for the analyzer: each note at its frequency's band
   (and a little at its harmonics'), as loud as its velocity, volume
   and expression, less what time has taken off since it began */
static void md_levels( MDSTATE *md, DECBUF *buf, u32 at )
{
    static const u8 harm_band[3] = { 0, 4, 6 };
    static const u8 harm_drop[3] = { 0, 40, 72 };
    const MDVOICE *vc;
    u32 i, k, att, ms, decay;
    s32 band, height, note;

    mem_set( buf->levels, 0, LEVEL_BANDS );
    for ( i = 0; i < md->voices; i++ ) {
        vc = &md->voice[i];
        if ( vc->state == VS_FREE ) {
            continue;
        }
        att = md_voice_att( md, vc );
        ms = at > vc->on_frame ? fx_umuldiv( at - vc->on_frame, 1000, md->rate ) : 0;
        if ( ms > 60000 ) {
            ms = 60000;                 /* long since at the floor; and ms * 256 stays in range */
        }
        decay = ms * 256 / 600;         /* 6 dB a 0.6 s while held, to 18 dB down */
        if ( decay > 768 ) {
            decay = 768;
        }
        if ( vc->drum ) {
            decay += ms * 256 / 120;
        }
        if ( vc->state == VS_OFF ) {
            ms = at > vc->off_frame ? fx_umuldiv( at - vc->off_frame, 1000, md->rate ) : 0;
            decay += ms > 2000 ? 5120 : ms * 256 / 100;    /* then 6 dB a 0.1 s */
        }
        height = 255 - (s32)((att + decay) >> 3);
        if ( height <= 0 ) {
            continue;
        }
        if ( vc->drum ) {
            band = md_drum_band[vc->note - MD_DRUM_FIRST];
        } else {
            note = md_voice_pitch( md, vc ) / 64;
            band = (note - 23) * 32 / 108;
        }
        for ( k = 0; k < 3; k++ ) {
            s32 bk = band + harm_band[k], hk = height - harm_drop[k];

            if ( vc->drum && k > 0 ) {
                break;
            }
            if ( bk < 0 ) {
                bk = 0;
            }
            if ( bk >= LEVEL_BANDS || hk <= 0 ) {
                continue;
            }
            if ( hk > buf->levels[bk] ) {
                buf->levels[bk] = (u8)hk;
            }
        }
    }
}

static int md_decode( void *state, DECBUF *buf )
{
    MDSTATE *md = (MDSTATE *)state;
    MDTRACK *trk;
    u32 start = md->frame, end, at, frac, step;

    buf->frames = 0;
    buf->ev_count = 0;
    if ( md->ended == 3 ) {
        mem_set( buf->levels, 0, LEVEL_BANDS );
        return DE_OK;
    }
    md->pcm = buf->pcm;
    md->piece_start = start;
    md->out_at = start;
    end = buf->max > MD_ENDFRAME - start ? MD_ENDFRAME : start + buf->max;
    for ( ;; ) {
        if ( md->stage_used < md->stage_count ) {
            if ( md->stage_frame >= end ) {
                break;                  /* they belong to a later piece */
            }
            md_output_to( md, md->stage_frame );
            if ( !md_flush( md, buf ) ) {
                end = md->stage_frame;
                break;
            }
            continue;
        }
        if ( md->sx_left > 0 ) {
            md_sysex_more( md );
            continue;
        }
        if ( md->job != JOB_NONE ) {
            md_job_step( md );
            continue;
        }
        if ( md->ended == 1 ) {
            at = md->tail_end;
            if ( md->synth == SYN_SOFT ) {
                /* the tail ends as soon as the last voice has died away */
                while ( md->out_at < end && md->out_at < at && syn_sounding( md ) ) {
                    step = end - md->out_at;
                    if ( step > at - md->out_at ) {
                        step = at - md->out_at;
                    }
                    if ( step > 256 ) {
                        step = 256;
                    }
                    md_output_to( md, md->out_at + step );
                }
                if ( !syn_sounding( md ) && md->out_at < at ) {
                    at = md->out_at;
                    md->tail_end = at;
                }
            }
            if ( at >= end ) {
                break;
            }
            md_output_to( md, at );
            md_end_silence( md );
            md->ended = 2;
            continue;
        }
        if ( md->ended == 2 ) {
            md->ended = 3;
            end = md->out_at;
            break;
        }
        trk = md_next_track( md );
        if ( trk == NULL ) {
            md_song_over( md );
            continue;
        }
        at = md_tick_frame( md, trk->tick, &frac );
        if ( at >= end ) {
            break;
        }
        md_output_to( md, at );
        md_begin( md, at );
        md_event( md, trk );
    }
    md_output_to( md, end );
    buf->frames = end - start;
    md->frame = end;
    if ( md->synth != SYN_SOFT ) {
        md_levels( md, buf, end );
    }
    return DE_OK;
}

static int md_seek( void *state, u32 ms, u32 *at_ms )
{
    MDSTATE *md = (MDSTATE *)state;
    MDTRACK *trk;
    u32 target, at, frac;

    if ( ms >= md->length_ms ) {
        target = md->song_end;
    } else {
        target = fx_umuldiv( ms, md->rate, 1000 );
        if ( target > md->song_end ) {
            target = md->song_end;
        }
    }
    md_rewind( md );
    md->mode = MM_REPLAY;
    for ( ;; ) {
        trk = md_next_track( md );
        if ( trk == NULL ) {
            break;
        }
        at = md_tick_frame( md, trk->tick, &frac );
        if ( at >= target ) {
            break;
        }
        md->now = at;
        md_event( md, trk );
    }
    md->mode = MM_PLAY;
    md->frame = target;
    md->job = JOB_START;
    md->job_step = 0;
    md->job_frame = target;
    *at_ms = fx_umuldiv( target, 1000, md->rate );
    return DE_OK;
}

/* ------------------------------------------------------------------ */
/* opening                                                             */
/* ------------------------------------------------------------------ */
static int md_detect( SRC *src, const u8 *head, u32 len, const char *ext )
{
    (void)src;
    if ( len >= 4 && mem_cmp( head, "MThd", 4 ) == 0 ) {
        return 100;
    }
    if ( len >= 12 && mem_cmp( head, "RIFF", 4 ) == 0 && mem_cmp( head + 8, "RMID", 4 ) == 0 ) {
        return 100;
    }
    if ( (str_cmp( ext, "MID" ) == 0 || str_cmp( ext, "MIDI" ) == 0 || str_cmp( ext, "RMI" ) == 0
          || str_cmp( ext, "KAR" ) == 0) && mem_find( head, len, "MThd" ) >= 0 ) {
        return 70;                      /* a MIDI file behind something else (MacBinary) */
    }
    return 0;
}

static void md_close( void *state )
{
    MDSTATE *md = (MDSTATE *)state;

    if ( md == NULL ) {
        return;
    }
    if ( md->syn != NULL ) {
        syn_close( md );
    }
    if ( md->track != NULL ) {
        xfree( md->track );
    }
    if ( md->file != NULL ) {
        xfree( md->file );
    }
    xfree( md );
}

/* A RIFF RMID file: the SMF's place in the "data" chunk, and the name
   and artist from an INFO list.  0 if there is no data chunk. */
static int md_riff( MDSTATE *md, u32 *smf, u32 *smf_len )
{
    const u8 *file = md->file;
    u32 pos = 12, size, body, chunks, sub, sub_end, sub_size;
    int found = 0;

    for ( chunks = 0; chunks < 1000 && pos + 8 <= md->file_len; chunks++ ) {
        size = LE32( file + pos + 4 );
        body = pos + 8;
        if ( size > md->file_len - body ) {
            size = md->file_len - body;
        }
        if ( mem_cmp( file + pos, "data", 4 ) == 0 && !found ) {
            *smf = body;
            *smf_len = size;
            found = 1;
        } else if ( mem_cmp( file + pos, "LIST", 4 ) == 0 && size >= 4
                    && mem_cmp( file + body, "INFO", 4 ) == 0 && md->info != NULL ) {
            sub = body + 4;
            sub_end = body + size;
            while ( sub + 8 <= sub_end ) {
                sub_size = LE32( file + sub + 4 );
                if ( sub_size > sub_end - sub - 8 ) {
                    sub_size = sub_end - sub - 8;
                }
                if ( mem_cmp( file + sub, "INAM", 4 ) == 0 ) {
                    tag_latin1( md->info->title, TAG_LEN, file + sub + 8, sub_size );
                    md->title_prio = 3;
                } else if ( mem_cmp( file + sub, "IART", 4 ) == 0 ) {
                    tag_latin1( md->info->artist, TAG_LEN, file + sub + 8, sub_size );
                    md->artist_prio = 3;
                }
                sub += 8 + sub_size + (sub_size & 1);
            }
        }
        if ( size + (size & 1) > md->file_len - body ) {
            break;
        }
        pos = body + size + (size & 1);
    }
    return found;
}

/* The track chunks after the header, as far as the file goes, at most
   "max" of them.  With md->track NULL they are only counted.  Bytes
   that are not a chunk are stepped over to the next "MTrk". */
static u32 md_find_tracks( MDSTATE *md, const u8 *pos, const u8 *end, u32 max )
{
    u32 count = 0, size, i;
    int at;

    while ( count < max && (u32)(end - pos) >= 8 ) {
        for ( i = 0; i < 4; i++ ) {
            if ( pos[i] < 0x20 || pos[i] > 0x7E ) {
                break;
            }
        }
        if ( i < 4 ) {
            at = mem_find( pos + 1, (u32)(end - pos - 1), "MTrk" );
            if ( at < 0 ) {
                break;
            }
            pos += 1 + at;
            continue;
        }
        size = BE32( pos + 4 );
        if ( size > (u32)(end - pos) - 8 ) {
            size = (u32)(end - pos) - 8;    /* a chunk longer than the file is cut to it */
        }
        if ( mem_cmp( pos, "MTrk", 4 ) == 0 ) {
            if ( md->track != NULL ) {
                md->track[count].start = pos + 8;
                md->track[count].end = pos + 8 + size;
            }
            count++;
        }
        pos += 8 + size;
    }
    return count;
}

static int md_open( SRC *src, const DECOPT *opt, DECINFO *info, void **state )
{
    MDSTATE *md;
    MDTRACK *trk;
    u32 len, smf = 0, smf_len, hlen, division, fps, tpf, rate, at, frac, count, i;
    const u8 *hdr;
    int riff = 0, pos, rc;

    *state = NULL;
    md = (MDSTATE *)try_alloc( sizeof( MDSTATE ) );
    if ( md == NULL ) {
        return DE_NOMEM;
    }
    mem_set( md, 0, sizeof( MDSTATE ) );
    md->info = info;

    /* the file, into memory */
    len = src->size < MAX_FILE ? src->size : MAX_FILE;
    if ( len < 14 ) {
        md_close( md );
        return DE_FORMAT;
    }
    md->file = (u8 *)try_alloc( len );
    if ( md->file == NULL ) {
        md_close( md );
        return DE_NOMEM;
    }
    src_seek( src, 0 );
    md->file_len = src_read( src, md->file, len );
    if ( md->file_len != len ) {
        rc = src->error ? DE_READ : DE_FORMAT;
        md_close( md );
        return rc;
    }

    /* where the SMF is */
    smf_len = md->file_len;
    if ( mem_cmp( md->file, "MThd", 4 ) == 0 ) {
        smf = 0;
    } else if ( mem_cmp( md->file, "RIFF", 4 ) == 0 && mem_cmp( md->file + 8, "RMID", 4 ) == 0 ) {
        if ( !md_riff( md, &smf, &smf_len ) ) {
            md_close( md );
            return DE_FORMAT;
        }
        riff = 1;
    } else {
        pos = mem_find( md->file, md->file_len < SRC_PEEK ? md->file_len : SRC_PEEK, "MThd" );
        if ( pos < 0 ) {
            md_close( md );
            return DE_FORMAT;
        }
        smf = (u32)pos;
        smf_len = md->file_len - smf;
    }
    hdr = md->file + smf;
    if ( smf_len < 14 || mem_cmp( hdr, "MThd", 4 ) != 0 ) {
        md_close( md );
        return DE_FORMAT;
    }
    hlen = BE32( hdr + 4 );
    if ( hlen < 6 || hlen > smf_len - 8 ) {
        md_close( md );
        return DE_FORMAT;
    }
    md->format = BE16( hdr + 8 );
    if ( md->format > 2 ) {
        md->format = 1;
    }
    division = BE16( hdr + 12 );
    if ( division & 0x8000 ) {
        fps = 256 - (division >> 8);    /* the high byte is minus the frames a second */
        tpf = division & 0xFF;
        if ( tpf == 0 ) {
            tpf = 1;
        }
        if ( fps == 29 ) {              /* 29.97: 30000/1001 */
            md->smpte_num = 1001;
            md->smpte_den = 30000 * tpf;
        } else {
            md->smpte_num = 1;
            md->smpte_den = fps * tpf;
        }
        md->tpq = 0;
    } else {
        md->tpq = division != 0 ? division : 96;
    }

    /* the tracks */
    count = md_find_tracks( md, hdr + 8 + hlen, hdr + smf_len, MAX_TRACKS );
    if ( count == 0 ) {
        md_close( md );
        return DE_FORMAT;
    }
    md->track = (MDTRACK *)try_alloc( count * sizeof( MDTRACK ) );
    if ( md->track == NULL ) {
        md_close( md );
        return DE_NOMEM;
    }
    mem_set( md->track, 0, count * sizeof( MDTRACK ) );
    md->tracks = md_find_tracks( md, hdr + 8 + hlen, hdr + smf_len, count );
    if ( md->format == 0 && md->tracks > 1 ) {
        md->format = 1;                 /* more than a format 0 file should have: all together */
    }

    /* what is played, and how fast */
    md->synth = opt->synth <= SYN_MPU ? opt->synth : SYN_SOFT;
    md->fast = opt->fast;
    md->mono = opt->mono;
    rate = opt->rate;
    if ( rate == 0 ) {
        rate = 22050;
    } else if ( rate < 8000 ) {
        rate = 8000;
    } else if ( rate > 96000UL ) {
        rate = 96000UL;
    }
    for ( i = 0; i < opt->down && i < 2 && (rate >> 1) >= 8000; i++ ) {
        rate >>= 1;
    }
    md->rate = rate;
    switch ( md->synth ) {
    case SYN_OPL2:
        md->voices = 9;
        md->out_ch = 1;
        break;
    case SYN_OPL3:
        md->voices = 18;
        md->out_ch = 1;
        break;
    case SYN_MPU:
        md->voices = MPU_VOICES;
        md->out_ch = 1;
        break;
    default:
        md->voices = opt->fast ? 12 : 24;
        md->out_ch = (u8)(opt->mono ? 1 : 2);
        rc = syn_open( md );
        if ( rc != DE_OK ) {
            md_close( md );
            return rc;
        }
        break;
    }

    /* the whole file once, for its length and its names */
    md_rewind( md );
    md->mode = MM_SCAN;
    for ( ;; ) {
        trk = md_next_track( md );
        if ( trk == NULL ) {
            break;
        }
        at = md_tick_frame( md, trk->tick, &frac );
        md->now = at;
        md->last_frame = at;
        md_event( md, trk );
    }
    md->song_end = md->last_frame;
    md->length_ms = fx_umuldiv( md->song_end, 1000, md->rate );
    md->info = NULL;
    md_rewind( md );
    md->mode = MM_PLAY;
    md->frame = 0;
    md->job = JOB_START;
    md->job_frame = 0;

    info->rate = md->rate;
    info->file_rate = md->rate;
    info->channels = md->out_ch;
    count = 0;
    for ( i = 0; i < MD_CHANNELS; i++ ) {
        if ( md->chan_used & (1UL << i) ) {
            count++;
        }
    }
    info->file_channels = (u8)(count ? count : 1);
    info->bits = 0;
    info->flags = DIF_SEEK;
    if ( md->synth != SYN_SOFT ) {
        info->flags |= DIF_EVENTS | DIF_LEVELS;
    }
    info->length_ms = md->length_ms;
    info->kbps = md->length_ms ? fx_umuldiv( smf_len, 8, md->length_ms ) : 0;
    sfmt( info->format, sizeof( info->format ), "%sMIDI type %u, %u track%s", riff ? "RIFF " : "",
          md->format, md->tracks, md->tracks == 1 ? "" : "s" );
    *state = md;
    return DE_OK;
}

const CODEC codec_midi = {
    "MIDI",
    "MID MIDI RMI KAR",
    md_detect,
    md_open,
    md_decode,
    md_seek,
    md_close
};
