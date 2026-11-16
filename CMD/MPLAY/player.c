/*
 * PLAYER.C - MPLAY's engine: a file decoded, converted to what the
 * card plays, and kept flowing into the sound driver (PLAYER.H).
 *
 * THE CARD'S FORMAT FOLLOWS THE FILE'S: its rate, its channels, 16
 * bits - as near as the card comes.  A Sound Blaster Pro plays 8 bits
 * and at most 22 kHz in stereo, so a 44.1 kHz file asks its codec for
 * half rate first (which MP3 and the MDCT codecs give for less work,
 * not more), and only what is still different is converted here: two
 * channels to one, 16 bits to 8, and the rate by interpolating
 * between neighboring samples.
 *
 * DECODING RUNS AHEAD OF THE CARD by about half a second.  Each
 * pl_pump decodes pieces until the driver's ring holds that much, or
 * until its time is up, so a busy screen takes its turn and the sound
 * does not starve; a slow floppy read is covered by what is in the
 * ring.
 *
 * MIDI TO A CHIP IS TIMED BY THE CARD'S SAMPLES.  The codec gives
 * silence and register writes stamped with frames; the engine turns
 * each stamp into the card's frame count and queues it in the driver,
 * which writes it when the card gets there.  The card plays the
 * silence at 11 kHz in 8 bits - the clock costs next to nothing.
 *
 * WHEN THE PROCESSOR CANNOT KEEP UP the engine notices it before the
 * sound does: it adds up how long decoding took against how much sound
 * came out.  On "Auto" quality, more than nine tenths of the processor
 * for two seconds reopens the file one step cheaper - half rate, then
 * one channel, then quarter rate - at the place it had reached.
 *
 * WITH NO SOUND DRIVER the engine still plays, to nobody: the
 * millisecond clock stands in for the card, so the screen, the
 * analyzer and the position all behave, and a machine without a card
 * can still be used to look at files.
 */
#include "rt.h"
#include "sys.h"
#include "fix.h"
#include "dec.h"
#include "audio.h"
#include "player.h"
#include "eqtd.h"
#include "fft.h"

#define PIECE           2304UL          /* frames asked of a codec at a time */
#define EV_PIECE        2048UL          /* events it may give with them */
#define HIST            32768UL         /* frames of what was decoded, for the analyzer */
#define LEVEL_SNAPS     64
#define AHEAD_MS        600UL           /* how far decoding runs ahead */
#define START_MS        300UL           /* ...before the card is started */
#define MIDI_RATE       11025UL         /* the clock for MIDI to a chip */

/* ------------------------------------------------------------------ */
/* the card                                                            */
/* ------------------------------------------------------------------ */
static PLCARD card;
static int drv = -1;
static AUD_INFOBLOCK hw;
static int cpu_class = 2;
static u32 cpu_speed;
static char cpu_label[16] = "486";
static PLSET set = { PQ_AUTO, 0, -1, 0, 1 };

/* ------------------------------------------------------------------ */
/* the file                                                            */
/* ------------------------------------------------------------------ */
static SRC src;
static const CODEC *codec;
static void *dstate;
static DECINFO dinfo;
static DECOPT dopt;
static EQSET eq_live;
static char path_now[PATH_LEN];
static int state = PS_EMPTY;
static int events_mode;                 /* MIDI to a chip or a port */
static int eof_seen;
static int card_running;                /* AUD_START given */
static int card_open;

/* the output */
static AUD_OPENBLOCK ofmt;
static u32 in_rate, out_rate;
static int in_ch, out_ch, out_bits, out_frame;
static u32 rs_step;                     /* input frames an output frame, 16.16 */
static s32 rs_pos;                      /* where the next output frame is, 16.16, from
                                           the piece's first frame; -1.0 is the last
                                           frame of the piece before */
static s32 rs_prev[2];
static int volume = 100, balance;
static s32 gain_left = 256, gain_right = 256;   /* software volume, 256 = full */
static int soft_volume;                 /* the card has no mixer */

/* buffers */
static s16 *pcm;
static u8 *outbuf;
static u32 out_room;
static u32 out_have, out_used;          /* bytes converted and not yet taken */
static DECEV *dev;
static AUDEV *aev;
static u32 ev_have, ev_used;

/* counts since the file was opened or sought */
static u32 base_ms;                     /* where frame 0 is in the file */
static u32 dec_frames;                  /* decoded, at the codec's rate */
static u32 out_frames;                  /* converted, at the card's rate */
static u32 played_last;                 /* the card's count at the last look */
static u32 dry_frames;                  /* frames the card played that were not ours: silence */
static u32 underruns;
static u32 irq_seen;
static u8 drv_state_seen;

/* the clock that stands in for a card */
static u32 clock_start, clock_frames;
static int clock_running;

/* what was decoded, for the analyzer: mono, at the codec's rate */
static s16 *hist;
static u8 snap_level[LEVEL_SNAPS][LEVEL_BANDS];
static u32 snap_frame[LEVEL_SNAPS];
static u32 snap_count;

/* the load */
static u32 load_work_ms, load_sound_ms;
static int load_now;
static u32 load_high_since;
static int auto_step;                   /* steps taken down by the governor */

static int open_again( u32 at_ms );
static void synth_quiet( int drop );

/* ------------------------------------------------------------------ */
/* the processor                                                       */
/* ------------------------------------------------------------------ */
/* 1 if the AC flag (bit 18) can be changed: a 486 or later */
int cpu_has_ac( void );
#pragma aux cpu_has_ac = \
    "pushfd" \
    "pop eax" \
    "mov ecx,eax" \
    "xor eax,40000h" \
    "push eax" \
    "popfd" \
    "pushfd" \
    "pop eax" \
    "push ecx" \
    "popfd" \
    "xor eax,ecx" \
    "shr eax,18" \
    "and eax,1" \
    value [eax] modify exact [eax ecx];

