/*
 * TRIG.H - sine and cosine with no floating point (TRIG.C).
 */
#ifndef TRIG_H
#define TRIG_H

#include "types.h"

/* sin and cos of "turn" (2^32 is a whole turn) in Q30: 1.0 is 2^30.
   About 28 bits right.  For making tables, not for loops over samples. */
void fx_sincos( uint32_t turn, int32_t *sin_out, int32_t *cos_out );

#endif
