/*
 * X87EMU.H - the coprocessor, for a native program on a machine that
 *            has none
 *
 * A 386 without a 387 and a 486SX execute no floating-point instruction
 * at all.  With EM set in CR0 every one of them is exception 7 instead,
 * and for a native program that exception comes here (IO\FPU.INC,
 * fpu_nm): the instruction is decoded, carried out on a state kept in
 * memory, and stepped over.  A DOS box and a DPMI client are not
 * emulated - they were never given a coprocessor on such a machine and
 * bring their own emulators, as they always have.
 *
 * ONE NUMBER FORMAT DOES ALL THE ARITHMETIC: a 128-bit significand with
 * a 32-bit exponent (wide).  Every operand the 387 can be handed fits in
 * it exactly, the product of two of them fits exactly, and a sum,
 * quotient or root is carried far enough past 64 bits that rounding it
 * once gives the answer IEEE arithmetic asks for.  The transcendental
 * instructions use the same format, so their results are good to the
 * last of the 64 bits.
 *
 * Built with wcc386 into X87EMU.SYS, a 32-bit driver, behind X87DRV.ASM
 * (which hands the kernel x87_trap and x87_init, and gives this code
 * fpu_uok).  It was in PMIO's image until the driver was made.  The code
 * runs at ring 0 with interrupts off, in the flat segment, and has no
 * data of its own: the state is PMIO's (fpu_emst), and every table is
 * constant and lives in the code segment (-zc).  No 64-bit integer type
 * is used, because wcc386 would call run-time helpers for it.
 */
#ifndef X87EMU_H
#define X87EMU_H

typedef unsigned int    u32;
typedef int             s32;
typedef unsigned short  u16;
typedef unsigned char   u8;

/*---------------------------------------------------------------------
 * The working number
 *-------------------------------------------------------------------*/
typedef struct {
    u32 m[4];           /* the significand, m[3] the most significant */
    s32 e;              /* value = m * 2^(e - 127); normalized, bit 127
                           is set and 2^e <= |value| < 2^(e + 1) */
    u8  sign;           /* 1: negative */
    u8  kind;           /* K_* */
    u8  denorm;         /* it was a denormal when it was read */
    u8  pad;
} wide;

#define K_ZERO          0
#define K_FIN           1
#define K_INF           2
#define K_QNAN          3
#define K_SNAN          4
#define K_BAD           5       /* a format the 387 refuses: an unnormal,
                                   a pseudo-NaN, a pseudo-infinity */

/* the three formats a result can be fitted to */
#define EXT_BIAS        16383
#define EXT_EMIN        (-16382)
#define EXT_EMAX        16383
#define DBL_BIAS        1023
#define DBL_EMIN        (-1022)
#define DBL_EMAX        1023
#define FLT_BIAS        127
#define FLT_EMIN        (-126)
#define FLT_EMAX        127

/*---------------------------------------------------------------------
 * The coprocessor's own state, kept by PMIO (FPU.INC, fpu_emst)
 *-------------------------------------------------------------------*/
typedef struct {
    u32 lo, hi;         /* the significand; bit 31 of hi is the units bit */
    u16 se;             /* the sign and the biased exponent */
    u16 pad;
} xreg;

typedef struct {
    xreg st[8];         /* by physical number, not by stack position */
    u16  cw, sw;        /* the top of the stack is in sw, bits 11-13 */
    u16  fop, fcs;      /* the last instruction that was not a control */
    u16  fds;           /* instruction, and its memory operand */
    u8   empty;         /* bit n set: physical register n is empty */
    u8   pad;
    u32  fip, fdp;
} x87state;

#define X87_STATE       128     /* what FPU.INC keeps for it (X87_STATE) */

#define CW_IM           0x0001
#define CW_DM           0x0002
#define CW_ZM           0x0004
#define CW_OM           0x0008
#define CW_UM           0x0010
#define CW_PM           0x0020
#define CW_PC           0x0300
#define CW_RC           0x0C00
#define CW_INIT         0x037F

