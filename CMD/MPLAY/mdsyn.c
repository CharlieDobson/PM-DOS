/*
 * MDSYN.C - the software synthesizer: MIDI voices played as
 * two-operator FM, for a sound card with no FM chip (MIDI.H).
 *
 * IT PLAYS THE SAME BANK THE OPL DOES, THE WAY AN OPL DOES.  Each voice
 * is a modulator and a carrier, each with a phase, a waveform (the
 * OPL's four: sine, half sine, rectified sine, quarter pulses), a
 * frequency multiple, a total level, key scaling of level and rate, and
 * an envelope that attacks, decays to a sustain level and holds there
 * or goes on falling, and releases.  The modulator feeds back on itself
 * and either moves the carrier's phase or is heard beside it.
 *
 * THERE IS NO MULTIPLY IN THE SAMPLE LOOP.  Like the chip, the
 * synthesizer works in logarithms: a waveform table gives the sine's
 * attenuation, the envelope and the levels are attenuations too, they
 * are added, and one table of 2^-x and a shift turn the sum into an
 * amplitude.  Pan is two more attenuations, so a stereo voice costs two
 * look-ups more and still no multiply.  A voice's modulator is run over
 * a block first, into a small buffer, and then its carrier, so that each
 * loop keeps few enough values for a 486's registers: some sixty
 * instructions a sample for a voice with feedback, fewer without, and
 * fewer again for a voice in the middle, which needs one look-up for
 * both sides.
 *
 * THE ENVELOPE MOVES IN STEPS OF SYN_BLOCK SAMPLES.  At every step each
 * operator's attenuation for the end of the next block is worked out,
 * and the samples in between move to it in equal amounts, so a fast
 * attack or a release is a smooth ramp and not a click.  With fast = 1
 * the steps are twice as far apart and the vibrato and tremolo are left
 * out.
 *
 * NOTHING STARTS FROM A SOUND THAT IS STILL THERE.  A voice taken for a
 * new note while it is still heard first fades out in two blocks (a
 * millisecond or two) and then starts the new note at phase 0, where the
 * sine is 0.  A note that is let go in the first block of its life is
 * let go at the end of that block, so that even a note of no length is
 * heard.
 *
 * A 22 KHZ RATE HOLDS LESS THAN A CHIP'S 49.7 KHZ.  FM puts partials
 * far above the note, and what passes half the rate folds back as
 * noise.  So a modulator above a twelfth of the rate is turned down
 * 6 dB an octave, which keeps the width of the spectrum about where it
 * was, and a carrier above half the rate is played octaves lower.
 *
 * LOUDNESS: one voice at full level is a third of full scale, which
 * leaves room for the usual handful of voices at the usual levels.  An
 * orchestra playing loudly with two dozen voices would still go over,
 * so the master gain comes down for as long as that lasts and goes back
 * up slowly (syn_out), and whatever gets past it is held at full scale.
 * That is one multiply a sample of output, not a voice.
 */
#include "rt.h"
#include "fix.h"
#include "dec.h"
#include "midi.h"

#define SYN_BLOCK       16              /* samples between envelope steps */
#define SYN_BLOCK_FAST  32
#define MIX_MAX         32              /* frames mixed at a time */
#define ATT_MAX         4095            /* silence, in attenuation units */
#define ENV_MAX         ((int32_t)ATT_MAX << 16)
#define ENV_DONE        (8L << 16)      /* an attack this close to the top has arrived */
#define AUDIBLE         3300            /* quieter than this (about -78 dB) is not heard */
#define MIX_SHIFT       2               /* a voice at full level is a quarter of full scale... */
#define GAIN_NOMINAL    5793            /* ...times 1.41 (+3 dB, Q12) */
#define OUT_LIMIT       (30934UL << (12 + MIX_SHIFT))   /* 0.5 dB under full scale */
#define GAIN_RECOVER_MS 500             /* the gain from 0 back to nominal */
#define VIB_DEPTH       530             /* 14 cents, Q16, as the OPL's deep vibrato */
#define MOD_CENTS       50              /* controller 1 at 127 */
#define TREM_DEPTH      43              /* 1 dB in units, the OPL's light tremolo */
#define KSL_FROM        661             /* G1, 1/256 octave: where key scaling begins */

enum { ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE, ST_QUICK, ST_OFF };

/* one operator: [0] is the level the modulator feeds back from, [1]
   and [2] the left and right (or the only) outputs */
typedef struct {
    uint32_t phase, inc, inc_base;
    int32_t  env;               /* the envelope's attenuation, 16 bits of fraction */
    int32_t  tot[3], step[3];   /* the attenuations now, and their change a sample */
    uint32_t base;              /* total level, key scaling, velocity, volume */
    uint32_t ksl, alias;        /* parts of base */
    uint8_t  ar, dr, rr;        /* rates as the chip has them, 0..63 */
    uint8_t  stage, egt, am, vib;
    int32_t  sl;                /* the sustain level */
    const uint16_t *wave;
} SOP;

typedef struct {
    SOP      op[2];
    int32_t  fb1, fb2;          /* the modulator's last two outputs */
    int32_t  fb_mask;           /* 0 for no feedback */
    uint32_t fb_shift;
    uint32_t pan[3];            /* 0, left, right */
    uint32_t vib_depth;         /* Q16 */
    uint8_t  con;               /* 1: the operators are added */
    uint8_t  active;
    uint8_t  pending;           /* a new note waits for the old one to fade */
    uint8_t  stop_wanted;       /* let go before its first block was out */
    uint8_t  young;             /* still in its first block */
    uint32_t level;             /* the quietest output's attenuation at the last step */
} SVOICE;