/* 1 if the ID flag (bit 21) can be changed: CPUID is there */
int cpu_has_id( void );
#pragma aux cpu_has_id = \
    "pushfd" \
    "pop eax" \
    "mov ecx,eax" \
    "xor eax,200000h" \
    "push eax" \
    "popfd" \
    "pushfd" \
    "pop eax" \
    "push ecx" \
    "popfd" \
    "xor eax,ecx" \
    "shr eax,21" \
    "and eax,1" \
    value [eax] modify exact [eax ecx];

/* CPUID 1: the family in bits 8-11 */
u32 cpu_signature( void );
#pragma aux cpu_signature = \
    "push ebx" \
    "mov eax,1" \
    0x0F 0xA2 \
    "pop ebx" \
    value [eax] modify exact [eax ecx edx];

/* How many multiplies of the kind the decoders make the processor does
   in a millisecond, near enough: the loop runs for about 110 ms of the
   millisecond clock.  This is what decides the first guess at what
   the machine can play; the load measured while playing corrects it. */
static u32 bench( void )
{
    u32 start, now, count = 0, acc = 1, val = 0x12345678UL;
    int i;

    start = sys_clock_ms();
    do {
        for ( i = 0; i < 256; i++ ) {
            acc += (u32)fx_mulhi( (s32)val, (s32)(acc | 0x10000UL) );
            val = val * 69069UL + 1;
        }
        count += 256;
        now = sys_clock_ms();
    } while ( now - start < 110 && count < 100000000UL );
    if ( acc == 0 ) {
        count++;                        /* so the work is not optimized away */
    }
    return count / (now - start ? now - start : 1);
}

static void find_cpu( void )
{
    u32 speed, family;

    if ( !cpu_has_ac() ) {
        str_cpy( cpu_label, "386" );
    } else if ( !cpu_has_id() ) {
        str_cpy( cpu_label, "486" );
    } else {
        family = (cpu_signature() >> 8) & 15;
        str_cpy( cpu_label, family <= 4 ? "486" : family == 5 ? "Pentium" : "Pentium Pro or later" );
    }
    sys_clock_ms();                     /* the clock's first reading only starts it */
    speed = bench();
    cpu_speed = speed;
    /* measured in 86Box: a 486 DX4-100 gives 1156, a Pentium 60 1803;
       by the instruction timings a 486 DX2-66 is about 770 and a 386
       DX-40 about 440 - two full 32-bit multiplies a turn of the loop */
    if ( speed < 500 ) {
        cpu_class = 0;
    } else if ( speed < 1000 ) {
        cpu_class = 1;
    } else if ( speed < 1600 ) {
        cpu_class = 2;
    } else {
        cpu_class = 3;
    }
}

int pl_cpu_class( void )
{
    return cpu_class;
}

u32 pl_cpu_speed( void )
{
    return cpu_speed;
}

const char *pl_cpu_name( void )
{
    return cpu_label;
}

/* ------------------------------------------------------------------ */
/* the driver                                                          */
/* ------------------------------------------------------------------ */
static int drv_call( u32 fn, void *buf, u32 len, u32 *answer )
{
    u32 dummy;

    if ( drv < 0 ) {
        return 1;
    }
    return sys_driver_call( drv, fn, buf, len, answer != NULL ? answer : &dummy );
}

int pl_init( void )
{
    u32 answer;
    char num[16];

    mem_set( &card, 0, sizeof( card ) );
    str_cpy( card.name, "No sound card" );
    find_cpu();
    drv = sys_driver_find( NDTF_AUDIO );
    if ( drv >= 0 ) {
        mem_set( &hw, 0, sizeof( hw ) );
        if ( drv_call( AUD_INFO, &hw, sizeof( hw ), &answer ) != 0 || hw.sig != AUD_SIGVAL
             || !(hw.flags & AUDF_FOUND) ) {
            drv = -1;
        }
    }
    if ( drv >= 0 ) {
        card.present = 1;
        str_cpyt( card.name, hw.name, sizeof( card.name ) );
        card.flags = hw.flags;
        card.rate_max = hw.rate_max;
        card.rate_max2 = hw.rate_max2;
        card.bits = hw.bits;
        card.irq = hw.irq;
        card.line[0] = 0;
        if ( hw.port ) {
            sfmt( card.line, sizeof( card.line ), "%Xh", hw.port );
        }
        if ( hw.irq != AUD_NONE ) {
            sfmt( num, sizeof( num ), ", IRQ %u", hw.irq );
            str_catn( card.line, num, sizeof( card.line ) );
        }
        if ( hw.dma8 != AUD_NONE ) {
            sfmt( num, sizeof( num ), ", DMA %u", hw.dma8 );
            str_catn( card.line, num, sizeof( card.line ) );
        }
        if ( hw.dma16 != AUD_NONE ) {
            sfmt( num, sizeof( num ), hw.dma8 != AUD_NONE ? " and %u" : ", DMA %u", hw.dma16 );
            str_catn( card.line, num, sizeof( card.line ) );
        }
    }
    hist = (s16 *)try_alloc( HIST * sizeof( s16 ) );
    pcm = (s16 *)try_alloc( PIECE * 2 * sizeof( s16 ) );
    dev = (DECEV *)try_alloc( EV_PIECE * sizeof( DECEV ) );
    aev = (AUDEV *)try_alloc( EV_PIECE * sizeof( AUDEV ) );
    if ( hist == NULL || pcm == NULL || dev == NULL || aev == NULL ) {
        return 0;
    }
    mem_set( hist, 0, HIST * sizeof( s16 ) );
    eq_live.on = 0;
    eq_live.serial = 1;
    eqtd_init();
    return 1;
}

