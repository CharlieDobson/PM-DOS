/*
 * MATHFN.C - sqrt, the trigonometric, exponential, logarithmic and
 * hyperbolic functions, pow, and rounding
 *
 * The x87 does the hard part (X87.ASM/X87.S): FSQRT, FSIN, FCOS, FPTAN,
 * FPATAN, FYL2X, FYL2XP1, F2XM1, FSCALE, FRNDINT, FPREM and FPREM1, all
 * in 64-bit precision.
 *
 * exp() reduces its argument the Cody and Waite way - x = n ln 2 + r,
 * with ln 2 in two parts so that n times the first is exact - because
 * x * log2(e) alone loses the last bits of a large x.  pow() takes
 * small integer powers by repeated squaring, which is exact far more
 * often than exp(y log x), and the rest through log2 and exp2.
 */
#include "pmlib.h"
#include "mathx.h"
#include <limits.h>

/* ln 2 as a short high part, exact when multiplied by any n below
   2^20, and the rest */
#define LN2_HI      0.693145751953125L
#define LN2_LO      1.42860682030941723212e-6L

static int is_nan( long double x )  { return __pm_fpclassifyl( x ) == FP_NAN; }
static int is_inf( long double x )  { return __pm_fpclassifyl( x ) == FP_INFINITE; }

/*--- roots and trigonometry ------------------------------------------*/

long double sqrtl( long double x )
{
    if ( x < 0.0L ) {
        return __pm_domain();
    }
    return __pm_xsqrt( x );
}

/* x = n pi/2 + r, |r| <= pi/4.  Up to 1.6e6 this is Cody and Waite's
   way, with pi/2 in three 33-bit parts so that n times each is exact
   while n < 2^20.  The parts are double constants, no L: as doubles
   they are the exact 33-bit values, and so as long doubles.  Past that
   it is Payne and Hanek's, below.  The x87's own FSIN reduces with a
   66-bit pi, which is short by sin(1e5).  trig() below does both. */
#define PIO2_1      1.57079632673412561417e+00
#define PIO2_2      6.07710050630396597660e-11
#define PIO2_3      2.02226624871116645580e-21
#define PIO2_3T     8.47842766036889956997e-32
#define TWO_OVER_PI 0.636619772367581343075535053490057448L

/* the bits of 2/pi after the point, 32 to a word.  An x uses the first
   e + 190 of them, e its exponent by frexp: 1214 at most for a double,
   16574 for the x87's long double */