typedef struct {
    uint16_t wave[4][1024];     /* bit 15 the sign, the rest attenuation */
    uint32_t dec_step[64];      /* a decay rate as units a sample, Q16 */
    uint32_t atk_frac[64];      /* an attack: the share of the attenuation a block takes off, Q16 */
    uint32_t quick;             /* the steal fade, a sample */
    uint32_t block, blk_left;
    uint32_t lfo_vib, lfo_am, vib_inc, am_inc;
    int32_t  vib_now;           /* -256..256 */
    uint32_t am_now;            /* units */
    int32_t  alias_pitch;       /* a modulator above this pitch is turned down */
    uint32_t gain, recover;     /* the master gain (Q12), and how far it may rise a frame (Q8) */
    int32_t  mix[MIX_MAX * 2];
    uint32_t pmod[MIX_MAX];     /* a modulator's block, as the carrier's phase offsets */
    SVOICE   v[MD_MAXVOICES];
} SYN;

/* -log2( sin( (i + 0.5) * pi / 512 ) ) * 256: a quarter of a sine as
   attenuation units (TOOLS\MKTAB.PL) */
static const uint16_t syn_logsin[256] = {
    2137, 1731, 1543, 1419, 1326, 1252, 1190, 1137, 1091, 1050, 1013,  979,
     949,  920,  894,  869,  846,  825,  804,  785,  767,  749,  732,  717,
     701,  687,  672,  659,  646,  633,  621,  609,  598,  587,  576,  566,
     556,  546,  536,  527,  518,  509,  501,  492,  484,  476,  468,  461,
     453,  446,  439,  432,  425,  418,  411,  405,  399,  392,  386,  380,
     375,  369,  363,  358,  352,  347,  341,  336,  331,  326,  321,  316,
     311,  307,  302,  297,  293,  289,  284,  280,  276,  271,  267,  263,
     259,  255,  251,  248,  244,  240,  236,  233,  229,  226,  222,  219,
     215,  212,  209,  205,  202,  199,  196,  193,  190,  187,  184,  181,
     178,  175,  172,  169,  167,  164,  161,  159,  156,  153,  151,  148,
     146,  143,  141,  138,  136,  134,  131,  129,  127,  125,  122,  120,
     118,  116,  114,  112,  110,  108,  106,  104,  102,  100,   98,   96,
      94,   92,   91,   89,   87,   85,   83,   82,   80,   78,   77,   75,
      74,   72,   70,   69,   67,   66,   64,   63,   62,   60,   59,   57,
      56,   55,   53,   52,   51,   49,   48,   47,   46,   45,   43,   42,
      41,   40,   39,   38,   37,   36,   35,   34,   33,   32,   31,   30,
      29,   28,   27,   26,   25,   24,   23,   23,   22,   21,   20,   20,
      19,   18,   17,   17,   16,   15,   15,   14,   13,   13,   12,   12,
      11,   10,   10,    9,    9,    8,    8,    7,    7,    7,    6,    6,
       5,    5,    5,    4,    4,    4,    3,    3,    3,    2,    2,    2,
       2,    1,    1,    1,    1,    1,    1,    1,    0,    0,    0,    0,
       0,    0,    0,    0
};

/* 32768 * 2^(-i / 256): an attenuation's fraction as an amplitude */
static const uint16_t syn_exp[256] = {
    32768, 32679, 32591, 32503, 32415, 32327, 32240, 32153, 32066, 31979, 31893, 31806,
    31720, 31635, 31549, 31464, 31379, 31294, 31209, 31125, 31041, 30957, 30873, 30790,
    30706, 30623, 30541, 30458, 30376, 30293, 30212, 30130, 30048, 29967, 29886, 29805,
    29725, 29644, 29564, 29484, 29405, 29325, 29246, 29167, 29088, 29009, 28931, 28852,
    28774, 28697, 28619, 28542, 28464, 28388, 28311, 28234, 28158, 28082, 28006, 27930,
    27855, 27779, 27704, 27629, 27554, 27480, 27406, 27332, 27258, 27184, 27110, 27037,
    26964, 26891, 26818, 26746, 26674, 26601, 26530, 26458, 26386, 26315, 26244, 26173,
    26102, 26031, 25961, 25891, 25821, 25751, 25681, 25612, 25543, 25474, 25405, 25336,
    25268, 25199, 25131, 25063, 24995, 24928, 24860, 24793, 24726, 24659, 24593, 24526,
    24460, 24394, 24328, 24262, 24196, 24131, 24066, 24001, 23936, 23871, 23806, 23742,
    23678, 23614, 23550, 23486, 23423, 23359, 23296, 23233, 23170, 23108, 23045, 22983,
    22921, 22859, 22797, 22735, 22674, 22613, 22552, 22491, 22430, 22369, 22309, 22248,
    22188, 22128, 22068, 22009, 21949, 21890, 21831, 21772, 21713, 21654, 21595, 21537,
    21479, 21421, 21363, 21305, 21247, 21190, 21133, 21076, 21019, 20962, 20905, 20849,
    20792, 20736, 20680, 20624, 20568, 20513, 20457, 20402, 20347, 20292, 20237, 20182,
    20127, 20073, 20019, 19965, 19911, 19857, 19803, 19750, 19696, 19643, 19590, 19537,
    19484, 19431, 19379, 19326, 19274, 19222, 19170, 19118, 19066, 19015, 18963, 18912,
    18861, 18810, 18759, 18708, 18658, 18607, 18557, 18507, 18457, 18407, 18357, 18308,
    18258, 18209, 18160, 18110, 18061, 18013, 17964, 17915, 17867, 17819, 17770, 17722,
    17674, 17627, 17579, 17531, 17484, 17437, 17390, 17343, 17296, 17249, 17202, 17156,
    17109, 17063, 17017, 16971, 16925, 16879, 16834, 16788, 16743, 16697, 16652, 16607,
    16562, 16518, 16473, 16428
};