void pl_done( void )
{
    pl_close();
    eqtd_done();
    fft_done();
    if ( hist != NULL ) {
        xfree( hist );
        hist = NULL;
    }
    if ( pcm != NULL ) {
        xfree( pcm );
        pcm = NULL;
    }
    if ( dev != NULL ) {
        xfree( dev );
        dev = NULL;
    }
    if ( aev != NULL ) {
        xfree( aev );
        aev = NULL;
    }
}

const PLCARD *pl_card( void )
{
    return &card;
}

void pl_settings( const PLSET *newset )
{
    set = *newset;
}

const PLSET *pl_get_settings( void )
{
    return &set;
}

/* ------------------------------------------------------------------ */
/* the volume                                                          */
/* ------------------------------------------------------------------ */
static void apply_volume( void )
{
    s32 left = volume, right = volume;
    u8 lr[2];

    if ( balance > 0 ) {
        left = left * (100 - balance) / 100;
    } else if ( balance < 0 ) {
        right = right * (100 + balance) / 100;
    }
    /* a level that sounds even: the square of the slider */
    left = left * left * 255 / 10000;
    right = right * right * 255 / 10000;
    if ( card_open && !soft_volume ) {
        lr[0] = (u8)left;
        lr[1] = (u8)right;
        drv_call( AUD_VOLUME, lr, 2, NULL );
        gain_left = gain_right = 256;
    } else {
        gain_left = left * 256 / 255;
        gain_right = right * 256 / 255;
    }
}

void pl_set_volume( int vol, int bal )
{
    volume = vol < 0 ? 0 : vol > 100 ? 100 : vol;
    balance = bal < -100 ? -100 : bal > 100 ? 100 : bal;
    apply_volume();
}

void pl_set_eq( const EQSET *eq )
{
    u16 serial = (u16)(eq_live.serial + 1);

    eq_live = *eq;
    eq_live.serial = serial;
    eqtd_set( &eq_live, in_rate ? in_rate : 44100, in_ch ? in_ch : 2 );
}

/* ------------------------------------------------------------------ */
/* converting: the codec's samples to the card's bytes                 */
/* ------------------------------------------------------------------ */
/* one output frame from a left/right pair, onto "out" -> past it */
static u8 *put_frame( u8 *out, s32 left, s32 right )
{
    s32 mono;

    if ( soft_volume ) {
        left = (left * gain_left) >> 8;
        right = (right * gain_right) >> 8;
    }
    if ( out_ch == 1 ) {
        mono = (left + right) >> 1;
        if ( out_bits == 8 ) {
            *out++ = (u8)((FX_SAT16( mono ) >> 8) + 128);
        } else {
            *(s16 *)out = (s16)FX_SAT16( mono );
            out += 2;
        }
    } else if ( out_bits == 8 ) {
        out[0] = (u8)((FX_SAT16( left ) >> 8) + 128);
        out[1] = (u8)((FX_SAT16( right ) >> 8) + 128);
        out += 2;
    } else {
        ((s16 *)out)[0] = (s16)FX_SAT16( left );
        ((s16 *)out)[1] = (s16)FX_SAT16( right );
        out += 4;
    }
    return out;
}

/* "frames" of pcm into outbuf -> output frames made */
static u32 convert( const s16 *in, u32 frames )
{
    u8 *out = outbuf;
    u32 made = 0, i, idx, frac;
    s32 left, right, l0, r0, l1, r1;

    if ( frames == 0 ) {
        return 0;
    }
    if ( rs_step == 0x10000UL ) {
        /* the same rate: straight through, the common case */
        if ( in_ch == out_ch && out_bits == 16 && !soft_volume ) {
            mem_cpy( out, in, frames * in_ch * 2 );
            return frames;
        }
        for ( i = 0; i < frames; i++ ) {
            if ( in_ch == 2 ) {
                out = put_frame( out, in[i * 2], in[i * 2 + 1] );
            } else {
                out = put_frame( out, in[i], in[i] );
            }
        }
        return frames;
    }
    /* interpolating: rs_pos -1.0 is the last frame of the piece before */
    while ( rs_pos < (s32)((frames - 1) << 16) && (u32)(out - outbuf) + 4 <= out_room ) {
        if ( rs_pos < 0 ) {
            l0 = rs_prev[0];
            r0 = rs_prev[1];
            idx = 0;
            frac = (u32)(rs_pos + 0x10000L);
        } else {
            idx = (u32)rs_pos >> 16;
            frac = (u32)rs_pos & 0xFFFF;
            if ( in_ch == 2 ) {
                l0 = in[idx * 2];
                r0 = in[idx * 2 + 1];
            } else {
                l0 = r0 = in[idx];
            }
            idx++;
        }
        if ( in_ch == 2 ) {
            l1 = in[idx * 2];
            r1 = in[idx * 2 + 1];
        } else {
            l1 = r1 = in[idx];
        }
        left = l0 + (((l1 - l0) * (s32)(frac >> 1)) >> 15);
        right = r0 + (((r1 - r0) * (s32)(frac >> 1)) >> 15);
        out = put_frame( out, left, right );
        made++;
        rs_pos += (s32)rs_step;
    }
    rs_pos -= (s32)(frames << 16);
    if ( in_ch == 2 ) {
        rs_prev[0] = in[(frames - 1) * 2];
        rs_prev[1] = in[(frames - 1) * 2 + 1];
    } else {
        rs_prev[0] = rs_prev[1] = in[frames - 1];
    }
    return made;
}

