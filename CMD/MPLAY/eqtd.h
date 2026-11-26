/*
 * EQTD.H - the equalizer on samples, for the codecs that cannot apply
 * it in the frequency domain (EQTD.C).
 */
#ifndef EQTD_H
#define EQTD_H

#include "types.h"
#include "dec.h"

void eqtd_init( void );
void eqtd_done( void );
void eqtd_set( const EQSET *eq, uint32_t rate, int channels );  /* the filters made again */
void eqtd_apply( int16_t *pcm, unsigned int frames );           /* in place, interleaved */
int  eqtd_cost( void );         /* multiplies a sample per channel, 0 when nothing is done */

#endif