static const unsigned long two_over_pi[] = {
    0xA2F9836EUL, 0x4E441529UL, 0xFC2757D1UL, 0xF534DDC0UL, 0xDB629599UL, 0x3C439041UL,
    0xFE5163ABUL, 0xDEBBC561UL, 0xB7246E3AUL, 0x424DD2E0UL, 0x06492EEAUL, 0x09D1921CUL,
    0xFE1DEB1CUL, 0xB129A73EUL, 0xE88235F5UL, 0x2EBB4484UL, 0xE99C7026UL, 0xB45F7E41UL,
    0x3991D639UL, 0x835339F4UL, 0x9C845F8BUL, 0xBDF9283BUL, 0x1FF897FFUL, 0xDE05980FUL,
    0xEF2F118BUL, 0x5A0A6D1FUL, 0x6D367ECFUL, 0x27CB09B7UL, 0x4F463F66UL, 0x9E5FEA2DUL,
    0x7527BAC7UL, 0xEBE5F17BUL, 0x3D0739F7UL, 0x8A5292EAUL, 0x6BFB5FB1UL, 0x1F8D5D08UL,
    0x56033046UL, 0xFC7B6BABUL, 0xF0CFBC20UL, 0x9AF4361DUL, 0xA9E39161UL, 0x5EE61B08UL,
#if X87_LONG_DOUBLE
    0x6599855FUL, 0x14A06840UL, 0x8DFFD880UL, 0x4D732731UL, 0x06061556UL, 0xCA73A8C9UL,
    0x60E27BC0UL, 0x8C6B47C4UL, 0x19C367CDUL, 0xDCE8092AUL, 0x8359C476UL, 0x8B961CA6UL,
    0xDDAF44D1UL, 0x5719053EUL, 0xA5FF0705UL, 0x3F7E33E8UL, 0x32C2DE4FUL, 0x98327DBBUL,
    0xC33D26EFUL, 0x6B1E5EF8UL, 0x9F3A1F35UL, 0xCAF27F1DUL, 0x87F12190UL, 0x7C7C246AUL,
    0xFA6ED577UL, 0x2D30433BUL, 0x15C614B5UL, 0x9D19C3C2UL, 0xC4AD414DUL, 0x2C5D000CUL,
    0x467D862DUL, 0x71E39AC6UL, 0x9B006233UL, 0x7CD2B497UL, 0xA7B4D555UL, 0x37F63ED7UL,
    0x1810A3FCUL, 0x764D2A9DUL, 0x64ABD770UL, 0xF87C6357UL, 0xB07AE715UL, 0x175649C0UL,
    0xD9D63B38UL, 0x84A7CB23UL, 0x24778AD6UL, 0x23545AB9UL, 0x1F001B0AUL, 0xF1DFCE19UL,
    0xFF319F6AUL, 0x1E666157UL, 0x9947FBACUL, 0xD87F7EB7UL, 0x652289E8UL, 0x3260BFE6UL,
    0xCDC4EF09UL, 0x366CD43FUL, 0x5DD7DE16UL, 0xDE3B5892UL, 0x9BDE2822UL, 0xD2E88628UL,
    0x4D58E232UL, 0xCAC616E3UL, 0x08CB7DE0UL, 0x50C017A7UL, 0x1DF35BE0UL, 0x1834132EUL,
    0x62128301UL, 0x48835B8EUL, 0xF57FB0ADUL, 0xF2E91E43UL, 0x4A48D367UL, 0x10D8DDAAUL,
    0x425FAECEUL, 0x616AA428UL, 0x0AB499D3UL, 0xF2A6067FUL, 0x775C83C2UL, 0xA3883C61UL,
    0x78738A5AUL, 0x8CAFBDD7UL, 0x6F63A62DUL, 0xCBBFF4EFUL, 0x818D67C1UL, 0x2645CA55UL,
    0x36D9CAD2UL, 0xA8288D61UL, 0xC277C912UL, 0x1426049BUL, 0x4612C459UL, 0xC444C5C8UL,
    0x91B24DF3UL, 0x1700AD43UL, 0xD4E54929UL, 0x10D5FDFCUL, 0xBE00CC94UL, 0x1EEECE70UL,
    0xF53E1380UL, 0xF1ECC3E7UL, 0xB328F8C7UL, 0x9405933EUL, 0x71C1B309UL, 0x2EF3450BUL,
    0x9C12887BUL, 0x20AB9FB5UL, 0x2EC29247UL, 0x2F327B6DUL, 0x550C90A7UL, 0x721FE76BUL,
    0x96CB314AUL, 0x1679E279UL, 0x4189DFF4UL, 0x9794E884UL, 0xE6E29731UL, 0x996BED88UL,
    0x365F5F0EUL, 0xFDBBB49AUL, 0x486CA467UL, 0x42727132UL, 0x5D8DB815UL, 0x9F09E5BCUL,
    0x25318D39UL, 0x74F71C05UL, 0x30010C0DUL, 0x68084B58UL, 0xEE2C90AAUL, 0x4702E774UL,
    0x24D6BDA6UL, 0x7DF77248UL, 0x6EEF169FUL, 0xA6948EF6UL, 0x91B45153UL, 0xD1F20ACFUL,
    0x3398207EUL, 0x4BF56863UL, 0xB25F3EDDUL, 0x035D407FUL, 0x89852952UL, 0x55C06437UL,
    0x10D86D32UL, 0x4832754CUL, 0x5BD4714EUL, 0x6E5445C1UL, 0x090B69F5UL, 0x2AD56614UL,
    0x9D072750UL, 0x045DDB3BUL, 0xB4C576EAUL, 0x17F9877DUL, 0x6B49BA27UL, 0x1D296996UL,
    0xACCCC654UL, 0x14AD6AE2UL, 0x9089D988UL, 0x50722CBEUL, 0xA4049407UL, 0x777030F3UL,
    0x27FC00A8UL, 0x71EA49C2UL, 0x663DE064UL, 0x83DD9797UL, 0x3FA3FD94UL, 0x438C860DUL,
    0xDE41319DUL, 0x39928C70UL, 0xDDE7B717UL, 0x3BDF082BUL, 0x3715A080UL, 0x5C93805AUL,
    0x921110D8UL, 0xE80FAF80UL, 0x6C4BFFDBUL, 0x0F903876UL, 0x185915A5UL, 0x62BBCB61UL,
    0xB989C7BDUL, 0x401004F2UL, 0xD2277549UL, 0xF6B6EBBBUL, 0x22DBAA14UL, 0x0A2F2689UL,
    0x76836433UL, 0x3B091A94UL, 0x0EAA3A51UL, 0xC2A31DAEUL, 0xEDAF1226UL, 0x5C4DC26DUL,
    0x9C7A2D97UL, 0x56C0833FUL, 0x03F6F009UL, 0x8C402B99UL, 0x316D07B4UL, 0x3915200CUL,
    0x5BC3D8C4UL, 0x92F54BADUL, 0xC6A5CA4EUL, 0xCD37A736UL, 0xA9E69492UL, 0xAB6842DDUL,
    0xDE6319EFUL, 0x8C76528BUL, 0x6837DBFCUL, 0xABA1AE31UL, 0x15DFA1AEUL, 0x00DAFB0CUL,
    0x664D64B7UL, 0x05ED3065UL, 0x29BF5657UL, 0x3AFF47B9UL, 0xF96AF3BEUL, 0x75DF9328UL,
    0x3080ABF6UL, 0x8C6615CBUL, 0x040622FAUL, 0x1DE4D9A4UL, 0xB33D8F1BUL, 0x5709CD36UL,
    0xE9424EA4UL, 0xBE13B523UL, 0x331AAAF0UL, 0xA8654FA5UL, 0xC1D20F3FUL, 0x0BCD785BUL,
    0x76F92304UL, 0x8B7B7217UL, 0x8953A6C6UL, 0xE26E6F00UL, 0xEBEF584AUL, 0x9BB7DAC4UL,
    0xBA66AACFUL, 0xCF761D02UL, 0xD12DF1B1UL, 0xC1998C77UL, 0xADC3DA48UL, 0x86A05DF7UL,
    0xF480C62FUL, 0xF0AC9AECUL, 0xDDBC5C3FUL, 0x6DDED01FUL, 0xC790B6DBUL, 0x2A3A25A3UL,
    0x9AAF0093UL, 0x53AD0457UL, 0xB6B42D29UL, 0x7E804BA7UL, 0x07DA0EAAUL, 0x76A1597BUL,
    0x2A12162DUL, 0xB7DCFDE5UL, 0xFAFEDB89UL, 0xFDBE896CUL, 0x76E4FCA9UL, 0x0670803EUL,
    0x156E85FFUL, 0x87FD073EUL, 0x28336761UL, 0x86182AEAUL, 0xBD4DAFE7UL, 0xB36E6D8FUL,
    0x3967955BUL, 0xBF3148D7UL, 0x8416DF30UL, 0x432DC735UL, 0x6125CE70UL, 0xC9B8CB30UL,
    0xFD6CBFA2UL, 0x00A4E46CUL, 0x05A0DD5AUL, 0x476F21D2UL, 0x1262845CUL, 0xB9496170UL,
    0xE0566B01UL, 0x52993755UL, 0x50B7D51EUL, 0xC4F1335FUL, 0x6E13E430UL, 0x5DA92E85UL,
    0xC3B21D36UL, 0x32A1A4B7UL, 0x08D4B1EAUL, 0x21F716E4UL, 0x698F77FFUL, 0x2780030CUL,
    0x2D408DA0UL, 0xCD4F99A5UL, 0x20D3A2B3UL, 0x0A5D2F42UL, 0xF9B4CBDAUL, 0x11D0BE7DUL,
    0xC1DB9BBDUL, 0x17AB81A2UL, 0xCA5C6A08UL, 0x17552E55UL, 0x0027F014UL, 0x7F8607E1UL,
    0x640B148DUL, 0x4196DEBEUL, 0x872AFDDAUL, 0xB6256B34UL, 0x897BFEF3UL, 0x059EBFB9UL,
    0x4F6A68A8UL, 0x2A4A5AC4UL, 0x4FBCF82DUL, 0x985AD795UL, 0xC7F48D4DUL, 0x0DA63A20UL,
    0x5F57A4B1UL, 0x3F149538UL, 0x800120CCUL, 0x86DD71B6UL, 0xDEC9F560UL, 0xBF11654DUL,
    0x6B0701ACUL, 0xB08CD0C0UL, 0xB2485551UL, 0x0EFB1EC3UL, 0x72953B06UL, 0xA33540C0UL,
    0x7BDC06CCUL, 0x45E0FA29UL, 0x4EC8CAD6UL, 0x41F3E8DEUL, 0x647CD864UL, 0x9B31BED9UL,
    0xC397A4D4UL, 0x5877C5E3UL, 0x6913DAF0UL, 0x3C3ABA46UL, 0x18465F75UL, 0x55F5BDD2UL,
    0xC6926E5DUL, 0x2EACED44UL, 0x0E423E1CUL, 0x87C461E9UL, 0xFD29F3D6UL, 0xE7CA7C22UL,
    0x35916FC5UL, 0xE0088DD7UL, 0xFFE26A6EUL, 0xC6FDB0C1UL, 0x0893745DUL, 0x7CB2AD6BUL,
    0x9D6ECD7BUL, 0x723E6A11UL, 0xC6A9CFF7UL, 0xDF7329BAUL, 0xC9B55100UL, 0xB70DB2E2UL,
    0x24BA7460UL, 0x7DE58AD8UL, 0x742C150DUL, 0x0C188194UL, 0x667E1629UL, 0x01767A9FUL,
    0xBEFDFDEFUL, 0x4556367EUL, 0xD913D9ECUL, 0xB9BA8BFCUL, 0x97C427A8UL, 0x31C36EF1UL,
    0x36C59456UL, 0xA8D8B5A8UL, 0xB40ECCCFUL, 0x2D891234UL, 0x576F8956UL, 0x2CE3CE99UL,
    0xB920D6AAUL, 0x5E6B9C2AUL, 0x3ECC5F11UL, 0x4A0BFDFBUL, 0xF4E16D3BUL, 0x8E2C86E2UL,
    0x84D4E9A9UL, 0xB4FCD1EEUL, 0xEFC9352EUL, 0x61392F44UL, 0x2138C8D9UL, 0x1B0AFC81UL,
    0x6A4AFBD8UL, 0x1C2F84B4UL, 0x538C994EUL, 0xCC2254DCUL, 0x552AD6C6UL, 0xC096190BUL,
    0xB8701A64UL, 0x9569605AUL, 0x26EE523FUL, 0x0F117F11UL, 0xB5F4F5CBUL, 0xFC2DBC34UL,
    0xEEBC34CCUL, 0x5DE8605EUL, 0xDD9B8E67UL, 0xEF3392B8UL, 0x17C99B58UL, 0x61BC57E1UL,
    0xC6835110UL, 0x3ED84871UL, 0xDDDD1C2DUL, 0xA118AF46UL, 0x2C21D7F3UL, 0x59987AD9UL,
    0xC0549EFAUL, 0x864FFC06UL, 0x56AE79E5UL, 0x36228922UL, 0xAD38DC93UL, 0x67AAE855UL,
    0x3826829BUL, 0xE7CAA40DUL, 0x51B13399UL, 0x0ED7A948UL, 0x0569F0B2UL, 0x65A7887FUL,
    0x974C8836UL, 0xD1F9B392UL, 0x214A827BUL, 0x21CF98DCUL, 0x9F405547UL, 0xDC3A74E1UL,
    0x42EB67DFUL, 0x9DFE5FD4UL, 0x5EA4677BUL, 0x7AACBAA2UL, 0xF6552388UL, 0x2B55BA41UL,
    0x086E5986UL, 0x2A218347UL, 0x39E6E389UL, 0xD49EE540UL, 0xFB49E956UL, 0xFFCA0F1CUL,
    0x8A59C52BUL, 0xFA94C5C1UL, 0xD3CFC50FUL, 0xAE5ADB86UL, 0xC5476243UL, 0x853B8621UL,
    0x94792C87UL, 0x61107B4CUL, 0x2A1A2C80UL, 0x12BF4390UL, 0x2688893CUL, 0x78E4C4A8UL,
    0x7BDBE5C2UL, 0x3AC4EAF4UL, 0x268A67F7UL, 0xBF920D2BUL, 0xA365B193UL, 0x3D0B7CBDUL,
    0xDC51A463UL, 0xDD27DDE1UL, 0x6919949AUL, 0x9529A828UL, 0xCE68B4EDUL, 0x09209F44UL,
    0xCA984E63UL, 0x8270237CUL, 0x7E32B90FUL, 0x8EF5A7E7UL, 0x561408F1UL, 0x212A9DB5UL,
    0x4D7E6F51UL, 0x19A5ABF9UL, 0xB5D6DF82UL, 0x61DD9602UL, 0x36169F3AUL, 0xC4A1A283UL,
    0x6DED727AUL, 0x8D39A9B8UL, 0x825C326BUL, 0x5B2746EDUL, 0x34007700UL, 0xD255F4FCUL,
    0x4D590180UL, 0x71E0E13FUL, 0x89B295F3UL, 0x64A8F1AEUL, 0xA74B38FCUL, 0x4CEAB2BBUL
#endif
};
#define TOP_WORDS   ((long)(sizeof( two_over_pi ) / sizeof( two_over_pi[0] )))