/* what was decoded, mono, into the analyzer's history */
static void remember( const s16 *in, u32 frames )
{
    u32 i, at = dec_frames;

    if ( hist == NULL ) {
        return;
    }
    for ( i = 0; i < frames; i++ ) {
        hist[(at + i) & (HIST - 1)] = in_ch == 2 ? (s16)(((s32)in[i * 2] + in[i * 2 + 1]) >> 1) : in[i];
    }
}

/* ------------------------------------------------------------------ */
/* opening a file                                                      */
/* ------------------------------------------------------------------ */
static void close_card( void )
{
    if ( card_open ) {
        drv_call( AUD_DROPEV, NULL, 0, NULL );
        drv_call( AUD_STOP, NULL, 0, NULL );
        drv_call( AUD_CLOSE, NULL, 0, NULL );
        card_open = 0;
    }
    card_running = 0;
    if ( outbuf != NULL ) {
        xfree( outbuf );
        outbuf = NULL;
    }
}

void pl_close( void )
{
    close_card();
    if ( codec != NULL ) {
        codec->close( dstate );
        codec = NULL;
        dstate = NULL;
        src_close( &src );
    }
    state = PS_EMPTY;
}

/* ------------------------------------------------------------------ */
/* how hard a file may make the processor work                         */
/*                                                                     */
/* THE MODES, best first: everything; half rate; one channel; half     */
/* rate and one channel; quarter rate; quarter rate and one channel.   */
/* Half rate keeps the stereo picture and loses only the top octave,   */
/* which is why it comes before one channel.                           */
/*                                                                     */
/* WHAT EACH COSTS is measured, not guessed: the benchmark figure a    */
/* machine needs to decode 44.1 kHz stereo at exactly real time in     */
/* that mode, from MPTEST /X on an emulated 486 DX4-100 (benchmark     */
/* 1156) - MP3 0.99 times real time at full quality, 1.78 at half      */
/* rate - and the method checked on a Pentium 60 (benchmark 1803).     */
/* A file is opened in the best mode the machine plays at 1.3 times    */
/* real time or better, which leaves a quarter of the processor for   */
/* the screen and the card.  A mono file or a lower rate costs less in */
/* proportion.  Wave and VOC files cost next to nothing; MIDI's        */
/* synthesizer chooses its own rate.                                   */
/* ------------------------------------------------------------------ */
#define MODES           6
static const u8 mode_down[MODES] = { 0, 1, 0, 1, 2, 2 };
static const u8 mode_mono[MODES] = { 0, 0, 1, 1, 0, 1 };

typedef struct {
    const char *codec;          /* the CODEC's name */
    u16 cost[MODES];
} CODECCOST;

static const CODECCOST costs[] = {
    { "MPEG audio",          { 1168, 649, 605, 342, 327, 177 } },
    { "Ogg Vorbis",          {  741, 400, 472, 270, 247, 150 } }
#ifdef HAVE_WMA
  , { "Windows Media Audio", {  792, 453, 466, 292, 279, 203 } }      /* 128 kbps; 320 kbps costs a sixth more */
#endif
};

/* the mode for this file on this machine: 0 (everything) .. MODES - 1 */
static int auto_mode( const CODEC *dec, const DECINFO *info )
{
    u32 cost, speed = cpu_speed ? cpu_speed : 1;
    int i, mode;

    for ( i = 0; i < (int)(sizeof( costs ) / sizeof( costs[0] )); i++ ) {
        if ( str_cmp( dec->name, costs[i].codec ) != 0 ) {
            continue;
        }
        for ( mode = 0; mode < MODES; mode++ ) {
            if ( set.mono && !mode_mono[mode] ) {
                continue;
            }
            cost = fx_umuldiv( costs[i].cost[mode], info->file_rate ? info->file_rate : 44100, 44100 );
            if ( info->file_channels == 1 ) {
                cost = cost * 11 / 20;
            }
            /* speed / cost >= 1.3 */
            if ( speed * 10 >= cost * 13 ) {
                return mode;
            }
        }
        return MODES - 1;
    }
    return 0;
}

/* what the codec should be asked for, for this card and this machine */
static void choose_opts( DECOPT *opt, int mode )
{
    int down = 0, mono = set.mono, fast = 0;

    mem_set( opt, 0, sizeof( DECOPT ) );
    switch ( set.quality ) {
    case PQ_BEST:
        break;
    case PQ_HALF:
        down = 1;
        break;
    case PQ_QUARTER:
        down = 2;
        break;
    default:
        if ( mode >= MODES ) {
            mode = MODES - 1;
        }
        down = mode_down[mode];
        mono = mono || mode_mono[mode];
        fast = mode >= 4;
        break;
    }
    opt->down = (u8)down;

    opt->mono = (u8)mono;
    opt->fast = (u8)fast;
    opt->eq = &eq_live;
    /* MIDI: the chip if there is one, else the synthesizer at a rate the
       processor can carry */
    if ( set.synth >= 0 ) {
        opt->synth = (u8)set.synth;
    } else if ( card.flags & AUDF_OPL3 ) {
        opt->synth = SYN_OPL3;
    } else if ( card.flags & AUDF_OPL2 ) {
        opt->synth = SYN_OPL2;
    } else {
        opt->synth = SYN_SOFT;
    }
    if ( (opt->synth == SYN_OPL2 && !(card.flags & (AUDF_OPL2 | AUDF_OPL3)))
         || (opt->synth == SYN_OPL3 && !(card.flags & AUDF_OPL3))
         || (opt->synth == SYN_MPU && !(card.flags & AUDF_MPU)) ) {
        opt->synth = SYN_SOFT;
    }
    if ( opt->synth == SYN_SOFT ) {
        opt->rate = cpu_class >= 3 ? 22050UL : cpu_class == 2 ? 22050UL : 11025UL;
        if ( card.present && card.rate_max && opt->rate > card.rate_max ) {
            opt->rate = card.rate_max;
        }
    } else {
        opt->rate = MIDI_RATE;
    }
}