#define SW_IE           0x0001
#define SW_DE           0x0002
#define SW_ZE           0x0004
#define SW_OE           0x0008
#define SW_UE           0x0010
#define SW_PE           0x0020
#define SW_SF           0x0040
#define SW_ES           0x0080
#define SW_C0           0x0100
#define SW_C1           0x0200
#define SW_C2           0x0400
#define SW_TOP          0x3800
#define SW_C3           0x4000
#define SW_B            0x8000
#define SW_EXC          0x003F
#define SW_CC           (SW_C0 | SW_C1 | SW_C2 | SW_C3)

#define RC_NEAR         0
#define RC_DOWN         1
#define RC_UP           2
#define RC_CHOP         3

/* What fitting a result says, beyond the status bits: the result was
   tiny before the denormal step, which is underflow when UE is unmasked
   whether or not anything was lost (x87_fit's caller decides). */
#define F_TINY          0x10000

/*---------------------------------------------------------------------
 * What fpu_nm hands over: the fault frame and the six segments
 *-------------------------------------------------------------------*/
typedef struct {
    u32 edi, esi, ebp, esp0, ebx, edx, ecx, eax;        /* PUSHAD */
    u32 vec, err, eip, cs, eflags, esp, ss;             /* the fault */
} x87frame;

typedef struct {
    u32 base, limit;
    u16 sel;
    u8  ok;             /* the selector describes a segment */
    u8  pad;
} x87seg;

/* in the order a segment register is numbered: ES CS SS DS FS GS */
#define SEG_ES          0
#define SEG_CS          1
#define SEG_SS          2
#define SEG_DS          3
#define SEG_FS          4
#define SEG_GS          5

/* x87_trap's answer: resumed, or the exception the program takes */
#define TRAP_DONE       0
#define TRAP_UD         6
#define TRAP_GP         13
#define TRAP_MF         16

#define EFL_CF          0x0001
#define EFL_PF          0x0004
#define EFL_AF          0x0010
#define EFL_ZF          0x0040
#define EFL_SF          0x0080
#define EFL_OF          0x0800

/*---------------------------------------------------------------------
 * One instruction, as X87EMU.C has decoded it
 *-------------------------------------------------------------------*/
typedef struct {
    x87frame *frame;
    x87seg   *segs;
    x87state *st;
    u32       start;    /* the instruction's first byte, prefixes and all */
    u32       next;     /* the byte after the last one read */
    u32       ea;       /* the memory operand's offset... */
    s32       easeg;    /* ...and its segment */
    s32       sreg;     /* a segment override, or -1 */
    u32       exc;      /* the status bits this instruction raised */
    u8        o16;      /* a 66h prefix: 16-bit environment layouts */
    u8        a16;      /* a 67h prefix: 16-bit addressing */
    u8        esc;      /* the escape byte, D8h to DFh, less D8h */
    u8        modrm;
    u8        fault;    /* the TRAP_* the instruction ends with, or 0 */
    u8        pad[3];
} x87ctx;

/* the formats a memory operand can have */
#define FMT_F32         0
#define FMT_F64         1
#define FMT_F80         2
#define FMT_I16         3
#define FMT_I32         4
#define FMT_I64         5
#define FMT_BCD         6

/* the arithmetic, numbered as the ModRM reg field numbers it after D8h */
#define OP_ADD          0
#define OP_MUL          1
#define OP_COM          2
#define OP_COMP         3
#define OP_SUB          4
#define OP_SUBR         5
#define OP_DIV          6
#define OP_DIVR         7

/* how op_compare is to compare */
#define CMP_QUIET       1       /* FUCOM: only a signaling NaN is invalid */
#define CMP_EFLAGS      2       /* FCOMI: the answer goes to EFLAGS */

/* X87EMU.C, for X87OPS.C */
int  xs_stop( x87ctx *c, u32 bits );
int  xs_rc( x87ctx *c );
int  xs_pc( x87ctx *c );
int  xs_empty( x87ctx *c, int i );
void xs_get( x87ctx *c, int i, wide *w );
void xs_getraw( x87ctx *c, int i, xreg *x );
void xs_put( x87ctx *c, int i, const wide *w );
void xs_putraw( x87ctx *c, int i, const xreg *x );
void xs_pop( x87ctx *c );
int  xs_push( x87ctx *c );
int  xs_under( x87ctx *c );
void xs_cc( x87ctx *c, u32 cc );
void xs_result( x87ctx *c, int i, wide *r, int prec );
void xs_indef( wide *w );
u8  *xs_mem( x87ctx *c, u32 len );