/* 32 bits of 2/pi from bit number from on, 0 being the first after the
   point; the bits before the point are 0 */
static unsigned long bits32( long from )
{
    long word;
    int shift;

    if ( from <= -32 ) {
        return 0;
    }
    if ( from < 0 ) {
        return two_over_pi[0] >> (int)-from;
    }
    word = from >> 5;
    shift = (int)(from & 31);
    if ( word >= TOP_WORDS ) {
        return 0;
    }
    if ( shift == 0 ) {
        return two_over_pi[word];
    }
    return (two_over_pi[word] << shift)
         | (word + 1 < TOP_WORDS ? two_over_pi[word + 1] >> (32 - shift) : 0);
}

/* Payne and Hanek: |x| is a 64-bit whole number m times 2^(e - 64), and
   the bits of 2/pi worth 4 or more once multiplied by that are left out,
   because a multiple of 4 quadrants changes nothing.  m times the next
   256 bits is a fixed point number with 254 bits after the point: the 2
   bits before it are n mod 4, the bits after are r / (pi/2).  Even the
   x closest to a multiple of pi/2 keeps over 100 bits after the zeros
   at the start of r.  Returns n mod 4, and r / (pi/2) as *hi + *lo: two
   parts that are exact even as doubles, so that X87.ASM's
   __pm_xtrigq() can add them in 64 bits under Open Watcom too. */