/* the card opened for what the codec gives -> DE_OK, DE_CARD or
   DE_NOMEM */
static int open_output( void )
{
    u32 answer, want_rate, max_out;

    in_rate = dinfo.rate;
    in_ch = dinfo.channels;
    events_mode = (dinfo.flags & DIF_EVENTS) != 0;
    mem_set( &ofmt, 0, sizeof( ofmt ) );
    want_rate = in_rate;
    if ( set.rate_limit >= 8000 && want_rate > (u32)set.rate_limit ) {
        want_rate = (u32)set.rate_limit;
    }
    ofmt.rate = want_rate;
    ofmt.channels = (u8)in_ch;
    ofmt.bits = (u8)((card.bits & AUDB_16) ? 16 : 8);
    if ( events_mode ) {
        ofmt.bits = 8;
        ofmt.channels = 1;
        ofmt.ring = 16384;
        ofmt.block = 64;                /* about 6 ms at 11 kHz: the events' grain */
        ofmt.events = 16384;
    } else {
        ofmt.ring = want_rate * ofmt.channels * (ofmt.bits / 8);    /* a second */
        ofmt.block = 0;
        ofmt.events = 0;
    }
    soft_volume = 1;
    if ( card.present ) {
        if ( drv_call( AUD_OPEN, &ofmt, sizeof( ofmt ), &answer ) != 0 ) {
            return DE_CARD;
        }
        card_open = 1;
        if ( ofmt.rate < 1000 || ofmt.rate > 384000UL ) {
            close_card();               /* a rate no one plays: nothing to convert to */
            return DE_CARD;
        }
        underruns = 0;
        soft_volume = !(card.flags & AUDF_MIXER);
        out_rate = ofmt.rate;
        out_bits = ofmt.bits;
        out_ch = ofmt.channels;
    } else {
        out_rate = want_rate;
        out_bits = 16;
        out_ch = in_ch;
    }
    out_frame = (out_bits / 8) * out_ch;
    /* both rates are 1000 to 384000 here, so the step is 170 or more */
    rs_step = out_rate == in_rate ? 0x10000UL : fx_umuldiv( in_rate, 0x10000UL, out_rate );
    rs_pos = 0;
    rs_prev[0] = rs_prev[1] = 0;
    /* room for a piece at the most the rate can grow it: by the step as
       it is used, rounded down, which makes a few more frames than the
       exact ratio would */
    max_out = fx_umuldiv( PIECE, 0x10000UL, rs_step ) + 4;
    out_room = max_out * out_frame;
    if ( out_room < PIECE * 4 ) {
        out_room = PIECE * 4;
    }
    outbuf = (u8 *)try_alloc( out_room );
    if ( outbuf == NULL ) {
        close_card();
        return DE_NOMEM;
    }
    apply_volume();
    return DE_OK;
}

/* what the codec said it gives is something the engine can convert:
   one or two channels, 1 kHz to 384 kHz (DEC.H's contract) */
static int info_fits( void )
{
    return dinfo.rate >= 1000 && dinfo.rate <= 384000UL && dinfo.channels >= 1 && dinfo.channels <= 2;
}

static void reset_counts( u32 at_ms )
{
    base_ms = at_ms;
    dec_frames = out_frames = 0;
    played_last = 0;
    dry_frames = 0;
    out_have = out_used = 0;
    ev_have = ev_used = 0;
    eof_seen = 0;
    card_running = 0;
    clock_running = 0;
    clock_frames = 0;
    snap_count = 0;
    rs_pos = 0;
    if ( hist != NULL ) {
        mem_set( hist, 0, HIST * sizeof( s16 ) );
    }
}

/* the file opened in the mode its codec and this machine call for, one
   step cheaper for every step the governor has taken */
static int open_at( u32 at_ms, int step )
{
    int rc, mode;
    u32 landed;

    choose_opts( &dopt, step );
    rc = dec_open( &src, path_now, &dopt, &codec, &dstate, &dinfo );
    if ( rc != DE_OK ) {
        codec = NULL;
        return rc;
    }
    if ( set.quality == PQ_AUTO ) {
        mode = auto_mode( codec, &dinfo ) + step;
        if ( mode >= MODES ) {
            mode = MODES - 1;
        }
        if ( mode != step ) {
            codec->close( dstate );
            src_close( &src );
            choose_opts( &dopt, mode );
            rc = dec_open( &src, path_now, &dopt, &codec, &dstate, &dinfo );
            if ( rc != DE_OK ) {
                codec = NULL;
                return rc;
            }
        }
    }
    rc = info_fits() ? open_output() : DE_FORMAT;
    if ( rc != DE_OK ) {
        codec->close( dstate );
        codec = NULL;
        src_close( &src );
        return rc;
    }
    eqtd_set( &eq_live, in_rate, in_ch );
    reset_counts( 0 );
    if ( at_ms && (dinfo.flags & DIF_SEEK) && codec->seek( dstate, at_ms, &landed ) == DE_OK ) {
        base_ms = landed;
    }
    load_work_ms = load_sound_ms = 0;
    load_now = 0;
    load_high_since = 0;
    return DE_OK;
}

