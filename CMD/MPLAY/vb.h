/*
 * VB.H - the Vorbis I decoder inside MPLAY's Ogg codec: what ogg.c,
 * vbdec.c, vbmdct.c and vbtab.c share.
 *
 * THE OGG LAYER HANDS OVER WHOLE PACKETS.  ogg.c finds the pages,
 * checks them and joins their segments into packets; the three header
 * packets go to vb_ident, vb_comment and vb_open, and every packet
 * after them to vb_decode, which gives back the finished frames.  What
 * the granule positions say - where the stream starts and ends, where
 * a seek lands - is the Ogg layer's business, and vb_block tells it
 * how long a packet is without decoding it.
 *
 * THE DECODER DOES THE PLAYER'S MIXING ITSELF.  More than two channels
 * become two, two become one when the player asks for one, and the
 * rate is halved or quartered by a smaller inverse transform - all on
 * the spectrum, before the transform, so that only one or two
 * transforms run whatever the file holds.
 *
 * NUMBERS ARE FIXED POINT.  The spectrum is Q24 (1.0 is 2^24), the
 * residue a binary point chosen for each stream from its codebooks
 * (vbdec.c), sines and cosines Q30, the window Q31.
 */
#ifndef VB_H
#define VB_H

#include "types.h"
#include "dec.h"

#define VB_MAX_CH       8       /* channels in a stream: the spec's layouts end at 7.1 */
#define VB_MIN_LOG      6       /* the smallest block, 64 */
#define VB_MAX_LOG      13      /* the largest, 8192 */
#define VB_MAX_FRAMES   4096    /* frames one packet can finish: 8192/4 + 8192/4 */

/* the identification header */
typedef struct {
    u32  rate;
    u32  channels;
    int  log0, log1;            /* the two block sizes as powers of two */
    s32  br_max, br_nom, br_min;
} VBID;

/* what the player asked for, as the decoder takes it */
typedef struct {
    int  down;                  /* 0..2, already limited to keep 8000 Hz */
    int  out_ch;                /* 1 or 2 */
    int  fast;                  /* 16-bit twiddles and window */
    const EQSET *eq;            /* NULL: never */
} VBOPT;

typedef struct VB VB;

/* vbdec.c */
int  vb_ident( VBID *id, const u8 *pkt, u32 len );         /* DE_OK, DE_FORMAT, DE_UNSUPPORTED */
void vb_comment( const u8 *pkt, u32 len, DECINFO *info );
int  vb_open( VB **vbp, const VBID *id, const u8 *pkt, u32 len, const VBOPT *opt );
void vb_close( VB *vb );
u32  vb_block( const VB *vb, const u8 *pkt, u32 len );    /* its block size, 0: not audio */
u32  vb_decode( VB *vb, const u8 *pkt, u32 len, const s16 **pcm );    /* frames finished */
void vb_restart( VB *vb );                                /* the next packet starts afresh */

/* ------------------------------------------------------------------ */
/* the inverse transform (vbmdct.c)                                    */
/* ------------------------------------------------------------------ */
/* one transform size: n samples out of n/2 lines */
typedef struct {
    int  log2n;
    u32  n;
    s32 *tw;                    /* n/4 pairs: cos and sin of 2 pi (m + 1/8) / n, Q30 */
    s16 *tw16;                  /* the same, Q14 */
    u16 *rev;                   /* n/4: where the FFT leaves output m */
    s32 *win;                   /* n/2: the rising half of the window, Q31 */
    s16 *win16;                 /* the same, Q15 */
    s32 *eqtw;                  /* n/4 fours: the twiddles with the equalizer in them, Q28 */
} VBMDCT;

/* the FFT's twiddles, for the largest size: e^(j 2 pi t / K) for t < 3K/4 */
typedef struct {
    int  log2k;
    s32 *tw;                    /* pairs, Q30 */
    s16 *tw16;                  /* the same, Q14 */
} VBFFT;

void vb_sincos( u32 phase, s32 *cosv, s32 *sinv );        /* phase: 2^32 to the turn; Q30 */
int  vb_mdct_init( VBMDCT *md, int log2n );               /* 0, or -1 out of memory */
void vb_mdct_free( VBMDCT *md );
int  vb_fft_init( VBFFT *fft, int log2k );
void vb_fft_free( VBFFT *fft );
int  vb_mdct_eq( VBMDCT *md, const u32 *gain );          /* gain: n/2 lines, Q16; -1 no memory */

/* u (n/2 values) from the spectrum spec (n/2 lines); work holds n/2 */
void vb_imdct( const VBMDCT *md, const VBFFT *fft, const s32 *spec, s32 *work, s32 *u,
               int fast, int use_eq );

/* The frames from the middle of the previous block to the middle of
   this one, into pcm with "stride" s16s between frames.  prev and cur
   are two blocks' u; win is the window of the smaller block.
   -> pn/4 + n/4 */
u32  vb_lap( s16 *pcm, int stride, const s32 *prev, u32 pn, const s32 *cur, u32 n,
             const VBMDCT *small, int fast );

/* ------------------------------------------------------------------ */
/* the tables (vbtab.c)                                                */
/* ------------------------------------------------------------------ */
extern const u32 vb_floor1_mant[256];
extern const u8  vb_floor1_exp[256];
extern const s32 vb_cos_coarse[256];
extern const s32 vb_cos_fine[256];
extern const s32 vb_sin_fine[256];
extern const u32 vb_atan_turn[31];
extern const u32 vb_exp2_tab[257];
extern const u32 vb_log2_tab[257];
extern const u8  vb_rev8[256];

/* ------------------------------------------------------------------ */
/* products the decoder needs besides FIX.H's                          */
/* ------------------------------------------------------------------ */
/* (a * b) >> 30, rounded */
s32 vb_mul30r( s32 a, s32 b );
#pragma aux vb_mul30r = \
    "imul edx" \
    "add eax,20000000h" \
    "adc edx,0" \
    "shrd eax,edx,30" \
    parm [eax] [edx] value [eax] modify exact [eax edx] nomemory;

/* (a * b) >> 14, rounded: the fast path's twiddle (b) times a sample */
s32 vb_mul14r( s32 a, s32 b );
#pragma aux vb_mul14r = \
    "imul edx" \
    "add eax,2000h" \
    "adc edx,0" \
    "shrd eax,edx,14" \
    parm [eax] [edx] value [eax] modify exact [eax edx] nomemory;

/* (a * b) >> 28, rounded */
s32 vb_mul28r( s32 a, s32 b );
#pragma aux vb_mul28r = \
    "imul edx" \
    "add eax,8000000h" \
    "adc edx,0" \
    "shrd eax,edx,28" \
    parm [eax] [edx] value [eax] modify exact [eax edx] nomemory;

/* (a * b) >> 32, unsigned */
u32 vb_umulhi( u32 a, u32 b );
#pragma aux vb_umulhi = \
    "mul edx" \
    parm [eax] [edx] value [edx] modify exact [eax edx] nomemory;

/* a * b as 64 bits: the low half returned, the high half in *hi */
u32 vb_mul64( s32 a, s32 b, s32 *hi );
#pragma aux vb_mul64 = \
    "imul edx" \
    "mov [ebx],edx" \
    parm [eax] [edx] [ebx] value [eax] modify exact [eax edx];

#endif