static int reduce_far( long double x, long double *hi, long double *lo )
{
    unsigned long m[2], w[8], p[10];
    unsigned long top_word, mid_word, low_word;
    unsigned long long acc;
    long double f;
    long first;
    int e, i, j, top, q, scale;

    f = frexpl( fabsl( x ), &e ) * 4294967296.0L;      /* |x| = f 2^(e - 32) */
    m[1] = (unsigned long)f;
    m[0] = (unsigned long)((f - m[1]) * 4294967296.0L);
    first = (long)e - 66;               /* the bit worth 2 */
    for ( i = 0; i < 8; i++ ) {
        w[7 - i] = bits32( first + 32L * i );
    }
    for ( i = 0; i < 10; i++ ) {
        p[i] = 0;
    }
    for ( i = 0; i < 2; i++ ) {
        acc = 0;
        for ( j = 0; j < 8; j++ ) {
            acc += (unsigned long long)m[i] * w[j] + p[i + j];
            p[i + j] = (unsigned long)acc;
            acc >>= 32;
        }
        p[i + 8] = (unsigned long)acc;
    }
    q = (int)(p[7] >> 30);
    p[7] &= 0x3FFFFFFFUL;
    if ( p[7] & 0x20000000UL ) {
        /* r / (pi/2) is 1/2 or more: one quadrant on, and r is negative,
           2^254 less the fraction */
        q = (q + 1) & 3;
        acc = 1;
        for ( i = 0; i < 8; i++ ) {
            acc += (unsigned long)~p[i];
            p[i] = (unsigned long)acc;
            acc >>= 32;
        }
        p[7] &= 0x3FFFFFFFUL;
        q |= 4;                         /* for now: r is negative */
    }

    /* the 96 bits from the first word that is not 0: the top 53 in hi,
       the other 43 in lo */
    for ( top = 7; top > 0 && p[top] == 0; top-- ) {
    }
    top_word = p[top];
    mid_word = top >= 1 ? p[top - 1] : 0;
    low_word = top >= 2 ? p[top - 2] : 0;
    scale = 32 * (top - 2) - 254;
    *hi = __pm_xscale( (long double)top_word * 2097152.0L + (mid_word >> 11),
                       (long double)(scale + 43) );
    *lo = __pm_xscale( (long double)(mid_word & 0x7FFUL) * 4294967296.0L + low_word,
                       (long double)scale );
    if ( q & 4 ) {
        *hi = -*hi;
        *lo = -*lo;
    }
    q &= 3;
    if ( x < 0.0L ) {                   /* -x = -n pi/2 - r */
        *hi = -*hi;
        *lo = -*lo;
        q = (4 - q) & 3;
    }
    return q;
}