/* how long the chip's envelope takes at each of its 64 rates, in
   microseconds: an attack from silence to full (2826.24 ms at rate 4,
   half as long for every 4 more, x 4 / (4 + rate % 4) between; 60 and
   up are at once), and a decay through 96 dB (39280.64 ms at rate 4,
   the same steps).  0 is never.  From the OPL's published rate tables
   (TOOLS\MKTAB.PL) */
static const uint32_t syn_atk_us[64] = {
           0UL,        0UL,        0UL,        0UL,  2826240UL,  2260992UL,  1884160UL,  1614994UL,
     1413120UL,  1130496UL,   942080UL,   807497UL,   706560UL,   565248UL,   471040UL,   403749UL,
      353280UL,   282624UL,   235520UL,   201874UL,   176640UL,   141312UL,   117760UL,   100937UL,
       88320UL,    70656UL,    58880UL,    50469UL,    44160UL,    35328UL,    29440UL,    25234UL,
       22080UL,    17664UL,    14720UL,    12617UL,    11040UL,     8832UL,     7360UL,     6309UL,
        5520UL,     4416UL,     3680UL,     3154UL,     2760UL,     2208UL,     1840UL,     1577UL,
        1380UL,     1104UL,      920UL,      789UL,      690UL,      552UL,      460UL,      394UL,
         345UL,      276UL,      230UL,      197UL,        1UL,        1UL,        1UL,        1UL
};

static const uint32_t syn_dec_us[64] = {
           0UL,        0UL,        0UL,        0UL, 39280640UL, 31424512UL, 26187093UL, 22446080UL,
    19640320UL, 15712256UL, 13093547UL, 11223040UL,  9820160UL,  7856128UL,  6546773UL,  5611520UL,
     4910080UL,  3928064UL,  3273387UL,  2805760UL,  2455040UL,  1964032UL,  1636693UL,  1402880UL,
     1227520UL,   982016UL,   818347UL,   701440UL,   613760UL,   491008UL,   409173UL,   350720UL,
      306880UL,   245504UL,   204587UL,   175360UL,   153440UL,   122752UL,   102293UL,    87680UL,
       76720UL,    61376UL,    51147UL,    43840UL,    38360UL,    30688UL,    25573UL,    21920UL,
       19180UL,    15344UL,    12787UL,    10960UL,     9590UL,     7672UL,     6393UL,     5480UL,
        4795UL,     3836UL,     3197UL,     2740UL,     2398UL,     1918UL,     1598UL,     1370UL
};

/* the OPL's frequency multiples, doubled: 1/2, 1, 2 ... 10, 10, 12, 12, 15, 15 */
static const uint8_t syn_mult2[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };

/* the same as pitch: 768 * log2( multiple ), 1/64 semitone */
static const int16_t syn_mult_pitch[16] = {
    -768, 0, 768, 1217, 1536, 1783, 1985, 2156, 2304, 2434, 2551, 2551, 2753, 2753, 3000, 3000
};

/* ------------------------------------------------------------------ */
/* setting up                                                          */
/* ------------------------------------------------------------------ */
/* 2^-x, x with 16 bits of fraction, as Q16 */
static uint32_t syn_exp2neg( uint32_t x )
{
    uint32_t ip = x >> 16, idx = (x >> 8) & 0xFF, e0, e1, val;

    if ( ip >= 16 ) {
        return 0;
    }
    e0 = syn_exp[idx];
    e1 = idx < 255 ? syn_exp[idx + 1] : 16384;
    val = e0 - (((e0 - e1) * (x & 0xFF)) >> 8);
    return (val << 1) >> ip;
}

