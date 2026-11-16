/*
 * TRIG.C - sine and cosine with no floating point, for the tables the
 * analyzer and the equalizer make once (TRIG.H).
 *
 * CORDIC TURNS A VECTOR BY SHIFTS AND ADDS.  Starting from (K, 0),
 * thirty turns by the angles whose tangents are 1, 1/2, 1/4 ... -
 * each one way or the other, whichever brings the angle left over
 * nearer nothing - end on (cos a, sin a).  K is the product of the
 * lengthening every turn brings, taken out in advance.  Thirty steps
 * give about 28 good bits, which is more than any table here keeps.
 *
 * THE ANGLE IS A FRACTION OF A WHOLE TURN, as an unsigned 32-bit
 * number: 2^32 is 360 degrees.  Its top two bits are the quadrant, so
 * only the first quadrant is ever worked out, and a frequency over a
 * sample rate - which is what every caller has - is that fraction
 * already: fx_umuldiv( hz, 1UL << 31, rate ) * 2.
 *
 * The arctangent table and K were computed on the host with [Math] in
 * PowerShell: round( atan( 2^-i ) * 2^30 ) and round( prod( 1 /
 * sqrt( 1 + 2^-2i ) ) * 2^30 ).
 */
#include "rt.h"
#include "fix.h"
#include "trig.h"

static const s32 atan_tab[30] = {
    843314857L, 497837829L, 263043837L, 133525159L, 67021687L, 33543516L,
    16775851L, 8388437L, 4194283L, 2097149L, 1048576L, 524288L, 262144L,
    131072L, 65536L, 32768L, 16384L, 8192L, 4096L, 2048L, 1024L, 512L,
    256L, 128L, 64L, 32L, 16L, 8L, 4L, 2L
};

#define CORDIC_K        652032874L      /* the gain thirty steps bring, in Q30 */
#define HALF_PI_Q30     1686629713L

void fx_sincos( u32 turn, s32 *sin_out, s32 *cos_out )
{
    s32 x = CORDIC_K, y = 0, z, nx;
    u32 quadrant = turn >> 30;
    int i;

    /* the angle into its quadrant, as radians in Q30 */
    z = fx_mulsh( (s32)(turn & 0x3FFFFFFFUL), HALF_PI_Q30, 30 );
    for ( i = 0; i < 30; i++ ) {
        if ( z >= 0 ) {
            nx = x - (y >> i);
            y += x >> i;
            z -= atan_tab[i];
        } else {
            nx = x + (y >> i);
            y -= x >> i;
            z += atan_tab[i];
        }
        x = nx;
    }
    switch ( quadrant ) {
    case 0:
        *cos_out = x;
        *sin_out = y;
        break;
    case 1:
        *cos_out = -y;
        *sin_out = x;
        break;
    case 2:
        *cos_out = -x;
        *sin_out = -y;
        break;
    default:
        *cos_out = y;
        *sin_out = -x;
        break;
    }
}