/* the functions __pm_xtrigq() and trig() take */
#define TRIG_SIN    0
#define TRIG_COS    1
#define TRIG_TAN    2
#define TRIG_COT    3

/* sin, cos or tan x, from sin, cos, tan or cot r */
static long double trig( long double x, int op )
{
    long double r, n;
    long double hi = 0.0L;
    long double lo = 0.0L;
    int q, distant, negate;

    distant = fabsl( x ) > 1.6e6L;
    if ( distant ) {
        q = reduce_far( x, &hi, &lo );
        r = 0.0L;
    } else if ( fabsl( x ) <= PI_2 / 2.0L ) {
        q = 0;
        r = x;
    } else {
        n = __pm_xrndint( x * TWO_OVER_PI, RND_NEAR );
        q = (int)((long)n & 3);
        r = ((x - n * PIO2_1) - n * PIO2_2) - n * PIO2_3 - n * PIO2_3T;
    }
    switch ( op ) {
    case TRIG_SIN:                      /* sin, cos, -sin, -cos */
        negate = q & 2;
        op = (q & 1) ? TRIG_COS : TRIG_SIN;
        break;
    case TRIG_COS:                      /* cos, -sin, -cos, sin */
        negate = (q + 1) & 2;
        op = (q & 1) ? TRIG_SIN : TRIG_COS;
        break;
    default:                            /* tan, -cot, tan, -cot */
        negate = q & 1;
        op = (q & 1) ? TRIG_COT : TRIG_TAN;
        break;
    }
    if ( distant ) {
        r = __pm_xtrigq( hi, lo, op );
    } else if ( op == TRIG_SIN ) {
        r = __pm_xsin( r );
    } else if ( op == TRIG_COS ) {
        r = __pm_xcos( r );
    } else if ( op == TRIG_TAN ) {
        r = __pm_xtan( r );
    } else {
        r = 1.0L / __pm_xtan( r );
    }
    return negate ? -r : r;
}