int pl_open( const char *path, DECINFO *info )
{
    int rc;

    pl_close();
    str_cpyt( path_now, path, sizeof( path_now ) );
    auto_step = 0;
    rc = open_at( 0, 0 );
    if ( info != NULL ) {
        *info = dinfo;
    }
    if ( rc != DE_OK ) {
        state = PS_EMPTY;
        return rc;
    }
    state = PS_STOPPED;
    return DE_OK;
}

/* ------------------------------------------------------------------ */
/* playing                                                             */
/* ------------------------------------------------------------------ */
/* The card's frames played since the counts began, as frames of OURS:
   a card (or the clock) that ran dry went on counting while it played
   silence, and those frames are taken off here.  Without that, the
   count would pass the frames the card was ever given, "how far ahead"
   would come out negative - a huge number, unsigned - and the engine
   would believe itself ahead for ever and never decode again. */
static u32 frames_played( void )
{
    AUD_STATBLOCK st;
    u32 answer, now, raw, given;

    if ( card_open ) {
        if ( !card_running ) {
            return 0;
        }
        mem_set( &st, 0, sizeof( st ) );
        if ( drv_call( AUD_STATUS, &st, sizeof( st ), &answer ) == 0 ) {
            played_last = st.played;
            irq_seen = st.irqs;
            drv_state_seen = st.state;
            if ( !eof_seen ) {
                underruns = st.underruns;       /* running dry at the end is not one */
            }
        }
        raw = played_last;
    } else if ( !clock_running ) {
        raw = clock_frames;
    } else {
        now = sys_clock_ms();
        raw = clock_frames + fx_umuldiv( now - clock_start, out_rate, 1000 );
    }
    given = out_frames - (out_frame ? (out_have - out_used) / (u32)out_frame : 0);
    if ( (long)(raw - dry_frames - given) > 0 ) {
        dry_frames = raw - given;
    }
    return raw - dry_frames;
}

static void start_output( void )
{
    if ( card_open ) {
        drv_call( AUD_START, NULL, 0, NULL );
    } else {
        clock_start = sys_clock_ms();
        clock_running = 1;
    }
    card_running = 1;
}

void pl_play( void )
{
    if ( state == PS_ENDED ) {
        pl_stop();
    }
    if ( state == PS_PAUSED ) {
        if ( card_running ) {
            if ( card_open ) {
                drv_call( AUD_START, NULL, 0, NULL );
            } else {
                clock_start = sys_clock_ms();
                clock_running = 1;
            }
        }
        state = PS_PLAYING;
    } else if ( state == PS_STOPPED ) {
        state = PS_PLAYING;
    }
}

void pl_pause( void )
{
    if ( state != PS_PLAYING ) {
        return;
    }
    if ( card_running ) {
        if ( card_open ) {
            drv_call( AUD_PAUSE, NULL, 0, NULL );
            synth_quiet( 0 );          /* held notes stop; the queue waits */
        } else if ( clock_running ) {
            clock_frames = frames_played() + dry_frames;
            clock_running = 0;
        }
    }
    state = PS_PAUSED;
}

/* every voice of a chip or port let go, at once: the events still
   queued dropped (on a seek, not a pause), and then every OPL
   channel's key released, or every MIDI channel told "all notes off"
   and "all sound off" - six bytes for each of 16 channels.  Nothing
   else is written to an OPL: the MIDI codec keeps a copy of what its
   registers hold and writes only changes, so a level turned down
   behind its back would stay down.  The driver turns every operator
   down when the card is closed, which is how Stop silences a chip */
static void synth_quiet( int drop )
{
    AUDEV quiet[96];
    u32 count = 0, ch, bank, banks;

    if ( !events_mode || !card_open ) {
        return;
    }
    if ( drop ) {
        drv_call( AUD_DROPEV, NULL, 0, NULL );
    }
    mem_set( quiet, 0, sizeof( quiet ) );
    if ( dopt.synth == SYN_MPU ) {
        for ( ch = 0; ch < 16; ch++ ) {
            quiet[count].kind = EV_MPU;
            quiet[count++].val = (u8)(0xB0 | ch);
            quiet[count].kind = EV_MPU;
            quiet[count++].val = 123;
            quiet[count++].kind = EV_MPU;
            quiet[count].kind = EV_MPU;
            quiet[count++].val = (u8)(0xB0 | ch);
            quiet[count].kind = EV_MPU;
            quiet[count++].val = 120;
            quiet[count++].kind = EV_MPU;
        }
    } else {
        banks = dopt.synth == SYN_OPL3 ? 2 : 1;
        for ( bank = 0; bank < banks; bank++ ) {
            for ( ch = 0; ch < 9; ch++ ) {
                quiet[count].kind = EV_OPL;
                quiet[count].bank = (u8)bank;
                quiet[count++].reg = (u8)(0xB0 + ch);
            }
        }
    }
    drv_call( AUD_SENDNOW, quiet, count * sizeof( AUDEV ), NULL );
}

static int move_to( u32 ms )
{
    u32 landed = 0;
    int was = state;

    if ( codec == NULL || !(dinfo.flags & DIF_SEEK) ) {
        return 0;
    }
    synth_quiet( 1 );
    if ( card_open ) {
        drv_call( AUD_STOP, NULL, 0, NULL );
    }
    if ( codec->seek( dstate, ms, &landed ) != DE_OK ) {
        /* the card is stopped and where the codec is no one knows: the
           file opened again, at the place if it can be found */
        return open_again( ms );
    }
    reset_counts( landed );
    /* paused stays paused: nothing is queued, so pl_play starts the
       card again from the new place */
    state = was == PS_ENDED ? PS_STOPPED : was;
    return 1;
}

