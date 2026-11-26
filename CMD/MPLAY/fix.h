/*
 * FIX.H - fixed-point arithmetic for MPLAY's decoders and filters.
 *
 * THERE IS NO FLOATING POINT.  MPLAY has to play on a 386 with no
 * coprocessor, and even on a 486DX the integer unit is the faster one
 * for this work.  Every decoder keeps its samples and coefficients as
 * 32-bit integers with an agreed number of fraction bits, and these are
 * the operations that cost one instruction each.
 *
 * THE ONE MULTIPLY THAT MATTERS is fx_mulhi: a full 32 by 32 multiply
 * whose top half is the answer.  That is one IMUL on every processor
 * from the 386 on, and it keeps 32 bits of precision where a plain C
 * multiply would have to throw bits away first to stay inside 32 bits.
 * With a Q31 coefficient (1.0 is 2^31) the result is the sample times
 * the coefficient, halved; with Q30 it is exact.  Pick the formats so
 * that the halving comes out in the wash.
 *
 * ON A 386 AND A 486, IMUL TAKES LONGER FOR A BIGGER MULTIPLIER.  The
 * second operand (EDX here) is the one the processor walks through, so
 * a loop that has a choice should pass the value with fewer significant
 * bits as "b".  On a Pentium it makes no difference.
 *
 * No 64-bit C arithmetic either: Open Watcom calls its C library for a
 * 64-bit multiply, and there is no C library.  The pragmas below are
 * the way to a 64-bit product.  A module that wants a 64-bit
 * accumulator writes its own pragma in the same style.
 *
 * The pragmas are Open Watcom's and work the same in the PM-DOS build
 * and the Windows test build (MKHOST.CMD).
 */
#ifndef FIX_H
#define FIX_H

#include "types.h"

/* (a * b) >> 32 */
int32_t fx_mulhi( int32_t a, int32_t b );
#pragma aux fx_mulhi = \
    "imul edx" \
    parm [eax] [edx] value [edx] modify exact [eax edx] nomemory;

/* (a * b) >> 31: a Q31 times a Q31 is a Q31 */
int32_t fx_mul31( int32_t a, int32_t b );
#pragma aux fx_mul31 = \
    "imul edx" \
    "shld edx,eax,1" \
    parm [eax] [edx] value [edx] modify exact [eax edx] nomemory;

/* (a * b) >> 30 */
int32_t fx_mul30( int32_t a, int32_t b );
#pragma aux fx_mul30 = \
    "imul edx" \
    "shrd eax,edx,30" \
    parm [eax] [edx] value [eax] modify exact [eax edx] nomemory;

/* (a * b) >> 28 */
int32_t fx_mul28( int32_t a, int32_t b );
#pragma aux fx_mul28 = \
    "imul edx" \
    "shrd eax,edx,28" \
    parm [eax] [edx] value [eax] modify exact [eax edx] nomemory;

/* (a * b) >> 24 */
int32_t fx_mul24( int32_t a, int32_t b );
#pragma aux fx_mul24 = \
    "imul edx" \
    "shrd eax,edx,24" \
    parm [eax] [edx] value [eax] modify exact [eax edx] nomemory;

/* (a * b) >> 16 */
int32_t fx_mul16( int32_t a, int32_t b );
#pragma aux fx_mul16 = \
    "imul edx" \
    "shrd eax,edx,16" \
    parm [eax] [edx] value [eax] modify exact [eax edx] nomemory;

/* (a * b) >> shift, shift 1 to 31 */
int32_t fx_mulsh( int32_t a, int32_t b, int shift );
#pragma aux fx_mulsh = \
    "imul edx" \
    "shrd eax,edx,cl" \
    parm [eax] [edx] [ecx] value [eax] modify exact [eax edx] nomemory;

/* (a * b) / c with the product kept in 64 bits.  c must not be 0, and
   the quotient must fit in 32 bits: a * b / c with a, b < 2^31 and
   c > |a| or c > |b| always does.  For scaling a position by a ratio:
   fx_muldiv( frames, 1000, rate ) is milliseconds. */
int32_t fx_muldiv( int32_t a, int32_t b, int32_t c );
#pragma aux fx_muldiv = \
    "imul edx" \
    "idiv ecx" \
    parm [eax] [edx] [ecx] value [eax] modify exact [eax edx] nomemory;

/* the same, unsigned */
uint32_t fx_umuldiv( uint32_t a, uint32_t b, uint32_t c );
#pragma aux fx_umuldiv = \
    "mul edx" \
    "div ecx" \
    parm [eax] [edx] [ecx] value [eax] modify exact [eax edx] nomemory;

/* the number of the highest bit set (0 to 31); x must not be 0 */
int fx_topbit( uint32_t x );
#pragma aux fx_topbit = \
    "bsr eax,eax" \
    parm [eax] value [eax] modify exact [eax] nomemory;

/* a 32-bit value cut to what a 16-bit sample can hold */
#define FX_SAT16( x )   ((x) > 32767 ? 32767 : ((x) < -32768 ? -32768 : (x)))

/* 1.0 in the common formats */
#define FX_ONE31        0x7FFFFFFFL     /* as near as Q31 gets */
#define FX_ONE30        0x40000000L
#define FX_ONE28        0x10000000L
#define FX_ONE16        0x00010000L

#endif
