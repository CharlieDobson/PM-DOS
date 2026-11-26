/*
 * MP3.H - what the parts of the MPEG audio decoder share: MP3.C (the
 * frames and the codec), MP3L12.C (layers 1 and 2), MP3L3.C (layer 3),
 * MP3SYN.C (the synthesis filter bank) and MP3TAB.C (the tables).
 *
 * EVERY LAYER ENDS IN THE SAME PLACE: 32 subband samples for each slot
 * of time, which the polyphase synthesis turns into 32 output samples.
 * Layer 1 gives 12 slots a frame, layer 2 gives 36 and layer 3 gives
 * 18 a granule.  The subband samples are Q24 integers - 1.0 is 2^24 -
 * which leaves seven bits of room above full scale and nine bits of
 * fraction below the last bit of a 16-bit sample.
 *
 * LESS WORK MEANS FEWER SUBBANDS.  At half rate only the lower 16
 * subbands are made and a 16-band synthesis runs with every other tap
 * of the window; at quarter rate 8.  The output is then exactly every
 * second (or fourth) sample of the full synthesis of the lower bands.
 */
#ifndef MP3_H
#define MP3_H

#include "types.h"
#include "fix.h"
#include "dec.h"

/*
 * ROUNDED MULTIPLIES.  A shift alone rounds every product down, and a
 * decoder makes dozens of products for each sample: the half a bit
 * each one loses adds up to a bias that shows as output a bit low.
 * These add half of the last bit kept before the shift - two more
 * instructions that pair with nothing slow.  b is the operand the
 * multiplier walks (fix.h): the data, not the coefficient.
 */
/* (a * b + 2^30) >> 31 */
int32_t mp3_mul31( int32_t a, int32_t b );
#pragma aux mp3_mul31 = \
    "imul edx" \
    "add eax,40000000h" \
    "adc edx,0" \
    "shld edx,eax,1" \
    parm [eax] [edx] value [edx] modify exact [eax edx] nomemory;

/* (a * b + 2^29) >> 30 */
int32_t mp3_mul30( int32_t a, int32_t b );
#pragma aux mp3_mul30 = \
    "imul edx" \
    "add eax,20000000h" \
    "adc edx,0" \
    "shrd eax,edx,30" \
    parm [eax] [edx] value [eax] modify exact [eax edx] nomemory;

/* (a * b + 2^27) >> 28 */
int32_t mp3_mul28( int32_t a, int32_t b );
#pragma aux mp3_mul28 = \
    "imul edx" \
    "add eax,8000000h" \
    "adc edx,0" \
    "shrd eax,edx,28" \
    parm [eax] [edx] value [eax] modify exact [eax edx] nomemory;

/* (a * b + 2^26) >> 27 */
int32_t mp3_mul27( int32_t a, int32_t b );
#pragma aux mp3_mul27 = \
    "imul edx" \
    "add eax,4000000h" \
    "adc edx,0" \
    "shrd eax,edx,27" \
    parm [eax] [edx] value [eax] modify exact [eax edx] nomemory;

/* (a * b + 2^15) >> 16 */
int32_t mp3_mul16( int32_t a, int32_t b );
#pragma aux mp3_mul16 = \
    "imul edx" \
    "add eax,8000h" \
    "adc edx,0" \
    "shrd eax,edx,16" \
    parm [eax] [edx] value [eax] modify exact [eax edx] nomemory;

/*
 * THE SYNTHESIS WINDOW SUM: 16 (or 14) history values hv[k] times taps
 * wr[k], added in 64 bits so that nothing is lost until the end, then
 * rounded to the scale of a 16-bit sample (a Q24 value times a tap in
 * 2^-16 is Q40; a sample is Q15).  The tap is the IMUL's memory
 * operand, the one the multiplier walks: 17 bits at most.  The result
 * is not limited to 16 bits yet.
 */
#define MP3_TAP0  "mov eax,[esi]" "imul dword ptr [edi]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP1  "mov eax,[esi+4]" "imul dword ptr [edi+4]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP2  "mov eax,[esi+8]" "imul dword ptr [edi+8]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP3  "mov eax,[esi+12]" "imul dword ptr [edi+12]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP4  "mov eax,[esi+16]" "imul dword ptr [edi+16]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP5  "mov eax,[esi+20]" "imul dword ptr [edi+20]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP6  "mov eax,[esi+24]" "imul dword ptr [edi+24]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP7  "mov eax,[esi+28]" "imul dword ptr [edi+28]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP8  "mov eax,[esi+32]" "imul dword ptr [edi+32]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP9  "mov eax,[esi+36]" "imul dword ptr [edi+36]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP10 "mov eax,[esi+40]" "imul dword ptr [edi+40]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP11 "mov eax,[esi+44]" "imul dword ptr [edi+44]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP12 "mov eax,[esi+48]" "imul dword ptr [edi+48]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP13 "mov eax,[esi+52]" "imul dword ptr [edi+52]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP14 "mov eax,[esi+56]" "imul dword ptr [edi+56]" "add ebx,eax" "adc ecx,edx"
#define MP3_TAP15 "mov eax,[esi+60]" "imul dword ptr [edi+60]" "add ebx,eax" "adc ecx,edx"