void pl_stop( void )
{
    if ( state == PS_EMPTY ) {
        return;
    }
    /* in events mode the card is closed and opened, which silences the
       chip; a file that cannot seek is opened again too */
    if ( events_mode || !move_to( 0 ) ) {
        if ( !open_again( 0 ) ) {
            return;                     /* PS_EMPTY: the file is gone */
        }
    }
    state = PS_STOPPED;
}

int pl_seek( u32 ms )
{
    int was = state;

    if ( state == PS_EMPTY ) {
        return 0;
    }
    if ( dinfo.length_ms && ms > dinfo.length_ms ) {
        ms = dinfo.length_ms;
    }
    if ( !move_to( ms ) ) {
        return 0;
    }
    if ( was == PS_PLAYING ) {
        state = PS_PLAYING;
    }
    return 1;
}

/* the file again, one step cheaper or as it was, at a position */
static int open_again( u32 at_ms )
{
    int was = state, rc;

    close_card();
    if ( codec != NULL ) {
        codec->close( dstate );
        codec = NULL;
        src_close( &src );
    }
    rc = open_at( at_ms, auto_step );
    if ( rc != DE_OK ) {
        state = PS_EMPTY;
        return 0;
    }
    state = was == PS_EMPTY ? PS_STOPPED : was;
    if ( state == PS_PAUSED || state == PS_ENDED ) {
        state = PS_STOPPED;
    }
    return 1;
}

/* the governor: more than nine tenths of the processor for two
   seconds, on Auto, and the file is opened one step cheaper where it
   had got to */
static void govern( void )
{
    u32 now, pos;

    if ( set.quality != PQ_AUTO || auto_step >= MODES - 1 || load_sound_ms < 500 ) {
        return;
    }
    now = sys_clock_ms();
    if ( load_now < 90 ) {
        load_high_since = 0;
        return;
    }
    if ( load_high_since == 0 ) {
        load_high_since = now ? now : 1;
        return;
    }
    if ( now - load_high_since < 2000 ) {
        return;
    }
    pos = base_ms + fx_umuldiv( frames_played(), 1000, out_rate );
    auto_step++;
    if ( open_again( pos ) && state == PS_STOPPED ) {
        state = PS_PLAYING;
    }
}

/* the converted bytes and the events not yet taken, offered again ->
   1 when everything went */
static int flush_pending( void )
{
    u32 taken;

    if ( ev_used < ev_have ) {
        if ( card_open ) {
            if ( drv_call( AUD_EVENTS, aev + ev_used, (ev_have - ev_used) * sizeof( AUDEV ), &taken ) != 0 ) {
                taken = ev_have - ev_used;      /* refused: dropped rather than stuck */
            }
            ev_used += taken;
        } else {
            ev_used = ev_have;
        }
        if ( ev_used < ev_have ) {
            return 0;
        }
    }
    if ( out_used < out_have ) {
        if ( card_open ) {
            if ( drv_call( AUD_WRITE, outbuf + out_used, out_have - out_used, &taken ) != 0 ) {
                return 0;
            }
            out_used += taken;
        } else {
            out_used = out_have;
        }
        if ( out_used < out_have ) {
            return 0;
        }
    }
    return 1;
}

/* one piece decoded, converted and set waiting -> 0 at the end */
static int decode_piece( void )
{
    DECBUF buf;
    u32 made, i, start_out, t0, t1, n;
    int rc;

    mem_set( &buf, 0, sizeof( buf ) );
    buf.pcm = pcm;
    buf.max = PIECE;
    buf.ev = dev;
    buf.ev_max = events_mode ? EV_PIECE : 0;
    t0 = sys_clock_ms();
    rc = codec->decode( dstate, &buf );
    if ( rc != DE_OK || buf.frames == 0 || buf.frames > PIECE ) {
        return 0;
    }
    if ( !(dinfo.flags & DIF_EQ) && set.eq_pcm && eq_live.on && !events_mode ) {
        eqtd_apply( pcm, buf.frames );
    }
    t1 = sys_clock_ms();
    load_work_ms += t1 - t0;
    load_sound_ms += fx_umuldiv( buf.frames, 1000, in_rate );
    if ( load_sound_ms >= 2000 ) {
        load_now = (int)fx_umuldiv( load_work_ms, 100, load_sound_ms );
        load_work_ms /= 2;
        load_sound_ms /= 2;
    }
    remember( pcm, buf.frames );
    if ( (dinfo.flags & DIF_LEVELS) && snap_count < 0xFFFFFFF0UL ) {
        n = snap_count % LEVEL_SNAPS;
        mem_cpy( snap_level[n], buf.levels, LEVEL_BANDS );
        snap_frame[n] = dec_frames + buf.frames;
        snap_count++;
    }
    start_out = out_frames;
    made = convert( pcm, buf.frames );
    out_have = made * out_frame;
    out_used = 0;
    out_frames += made;
    dec_frames += buf.frames;
    /* the events, onto the card's frame count */
    ev_have = ev_used = 0;
    if ( events_mode ) {
        for ( i = 0; i < buf.ev_count && i < EV_PIECE; i++ ) {
            aev[i].frame = start_out + dry_frames + (rs_step == 0x10000UL ? dev[i].frame
                                        : fx_umuldiv( dev[i].frame, out_rate, in_rate ));
            aev[i].kind = dev[i].kind;
            aev[i].reg = dev[i].reg;
            aev[i].val = dev[i].val;
            aev[i].bank = dev[i].bank;
        }
        ev_have = i;
    }
    return 1;
}