/* X87OPS.C, for X87EMU.C */
void op_math( x87ctx *c, int op, int dst, int src, int pops );
void op_mathmem( x87ctx *c, int op, int fmt );
void op_compare( x87ctx *c, int src, const wide *b, int mode, int pops );
void op_load( x87ctx *c, int fmt );
void op_store( x87ctx *c, int fmt, int pops );
void op_fn( x87ctx *c );

/*---------------------------------------------------------------------
 * PMIO's side (FPU.INC)
 *-------------------------------------------------------------------*/
u32 x87_trap( x87frame *frame, x87seg *segs, x87state *st );
#pragma aux x87_trap "*" parm [eax] [edx] [ebx] value [eax];
void x87_init( x87state *st );
#pragma aux x87_init "*" parm [eax];

/* usr_wok, as a C function can call it: 1 if ring 3 could write every
   byte of the range itself, 0 if not */
u32 fpu_uok( u32 lin, u32 len );
#pragma aux fpu_uok "*" parm [eax] [ecx] value [eax] modify exact [eax];

/*---------------------------------------------------------------------
 * 32-by-32 multiply and 64-by-32 divide, as the instructions do them
 *-------------------------------------------------------------------*/
u32 mul32( u32 a, u32 b, u32 *hi );
#pragma aux mul32 = \
        "mul edx" \
        "mov [ebx],edx" \
        parm [eax] [edx] [ebx] value [eax] modify exact [eax edx];

/* (hi:lo) / d with hi < d, so that the quotient fits */
u32 div32( u32 lo, u32 hi, u32 d, u32 *rem );
#pragma aux div32 = \
        "div ecx" \
        "mov [ebx],edx" \
        parm [eax] [edx] [ecx] [ebx] value [eax] modify exact [eax edx];

/*---------------------------------------------------------------------
 * X87NUM.C
 *-------------------------------------------------------------------*/
void w_zero( wide *w );
void w_one( wide *w );
void w_set( wide *w, const u32 *k );            /* a table constant */
int  w_mzero( const wide *w );
void w_norm( wide *w );
void w_shr( wide *w, s32 count );
int  w_cmpmag( const wide *a, const wide *b );
void w_add( wide *res, const wide *a, const wide *b );
void w_sub( wide *res, const wide *a, const wide *b );
void w_mul( wide *res, const wide *a, const wide *b );
void w_div( wide *res, const wide *a, const wide *b, int bits );
void w_sqrt( wide *res, const wide *a, int bits );
u32  w_prem( wide *r, const wide *x, const wide *y, int n );
u32  w_bit( const wide *w, int n );
void w_divs( wide *w, u32 divisor );
void w_muls( wide *w, u32 factor );
void w_int( wide *w, u32 value );
u32  w_rint( wide *w, int rc );
u32  w_round( wide *w, int keep, int rc );
u32  w_fit( wide *w, int prec, s32 emin, s32 emax, int rc );

void x_unpack( wide *w, const xreg *x );
void x_pack( xreg *x, const wide *w );
void d_unpack( wide *w, u32 lo, u32 hi );
void d_pack( const wide *w, u32 *lo, u32 *hi );
void f_unpack( wide *w, u32 bits );
u32  f_pack( const wide *w );
void i_unpack( wide *w, u32 lo, u32 hi );
int  i_pack( const wide *w, u32 *lo, u32 *hi );

/* the results of w_round */
#define R_INEXACT       1
#define R_UP            2

/*---------------------------------------------------------------------
 * X87FN.C - each takes finite operands, gives the result unrounded
 *-------------------------------------------------------------------*/
void fn_log2( wide *res, const wide *x );
void fn_log2p1( wide *res, const wide *x );
void fn_exp2m1( wide *res, const wide *x );
void fn_atan2( wide *res, const wide *y, const wide *x );
int  fn_sincos( wide *sinv, wide *cosv, const wide *x );
void fn_const( wide *w, int which );

#define KC_LOG2_10      0       /* FLDL2T */
#define KC_LOG2_E       1       /* FLDL2E */
#define KC_PI           2       /* FLDPI */
#define KC_LOG10_2      3       /* FLDLG2 */
#define KC_LN_2         4       /* FLDLN2 */

#endif