long double sinl( long double x )
{
    if ( is_inf( x ) ) {
        return __pm_domain();
    }
    return trig( x, TRIG_SIN );
}

long double cosl( long double x )
{
    if ( is_inf( x ) ) {
        return __pm_domain();
    }
    return trig( x, TRIG_COS );
}

long double tanl( long double x )
{
    if ( is_inf( x ) ) {
        return __pm_domain();
    }
    return trig( x, TRIG_TAN );
}

long double atanl( long double x )
{
    return __pm_xatan2( x, 1.0L );
}

long double atan2l( long double y, long double x )
{
    return __pm_xatan2( y, x );
}

long double asinl( long double x )
{
    if ( x > 1.0L || x < -1.0L ) {
        return __pm_domain();
    }
    return __pm_xatan2( x, __pm_xsqrt( (1.0L - x) * (1.0L + x) ) );
}

long double acosl( long double x )
{
    if ( x > 1.0L || x < -1.0L ) {
        return __pm_domain();
    }
    return __pm_xatan2( __pm_xsqrt( (1.0L - x) * (1.0L + x) ), x );
}

WRAP1( sqrt )
WRAP1( sin )
WRAP1( cos )
WRAP1( tan )
WRAP1( atan )
WRAP2( atan2 )
WRAP1( asin )
WRAP1( acos )

/*--- exponentials ----------------------------------------------------*/

/* 2^x for |x| <= 1, then scaled: 2^(n + f) */
long double exp2l( long double x )
{
    long double n, f;

    if ( is_nan( x ) ) {
        return x;
    }
    if ( x > LD_MAXEXP ) {
        return __pm_range( __pm_infl() );
    }
    if ( x < LD_MINEXP - LD_MANT - 1 ) {
        return __pm_range( 0.0L );
    }
    n = __pm_xrndint( x, RND_NEAR );
    f = x - n;                          /* exact, |f| <= 0.5 */
    return __pm_xscale( __pm_x2xm1( f ) + 1.0L, n );
}

long double expl( long double x )
{
    long double n, r;

    if ( is_nan( x ) ) {
        return x;
    }
    if ( x > LD_MAXEXP * LN2 ) {
        return __pm_range( __pm_infl() );
    }
    if ( x < (LD_MINEXP - LD_MANT - 1) * LN2 ) {
        return __pm_range( 0.0L );
    }
    n = __pm_xrndint( x * LOG2E, RND_NEAR );
    r = (x - n * LN2_HI) - n * LN2_LO;  /* |r| <= about ln2 / 2 */
    return __pm_xscale( __pm_x2xm1( r * LOG2E ) + 1.0L, n );
}

long double expm1l( long double x )
{
    long double t;

    if ( is_nan( x ) ) {
        return x;
    }
    t = x * LOG2E;
    if ( t > -1.0L && t < 1.0L ) {
        return __pm_x2xm1( t );         /* full precision near zero */
    }
    if ( x < -LD_MANT ) {
        return -1.0L;
    }
    return expl( x ) - 1.0L;
}

WRAPR1( exp2 )
WRAPR1( exp )
WRAPR1( expm1 )

/*--- logarithms ------------------------------------------------------*/

static long double log_check( long double x, long double scale )
{
    if ( is_nan( x ) ) {
        return x;
    }
    if ( x < 0.0L ) {
        return __pm_domain();
    }
    if ( x == 0.0L ) {
        return __pm_range( -__pm_infl() );      /* a pole */
    }
    if ( is_inf( x ) ) {
        return x;
    }
    return __pm_xyl2x( x, scale );
}

long double logl( long double x )   { return log_check( x, LN2 ); }
long double log2l( long double x )  { return log_check( x, 1.0L ); }
long double log10l( long double x ) { return log_check( x, LOG10_2 ); }

long double log1pl( long double x )
{
    if ( is_nan( x ) ) {
        return x;
    }
    if ( x < -1.0L ) {
        return __pm_domain();
    }
    if ( x == -1.0L ) {
        return __pm_range( -__pm_infl() );
    }
    if ( x > -0.29L && x < 0.29L ) {
        return __pm_xyl2xp1( x, LN2 );  /* full precision near zero */
    }
    return logl( 1.0L + x );
}

WRAP1( log )
WRAP1( log2 )
WRAP1( log10 )
WRAP1( log1p )

/*--- powers ----------------------------------------------------------*/

static int is_integer( long double y )
{
    return !is_inf( y ) && __pm_xrndint( y, RND_ZERO ) == y;
}

static int is_odd_integer( long double y )
{
    return is_integer( y ) && fabsl( y ) < 18446744073709551616.0L
           && __pm_xprem( y, 2.0L ) != 0.0L;
}

