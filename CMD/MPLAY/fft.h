/*
 * FFT.H - the spectrum analyzer's arithmetic (FFT.C).
 */
#ifndef FFT_H
#define FFT_H

#include "types.h"

#define FFT_SIZE        512             /* samples looked at for one picture */

/* FFT_SIZE samples (mono, oldest first) at "rate" -> "count" bars, low
   to high, 0..255 on a scale of 60 decibels, spaced evenly in log
   frequency from 40 Hz to 16 kHz (or half the rate, if that is less).
   The first call makes the tables; it returns 0 only if there was no
   memory for them, and the bars are then all 0. */
int  fft_bars( const s16 *samples, u32 rate, u8 *bars, int count );
void fft_done( void );                  /* the tables freed (made again when needed) */

#endif