int pl_pump( u32 budget_ms )
{
    u32 start, played, ahead, want, start_at;

    if ( state != PS_PLAYING || codec == NULL ) {
        return state;
    }
    if ( budget_ms == 0 ) {
        budget_ms = 40;
    }
    start = sys_clock_ms();
    want = fx_umuldiv( AHEAD_MS, out_rate, 1000 );
    start_at = fx_umuldiv( START_MS, out_rate, 1000 );
    for ( ;; ) {
        if ( !flush_pending() ) {
            break;                      /* the ring is full */
        }
        played = frames_played();
        ahead = out_frames - played;
        if ( !card_running && (ahead >= start_at || eof_seen) && ahead > 0 ) {
            start_output();
        }
        if ( eof_seen || ahead >= want ) {
            break;
        }
        if ( sys_clock_ms() - start >= budget_ms ) {
            break;
        }
        if ( !decode_piece() ) {
            eof_seen = 1;
        }
    }
    if ( eof_seen && out_used >= out_have && ev_used >= ev_have ) {
        if ( !card_running && out_frames > 0 ) {
            start_output();
        }
        played = frames_played();
        if ( played >= out_frames || !card_running ) {
            synth_quiet( 1 );
            if ( card_open ) {
                drv_call( AUD_STOP, NULL, 0, NULL );
            }
            card_running = 0;
            clock_running = 0;
            state = PS_ENDED;
            return state;
        }
    }
    govern();
    return state;
}

u32 pl_ahead_ms( void )
{
    u32 played;

    if ( state != PS_PLAYING || out_rate == 0 ) {
        return 0;
    }
    played = frames_played();
    return out_frames > played ? fx_umuldiv( out_frames - played, 1000, out_rate ) : 0;
}

void pl_status( PLSTAT *st )
{
    u32 played = state == PS_PLAYING || state == PS_PAUSED ? frames_played() : 0;

    mem_set( st, 0, sizeof( PLSTAT ) );
    st->state = state;
    st->len_ms = codec != NULL ? dinfo.length_ms : 0;
    st->out_rate = out_rate;
    st->out_bits = (u8)out_bits;
    st->out_ch = (u8)out_ch;
    st->load = load_now;
    st->down = dopt.down;
    st->mono = dinfo.channels == 1 && dinfo.file_channels > 1;
    st->synth = dopt.synth;
    st->underruns = underruns;
    st->irqs = irq_seen;
    st->drv_state = drv_state_seen;
    if ( state == PS_ENDED ) {
        st->pos_ms = dinfo.length_ms ? dinfo.length_ms : base_ms + fx_umuldiv( out_frames, 1000, out_rate );
    } else if ( out_rate ) {
        st->pos_ms = base_ms + fx_umuldiv( played, 1000, out_rate );
    }
    if ( codec == NULL ) {
        str_cpy( st->quality, "" );
    } else if ( events_mode ) {
        str_cpy( st->quality, dopt.synth == SYN_MPU ? "MIDI port" : dopt.synth == SYN_OPL3 ? "OPL3 FM" : "OPL2 FM" );
    } else {
        sfmt( st->quality, sizeof( st->quality ), "%u Hz, %u-bit, %s", out_rate, out_bits,
              out_ch == 2 ? "stereo" : "mono" );
    }
}

/* ------------------------------------------------------------------ */
/* the analyzer                                                        */
/* ------------------------------------------------------------------ */
/* the codec's frame that is being heard */
static u32 heard_frame( void )
{
    u32 played;

    if ( state != PS_PLAYING && state != PS_PAUSED ) {
        return dec_frames;
    }
    played = frames_played();
    if ( rs_step != 0x10000UL ) {
        played = fx_umuldiv( played, in_rate, out_rate );
    }
    return played > dec_frames ? dec_frames : played;
}

void pl_levels( u8 *bars, int count )
{
    s16 win[FFT_SIZE];
    u32 at, i, best, k, band;

    mem_set( bars, 0, (u32)count );
    if ( codec == NULL || hist == NULL || (state != PS_PLAYING && state != PS_PAUSED) ) {
        return;
    }
    at = heard_frame();
    if ( dinfo.flags & DIF_LEVELS ) {
        /* the latest snapshot at or before the frame heard */
        if ( snap_count == 0 ) {
            return;
        }
        best = (snap_count - 1) % LEVEL_SNAPS;
        for ( k = 0; k < LEVEL_SNAPS && k < snap_count; k++ ) {
            i = (snap_count - 1 - k) % LEVEL_SNAPS;
            best = i;
            if ( (long)(at - snap_frame[i]) >= 0 ) {
                break;
            }
        }
        for ( i = 0; i < (u32)count; i++ ) {
            band = i * LEVEL_BANDS / (u32)count;
            bars[i] = snap_level[best][band];
        }
        return;
    }
    for ( i = 0; i < FFT_SIZE; i++ ) {
        win[i] = at >= FFT_SIZE - i ? hist[(at - (FFT_SIZE - i)) & (HIST - 1)] : 0;
    }
    fft_bars( win, in_rate, bars, count );
}

void pl_wave( s16 *out, int count )
{
    u32 at, i;

    if ( codec == NULL || hist == NULL || (state != PS_PLAYING && state != PS_PAUSED) ) {
        mem_set( out, 0, (u32)count * sizeof( s16 ) );
        return;
    }
    at = heard_frame();
    for ( i = 0; i < (u32)count; i++ ) {
        out[i] = at >= (u32)count - i ? hist[(at - ((u32)count - i)) & (HIST - 1)] : 0;
    }
}