int32_t mp3_dot16( const int32_t *hv, const int32_t *wr );
#pragma aux mp3_dot16 = \
    "xor ebx,ebx" \
    "xor ecx,ecx" \
    MP3_TAP0 MP3_TAP1 MP3_TAP2 MP3_TAP3 \
    MP3_TAP4 MP3_TAP5 MP3_TAP6 MP3_TAP7 \
    MP3_TAP8 MP3_TAP9 MP3_TAP10 MP3_TAP11 \
    MP3_TAP12 MP3_TAP13 MP3_TAP14 MP3_TAP15 \
    "add ebx,1000000h" \
    "adc ecx,0" \
    "shrd ebx,ecx,25" \
    parm [esi] [edi] value [ebx] modify exact [eax ebx ecx edx];

/* the same without the outermost tap at each end */
int32_t mp3_dot14( const int32_t *hv, const int32_t *wr );
#pragma aux mp3_dot14 = \
    "xor ebx,ebx" \
    "xor ecx,ecx" \
    MP3_TAP1 MP3_TAP2 MP3_TAP3 \
    MP3_TAP4 MP3_TAP5 MP3_TAP6 MP3_TAP7 \
    MP3_TAP8 MP3_TAP9 MP3_TAP10 MP3_TAP11 \
    MP3_TAP12 MP3_TAP13 MP3_TAP14 \
    "add ebx,1000000h" \
    "adc ecx,0" \
    "shrd ebx,ecx,25" \
    parm [esi] [edi] value [ebx] modify exact [eax ebx ecx edx];

/* the taps at odd ages only (output N/2, whose even-age values are 0):
   8 of them, or 7 without age 15 */
int32_t mp3_odd8( const int32_t *hv, const int32_t *wr );
#pragma aux mp3_odd8 = \
    "xor ebx,ebx" \
    "xor ecx,ecx" \
    MP3_TAP1 MP3_TAP3 MP3_TAP5 MP3_TAP7 \
    MP3_TAP9 MP3_TAP11 MP3_TAP13 MP3_TAP15 \
    "add ebx,1000000h" \
    "adc ecx,0" \
    "shrd ebx,ecx,25" \
    parm [esi] [edi] value [ebx] modify exact [eax ebx ecx edx];

int32_t mp3_odd7( const int32_t *hv, const int32_t *wr );
#pragma aux mp3_odd7 = \
    "xor ebx,ebx" \
    "xor ecx,ecx" \
    MP3_TAP1 MP3_TAP3 MP3_TAP5 MP3_TAP7 \
    MP3_TAP9 MP3_TAP11 MP3_TAP13 \
    "add ebx,1000000h" \
    "adc ecx,0" \
    "shrd ebx,ecx,25" \
    parm [esi] [edi] value [ebx] modify exact [eax ebx ecx edx];

/*
 * TWO OUTPUTS FROM ONE PASS.  Outputs j and N - j use the same 16
 * history values, and the window's symmetry makes each pair of ages - an
 * even one e and the odd one 15 - e - a complex multiply: with u, v the
 * two values and p, q their taps, out[j] gets p u + q v and out[N - j]
 * gets q u - p v, which three products make: u (p + q), (u + v) p and
 * (u - v) q.  So 12 multiplies a sample instead of 16.  Each product is
 * kept to Q27 (a Q24 value times a tap in 2^-16, less 13 bits) and the
 * sums are 32 bits, which may pass through a wrap on the way: only the
 * final sums have to fit.  They go to res[0] (out[j]) and res[1].  The
 * table holds p + q, p, q for each pair, the pair of ages 0 and 15
 * first; the fast path leaves that pair out.  ebp is saved on the stack.
 */
#define MP3_PAIR0 \
    "mov eax,[esi]" "mov ebp,eax" "imul dword ptr [edi]" "shrd eax,edx,13" \
    "add ebx,eax" "add ecx,eax" \
    "mov eax,ebp" "add eax,[esi+60]" "imul dword ptr [edi+4]" "shrd eax,edx,13" "sub ecx,eax" \
    "mov eax,ebp" "sub eax,[esi+60]" "imul dword ptr [edi+8]" "shrd eax,edx,13" "sub ebx,eax"