int syn_open( MDSTATE *md )
{
    SYN *syn;
    unsigned int i, q, j;
    uint32_t mag, samples, x, lo, hi, mid, nyq;

    syn = (SYN *)try_alloc( sizeof( SYN ) );
    if ( syn == NULL ) {
        return DE_NOMEM;
    }
    mem_set( syn, 0, sizeof( SYN ) );
    for ( i = 0; i < 1024; i++ ) {
        q = i >> 8;
        j = i & 0xFF;
        if ( q & 1 ) {
            j = 255 - j;
        }
        mag = syn_logsin[j];
        syn->wave[0][i] = (uint16_t)(mag | (q >= 2 ? 0x8000 : 0));
        syn->wave[1][i] = (uint16_t)(q < 2 ? mag : ATT_MAX);
        syn->wave[2][i] = (uint16_t)mag;
        syn->wave[3][i] = (uint16_t)((q & 1) ? ATT_MAX : mag);
    }
    syn->block = md->fast ? SYN_BLOCK_FAST : SYN_BLOCK;
    syn->blk_left = syn->block;
    for ( i = 4; i < 64; i++ ) {
        samples = fx_umuldiv( syn_dec_us[i], md->rate, 1000000UL );
        syn->dec_step[i] = 0x10000000UL / (samples ? samples : 1);
        samples = fx_umuldiv( syn_atk_us[i], md->rate, 1000000UL );
        if ( i >= 60 || samples < 2 ) {
            syn->atk_frac[i] = 0x10000UL;
        } else {
            /* from silence to 8 units is 9 halvings of the attenuation */
            x = 9UL * syn->block * 0x10000UL / samples;
            syn->atk_frac[i] = x >= (16UL << 16) ? 0x10000UL : 0x10000UL - syn_exp2neg( x );
        }
    }
    syn->quick = (uint32_t)ENV_MAX / (2 * syn->block);
    syn->gain = GAIN_NOMINAL;
    syn->recover = GAIN_NOMINAL * 256UL / GAIN_RECOVER_MS * 1000UL / md->rate;
    syn->vib_inc = fx_umuldiv( 399770UL, 65536UL, md->rate );      /* 6.1 Hz */
    syn->am_inc = fx_umuldiv( 242483UL, 65536UL, md->rate );       /* 3.7 Hz */
    /* the pitch of a twelfth of the rate */
    nyq = (md->rate / 12) << 16;
    lo = 0;
    hi = 132 * 64 - 1;
    while ( lo < hi ) {
        mid = (lo + hi) / 2;
        if ( md_pitch_hz( (int32_t)mid ) < nyq ) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    syn->alias_pitch = (int32_t)lo;
    md->syn = syn;
    return DE_OK;
}

void syn_close( MDSTATE *md )
{
    if ( md->syn != NULL ) {
        xfree( md->syn );
        md->syn = NULL;
    }
}

void syn_reset( MDSTATE *md )
{
    SYN *syn = (SYN *)md->syn;
    unsigned int i;

    for ( i = 0; i < MD_MAXVOICES; i++ ) {
        syn->v[i].active = 0;
        syn->v[i].pending = 0;
    }
    syn->gain = GAIN_NOMINAL;
}

int syn_sounding( MDSTATE *md )
{
    SYN *syn = (SYN *)md->syn;
    unsigned int i;

    for ( i = 0; i < md->voices; i++ ) {
        if ( syn->v[i].active ) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* a voice's numbers                                                   */
/* ------------------------------------------------------------------ */
/* A phase step for "hz" (Q16) times a multiple.  A carrier above half
   the rate goes down an octave at a time until it is under it: a high
   bell at 11 kHz keeps its note, a hi-hat its noise.  A modulator is
   left to wrap round - it aliases, but by then the alias guard has
   turned it well down, and for the noise of a cymbal it does not
   matter.  Half the note is under 8 kHz and the rate at least 8000, so
   the step for it fits in 32 bits before the multiple. */
static uint32_t syn_inc( const MDSTATE *md, uint32_t hz, int mult, int carrier )
{
    uint32_t m2 = syn_mult2[mult & 15], nyq = (md->rate / 2) << 16, half = hz >> 1;

    if ( carrier ) {
        while ( half > 0 && half >= nyq / m2 ) {
            half >>= 1;
        }
    }
    return fx_umuldiv( half, 65536UL, md->rate ) * m2;
}

static void syn_freqs( MDSTATE *md, int voice )
{
    SYN *syn = (SYN *)md->syn;
    SVOICE *sv = &syn->v[voice];
    const MDVOICE *vc = &md->voice[voice];
    const MDCHAN *ch = &md->chan[vc->chan];
    uint32_t hz = md_pitch_hz( md_voice_pitch( md, vc ) ), depth;
    int k;

    for ( k = 0; k < 2; k++ ) {
        sv->op[k].inc_base = syn_inc( md, hz, vc->patch->op[k].misc & 15, k == 1 || sv->con );
        sv->op[k].inc = sv->op[k].inc_base;
    }
    depth = vc->drum ? 0 : (uint32_t)ch->modulation * (MOD_CENTS * 3787UL / 100) / 127;
    sv->vib_depth = depth;
}

/* the levels: total level, key scaling, velocity, volume, expression */
static void syn_bases( MDSTATE *md, int voice )
{
    SYN *syn = (SYN *)md->syn;
    SVOICE *sv = &syn->v[voice];
    const MDVOICE *vc = &md->voice[voice];
    const MDPATCH *patch = vc->patch;
    uint32_t att = md_voice_att( md, vc ), bright = md_vel_att[vc->vel & 0x7F] >> 2, base;
    int k;

    for ( k = 0; k < 2; k++ ) {
        base = (uint32_t)(patch->op[k].level & 0x3F) * 32 + sv->op[k].ksl;
        if ( k == 1 || sv->con ) {
            base += att;
        } else {
            base += bright + sv->op[k].alias;
        }
        sv->op[k].base = base > ATT_MAX ? ATT_MAX : base;
    }
}

static void syn_pans( MDSTATE *md, int voice )
{
    SYN *syn = (SYN *)md->syn;
    SVOICE *sv = &syn->v[voice];
    uint32_t pan = md->chan[md->voice[voice].chan].pan;

    if ( pan < 1 ) {
        pan = 1;
    }
    sv->pan[0] = 0;
    if ( md->out_ch == 1 ) {
        sv->pan[1] = sv->pan[2] = md_pan_att[64];
    } else {
        sv->pan[1] = md_pan_att[pan];
        sv->pan[2] = md_pan_att[128 - pan];
    }
}

/* ------------------------------------------------------------------ */
/* the envelope                                                        */
/* ------------------------------------------------------------------ */
/* An operator's envelope over the next "count" samples */
static void syn_env( SYN *syn, SOP *op, unsigned int count )
{
    int32_t env = op->env;
    uint32_t frac;

    switch ( op->stage ) {
    case ST_ATTACK:
        frac = syn->atk_frac[op->ar];
        if ( count != syn->block ) {
            frac = frac * count / syn->block;
        }
        env -= fx_mul16( env, (int32_t)frac );
        if ( env < ENV_DONE ) {
            env = 0;
            op->stage = ST_DECAY;
        }
        break;
    case ST_DECAY:
        env += (int32_t)(syn->dec_step[op->dr] * count);
        if ( env >= op->sl ) {
            env = op->sl;
            op->stage = op->egt ? ST_SUSTAIN : ST_RELEASE;
        }
        break;
    case ST_RELEASE:
        env += (int32_t)(syn->dec_step[op->rr] * count);
        if ( env >= ENV_MAX ) {
            env = ENV_MAX;
            op->stage = ST_OFF;
        }
        break;
    case ST_QUICK:
        env += (int32_t)(syn->quick * count);
        if ( env >= ENV_MAX ) {
            env = ENV_MAX;
            op->stage = ST_OFF;
        }
        break;
    case ST_OFF:
        env = ENV_MAX;
        break;
    }
    op->env = env;
}

/* Where an operator's outputs will be in "count" samples, and the steps
   to them.  -> the quietest of them */
static uint32_t syn_aim( SYN *syn, SVOICE *sv, int k, int first, int last, unsigned int count )
{
    SOP *op = &sv->op[k];
    uint32_t att, aim, quiet = ATT_MAX;
    int32_t diff;
    int x;

    att = (uint32_t)(op->env >> 16) + op->base + (op->am ? syn->am_now : 0);
    for ( x = first; x <= last; x++ ) {
        aim = att + sv->pan[x];
        if ( aim > ATT_MAX ) {
            aim = ATT_MAX;
        }
        if ( aim < quiet ) {
            quiet = aim;
        }
        diff = ((int32_t)aim << 16) - op->tot[x];
        if ( count == SYN_BLOCK ) {
            op->step[x] = diff >> 4;
        } else if ( count == SYN_BLOCK_FAST ) {
            op->step[x] = diff >> 5;
        } else {
            op->step[x] = diff / (int32_t)count;
        }
    }
    return quiet;
}

static void syn_begin( MDSTATE *md, int voice );

/* One voice's step: its envelopes, vibrato and the ramps for the next
   "count" samples.  "first" is the step a note starts with. */
static void syn_tick( MDSTATE *md, int voice, unsigned int count, int first )
{
    SYN *syn = (SYN *)md->syn;
    SVOICE *sv = &syn->v[voice];
    uint32_t quiet, depth;
    int32_t shift;
    int k;

    if ( !first ) {
        if ( sv->stop_wanted ) {
            sv->stop_wanted = 0;
            for ( k = 0; k < 2; k++ ) {
                if ( sv->op[k].stage < ST_RELEASE ) {
                    sv->op[k].stage = ST_RELEASE;
                }
            }
        }
        sv->young = 0;
    }
    syn_env( syn, &sv->op[0], count );
    syn_env( syn, &sv->op[1], count );
    if ( sv->op[1].stage == ST_OFF && (!sv->con || sv->op[0].stage == ST_OFF) ) {
        if ( sv->pending ) {
            syn_begin( md, voice );
            return;
        }
        sv->active = 0;
        if ( md->voice[voice].state != VS_FREE ) {
            md->voice[voice].state = VS_FREE;
        }
        return;
    }
    if ( !md->fast ) {
        for ( k = 0; k < 2; k++ ) {
            depth = sv->vib_depth;
            if ( sv->op[k].vib && depth < VIB_DEPTH ) {
                depth = VIB_DEPTH;
            }
            if ( depth != 0 ) {
                shift = (int32_t)depth * syn->vib_now / 256;
                sv->op[k].inc = sv->op[k].inc_base
                                + (uint32_t)fx_mul16( (int32_t)sv->op[k].inc_base, shift );
            } else {
                sv->op[k].inc = sv->op[k].inc_base;
            }
        }
    }
    if ( sv->con ) {
        quiet = syn_aim( syn, sv, 0, 0, md->out_ch, count );
        k = (int)syn_aim( syn, sv, 1, 1, md->out_ch, count );
        sv->level = (uint32_t)k < quiet ? (uint32_t)k : quiet;
    } else {
        syn_aim( syn, sv, 0, 0, 0, count );
        sv->level = syn_aim( syn, sv, 1, 1, md->out_ch, count );
    }
}

/* A rate of the patch (0..15) as the chip runs it: four steps for each,
   plus the key scaling; 0 stays 0, which never moves */
static uint8_t syn_rate( uint32_t rate, uint32_t ks )
{
    if ( rate == 0 ) {
        return 0;
    }
    rate = rate * 4 + ks;
    return (uint8_t)(rate > 63 ? 63 : rate);
}

/* The note the voice now holds, from the start */
static void syn_begin( MDSTATE *md, int voice )
{
    SYN *syn = (SYN *)md->syn;
    SVOICE *sv = &syn->v[voice];
    const MDVOICE *vc = &md->voice[voice];
    const MDPATCH *patch = vc->patch;
    const MDOP *src;
    SOP *op;
    uint32_t fnum, block, ks, rate, sl;
    int32_t pitch, over;
    int k, x, fb;

    pitch = md_voice_pitch( md, vc );
    md_fnum( md_pitch_hz( pitch ), &fnum, &block );
    ks = block * 2 + (fnum >> 9);
    sv->con = (uint8_t)(patch->fbc & 1);
    fb = (patch->fbc >> 1) & 7;
    sv->fb_mask = fb ? -1L : 0;
    sv->fb_shift = 10 + (uint32_t)fb;
    sv->fb1 = sv->fb2 = 0;
    for ( k = 0; k < 2; k++ ) {
        op = &sv->op[k];
        src = &patch->op[k];
        op->phase = 0;
        op->env = ENV_MAX;
        for ( x = 0; x < 3; x++ ) {
            op->tot[x] = ENV_MAX;
            op->step[x] = 0;
        }
        op->stage = ST_ATTACK;
        op->egt = (uint8_t)((src->misc & OP_EGT) != 0);
        op->am = (uint8_t)((src->misc & OP_AM) != 0);
        op->vib = (uint8_t)((src->misc & OP_VIB) != 0);
        op->wave = syn->wave[src->wave & 3];
        rate = (src->misc & OP_KSR) ? ks : ks >> 2;
        op->ar = syn_rate( src->ad >> 4, rate );
        op->dr = syn_rate( src->ad & 15, rate );
        op->rr = syn_rate( src->sr & 15, rate );
        sl = src->sr >> 4;
        op->sl = sl == 15 ? ENV_MAX : (int32_t)(sl * 128) << 16;
        /* key scaling of level: 6, 3 or 1.5 dB an octave above G1 */
        over = pitch / 3 - KSL_FROM;
        op->ksl = 0;
        if ( over > 0 ) {
            switch ( src->level >> 6 ) {
            case 1:
                op->ksl = (uint32_t)over / 2;
                break;
            case 2:
                op->ksl = (uint32_t)over / 4;
                break;
            case 3:
                op->ksl = (uint32_t)over;
                break;
            }
        }
        op->alias = 0;
    }
    over = (pitch + syn_mult_pitch[patch->op[0].misc & 15] - syn->alias_pitch) / 3;
    if ( over > 0 ) {
        sv->op[0].alias = (uint32_t)over;
    }
    syn_freqs( md, voice );
    syn_bases( md, voice );
    syn_pans( md, voice );
    sv->active = 1;
    sv->pending = 0;
    sv->stop_wanted = (uint8_t)(md->voice[voice].state == VS_OFF);
    syn_tick( md, voice, syn->blk_left, 1 );
    sv->young = 1;               /* a key-off now waits for the end of this block */
}

/* ------------------------------------------------------------------ */
/* what midi.c asks                                                    */
/* ------------------------------------------------------------------ */
void syn_start( MDSTATE *md, int voice )
{
    SYN *syn = (SYN *)md->syn;
    SVOICE *sv = &syn->v[voice];

    if ( sv->active && sv->level < AUDIBLE ) {
        sv->pending = 1;
        sv->stop_wanted = 0;
        if ( sv->op[0].stage != ST_OFF ) {
            sv->op[0].stage = ST_QUICK;
        }
        if ( sv->op[1].stage != ST_OFF ) {
            sv->op[1].stage = ST_QUICK;
        }
        return;
    }
    syn_begin( md, voice );
}

void syn_stop( MDSTATE *md, int voice )
{
    SYN *syn = (SYN *)md->syn;
    SVOICE *sv = &syn->v[voice];
    int k;

    if ( !sv->active ) {
        return;
    }
    if ( sv->pending || sv->young ) {
        sv->stop_wanted = 1;
        return;
    }
    for ( k = 0; k < 2; k++ ) {
        if ( sv->op[k].stage < ST_RELEASE ) {
            sv->op[k].stage = ST_RELEASE;
        }
    }
}

void syn_kill( MDSTATE *md, int voice )
{
    SYN *syn = (SYN *)md->syn;
    SVOICE *sv = &syn->v[voice];
    int k;

    sv->pending = 0;
    sv->stop_wanted = 0;
    if ( !sv->active ) {
        return;
    }
    for ( k = 0; k < 2; k++ ) {
        if ( sv->op[k].stage != ST_OFF ) {
            sv->op[k].stage = ST_QUICK;
        }
    }
}

void syn_pitch( MDSTATE *md, int voice )
{
    SYN *syn = (SYN *)md->syn;

    if ( syn->v[voice].active && !syn->v[voice].pending ) {
        syn_freqs( md, voice );
    }
}

void syn_level( MDSTATE *md, int voice )
{
    SYN *syn = (SYN *)md->syn;

    if ( syn->v[voice].active && !syn->v[voice].pending ) {
        syn_bases( md, voice );
    }
}

void syn_pan( MDSTATE *md, int voice )
{
    SYN *syn = (SYN *)md->syn;

    if ( syn->v[voice].active && !syn->v[voice].pending ) {
        syn_pans( md, voice );
    }
}

/* ------------------------------------------------------------------ */
/* making the samples                                                  */
/*                                                                     */
/* One operator's sample is: the waveform's attenuation at the phase,  */
/* plus the envelope's, as one number; its low 8 bits pick 2^-x from   */
/* syn_exp and the rest is a shift; the waveform's bit 15 the sign.    */
/* ------------------------------------------------------------------ */
/* The modulator over a block, as the phase offsets it gives the
   carrier.  On its own the loop keeps few enough values for a 486's
   registers. */
static void syn_mod( SVOICE *sv, uint32_t *pmod, unsigned int count )
{
    const uint16_t *wm = sv->op[0].wave;
    uint32_t pm = sv->op[0].phase, im = sv->op[0].inc, fs = sv->fb_shift, w, a;
    int32_t tm = sv->op[0].tot[0], sm = sv->op[0].step[0], f1 = sv->fb1, f2 = sv->fb2, val;
    uint32_t *end = pmod + count;

    if ( sv->fb_mask != 0 ) {
        while ( pmod < end ) {
            w = wm[(pm + ((uint32_t)(f1 + f2) << fs)) >> 22];
            a = (w & 0x0FFF) + ((uint32_t)tm >> 16);
            val = (int32_t)(syn_exp[a & 0xFF] >> (a >> 8));
            if ( w & 0x8000 ) {
                val = -val;
            }
            f2 = f1;
            f1 = val;
            *pmod++ = (uint32_t)val << 19;
            pm += im;
            tm += sm;
        }
        sv->fb1 = f1;
        sv->fb2 = f2;
    } else {
        while ( pmod < end ) {
            w = wm[pm >> 22];
            a = (w & 0x0FFF) + ((uint32_t)tm >> 16);
            val = (int32_t)(syn_exp[a & 0xFF] >> (a >> 8));
            if ( w & 0x8000 ) {
                val = -val;
            }
            *pmod++ = (uint32_t)val << 19;
            pm += im;
            tm += sm;
        }
    }
    sv->op[0].phase = pm;
    sv->op[0].tot[0] = tm;
}

/* The carrier, its left and right different */
static void syn_car_stereo( SVOICE *sv, const uint32_t *pmod, int32_t *mix, unsigned int count )
{
    const uint16_t *wc = sv->op[1].wave;
    uint32_t pc = sv->op[1].phase, ic = sv->op[1].inc, w, a, b;
    int32_t tl = sv->op[1].tot[1], sl = sv->op[1].step[1];
    int32_t tr = sv->op[1].tot[2], sr = sv->op[1].step[2];
    int32_t left, right, *end = mix + 2 * count;

    while ( mix < end ) {
        w = wc[(pc + *pmod++) >> 22];
        a = (w & 0x0FFF) + ((uint32_t)tl >> 16);
        b = (w & 0x0FFF) + ((uint32_t)tr >> 16);
        left = (int32_t)(syn_exp[a & 0xFF] >> (a >> 8));
        right = (int32_t)(syn_exp[b & 0xFF] >> (b >> 8));
        if ( w & 0x8000 ) {
            mix[0] -= left;
            mix[1] -= right;
        } else {
            mix[0] += left;
            mix[1] += right;
        }
        mix += 2;
        pc += ic;
        tl += sl;
        tr += sr;
    }
    sv->op[1].phase = pc;
    sv->op[1].tot[1] = tl;
    sv->op[1].tot[2] = tr;
}

/* The carrier with one output: the only channel ("step" 1), or the
   same sample to both sides ("step" 2, a voice in the middle) */
static void syn_car_one( SVOICE *sv, const uint32_t *pmod, int32_t *mix, unsigned int count,
                         unsigned int step )
{
    const uint16_t *wc = sv->op[1].wave;
    uint32_t pc = sv->op[1].phase, ic = sv->op[1].inc, w, a;
    int32_t tl = sv->op[1].tot[1], sl = sv->op[1].step[1], val, *end = mix + step * count;

    if ( step == 2 ) {
        while ( mix < end ) {
            w = wc[(pc + *pmod++) >> 22];
            a = (w & 0x0FFF) + ((uint32_t)tl >> 16);
            val = (int32_t)(syn_exp[a & 0xFF] >> (a >> 8));
            if ( w & 0x8000 ) {
                val = -val;
            }
            mix[0] += val;
            mix[1] += val;
            mix += 2;
            pc += ic;
            tl += sl;
        }
        sv->op[1].tot[2] = tl;
    } else {
        while ( mix < end ) {
            w = wc[(pc + *pmod++) >> 22];
            a = (w & 0x0FFF) + ((uint32_t)tl >> 16);
            val = (int32_t)(syn_exp[a & 0xFF] >> (a >> 8));
            if ( w & 0x8000 ) {
                val = -val;
            }
            *mix++ += val;
            pc += ic;
            tl += sl;
        }
    }
    sv->op[1].phase = pc;
    sv->op[1].tot[1] = tl;
}

/* One operator heard on its own (the two added): its outputs into the
   mix, and with "fb" its feedback from tot[0] as well */
static void syn_alone( SVOICE *sv, int k, int32_t *mix, unsigned int count, unsigned int outs, int fb )
{
    SOP *op = &sv->op[k];
    const uint16_t *wv = op->wave;
    uint32_t ph = op->phase, inc = op->inc, fs = sv->fb_shift, w, a;
    unsigned int i;
    int32_t t0 = op->tot[0], s0 = op->step[0], tl = op->tot[1], sl = op->step[1];
    int32_t tr = op->tot[2], sr = op->step[2], f1 = sv->fb1, f2 = sv->fb2, val;

    for ( i = 0; i < count; i++ ) {
        if ( fb ) {
            w = wv[(ph + ((uint32_t)(f1 + f2) << fs)) >> 22];
            a = (w & 0x0FFF) + ((uint32_t)t0 >> 16);
            val = (int32_t)(syn_exp[a & 0xFF] >> (a >> 8));
            f2 = f1;
            f1 = (w & 0x8000) ? -val : val;
            t0 += s0;
        } else {
            w = wv[ph >> 22];
        }
        a = (w & 0x0FFF) + ((uint32_t)tl >> 16);
        val = (int32_t)(syn_exp[a & 0xFF] >> (a >> 8));
        *mix += (w & 0x8000) ? -val : val;
        tl += sl;
        if ( outs == 2 ) {
            a = (w & 0x0FFF) + ((uint32_t)tr >> 16);
            val = (int32_t)(syn_exp[a & 0xFF] >> (a >> 8));
            mix[1] += (w & 0x8000) ? -val : val;
            tr += sr;
        }
        mix += outs;
        ph += inc;
    }
    op->phase = ph;
    op->tot[0] = t0;
    op->tot[1] = tl;
    op->tot[2] = tr;
    if ( fb ) {
        sv->fb1 = f1;
        sv->fb2 = f2;
    }
}

/* The mix into samples through the master gain.  When a block would go
   past OUT_LIMIT the gain comes down to what fits by the end of the
   block, and it goes back up over half a second; inside a block it
   moves in equal steps, never at once, because a step in the gain of a
   loud chord is a click.  The first samples of a block that got louder
   suddenly may still go past, and full scale holds those: a flattened
   peak is far less audible than a step.  Most of the time the gain sits
   at GAIN_NOMINAL. */
static void syn_out( SYN *syn, int16_t *pcm, unsigned int count, unsigned int outs )
{
    unsigned int total = count * outs, i, ch;
    uint32_t peak = 0, mag, target, fit;
    int32_t gain, step, val;

    for ( i = 0; i < total; i++ ) {
        mag = (uint32_t)(syn->mix[i] < 0 ? -syn->mix[i] : syn->mix[i]);
        if ( mag > peak ) {
            peak = mag;
        }
    }
    target = syn->gain + ((syn->recover * count) >> 8);
    if ( target > GAIN_NOMINAL ) {
        target = GAIN_NOMINAL;
    }
    if ( peak > 0 ) {
        fit = OUT_LIMIT / peak;
        if ( fit < target ) {
            target = fit;
        }
    }
    gain = (int32_t)syn->gain << 8;
    step = (((int32_t)target - (int32_t)syn->gain) << 8) / (int32_t)count;
    for ( i = 0; i < count; i++ ) {
        gain += step;
        for ( ch = 0; ch < outs; ch++ ) {
            val = fx_mulsh( syn->mix[i * outs + ch], gain >> 8, 12 + MIX_SHIFT );
            pcm[i * outs + ch] = (int16_t)FX_SAT16( val );
        }
    }
    syn->gain = target;
}

void syn_render( MDSTATE *md, int16_t *pcm, unsigned int frames )
{
    SYN *syn = (SYN *)md->syn;
    SVOICE *sv;
    unsigned int count, total, i, outs = md->out_ch;
    int32_t val;

    while ( frames > 0 ) {
        count = syn->blk_left;
        if ( count > frames ) {
            count = frames;
        }
        if ( count > MIX_MAX ) {
            count = MIX_MAX;
        }
        total = count * outs;
        mem_set( syn->mix, 0, total * sizeof( int32_t ) );
        for ( i = 0; i < md->voices; i++ ) {
            sv = &syn->v[i];
            if ( !sv->active ) {
                continue;
            }
            if ( sv->con ) {
                syn_alone( sv, 0, syn->mix, count, outs, sv->fb_mask != 0 );
                syn_alone( sv, 1, syn->mix, count, outs, 0 );
                continue;
            }
            syn_mod( sv, syn->pmod, count );
            if ( outs == 1 ) {
                syn_car_one( sv, syn->pmod, syn->mix, count, 1 );
            } else if ( sv->op[1].tot[1] == sv->op[1].tot[2]
                        && sv->op[1].step[1] == sv->op[1].step[2] ) {
                syn_car_one( sv, syn->pmod, syn->mix, count, 2 );
            } else {
                syn_car_stereo( sv, syn->pmod, syn->mix, count );
            }
        }
        syn_out( syn, pcm, count, outs );
        pcm += total;
        frames -= count;
        syn->blk_left -= count;
        if ( syn->blk_left == 0 ) {
            syn->blk_left = syn->block;
            if ( !md->fast ) {
                syn->lfo_vib += syn->vib_inc * syn->block;
                syn->lfo_am += syn->am_inc * syn->block;
                val = (int32_t)(syn->lfo_vib >> 22);
                syn->vib_now = val < 512 ? val - 256 : 768 - val;
                val = (int32_t)(syn->lfo_am >> 22);
                val = val < 512 ? val : 1023 - val;
                syn->am_now = (uint32_t)val * TREM_DEPTH / 512;
            }
            for ( i = 0; i < md->voices; i++ ) {
                if ( syn->v[i].active ) {
                    syn_tick( md, (int)i, syn->block, 0 );
                }
            }
        }
    }
}
