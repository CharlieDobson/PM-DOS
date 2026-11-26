/*
 * EQ.C - the equalizer's curve: ten sliders turned into a gain for any
 * frequency (DEC.H).
 *
 * TEN BANDS AN OCTAVE APART, from 31 Hz to 16 kHz - the bands of every
 * graphic equalizer since the hi-fi rack.  31.25 Hz times a power of
 * two gives every center, so the position of a frequency among them is
 * its base-two logarithm, and that is the axis the curve is smooth on.
 *
 * BETWEEN TWO CENTERS THE CURVE EASES FROM ONE SLIDER TO THE NEXT
 * (t * t * (3 - 2t), the curve that is flat at both ends), so a single
 * slider raised makes a hill and not a tent.  Outside the first and
 * last centers it stays at their level.
 *
 * DECIBELS BECOME A GAIN THROUGH A TABLE of whole decibels from -12 to
 * +12, with a straight line between neighbors - less than half a per
 * cent from the true curve, and no logarithms at run time.  The curve
 * is made again only when a slider moves, so none of this is in the
 * path of the samples.
 */
#include "rt.h"
#include "fix.h"
#include "dec.h"

const uint16_t eq_band_hz[EQ_BANDS] = {
    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
};

/* 10^(dB / 20) in Q16, dB from -12 to +12 */
static const uint32_t db_gain[25] = {
     16462,  18471,  20724,  23253,  26090,  29274,  32846,  36854,
     41350,  46396,  52057,  58409,  65536,  73533,  82505,  92572,
    103868, 116541, 130762, 146717, 164619, 184706, 207243, 232530,
    260904
};

/* log2( 1 + i / 32 ) in Q16 */
static const uint32_t log2_frac[33] = {
         0,  2909,  5732,  8473, 11136, 13727, 16248, 18704,
     21098, 23433, 25711, 27936, 30109, 32234, 34312, 36346,
     38336, 40286, 42196, 44068, 45904, 47705, 49472, 51207,
     52911, 54584, 56229, 57845, 59434, 60997, 62534, 64047,
     65536
};

/* log2( x ) in Q16, x > 0 */
static uint32_t log2_q16( uint32_t x )
{
    int top = fx_topbit( x );
    uint32_t mant, index, part;

    /* the bits under the top one, as a fraction of 2^32 */
    mant = top < 31 ? x << (31 - top) : x;
    mant <<= 1;
    index = mant >> 27;                 /* five bits: the table's step */
    part = (mant >> 11) & 0xFFFF;       /* sixteen more: how far into it */
    return ((uint32_t)top << 16) + log2_frac[index]
           + (((log2_frac[index + 1] - log2_frac[index]) * part) >> 16);
}

/* decibels times 256 -> Q16 gain, limited to +-12 dB */
static uint32_t gain_of( int32_t db256 )
{
    int32_t off;
    uint32_t step, frac;

    if ( db256 < -12 * 256 ) {
        db256 = -12 * 256;
    } else if ( db256 > 12 * 256 ) {
        db256 = 12 * 256;
    }
    off = db256 + 12 * 256;
    step = (uint32_t)off >> 8;
    frac = (uint32_t)off & 255;
    if ( step >= 24 ) {
        return db_gain[24];
    }
    return db_gain[step] + (((db_gain[step + 1] - db_gain[step]) * frac) >> 8);
}

/* The curve in decibels times 256 at a frequency given in sixteenths
   of a hertz */
static int32_t db_at( const EQSET *eq, uint32_t hz16 )
{
    static uint32_t base;
    uint32_t oct, t, smooth;
    int band;
    int32_t lo, hi;

    /* 31.25 Hz is 500 sixteenths: the octaves are counted from there,
       with the same table everything else goes through */
    if ( hz16 <= 500 ) {
        return (int32_t)eq->band[0] * 128 + (int32_t)eq->preamp * 128;
    }
    if ( base == 0 ) {
        base = log2_q16( 500 );
    }
    oct = log2_q16( hz16 );
    oct = oct > base ? oct - base : 0;
    band = (int)(oct >> 16);
    if ( band >= EQ_BANDS - 1 ) {
        return (int32_t)eq->band[EQ_BANDS - 1] * 128 + (int32_t)eq->preamp * 128;
    }
    t = oct & 0xFFFF;
    smooth = (uint32_t)fx_mul16( (int32_t)((t * t) >> 16), (int32_t)(3 * 65536UL - 2 * t) );
    lo = (int32_t)eq->band[band] * 128;
    hi = (int32_t)eq->band[band + 1] * 128;
    return lo + fx_mul16( hi - lo, (int32_t)smooth ) + (int32_t)eq->preamp * 128;
}

uint32_t eq_db_gain( int32_t db256 )
{
    return gain_of( db256 );
}

uint32_t eq_gain_at( const EQSET *eq, uint32_t hz )
{
    if ( eq == NULL || !eq->on ) {
        return 65536UL;
    }
    if ( hz > 0x0FFFFFFFUL ) {
        hz = 0x0FFFFFFFUL;
    }
    return gain_of( db_at( eq, hz << 4 ) );
}

void eq_curve( const EQSET *eq, uint32_t rate, unsigned int count, uint32_t *gain )
{
    unsigned int i;
    uint32_t hz16;

    for ( i = 0; i < count; i++ ) {
        if ( eq == NULL || !eq->on ) {
            gain[i] = 65536UL;
            continue;
        }
        /* (i + 0.5) * (rate / 2) / count, in sixteenths: (2i + 1) * rate * 4 / count */
        hz16 = fx_umuldiv( 2 * i + 1, rate * 4, count );
        gain[i] = gain_of( db_at( eq, hz16 ) );
    }
}