#define MP3_PAIR1 \
    "mov eax,[esi+8]" "mov ebp,eax" "imul dword ptr [edi+12]" "shrd eax,edx,13" \
    "add ebx,eax" "add ecx,eax" \
    "mov eax,ebp" "add eax,[esi+52]" "imul dword ptr [edi+16]" "shrd eax,edx,13" "sub ecx,eax" \
    "mov eax,ebp" "sub eax,[esi+52]" "imul dword ptr [edi+20]" "shrd eax,edx,13" "sub ebx,eax"
#define MP3_PAIR2 \
    "mov eax,[esi+16]" "mov ebp,eax" "imul dword ptr [edi+24]" "shrd eax,edx,13" \
    "add ebx,eax" "add ecx,eax" \
    "mov eax,ebp" "add eax,[esi+44]" "imul dword ptr [edi+28]" "shrd eax,edx,13" "sub ecx,eax" \
    "mov eax,ebp" "sub eax,[esi+44]" "imul dword ptr [edi+32]" "shrd eax,edx,13" "sub ebx,eax"
#define MP3_PAIR3 \
    "mov eax,[esi+24]" "mov ebp,eax" "imul dword ptr [edi+36]" "shrd eax,edx,13" \
    "add ebx,eax" "add ecx,eax" \
    "mov eax,ebp" "add eax,[esi+36]" "imul dword ptr [edi+40]" "shrd eax,edx,13" "sub ecx,eax" \
    "mov eax,ebp" "sub eax,[esi+36]" "imul dword ptr [edi+44]" "shrd eax,edx,13" "sub ebx,eax"
#define MP3_PAIR4 \
    "mov eax,[esi+32]" "mov ebp,eax" "imul dword ptr [edi+48]" "shrd eax,edx,13" \
    "add ebx,eax" "add ecx,eax" \
    "mov eax,ebp" "add eax,[esi+28]" "imul dword ptr [edi+52]" "shrd eax,edx,13" "sub ecx,eax" \
    "mov eax,ebp" "sub eax,[esi+28]" "imul dword ptr [edi+56]" "shrd eax,edx,13" "sub ebx,eax"
#define MP3_PAIR5 \
    "mov eax,[esi+40]" "mov ebp,eax" "imul dword ptr [edi+60]" "shrd eax,edx,13" \
    "add ebx,eax" "add ecx,eax" \
    "mov eax,ebp" "add eax,[esi+20]" "imul dword ptr [edi+64]" "shrd eax,edx,13" "sub ecx,eax" \
    "mov eax,ebp" "sub eax,[esi+20]" "imul dword ptr [edi+68]" "shrd eax,edx,13" "sub ebx,eax"
#define MP3_PAIR6 \
    "mov eax,[esi+48]" "mov ebp,eax" "imul dword ptr [edi+72]" "shrd eax,edx,13" \
    "add ebx,eax" "add ecx,eax" \
    "mov eax,ebp" "add eax,[esi+12]" "imul dword ptr [edi+76]" "shrd eax,edx,13" "sub ecx,eax" \
    "mov eax,ebp" "sub eax,[esi+12]" "imul dword ptr [edi+80]" "shrd eax,edx,13" "sub ebx,eax"
#define MP3_PAIR7 \
    "mov eax,[esi+56]" "mov ebp,eax" "imul dword ptr [edi+84]" "shrd eax,edx,13" \
    "add ebx,eax" "add ecx,eax" \
    "mov eax,ebp" "add eax,[esi+4]" "imul dword ptr [edi+88]" "shrd eax,edx,13" "sub ecx,eax" \
    "mov eax,ebp" "sub eax,[esi+4]" "imul dword ptr [edi+92]" "shrd eax,edx,13" "sub ebx,eax"

void mp3_pair8( const int32_t *hv, const int32_t *cf, int32_t *res );
#pragma aux mp3_pair8 = \
    "push ebp" \
    "push eax" \
    "xor ebx,ebx" \
    "xor ecx,ecx" \
    MP3_PAIR0 \
    MP3_PAIR1 \
    MP3_PAIR2 \
    MP3_PAIR3 \
    MP3_PAIR4 \
    MP3_PAIR5 \
    MP3_PAIR6 \
    MP3_PAIR7 \
    "pop ebp" \
    "mov [ebp],ebx" \
    "mov [ebp+4],ecx" \
    "pop ebp" \
    parm [esi] [edi] [eax] modify exact [eax ebx ecx edx];