long double powl( long double x, long double y )
{
    long double r, base;
    long n;
    int neg = 0;

    /* C99's special cases, in its order */
    if ( y == 0.0L || x == 1.0L ) {
        return 1.0L;
    }
    if ( is_nan( x ) || is_nan( y ) ) {
        return x + y;
    }
    if ( x == 0.0L ) {
        if ( y < 0.0L ) {
            errno = ERANGE;             /* a pole */
            return (is_odd_integer( y ) && __pm_signbitl( x )) ? -__pm_infl() : __pm_infl();
        }
        return is_odd_integer( y ) ? x : 0.0L;
    }
    if ( x == -1.0L && is_inf( y ) ) {
        return 1.0L;
    }
    if ( is_inf( y ) ) {
        if ( (fabsl( x ) < 1.0L) == (y < 0.0L) ) {
            return __pm_infl();
        }
        return 0.0L;
    }
    if ( is_inf( x ) ) {
        if ( x < 0.0L ) {
            if ( y < 0.0L ) {
                return is_odd_integer( y ) ? -0.0L : 0.0L;
            }
            return is_odd_integer( y ) ? x : -x;
        }
        return y < 0.0L ? 0.0L : x;
    }
    if ( x < 0.0L ) {
        if ( !is_integer( y ) ) {
            return __pm_domain();       /* a negative to a fraction */
        }
        neg = is_odd_integer( y );
        x = -x;
    }

    if ( is_integer( y ) && fabsl( y ) <= 64.0L ) {
        n = (long)fabsl( y );
        r = 1.0L;
        base = x;
        while ( n ) {
            if ( n & 1 ) {
                r *= base;
            }
            base *= base;
            n >>= 1;
        }
        if ( y < 0.0L ) {
            r = 1.0L / r;
        }
    } else {
        r = __pm_xpow( x, y );          /* all of it in the registers */
    }
    if ( is_inf( r ) || r == 0.0L ) {
        errno = ERANGE;
    }
    return neg ? -r : r;
}

WRAPR2( pow )

long double cbrtl( long double x )
{
    long double y;

    if ( is_nan( x ) || is_inf( x ) || x == 0.0L ) {
        return x;
    }
    y = exp2l( __pm_xyl2x( fabsl( x ), 1.0L ) / 3.0L );
    y -= (y * y * y - fabsl( x )) / (3.0L * y * y);    /* one Newton step */
    return x < 0.0L ? -y : y;
}

WRAP1( cbrt )

long double hypotl( long double x, long double y )
{
    long double big, small, q;

    if ( is_inf( x ) || is_inf( y ) ) {
        return __pm_infl();             /* even if the other is a NaN */
    }
    if ( is_nan( x ) || is_nan( y ) ) {
        return x + y;
    }
    big = fabsl( x );
    small = fabsl( y );
    if ( small > big ) {
        q = big;
        big = small;
        small = q;
    }
    if ( big == 0.0L ) {
        return 0.0L;
    }
    q = small / big;
    q = big * __pm_xsqrt( 1.0L + q * q );
    if ( is_inf( q ) ) {
        errno = ERANGE;
    }
    return q;
}

WRAPR2( hypot )

/*--- hyperbolic functions --------------------------------------------*/

long double sinhl( long double x )
{
    long double a = fabsl( x );
    long double e, r;

    if ( a < 1.0L ) {
        e = expm1l( a );
        r = (e + e / (e + 1.0L)) / 2.0L;
    } else if ( a < LD_MAXEXP * LN2 - 1.0L ) {
        e = expl( a );
        r = (e - 1.0L / e) / 2.0L;
    } else {
        r = expl( a - LN2 );            /* e^a / 2 without overflowing first */
    }
    return x < 0.0L ? -r : r;
}

long double coshl( long double x )
{
    long double a = fabsl( x );
    long double e;

    if ( a < LD_MAXEXP * LN2 - 1.0L ) {
        e = expl( a );
        return (e + 1.0L / e) / 2.0L;
    }
    return expl( a - LN2 );
}

long double tanhl( long double x )
{
    long double a = fabsl( x );
    long double e, r;

    if ( is_nan( x ) ) {
        return x;
    }
    if ( a > 40.0L ) {
        r = 1.0L;
    } else {
        e = expm1l( 2.0L * a );
        r = e / (e + 2.0L);
    }
    return x < 0.0L ? -r : r;
}

long double asinhl( long double x )
{
    long double a = fabsl( x );
    long double r;

    if ( is_nan( x ) || is_inf( x ) ) {
        return x;
    }
    if ( a > 1e10L ) {
        r = logl( a ) + LN2;
    } else {
        r = log1pl( a + a * a / (1.0L + __pm_xsqrt( 1.0L + a * a )) );
    }
    return x < 0.0L ? -r : r;
}

long double acoshl( long double x )
{
    if ( is_nan( x ) ) {
        return x;
    }
    if ( x < 1.0L ) {
        return __pm_domain();
    }
    if ( x > 1e10L ) {
        return logl( x ) + LN2;
    }
    return log1pl( (x - 1.0L) + __pm_xsqrt( (x - 1.0L) * (x + 1.0L) ) );
}

long double atanhl( long double x )
{
    long double a = fabsl( x );
    long double r;

    if ( is_nan( x ) ) {
        return x;
    }
    if ( a > 1.0L ) {
        return __pm_domain();
    }
    if ( a == 1.0L ) {
        return __pm_range( x < 0.0L ? -__pm_infl() : __pm_infl() );
    }
    r = 0.5L * log1pl( 2.0L * a / (1.0L - a) );
    return x < 0.0L ? -r : r;
}

WRAPR1( sinh )
WRAPR1( cosh )
WRAP1( tanh )
WRAP1( asinh )
WRAP1( acosh )
WRAPR1( atanh )

/*--- rounding --------------------------------------------------------*/

long double floorl( long double x )     { return __pm_xrndint( x, RND_DOWN ); }
long double ceill( long double x )      { return __pm_xrndint( x, RND_UP ); }
long double truncl( long double x )     { return __pm_xrndint( x, RND_ZERO ); }
long double rintl( long double x )      { return __pm_xrndint( x, RND_NEAR ); }
long double nearbyintl( long double x ) { return __pm_xrndint( x, RND_NEAR ); }

/* half away from zero; x - trunc(x) is exact */
long double roundl( long double x )
{
    long double t = __pm_xrndint( x, RND_ZERO );

    if ( fabsl( x - t ) >= 0.5L ) {
        t += (x < 0.0L) ? -1.0L : 1.0L;
    }
    return t;
}

WRAP1( floor )
WRAP1( ceil )
WRAP1( trunc )
WRAP1( rint )
WRAP1( nearbyint )
WRAP1( round )

static long to_long( long double r )
{
    if ( !(r >= (long double)LONG_MIN && r <= (long double)LONG_MAX) ) {
        errno = EDOM;
        return LONG_MIN;
    }
    return (long)r;
}

static long long to_llong( long double r )
{
    if ( !(r >= -9223372036854775808.0L && r < 9223372036854775808.0L) ) {
        errno = EDOM;
        return LLONG_MIN;
    }
    return (long long)r;
}

long lrintl( long double x )            { return to_long( rintl( x ) ); }
long lrint( double x )                  { return to_long( rintl( x ) ); }
long lrintf( float x )                  { return to_long( rintl( x ) ); }
long long llrintl( long double x )      { return to_llong( rintl( x ) ); }
long long llrint( double x )            { return to_llong( rintl( x ) ); }
long long llrintf( float x )            { return to_llong( rintl( x ) ); }
long lroundl( long double x )           { return to_long( roundl( x ) ); }
long lround( double x )                 { return to_long( roundl( x ) ); }
long lroundf( float x )                 { return to_long( roundl( x ) ); }
long long llroundl( long double x )     { return to_llong( roundl( x ) ); }
long long llround( double x )           { return to_llong( roundl( x ) ); }
long long llroundf( float x )           { return to_llong( roundl( x ) ); }

/*--- remainders ------------------------------------------------------*/

long double fmodl( long double x, long double y )
{
    if ( is_nan( x ) || is_nan( y ) ) {
        return x + y;
    }
    if ( is_inf( x ) || y == 0.0L ) {
        return __pm_domain();
    }
    if ( is_inf( y ) ) {
        return x;
    }
    return __pm_xprem( x, y );          /* exact */
}

long double remainderl( long double x, long double y )
{
    if ( is_nan( x ) || is_nan( y ) ) {
        return x + y;
    }
    if ( is_inf( x ) || y == 0.0L ) {
        return __pm_domain();
    }
    if ( is_inf( y ) ) {
        return x;
    }
    return __pm_xprem1( x, y );         /* exact, the IEEE remainder */
}

WRAP2( fmod )
WRAP2( remainder )

/* the remainder, and the low three bits of the quotient with its sign */
long double remquol( long double x, long double y, int *quo )
{
    long double r = remainderl( x, y );
    long double ay = fabsl( y );
    long double r8, q;

    *quo = 0;
    if ( is_nan( r ) ) {
        return r;
    }
    r8 = __pm_xprem( fabsl( x ), 8.0L * ay );       /* |x| mod 8|y| */
    q = __pm_xrndint( (r8 - fabsl( r ) * ((x < 0.0L) == (r < 0.0L) ? 1.0L : -1.0L)) / ay,
                      RND_NEAR );
    *quo = (int)__pm_xprem( q, 8.0L );
    if ( *quo < 0 ) {
        *quo += 8;
    }
    if ( (x < 0.0L) != (y < 0.0L) ) {
        *quo = -*quo;
    }
    return r;
}

double remquo( double x, double y, int *quo )   { return (double)remquol( x, y, quo ); }
float remquof( float x, float y, int *quo )     { return (float)remquol( x, y, quo ); }