/* 12 taps: without ages 0, 1, 14 and 15 (the pairs 0-15 and 14-1) */
int32_t mp3_dot12( const int32_t *hv, const int32_t *wr );
#pragma aux mp3_dot12 = \
    "xor ebx,ebx" \
    "xor ecx,ecx" \
    MP3_TAP2 MP3_TAP3 MP3_TAP4 MP3_TAP5 MP3_TAP6 MP3_TAP7 \
    MP3_TAP8 MP3_TAP9 MP3_TAP10 MP3_TAP11 MP3_TAP12 MP3_TAP13 \
    "add ebx,1000000h" \
    "adc ecx,0" \
    "shrd ebx,ecx,25" \
    parm [esi] [edi] value [ebx] modify exact [eax ebx ecx edx];

int32_t mp3_odd6( const int32_t *hv, const int32_t *wr );
#pragma aux mp3_odd6 = \
    "xor ebx,ebx" \
    "xor ecx,ecx" \
    MP3_TAP3 MP3_TAP5 MP3_TAP7 MP3_TAP9 MP3_TAP11 MP3_TAP13 \
    "add ebx,1000000h" \
    "adc ecx,0" \
    "shrd ebx,ecx,25" \
    parm [esi] [edi] value [ebx] modify exact [eax ebx ecx edx];

void mp3_pair6( const int32_t *hv, const int32_t *cf, int32_t *res );
#pragma aux mp3_pair6 = \
    "push ebp" \
    "push eax" \
    "xor ebx,ebx" \
    "xor ecx,ecx" \
    MP3_PAIR1 MP3_PAIR2 MP3_PAIR3 MP3_PAIR4 MP3_PAIR5 MP3_PAIR6 \
    "pop ebp" \
    "mov [ebp],ebx" \
    "mov [ebp+4],ecx" \
    "pop ebp" \
    parm [esi] [edi] [eax] modify exact [eax ebx ecx edx];

void mp3_pair7( const int32_t *hv, const int32_t *cf, int32_t *res );
#pragma aux mp3_pair7 = \
    "push ebp" \
    "push eax" \
    "xor ebx,ebx" \
    "xor ecx,ecx" \
    MP3_PAIR1 \
    MP3_PAIR2 \
    MP3_PAIR3 \
    MP3_PAIR4 \
    MP3_PAIR5 \
    MP3_PAIR6 \
    MP3_PAIR7 \
    "pop ebp" \
    "mov [ebp],ebx" \
    "mov [ebp+4],ecx" \
    "pop ebp" \
    parm [esi] [edi] [eax] modify exact [eax ebx ecx edx];

#define MP3_MAXFRAME    4096        /* the longest frame taken, header included */
#define MP3_SLOTS       36          /* most slots of time a frame makes (layer 2) */
#define MP3_SB          32          /* subbands */

/* versions */
#define MV_1            0           /* MPEG-1 */
#define MV_2            1           /* MPEG-2, the lower rates */
#define MV_25           2           /* MPEG-2.5, the lowest */

/* channel modes */
#define MODE_STEREO     0
#define MODE_JOINT      1
#define MODE_DUAL       2
#define MODE_MONO       3

/* one frame's header, taken apart */
typedef struct {
    uint8_t  version;               /* MV_* */
    uint8_t  layer;                 /* 1, 2, 3 */
    uint8_t  crc;                   /* 1: two bytes of CRC follow the header */
    uint8_t  br_index;              /* 0: free format */
    uint8_t  sr_index;              /* 0..2 within the version */
    uint8_t  padding;
    uint8_t  mode;                  /* MODE_* */
    uint8_t  mode_ext;
    uint8_t  channels;              /* 1 or 2 */
    uint8_t  sfreq;                 /* 0..8: the rate's row in the band tables */
    uint16_t samples;               /* a channel's samples in the frame */
    uint32_t rate;                  /* Hz */
    uint32_t kbps;                  /* 0: free format */
    uint32_t length;                /* the frame's bytes with the header; 0 for free format
                                       until the stream has shown it */
} MP3HDR;

/* the synthesis of one channel */
typedef struct {
    int32_t *hist;                  /* [2][17][32]: two histories of the matrixed vectors,
                                       each kept twice over so that 16 in a row are always
                                       next to each other */
    int      pos;                   /* where the newest vector is, 0..15 */
    int      phase;                 /* which history is read this time */
} MP3SYN;

/* the bits of a buffer, read from the top down */
typedef struct {
    const uint8_t *data;
    unsigned int   len;             /* bytes in data: past them come zeros */
    unsigned int   pos;             /* the next byte to load */
    uint32_t       cache;           /* bits waiting, the next one at bit 31 */
    int            count;           /* how many */
} MP3BITS;

#define MP3_BITPOS( bits )  ((bits)->pos * 8 - (unsigned int)(bits)->count)

void     mp3_bits_init( MP3BITS *bits, const uint8_t *data, unsigned int len, unsigned int bitpos );
uint32_t mp3_bits_get( MP3BITS *bits, int count );  /* count 0..24 */

/* a Huffman table by table_select */
typedef struct {
    uint16_t first;                 /* its first entry in mp3_huff */
    uint8_t  bits;                  /* bits the first lookup takes; 0: every value is 0 */
    uint8_t  linbits;
} MP3HTAB;

/* layer 3: everything a channel keeps from granule to granule */
typedef struct MP3L3 MP3L3;

/* the decoder's state as the layers see it */
typedef struct {
    MP3HDR   hdr;                   /* the frame being decoded */
    uint8_t  out_ch;                /* channels made: 1 or 2 */
    uint8_t  nsb;                   /* subbands made: 32, 16 or 8 */
    uint8_t  fast;
    uint8_t  eq_on;                 /* the equalizer is not flat */
    int32_t *sb[2];                 /* [MP3_SLOTS][32] subband samples for each channel made */
    uint32_t slots;                 /* slots the frame filled */
    uint32_t eq32[32];              /* layers 1 and 2: a Q16 gain for each subband */
    MP3L3   *l3;
} MP3DEC;

/* MP3L12.C: one frame of layer 1 or 2 into dec->sb.  0, or -1 when the
   frame is too short for what its header says */
int  mp3_layer1( MP3DEC *dec, const uint8_t *frame, unsigned int len );
int  mp3_layer2( MP3DEC *dec, const uint8_t *frame, unsigned int len );

/* MP3L3.C */
int  mp3_l3_open( MP3DEC *dec, unsigned int sfreq ); /* DE_OK or DE_NOMEM */
void mp3_l3_close( MP3DEC *dec );
void mp3_l3_reset( MP3DEC *dec );                    /* after a seek: no reservoir, no overlap */
void mp3_l3_eq( MP3DEC *dec, const EQSET *eq, uint32_t rate );
/* One frame into dec->sb (two granules for MPEG-1, one for the lower
   rates).  A frame whose bit reservoir is not there plays as silence. */
void mp3_layer3( MP3DEC *dec, const uint8_t *frame, unsigned int len );

/* MP3SYN.C */
int  mp3_syn_open( MP3SYN *syn );                    /* DE_OK or DE_NOMEM */
void mp3_syn_close( MP3SYN *syn );
void mp3_syn_reset( MP3SYN *syn );
/* One slot: nsb subband samples (Q24) into nsb output samples, every
   "stride"th int16_t of out.  taps is 16, or 12 for the fast path. */
void mp3_synth( MP3SYN *syn, const int32_t *sb, int nsb, int16_t *out, int stride, int taps );

/* MP3TAB.C */
extern const uint16_t mp3_huff[];
extern const MP3HTAB  mp3_htab[32];
extern const uint8_t  mp3_quad_a[64];
extern const int32_t  mp3_win32[16 + 15 * 24 + 16];
extern const int32_t  mp3_win16[16 + 7 * 24 + 16];
extern const int32_t  mp3_win8[16 + 3 * 24 + 16];
extern const int32_t  mp3_pow_small[4 * 16];
extern const uint32_t mp3_pow43[8207];
extern const int32_t  mp3_frac4[4];
extern const uint16_t mp3_sfb_long[9 * 23];
extern const uint16_t mp3_sfb_short[9 * 14];
extern const int32_t  mp3_lee32[16];
extern const int32_t  mp3_lee16[8];
extern const int32_t  mp3_lee8[4];
extern const int32_t  mp3_lee4[2];
extern const int32_t  mp3_lee2[1];
extern const int32_t  mp3_c9[7];
extern const int32_t  mp3_q9[9];
extern const int32_t  mp3_cwl[4 * 2 * 36];
extern const int32_t  mp3_c6[4];
extern const int32_t  mp3_cws[2 * 12];
extern const int32_t  mp3_alias[8 * 3];
extern const int32_t  mp3_is_ratio[7];
extern const int32_t  mp3_l12_sf[64];
extern const int32_t  mp3_l12_inv[17];
extern const uint8_t  mp3_l12_shift[17];
extern const int8_t   mp3_group3[32 * 3];
extern const int8_t   mp3_group5[128 * 3];
extern const int8_t   mp3_group9[1024 * 3];

#endif
