/*===========================================================================
 * LZMAENC.C  -  LZMA1 and LZMA2 encoder
 * Target: MSVC 2.2  Win32s
 *
 * The format is the one LZMADEC.C reads, which is Igor Pavlov's LZMA as
 * placed in the public domain with the LZMA SDK; the coder below is the
 * mirror image of that decoder - same probabilities, same state machine,
 * same bit trees - and the parse follows the structure of the SDK's own
 * encoder, because that structure is what the format was designed around.
 * LZMA2 is a framing of the same coded steps, near the end of the file.
 *
 * THREE PARTS:
 *
 *   The RANGE CODER.  The SDK keeps 'low' in 64 bits so the one carry that
 *   can come out of the top is simply bit 32.  MSVC 2.2 has no dependable
 *   64-bit integer (see ARCDEFS.H), so here 'low' is 32 bits and the carry
 *   is a flag beside it - set when an addition wraps, consumed by ShiftLow.
 *   One carry is all there can ever be between two shifts.
 *
 *   The MATCH FINDER ("BT4").  Positions are hashed on their first two,
 *   three and four bytes.  The two- and three-byte tables answer only "the
 *   newest place these bytes began"; the four-byte hash heads a BINARY TREE
 *   of every earlier position with that hash still inside the dictionary,
 *   kept in sorted order of the bytes that follow - so one walk down the tree
 *   finds the longest match at each distance and re-sorts the tree around
 *   the new position at the same time.  'son' holds the two child links of
 *   every position in a ring the size of the dictionary.  Positions are
 *   counted from cyclicSize upward, so that 0 can mean "nothing".
 *
 *   The OPTIMAL PARSER.  From the current position, the cheapest way to code
 *   each of the next few hundred (up to 4096) bytes is worked out by dynamic
 *   programming over every choice the format has - a literal, a one-byte
 *   repeat of the last distance, a match at one of the four remembered
 *   distances, a new match of every length the finder reported, and the two
 *   compound moves "match, literal, repeat" - priced in 1/16-bit units from
 *   the coder's current probabilities.  The path is then walked back and its
 *   steps handed out one at a time.
 *
 * EVERY MATCH IS CHECKED against the data before it is coded.  The parser
 * tracks the repeat distances along its own simulated path; if that
 * simulation and the coder ever disagreed, the result would be a stream that
 * decodes to the wrong bytes and passes no test.  The check costs one
 * comparison per matched byte and turns that failure into a refusal.
 *===========================================================================*/

#include <stdlib.h>
#include <string.h>
#include "lzmaenc.h"

/*---- Format constants ----------------------------------------------------- */
#define kNumOpts               ( 1 << 12 )
#define kMatchLenMin           2
#define kMatchLenMax           273
#define kNumReps               4
#define kNumStates             12
#define kNumPosBitsMax         4
#define kNumPosStatesMax       ( 1 << kNumPosBitsMax )
#define kNumLenToPosStates     4
#define kNumPosSlotBits        6
#define kDistTableSizeMax      ( 32 * 2 )
#define kNumAlignBits          4
#define kAlignTableSize        ( 1 << kNumAlignBits )
#define kAlignMask             ( kAlignTableSize - 1 )
#define kStartPosModelIndex    4
#define kEndPosModelIndex      14
#define kNumFullDistances      ( 1 << ( kEndPosModelIndex >> 1 ) )
/* The distance footers' reverse bit trees, ONE LONGER than the format's
 * count.  Each tree is indexed from 1, so the slot-4 tree, which starts at
 * offset base - posSlot = 0, would otherwise have to be handed in as
 * posEncoders - 1 (the SDK's idiom).  That pointer is outside the array,
 * and MSVC 2.2 warned C4756 (overflow in constant arithmetic) on the
 * expression.  Element 0 is never used. */
#define kNumPosEncoders        ( 1 + kNumFullDistances - kEndPosModelIndex )
#define kLenNumLowBits         3
#define kLenNumLowSymbols      ( 1 << kLenNumLowBits )
#define kLenNumMidBits         3
#define kLenNumMidSymbols      ( 1 << kLenNumMidBits )
#define kLenNumHighBits        8
#define kLenNumHighSymbols     ( 1 << kLenNumHighBits )
#define kLenNumSymbolsTotal    ( kLenNumLowSymbols + kLenNumMidSymbols + kLenNumHighSymbols )

#define kNumBitModelTotalBits  11
#define kBitModelTotal         ( 1 << kNumBitModelTotalBits )
#define kNumMoveBits           5
#define kProbInitValue         ( kBitModelTotal >> 1 )
#define kTopValue              ( (uint32_t)1 << 24 )
#define kNumMoveReducingBits   4
#define kNumBitPriceShiftBits  4
#define kInfinityPrice         ( (uint32_t)1 << 30 )

#define kHash2Size             ( 1 << 10 )
#define kHash3Size             ( 1 << 16 )
#define kFix3HashSize          ( kHash2Size )
#define kFix4HashSize          ( kHash2Size + kHash3Size )
#define kEmptyHashValue        0
#define kNormalizeAt           0xF0000000UL

#define OUT_BUFSIZE            65536U
#define PROGRESS_STEP          65536UL

/* LZMA2 chunk limits, from the format: an LZMA chunk records its sizes less
 * one in 21 bits (in) and 16 (out); a stored chunk its size in 16. */
#define LZMA2_UNPACK_MAX       ( (uint32_t)1 << 21 )
#define LZMA2_PACK_MAX         ( (uint32_t)1 << 16 )
#define LZMA2_COPY_MAX         ( (uint32_t)1 << 16 )
/* The most one coded step can add to a chunk: a byte per range-coder shift
 * and at most one shift per coded bit - a match is at most 48 bits (10 of
 * length, 6 of slot, 30 of distance, 2 of flags). */
#define LZMA2_SYMBOL_MAX       64
/* Room left when a chunk is closed between parses rather than in one, which
 * is where it is wanted: only there can it still be sent as stored bytes. */
#define LZMA2_RESERVE          2048

#define IsCharState( s )       ( (s) < 7 )
#define GetLenToPosState( len ) \
    ( ( (len) < kNumLenToPosStates + 1 ) ? (len) - 2 : kNumLenToPosStates - 1 )

typedef uint16_t CProb;

static const uint8_t kLiteralNextStates[kNumStates]  = { 0, 0, 0, 0, 1, 2, 3, 4, 5, 6,  4,  5 };
static const uint8_t kMatchNextStates[kNumStates]    = { 7, 7, 7, 7, 7, 7, 7, 10, 10, 10, 10, 10 };
static const uint8_t kRepNextStates[kNumStates]      = { 8, 8, 8, 8, 8, 8, 8, 11, 11, 11, 11, 11 };
static const uint8_t kShortRepNextStates[kNumStates] = { 9, 9, 9, 9, 9, 9, 9, 11, 11, 11, 11, 11 };

/*---- Tables built once ---------------------------------------------------- */
static uint32_t g_probPrices[kBitModelTotal >> kNumMoveReducingBits];
static uint8_t  g_log2[256];
static uint32_t g_crc[256];
static int      g_tablesBuilt = 0;

#define GET_PRICE( prob, bit ) \
    g_probPrices[ ( (prob) ^ ( ( 0 - (uint32_t)(bit) ) & ( kBitModelTotal - 1 ) ) ) >> kNumMoveReducingBits ]
#define GET_PRICE_0( prob ) g_probPrices[ (prob) >> kNumMoveReducingBits ]
#define GET_PRICE_1( prob ) g_probPrices[ ( (prob) ^ ( kBitModelTotal - 1 ) ) >> kNumMoveReducingBits ]

static void BuildTables( void )
{
    unsigned i;

    if ( g_tablesBuilt ) return;

    /* -log2(p) in 1/16 bits, for every probability the model can hold. */
    for ( i = ( 1 << kNumMoveReducingBits ) / 2; i < kBitModelTotal;
          i += ( 1 << kNumMoveReducingBits ) )
    {
        uint32_t w = i, bitCount = 0;
        int      j;
        for ( j = 0; j < kNumBitPriceShiftBits; j++ )
        {
            w = w * w;
            bitCount <<= 1;
            while ( w >= ( (uint32_t)1 << 16 ) )
            {
                w >>= 1;
                bitCount++;
            }
        }
        g_probPrices[ i >> kNumMoveReducingBits ] =
            ( ( kNumBitModelTotalBits << kNumBitPriceShiftBits ) - 15 - bitCount );
    }

    g_log2[0] = 0;
    g_log2[1] = 0;
    for ( i = 2; i < 256; i++ ) g_log2[i] = (uint8_t)( g_log2[i >> 1] + 1 );

    for ( i = 0; i < 256; i++ )
    {
        uint32_t r = i;
        int      k;
        for ( k = 0; k < 8; k++ )
            r = ( r & 1 ) ? ( ( r >> 1 ) ^ 0xEDB88320UL ) : ( r >> 1 );
        g_crc[i] = r;
    }
    g_tablesBuilt = 1;
}

/* Index of the highest set bit of v (v > 0). */
static unsigned HighBit( uint32_t v )
{
    if ( v >> 16 )
        return ( v >> 24 ) ? 24 + g_log2[v >> 24] : 16 + g_log2[v >> 16];
    return ( v >> 8 ) ? 8 + g_log2[v >> 8] : g_log2[v];
}

/* The position slot of a 0-based distance. */
static uint32_t PosSlot( uint32_t dist )
{
    unsigned n;
    if ( dist < 4 ) return dist;
    n = HighBit( dist );
    return ( (uint32_t)n << 1 ) | ( ( dist >> ( n - 1 ) ) & 1 );
}

/*===========================================================================
 * Range encoder
 *===========================================================================*/
typedef struct {
    uint32_t low;
    uint32_t carry;           /* bit 32 of the SDK's 64-bit 'low'           */
    uint32_t range;
    uint8_t  cache;
    uint32_t cacheSize;
    uint8_t *buf;
    uint32_t bufPos;
    uint32_t written;
    LzmaEncWrite write;       /* NULL for LZMA2: 'buf' holds one chunk      */
    void    *user;
    int      failed;
    int      overflow;        /* an LZMA2 chunk outgrew 'buf' - see below   */
} CRangeEnc;

static void RcFlushBuf( CRangeEnc *rc )
{
    if ( rc->bufPos == 0 ) return;
    if ( !rc->failed && !rc->write( rc->user, rc->buf, rc->bufPos ) )
        rc->failed = 1;
    rc->written += rc->bufPos;
    rc->bufPos = 0;
}

static void RcWriteByte( CRangeEnc *rc, uint8_t b )
{
    if ( rc->bufPos == OUT_BUFSIZE )
    {
        /* LZMA2 ends every chunk well before this (LZMA2_SYMBOL_MAX), so
         * this is a bug guard: the chunk is refused, never cut short. */
        if ( !rc->write ) { rc->overflow = 1; return; }
        RcFlushBuf( rc );
    }
    rc->buf[rc->bufPos++] = b;
}

static void RcInit( CRangeEnc *rc )
{
    rc->low = 0;
    rc->carry = 0;
    rc->range = 0xFFFFFFFFUL;
    rc->cache = 0;
    rc->cacheSize = 1;
    rc->bufPos = 0;
    rc->written = 0;
    rc->failed = 0;
    rc->overflow = 0;
}

static void RcShiftLow( CRangeEnc *rc )
{
    if ( rc->low < 0xFF000000UL || rc->carry )
    {
        uint8_t temp = rc->cache;
        do {
            RcWriteByte( rc, (uint8_t)( temp + (uint8_t)rc->carry ) );
            temp = 0xFF;
        } while ( --rc->cacheSize != 0 );
        rc->cache = (uint8_t)( rc->low >> 24 );
    }
    rc->cacheSize++;
    rc->low = ( rc->low & 0x00FFFFFFUL ) << 8;
    rc->carry = 0;
}

static void RcAddLow( CRangeEnc *rc, uint32_t add )
{
    uint32_t nl = rc->low + add;
    if ( nl < rc->low ) rc->carry = 1;
    rc->low = nl;
}

static void RcEncodeBit( CRangeEnc *rc, CProb *prob, uint32_t bit )
{
    uint32_t ttt = *prob;
    uint32_t bound = ( rc->range >> kNumBitModelTotalBits ) * ttt;

    if ( bit == 0 )
    {
        rc->range = bound;
        ttt += ( kBitModelTotal - ttt ) >> kNumMoveBits;
    }
    else
    {
        RcAddLow( rc, bound );
        rc->range -= bound;
        ttt -= ttt >> kNumMoveBits;
    }
    *prob = (CProb)ttt;
    if ( rc->range < kTopValue )
    {
        rc->range <<= 8;
        RcShiftLow( rc );
    }
}

static void RcEncodeDirectBits( CRangeEnc *rc, uint32_t value, unsigned numBits )
{
    do {
        rc->range >>= 1;
        RcAddLow( rc, rc->range & ( 0 - ( ( value >> --numBits ) & 1 ) ) );
        if ( rc->range < kTopValue )
        {
            rc->range <<= 8;
            RcShiftLow( rc );
        }
    } while ( numBits != 0 );
}

static void RcFlush( CRangeEnc *rc )
{
    int i;
    for ( i = 0; i < 5; i++ ) RcShiftLow( rc );
    if ( rc->write ) RcFlushBuf( rc );
}

/* What the stream would come to if it were flushed now: the bytes out, the
 * ones still held back in case of a carry, and the four of 'low' a flush
 * pushes out after them. */
static uint32_t RcPending( const CRangeEnc *rc )
{
    return rc->bufPos + rc->cacheSize + 4;
}

/*---- Bit trees ------------------------------------------------------------ */
static void RcTreeEncode( CRangeEnc *rc, CProb *probs, unsigned numBits, uint32_t symbol )
{
    uint32_t m = 1;
    while ( numBits != 0 )
    {
        uint32_t bit;
        numBits--;
        bit = ( symbol >> numBits ) & 1;
        RcEncodeBit( rc, probs + m, bit );
        m = ( m << 1 ) | bit;
    }
}

static void RcTreeReverseEncode( CRangeEnc *rc, CProb *probs, unsigned numBits, uint32_t symbol )
{
    uint32_t m = 1;
    unsigned i;
    for ( i = 0; i < numBits; i++ )
    {
        uint32_t bit = symbol & 1;
        RcEncodeBit( rc, probs + m, bit );
        m = ( m << 1 ) | bit;
        symbol >>= 1;
    }
}

static uint32_t RcTreeGetPrice( const CProb *probs, unsigned numBits, uint32_t symbol )
{
    uint32_t price = 0;
    symbol |= ( (uint32_t)1 << numBits );
    while ( symbol != 1 )
    {
        price += GET_PRICE( probs[symbol >> 1], symbol & 1 );
        symbol >>= 1;
    }
    return price;
}

static uint32_t RcTreeReverseGetPrice( const CProb *probs, unsigned numBits, uint32_t symbol )
{
    uint32_t price = 0, m = 1;
    unsigned i;
    for ( i = numBits; i != 0; i-- )
    {
        uint32_t bit = symbol & 1;
        symbol >>= 1;
        price += GET_PRICE( probs[m], bit );
        m = ( m << 1 ) | bit;
    }
    return price;
}

/*---- Literals ------------------------------------------------------------- */
static void LitEncode( CRangeEnc *rc, CProb *probs, uint32_t symbol )
{
    symbol |= 0x100;
    do {
        RcEncodeBit( rc, probs + ( symbol >> 8 ), ( symbol >> 7 ) & 1 );
        symbol <<= 1;
    } while ( symbol < 0x10000 );
}

/* After a match the decoder predicts each bit from the byte at the last
 * distance until the first bit that differs - so the encoder walks the same
 * three-way table. */
static void LitEncodeMatched( CRangeEnc *rc, CProb *probs, uint32_t symbol, uint32_t matchByte )
{
    uint32_t offs = 0x100;
    symbol |= 0x100;
    do {
        matchByte <<= 1;
        RcEncodeBit( rc, probs + ( offs + ( matchByte & offs ) + ( symbol >> 8 ) ),
                     ( symbol >> 7 ) & 1 );
        symbol <<= 1;
        offs &= ~( matchByte ^ symbol );
    } while ( symbol < 0x10000 );
}

static uint32_t LitGetPrice( const CProb *probs, uint32_t symbol )
{
    uint32_t price = 0;
    symbol |= 0x100;
    do {
        price += GET_PRICE( probs[symbol >> 8], ( symbol >> 7 ) & 1 );
        symbol <<= 1;
    } while ( symbol < 0x10000 );
    return price;
}

static uint32_t LitGetPriceMatched( const CProb *probs, uint32_t symbol, uint32_t matchByte )
{
    uint32_t price = 0, offs = 0x100;
    symbol |= 0x100;
    do {
        matchByte <<= 1;
        price += GET_PRICE( probs[offs + ( matchByte & offs ) + ( symbol >> 8 )],
                            ( symbol >> 7 ) & 1 );
        symbol <<= 1;
        offs &= ~( matchByte ^ symbol );
    } while ( symbol < 0x10000 );
    return price;
}

/*---- Lengths -------------------------------------------------------------- */
typedef struct {
    CProb choice;
    CProb choice2;
    CProb low[kNumPosStatesMax << kLenNumLowBits];
    CProb mid[kNumPosStatesMax << kLenNumMidBits];
    CProb high[kLenNumHighSymbols];
} CLenEnc;

typedef struct {
    CLenEnc  p;
    uint32_t prices[kNumPosStatesMax][kLenNumSymbolsTotal];
    uint32_t tableSize;
    uint32_t counters[kNumPosStatesMax];
} CLenPriceEnc;

static void LenEncInit( CLenEnc *p )
{
    unsigned i;
    p->choice = p->choice2 = kProbInitValue;
    for ( i = 0; i < ( kNumPosStatesMax << kLenNumLowBits ); i++ ) p->low[i] = kProbInitValue;
    for ( i = 0; i < ( kNumPosStatesMax << kLenNumMidBits ); i++ ) p->mid[i] = kProbInitValue;
    for ( i = 0; i < kLenNumHighSymbols; i++ ) p->high[i] = kProbInitValue;
}

static void LenEncEncode( CLenEnc *p, CRangeEnc *rc, uint32_t symbol, uint32_t posState )
{
    if ( symbol < kLenNumLowSymbols )
    {
        RcEncodeBit( rc, &p->choice, 0 );
        RcTreeEncode( rc, p->low + ( posState << kLenNumLowBits ), kLenNumLowBits, symbol );
    }
    else
    {
        RcEncodeBit( rc, &p->choice, 1 );
        if ( symbol < kLenNumLowSymbols + kLenNumMidSymbols )
        {
            RcEncodeBit( rc, &p->choice2, 0 );
            RcTreeEncode( rc, p->mid + ( posState << kLenNumMidBits ), kLenNumMidBits,
                          symbol - kLenNumLowSymbols );
        }
        else
        {
            RcEncodeBit( rc, &p->choice2, 1 );
            RcTreeEncode( rc, p->high, kLenNumHighBits,
                          symbol - kLenNumLowSymbols - kLenNumMidSymbols );
        }
    }
}

static void LenEncSetPrices( const CLenEnc *p, uint32_t posState, uint32_t numSymbols, uint32_t *prices )
{
    uint32_t a0 = GET_PRICE_0( p->choice );
    uint32_t a1 = GET_PRICE_1( p->choice );
    uint32_t b0 = a1 + GET_PRICE_0( p->choice2 );
    uint32_t b1 = a1 + GET_PRICE_1( p->choice2 );
    uint32_t i = 0;

    for ( ; i < kLenNumLowSymbols; i++ )
    {
        if ( i >= numSymbols ) return;
        prices[i] = a0 + RcTreeGetPrice( p->low + ( posState << kLenNumLowBits ),
                                         kLenNumLowBits, i );
    }
    for ( ; i < kLenNumLowSymbols + kLenNumMidSymbols; i++ )
    {
        if ( i >= numSymbols ) return;
        prices[i] = b0 + RcTreeGetPrice( p->mid + ( posState << kLenNumMidBits ),
                                         kLenNumMidBits, i - kLenNumLowSymbols );
    }
    for ( ; i < numSymbols; i++ )
        prices[i] = b1 + RcTreeGetPrice( p->high, kLenNumHighBits,
                                         i - kLenNumLowSymbols - kLenNumMidSymbols );
}

static void LenPriceUpdateTable( CLenPriceEnc *p, uint32_t posState )
{
    LenEncSetPrices( &p->p, posState, p->tableSize, p->prices[posState] );
    p->counters[posState] = p->tableSize;
}

static void LenPriceUpdateTables( CLenPriceEnc *p, uint32_t numPosStates )
{
    unsigned posState;
    for ( posState = 0; posState < numPosStates; posState++ )
        LenPriceUpdateTable( p, posState );
}

static void LenEncEncode2( CLenPriceEnc *p, CRangeEnc *rc, uint32_t symbol, uint32_t posState )
{
    LenEncEncode( &p->p, rc, symbol, posState );
    if ( --p->counters[posState] == 0 )
        LenPriceUpdateTable( p, posState );
}

/*===========================================================================
 * The encoder
 *===========================================================================*/
typedef struct {
    uint32_t price;
    uint32_t posPrev;
    uint32_t backPrev;
    uint32_t posPrev2;
    uint32_t backPrev2;
    uint32_t backs[kNumReps];
    uint8_t  state;
    uint8_t  prev1IsChar;
    uint8_t  prev2;
} COptimal;

/* The coder's model as it stood when an LZMA2 chunk began.  A chunk that is
 * sent as stored bytes never reaches the decoder's model, so the encoder's
 * goes back to this to stay in step with it.  The price tables are not kept:
 * they only steer the parse, and are worked out again from these. */
typedef struct {
    uint32_t state;
    uint32_t reps[kNumReps];
    CProb   *litProbs;
    CProb    isMatch[kNumStates][kNumPosStatesMax];
    CProb    isRep[kNumStates];
    CProb    isRepG0[kNumStates];
    CProb    isRepG1[kNumStates];
    CProb    isRepG2[kNumStates];
    CProb    isRep0Long[kNumStates][kNumPosStatesMax];
    CProb    posSlotEncoder[kNumLenToPosStates][1 << kNumPosSlotBits];
    CProb    posEncoders[kNumPosEncoders];
    CProb    posAlignEncoder[1 << kNumAlignBits];
    CLenEnc  lenProbs;
    CLenEnc  repLenProbs;
} CSaved;

#define MakeAsChar( o )     { (o)->backPrev = (uint32_t)-1; (o)->prev1IsChar = 0; }
#define MakeAsShortRep( o ) { (o)->backPrev = 0;          (o)->prev1IsChar = 0; }
#define IsShortRep( o )     ( (o)->backPrev == 0 )

typedef struct {
    /* settings */
    uint32_t dictSize;
    unsigned lc, lp, pb;
    uint32_t numFastBytes;
    uint32_t cutValue;
    uint32_t pbMask, lpMask;

    /* the window: bytes [0, dataEnd) of bufBase are valid, and 'cur' is the
     * offset of match-finder position 'pos' */
    uint8_t *bufBase;
    uint32_t bufSize;
    uint32_t cur;
    uint32_t dataEnd;
    int      streamEnd;
    uint32_t keepBefore;
    uint32_t keepAfter;
    LzmaEncRead read;
    void    *readUser;
    uint32_t inTotal;

    /* the match finder */
    uint32_t  pos;
    uint32_t  cyclicPos;
    uint32_t  cyclicSize;
    uint32_t *hash;
    uint32_t  hashMask;
    uint32_t  hashCount;
    uint32_t *son;

    /* the parser */
    uint32_t matches[kMatchLenMax * 2 + 2 + 1];
    uint32_t numAvail;
    uint32_t longestMatchLength;
    uint32_t numPairs;
    uint32_t additionalOffset;
    uint32_t optimumEndIndex;
    uint32_t optimumCurrentIndex;
    COptimal *opt;

    /* the model */
    uint32_t state;
    uint32_t reps[kNumReps];
    CProb   *litProbs;
    CProb    isMatch[kNumStates][kNumPosStatesMax];
    CProb    isRep[kNumStates];
    CProb    isRepG0[kNumStates];
    CProb    isRepG1[kNumStates];
    CProb    isRepG2[kNumStates];
    CProb    isRep0Long[kNumStates][kNumPosStatesMax];
    CProb    posSlotEncoder[kNumLenToPosStates][1 << kNumPosSlotBits];
    CProb    posEncoders[kNumPosEncoders];
    CProb    posAlignEncoder[1 << kNumAlignBits];
    CLenPriceEnc lenEnc;
    CLenPriceEnc repLenEnc;

    /* prices */
    uint32_t posSlotPrices[kNumLenToPosStates][kDistTableSizeMax];
    uint32_t distancesPrices[kNumLenToPosStates][kNumFullDistances];
    uint32_t alignPrices[kAlignTableSize];
    uint32_t alignPriceCount;
    uint32_t matchPriceCount;
    uint32_t distTableSize;

    CRangeEnc rc;
    int       badMatch;

    /* LZMA2: the range coder fills rc.buf with one chunk, which goes out
     * here with its header in front of it */
    int       lzma2;
    LzmaEncWrite out;
    void     *outUser;
    uint32_t  outTotal;
    uint32_t  chunkIn;        /* input bytes coded into the chunk so far     */
    int       dictReset;      /* nothing sent yet: the first chunk says so   */
    int       needProps;      /* the next LZMA chunk carries lc/lp/pb ...    */
    int       needState;      /* ... and resets the decoder's model          */
    uint8_t   propsByte;
    CSaved   *saved;
} CLzmaEnc;

#define LIT_PROBS( e, pos, prevByte ) \
    ( (e)->litProbs + (uint32_t)0x300 * \
      ( ( ( (pos) & (e)->lpMask ) << (e)->lc ) + ( (uint32_t)(prevByte) >> ( 8 - (e)->lc ) ) ) )

/*---- The window ----------------------------------------------------------- */

/* Read until the buffer is full or the input ends. */
static void MfReadBlock( CLzmaEnc *e )
{
    while ( !e->streamEnd && e->dataEnd < e->bufSize )
    {
        uint32_t got = e->read( e->readUser, e->bufBase + e->dataEnd,
                                e->bufSize - e->dataEnd );
        if ( got == 0 ) { e->streamEnd = 1; break; }
        e->dataEnd += got;
        e->inTotal += got;
    }
}

/* Make sure the finder has keepAfter bytes in front of it (or the input has
 * ended), moving the window down to make room when it has to.  The history
 * kept below 'cur' is the whole dictionary plus the parser's look-ahead,
 * since the coder works up to kNumOpts positions behind the finder. */
static void MfEnsure( CLzmaEnc *e )
{
    if ( e->streamEnd || e->dataEnd - e->cur >= e->keepAfter ) return;
    if ( e->bufSize - e->cur < e->keepAfter && e->cur > e->keepBefore )
    {
        uint32_t drop = e->cur - e->keepBefore;
        memmove( e->bufBase, e->bufBase + drop, e->dataEnd - drop );
        e->cur -= drop;
        e->dataEnd -= drop;
    }
    MfReadBlock( e );
}

static uint32_t MfAvail( CLzmaEnc *e )
{
    return e->dataEnd - e->cur;
}

/* Subtract from every stored position once the counter nears the top.  Not
 * reachable with the 2 GB archives this program can write, but cheap to have. */
static void MfNormalize( CLzmaEnc *e )
{
    uint32_t sub = e->pos - e->cyclicSize;
    unsigned i, n;

    n = e->hashCount;
    for ( i = 0; i < n; i++ )
        e->hash[i] = ( e->hash[i] <= sub ) ? kEmptyHashValue : e->hash[i] - sub;
    n = e->cyclicSize * 2;
    for ( i = 0; i < n; i++ )
        e->son[i] = ( e->son[i] <= sub ) ? kEmptyHashValue : e->son[i] - sub;
    e->pos -= sub;
}

static void MfMovePos( CLzmaEnc *e )
{
    if ( ++e->cyclicPos == e->cyclicSize ) e->cyclicPos = 0;
    e->cur++;
    if ( ++e->pos >= kNormalizeAt ) MfNormalize( e );
    MfEnsure( e );
}

/* Walk the tree from curMatch, collecting (length, distance-1) pairs of
 * strictly increasing length into 'd', and re-link the tree around 'pos'. */
static uint32_t *GetMatchesSpec( CLzmaEnc *e, uint32_t lenLimit, uint32_t curMatch,
                                 uint32_t *d, uint32_t maxLen )
{
    const uint8_t *cur = e->bufBase + e->cur;
    uint32_t *son = e->son;
    uint32_t  pos = e->pos;
    uint32_t  cyc = e->cyclicPos, cycSize = e->cyclicSize;
    uint32_t *ptr0 = son + ( cyc << 1 ) + 1;
    uint32_t *ptr1 = son + ( cyc << 1 );
    uint32_t  len0 = 0, len1 = 0;
    uint32_t  cutValue = e->cutValue;

    for ( ;; )
    {
        uint32_t delta = pos - curMatch;
        if ( cutValue-- == 0 || delta >= cycSize )
        {
            *ptr0 = *ptr1 = kEmptyHashValue;
            return d;
        }
        {
            uint32_t   *pair = son + ( ( cyc - delta +
                                         ( ( delta > cyc ) ? cycSize : 0 ) ) << 1 );
            const uint8_t *pb = cur - delta;
            uint32_t       len = ( len0 < len1 ) ? len0 : len1;

            if ( pb[len] == cur[len] )
            {
                if ( ++len != lenLimit && pb[len] == cur[len] )
                    while ( ++len != lenLimit )
                        if ( pb[len] != cur[len] )
                            break;
                if ( maxLen < len )
                {
                    *d++ = maxLen = len;
                    *d++ = delta - 1;
                    if ( len == lenLimit )
                    {
                        *ptr1 = pair[0];
                        *ptr0 = pair[1];
                        return d;
                    }
                }
            }
            if ( pb[len] < cur[len] )
            {
                *ptr1 = curMatch;
                ptr1 = pair + 1;
                curMatch = *ptr1;
                len1 = len;
            }
            else
            {
                *ptr0 = curMatch;
                ptr0 = pair;
                curMatch = *ptr0;
                len0 = len;
            }
        }
    }
}

/* The same walk, keeping nothing: re-link the tree around 'pos' only. */
static void SkipMatchesSpec( CLzmaEnc *e, uint32_t lenLimit, uint32_t curMatch )
{
    const uint8_t *cur = e->bufBase + e->cur;
    uint32_t *son = e->son;
    uint32_t  pos = e->pos;
    uint32_t  cyc = e->cyclicPos, cycSize = e->cyclicSize;
    uint32_t *ptr0 = son + ( cyc << 1 ) + 1;
    uint32_t *ptr1 = son + ( cyc << 1 );
    uint32_t  len0 = 0, len1 = 0;
    uint32_t  cutValue = e->cutValue;

    for ( ;; )
    {
        uint32_t delta = pos - curMatch;
        if ( cutValue-- == 0 || delta >= cycSize )
        {
            *ptr0 = *ptr1 = kEmptyHashValue;
            return;
        }
        {
            uint32_t   *pair = son + ( ( cyc - delta +
                                         ( ( delta > cyc ) ? cycSize : 0 ) ) << 1 );
            const uint8_t *pb = cur - delta;
            uint32_t       len = ( len0 < len1 ) ? len0 : len1;

            if ( pb[len] == cur[len] )
            {
                while ( ++len != lenLimit )
                    if ( pb[len] != cur[len] )
                        break;
                if ( len == lenLimit )
                {
                    *ptr1 = pair[0];
                    *ptr0 = pair[1];
                    return;
                }
            }
            if ( pb[len] < cur[len] )
            {
                *ptr1 = curMatch;
                ptr1 = pair + 1;
                curMatch = *ptr1;
                len1 = len;
            }
            else
            {
                *ptr0 = curMatch;
                ptr0 = pair;
                curMatch = *ptr0;
                len0 = len;
            }
        }
    }
}

/* The four hashes of the bytes at 'cur'.  The two- and three-byte values
 * carry the second and third bytes WHOLE in their low bits, so an equal hash
 * and an equal first byte means the first two (or three) bytes are equal -
 * which is why only cur[0] is ever compared before extending those. */
#define HASH4_CALC( e, cur, h2, h3, hv ) \
    { uint32_t t = g_crc[(cur)[0]] ^ (cur)[1]; \
      h2 = t & ( kHash2Size - 1 ); \
      t ^= (uint32_t)(cur)[2] << 8; \
      h3 = t & ( kHash3Size - 1 ); \
      hv = ( t ^ ( g_crc[(cur)[3]] << 5 ) ) & (e)->hashMask; }

/* Matches at the finder's position, as (len, dist-1) pairs in increasing
 * length; advances the finder by one.  Returns the number of uint32_t values. */
static uint32_t MfGetMatches( CLzmaEnc *e, uint32_t *distances )
{
    uint32_t lenLimit = e->numFastBytes;
    uint32_t h2, h3, hv, d2, d3, curMatch, maxLen, offset;
    const uint8_t *cur;

    if ( lenLimit > MfAvail( e ) ) lenLimit = MfAvail( e );
    if ( lenLimit < 4 ) { MfMovePos( e ); return 0; }

    cur = e->bufBase + e->cur;
    HASH4_CALC( e, cur, h2, h3, hv );
    d2 = e->pos - e->hash[h2];
    d3 = e->pos - e->hash[kFix3HashSize + h3];
    curMatch = e->hash[kFix4HashSize + hv];
    e->hash[h2] = e->pos;
    e->hash[kFix3HashSize + h3] = e->pos;
    e->hash[kFix4HashSize + hv] = e->pos;

    maxLen = 1;
    offset = 0;
    if ( d2 < e->cyclicSize && *( cur - d2 ) == *cur )
    {
        distances[0] = maxLen = 2;
        distances[1] = d2 - 1;
        offset = 2;
    }
    if ( d2 != d3 && d3 < e->cyclicSize && *( cur - d3 ) == *cur )
    {
        maxLen = 3;
        distances[offset + 1] = d3 - 1;
        offset += 2;
        d2 = d3;
    }
    if ( offset != 0 )
    {
        for ( ; maxLen != lenLimit; maxLen++ )
            if ( cur[maxLen - d2] != cur[maxLen] )
                break;
        distances[offset - 2] = maxLen;
        if ( maxLen == lenLimit )
        {
            SkipMatchesSpec( e, lenLimit, curMatch );
            MfMovePos( e );
            return offset;
        }
    }
    if ( maxLen < 3 ) maxLen = 3;
    offset = (uint32_t)( GetMatchesSpec( e, lenLimit, curMatch,
                                         distances + offset, maxLen ) - distances );
    MfMovePos( e );
    return offset;
}

static void MfSkip( CLzmaEnc *e, uint32_t num )
{
    do {
        uint32_t lenLimit = e->numFastBytes;
        uint32_t h2, h3, hv, curMatch;
        const uint8_t *cur;

        if ( lenLimit > MfAvail( e ) ) lenLimit = MfAvail( e );
        if ( lenLimit < 4 ) { MfMovePos( e ); continue; }
        cur = e->bufBase + e->cur;
        HASH4_CALC( e, cur, h2, h3, hv );
        curMatch = e->hash[kFix4HashSize + hv];
        e->hash[h2] = e->pos;
        e->hash[kFix3HashSize + h3] = e->pos;
        e->hash[kFix4HashSize + hv] = e->pos;
        SkipMatchesSpec( e, lenLimit, curMatch );
        MfMovePos( e );
    } while ( --num != 0 );
}

/*---- Prices --------------------------------------------------------------- */
static void FillAlignPrices( CLzmaEnc *e )
{
    unsigned i;
    for ( i = 0; i < kAlignTableSize; i++ )
        e->alignPrices[i] = RcTreeReverseGetPrice( e->posAlignEncoder, kNumAlignBits, i );
    e->alignPriceCount = 0;
}

static void FillDistancesPrices( CLzmaEnc *e )
{
    uint32_t tempPrices[kNumFullDistances];
    unsigned i, lenToPosState;

    for ( i = kStartPosModelIndex; i < kNumFullDistances; i++ )
    {
        uint32_t posSlot = PosSlot( i );
        uint32_t footerBits = ( posSlot >> 1 ) - 1;
        uint32_t base = ( 2 | ( posSlot & 1 ) ) << footerBits;
        tempPrices[i] = RcTreeReverseGetPrice( e->posEncoders + base - posSlot,
                                               (unsigned)footerBits, i - base );
    }

    for ( lenToPosState = 0; lenToPosState < kNumLenToPosStates; lenToPosState++ )
    {
        unsigned  posSlot;
        const CProb *encoder = e->posSlotEncoder[lenToPosState];
        uint32_t *posSlotPrices = e->posSlotPrices[lenToPosState];
        uint32_t *distancesPrices = e->distancesPrices[lenToPosState];

        for ( posSlot = 0; posSlot < e->distTableSize; posSlot++ )
            posSlotPrices[posSlot] = RcTreeGetPrice( encoder, kNumPosSlotBits, posSlot );
        for ( posSlot = kEndPosModelIndex; posSlot < e->distTableSize; posSlot++ )
            posSlotPrices[posSlot] +=
                ( ( ( ( posSlot >> 1 ) - 1 ) - kNumAlignBits ) << kNumBitPriceShiftBits );

        for ( i = 0; i < kStartPosModelIndex; i++ )
            distancesPrices[i] = posSlotPrices[i];
        for ( ; i < kNumFullDistances; i++ )
            distancesPrices[i] = posSlotPrices[ PosSlot( i ) ] + tempPrices[i];
    }
    e->matchPriceCount = 0;
}

static uint32_t GetRepLen1Price( CLzmaEnc *e, uint32_t state, uint32_t posState )
{
    return GET_PRICE_0( e->isRepG0[state] ) + GET_PRICE_0( e->isRep0Long[state][posState] );
}

static uint32_t GetPureRepPrice( CLzmaEnc *e, uint32_t repIndex, uint32_t state, uint32_t posState )
{
    uint32_t price;
    if ( repIndex == 0 )
    {
        price  = GET_PRICE_0( e->isRepG0[state] );
        price += GET_PRICE_1( e->isRep0Long[state][posState] );
    }
    else
    {
        price = GET_PRICE_1( e->isRepG0[state] );
        if ( repIndex == 1 )
            price += GET_PRICE_0( e->isRepG1[state] );
        else
        {
            price += GET_PRICE_1( e->isRepG1[state] );
            price += GET_PRICE( e->isRepG2[state], repIndex - 2 );
        }
    }
    return price;
}

static uint32_t GetRepPrice( CLzmaEnc *e, uint32_t repIndex, uint32_t len, uint32_t state, uint32_t posState )
{
    return e->repLenEnc.prices[posState][len - kMatchLenMin] +
           GetPureRepPrice( e, repIndex, state, posState );
}

/*---- The parse ------------------------------------------------------------ */
static uint32_t ReadMatchDistances( CLzmaEnc *e, uint32_t *numPairsRes )
{
    uint32_t lenRes = 0, numPairs;

    e->numAvail = MfAvail( e );
    numPairs = MfGetMatches( e, e->matches );
    if ( numPairs > 0 )
    {
        lenRes = e->matches[numPairs - 2];
        if ( lenRes == e->numFastBytes )
        {
            /* The finder stops at fastBytes; carry on by hand to the real
             * end of the match, up to the format's 273. */
            const uint8_t *pby = e->bufBase + e->cur - 1;
            uint32_t distance = e->matches[numPairs - 1] + 1;
            uint32_t numAvail = e->numAvail;
            const uint8_t *pby2;
            if ( numAvail > kMatchLenMax ) numAvail = kMatchLenMax;
            pby2 = pby - distance;
            for ( ; lenRes < numAvail && pby[lenRes] == pby2[lenRes]; lenRes++ ) ;
        }
    }
    e->additionalOffset++;
    *numPairsRes = numPairs;
    return lenRes;
}

static void MovePos( CLzmaEnc *e, uint32_t num )
{
    if ( num != 0 )
    {
        e->additionalOffset += num;
        MfSkip( e, num );
    }
}

static uint32_t Backward( CLzmaEnc *e, uint32_t *backRes, uint32_t cur )
{
    COptimal *opt = e->opt;
    uint32_t posMem  = opt[cur].posPrev;
    uint32_t backMem = opt[cur].backPrev;

    e->optimumEndIndex = cur;
    do {
        if ( opt[cur].prev1IsChar )
        {
            MakeAsChar( &opt[posMem] );
            opt[posMem].posPrev = posMem - 1;
            if ( opt[cur].prev2 )
            {
                opt[posMem - 1].prev1IsChar = 0;
                opt[posMem - 1].posPrev  = opt[cur].posPrev2;
                opt[posMem - 1].backPrev = opt[cur].backPrev2;
            }
        }
        {
            uint32_t posPrev = posMem;
            uint32_t backCur = backMem;

            backMem = opt[posPrev].backPrev;
            posMem  = opt[posPrev].posPrev;
            opt[posPrev].backPrev = backCur;
            opt[posPrev].posPrev  = cur;
            cur = posPrev;
        }
    } while ( cur != 0 );

    *backRes = opt[0].backPrev;
    e->optimumCurrentIndex = opt[0].posPrev;
    return e->optimumCurrentIndex;
}

static uint32_t GetOptimum( CLzmaEnc *e, uint32_t position, uint32_t *backRes )
{
    COptimal      *opt = e->opt;
    uint32_t       numAvail, mainLen, numPairs, repMaxIndex, i, posState;
    uint32_t       matchPrice, repMatchPrice, normalMatchPrice;
    uint32_t       reps[kNumReps], repLens[kNumReps];
    uint32_t       lenEnd, len, cur;
    uint32_t      *matches;
    const uint8_t *data;
    uint8_t        curByte, matchByte;

    if ( e->optimumEndIndex != e->optimumCurrentIndex )
    {
        const COptimal *o = &opt[e->optimumCurrentIndex];
        uint32_t lenRes = o->posPrev - e->optimumCurrentIndex;
        *backRes = o->backPrev;
        e->optimumCurrentIndex = o->posPrev;
        return lenRes;
    }
    e->optimumCurrentIndex = e->optimumEndIndex = 0;

    if ( e->additionalOffset == 0 )
        mainLen = ReadMatchDistances( e, &numPairs );
    else
    {
        mainLen  = e->longestMatchLength;
        numPairs = e->numPairs;
    }

    numAvail = e->numAvail;
    if ( numAvail < 2 )
    {
        *backRes = (uint32_t)-1;
        return 1;
    }
    if ( numAvail > kMatchLenMax ) numAvail = kMatchLenMax;

    data = e->bufBase + e->cur - 1;
    repMaxIndex = 0;
    for ( i = 0; i < kNumReps; i++ )
    {
        unsigned lenTest;
        const uint8_t *data2;
        reps[i] = e->reps[i];
        data2 = data - ( reps[i] + 1 );
        if ( data[0] != data2[0] || data[1] != data2[1] )
        {
            repLens[i] = 0;
            continue;
        }
        for ( lenTest = 2; lenTest < numAvail && data[lenTest] == data2[lenTest]; lenTest++ ) ;
        repLens[i] = lenTest;
        if ( lenTest > repLens[repMaxIndex] ) repMaxIndex = i;
    }
    if ( repLens[repMaxIndex] >= e->numFastBytes )
    {
        uint32_t lenRes;
        *backRes = repMaxIndex;
        lenRes = repLens[repMaxIndex];
        MovePos( e, lenRes - 1 );
        return lenRes;
    }

    matches = e->matches;
    if ( mainLen >= e->numFastBytes )
    {
        *backRes = matches[numPairs - 1] + kNumReps;
        MovePos( e, mainLen - 1 );
        return mainLen;
    }
    curByte = *data;
    matchByte = *( data - ( reps[0] + 1 ) );

    if ( mainLen < 2 && curByte != matchByte && repLens[repMaxIndex] < 2 )
    {
        *backRes = (uint32_t)-1;
        return 1;
    }

    opt[0].state = (uint8_t)e->state;
    posState = position & e->pbMask;

    {
        const CProb *probs = LIT_PROBS( e, position, *( data - 1 ) );
        opt[1].price = GET_PRICE_0( e->isMatch[e->state][posState] ) +
            ( !IsCharState( e->state )
              ? LitGetPriceMatched( probs, curByte, matchByte )
              : LitGetPrice( probs, curByte ) );
    }
    MakeAsChar( &opt[1] );

    matchPrice    = GET_PRICE_1( e->isMatch[e->state][posState] );
    repMatchPrice = matchPrice + GET_PRICE_1( e->isRep[e->state] );

    if ( matchByte == curByte )
    {
        uint32_t shortRepPrice = repMatchPrice + GetRepLen1Price( e, e->state, posState );
        if ( shortRepPrice < opt[1].price )
        {
            opt[1].price = shortRepPrice;
            MakeAsShortRep( &opt[1] );
        }
    }
    lenEnd = ( mainLen >= repLens[repMaxIndex] ) ? mainLen : repLens[repMaxIndex];

    if ( lenEnd < 2 )
    {
        *backRes = opt[1].backPrev;
        return 1;
    }

    opt[1].posPrev = 0;
    for ( i = 0; i < kNumReps; i++ )
        opt[0].backs[i] = reps[i];

    len = lenEnd;
    do {
        opt[len--].price = kInfinityPrice;
    } while ( len >= 2 );

    for ( i = 0; i < kNumReps; i++ )
    {
        uint32_t repLen = repLens[i];
        uint32_t price;
        if ( repLen < 2 ) continue;
        price = repMatchPrice + GetPureRepPrice( e, i, e->state, posState );
        do {
            uint32_t curAndLenPrice = price + e->repLenEnc.prices[posState][repLen - 2];
            COptimal *o = &opt[repLen];
            if ( curAndLenPrice < o->price )
            {
                o->price = curAndLenPrice;
                o->posPrev = 0;
                o->backPrev = i;
                o->prev1IsChar = 0;
            }
        } while ( --repLen >= 2 );
    }

    normalMatchPrice = matchPrice + GET_PRICE_0( e->isRep[e->state] );

    len = ( repLens[0] >= 2 ) ? repLens[0] + 1 : 2;
    if ( len <= mainLen )
    {
        uint32_t offs = 0;
        while ( len > matches[offs] ) offs += 2;
        for ( ;; len++ )
        {
            COptimal *o;
            uint32_t distance = matches[offs + 1];
            uint32_t curAndLenPrice = normalMatchPrice + e->lenEnc.prices[posState][len - kMatchLenMin];
            uint32_t lenToPosState = GetLenToPosState( len );
            if ( distance < kNumFullDistances )
                curAndLenPrice += e->distancesPrices[lenToPosState][distance];
            else
                curAndLenPrice += e->alignPrices[distance & kAlignMask] +
                                  e->posSlotPrices[lenToPosState][ PosSlot( distance ) ];
            o = &opt[len];
            if ( curAndLenPrice < o->price )
            {
                o->price = curAndLenPrice;
                o->posPrev = 0;
                o->backPrev = distance + kNumReps;
                o->prev1IsChar = 0;
            }
            if ( len == matches[offs] )
            {
                offs += 2;
                if ( offs == numPairs ) break;
            }
        }
    }

    cur = 0;

    for ( ;; )
    {
        uint32_t numAvailFull, newLen, posPrev, state, startLen;
        uint32_t curPrice, curAnd1Price, posStateCur;
        int      nextIsChar;
        COptimal *curOpt, *nextOpt;

        cur++;
        if ( cur == lenEnd )
            return Backward( e, backRes, cur );

        newLen = ReadMatchDistances( e, &numPairs );
        if ( newLen >= e->numFastBytes )
        {
            e->numPairs = numPairs;
            e->longestMatchLength = newLen;
            return Backward( e, backRes, cur );
        }
        position++;
        curOpt = &opt[cur];
        posPrev = curOpt->posPrev;
        if ( curOpt->prev1IsChar )
        {
            posPrev--;
            if ( curOpt->prev2 )
            {
                state = opt[curOpt->posPrev2].state;
                if ( curOpt->backPrev2 < kNumReps )
                    state = kRepNextStates[state];
                else
                    state = kMatchNextStates[state];
            }
            else
                state = opt[posPrev].state;
            state = kLiteralNextStates[state];
        }
        else
            state = opt[posPrev].state;

        if ( posPrev == cur - 1 )
        {
            if ( IsShortRep( curOpt ) )
                state = kShortRepNextStates[state];
            else
                state = kLiteralNextStates[state];
        }
        else
        {
            uint32_t pos;
            const COptimal *prevOpt;
            if ( curOpt->prev1IsChar && curOpt->prev2 )
            {
                posPrev = curOpt->posPrev2;
                pos = curOpt->backPrev2;
                state = kRepNextStates[state];
            }
            else
            {
                pos = curOpt->backPrev;
                if ( pos < kNumReps )
                    state = kRepNextStates[state];
                else
                    state = kMatchNextStates[state];
            }
            prevOpt = &opt[posPrev];
            if ( pos < kNumReps )
            {
                unsigned k;
                reps[0] = prevOpt->backs[pos];
                for ( k = 1; k <= pos; k++ )
                    reps[k] = prevOpt->backs[k - 1];
                for ( ; k < kNumReps; k++ )
                    reps[k] = prevOpt->backs[k];
            }
            else
            {
                unsigned k;
                reps[0] = pos - kNumReps;
                for ( k = 1; k < kNumReps; k++ )
                    reps[k] = prevOpt->backs[k - 1];
            }
        }
        curOpt->state = (uint8_t)state;
        for ( i = 0; i < kNumReps; i++ )
            curOpt->backs[i] = reps[i];

        curPrice = curOpt->price;
        nextIsChar = 0;
        data = e->bufBase + e->cur - 1;
        curByte = *data;
        matchByte = *( data - ( reps[0] + 1 ) );

        posStateCur = position & e->pbMask;

        {
            const CProb *probs = LIT_PROBS( e, position, *( data - 1 ) );
            curAnd1Price = curPrice + GET_PRICE_0( e->isMatch[state][posStateCur] ) +
                ( !IsCharState( state )
                  ? LitGetPriceMatched( probs, curByte, matchByte )
                  : LitGetPrice( probs, curByte ) );
        }

        nextOpt = &opt[cur + 1];

        if ( curAnd1Price < nextOpt->price )
        {
            nextOpt->price = curAnd1Price;
            nextOpt->posPrev = cur;
            MakeAsChar( nextOpt );
            nextIsChar = 1;
        }

        matchPrice    = curPrice + GET_PRICE_1( e->isMatch[state][posStateCur] );
        repMatchPrice = matchPrice + GET_PRICE_1( e->isRep[state] );

        if ( matchByte == curByte && !( nextOpt->posPrev < cur && nextOpt->backPrev == 0 ) )
        {
            uint32_t shortRepPrice = repMatchPrice + GetRepLen1Price( e, state, posStateCur );
            if ( shortRepPrice <= nextOpt->price )
            {
                nextOpt->price = shortRepPrice;
                nextOpt->posPrev = cur;
                MakeAsShortRep( nextOpt );
                nextIsChar = 1;
            }
        }

        numAvailFull = e->numAvail;
        {
            uint32_t temp = kNumOpts - 1 - cur;
            if ( temp < numAvailFull ) numAvailFull = temp;
        }
        if ( numAvailFull < 2 ) continue;
        numAvail = ( numAvailFull <= e->numFastBytes ) ? numAvailFull : e->numFastBytes;

        if ( !nextIsChar && matchByte != curByte )
        {
            /* literal, then a repeat of rep0 */
            unsigned temp, lenTest2;
            const uint8_t *data2 = data - ( reps[0] + 1 );
            uint32_t limit = e->numFastBytes + 1;
            if ( limit > numAvailFull ) limit = numAvailFull;

            for ( temp = 1; temp < limit && data[temp] == data2[temp]; temp++ ) ;
            lenTest2 = temp - 1;
            if ( lenTest2 >= 2 )
            {
                uint32_t state2 = kLiteralNextStates[state];
                uint32_t posStateNext = ( position + 1 ) & e->pbMask;
                uint32_t nextRepMatchPrice = curAnd1Price +
                    GET_PRICE_1( e->isMatch[state2][posStateNext] ) +
                    GET_PRICE_1( e->isRep[state2] );
                uint32_t curAndLenPrice;
                COptimal *o;
                uint32_t offset = cur + 1 + lenTest2;
                while ( lenEnd < offset ) opt[++lenEnd].price = kInfinityPrice;
                curAndLenPrice = nextRepMatchPrice +
                                 GetRepPrice( e, 0, lenTest2, state2, posStateNext );
                o = &opt[offset];
                if ( curAndLenPrice < o->price )
                {
                    o->price = curAndLenPrice;
                    o->posPrev = cur + 1;
                    o->backPrev = 0;
                    o->prev1IsChar = 1;
                    o->prev2 = 0;
                }
            }
        }

        startLen = 2;
        {
            unsigned repIndex;
            for ( repIndex = 0; repIndex < kNumReps; repIndex++ )
            {
                unsigned lenTest, lenTestTemp;
                uint32_t price;
                const uint8_t *data2 = data - ( reps[repIndex] + 1 );
                if ( data[0] != data2[0] || data[1] != data2[1] ) continue;
                for ( lenTest = 2; lenTest < numAvail && data[lenTest] == data2[lenTest]; lenTest++ ) ;
                while ( lenEnd < cur + lenTest ) opt[++lenEnd].price = kInfinityPrice;
                lenTestTemp = lenTest;
                price = repMatchPrice + GetPureRepPrice( e, repIndex, state, posStateCur );
                do {
                    uint32_t curAndLenPrice = price + e->repLenEnc.prices[posStateCur][lenTest - 2];
                    COptimal *o = &opt[cur + lenTest];
                    if ( curAndLenPrice < o->price )
                    {
                        o->price = curAndLenPrice;
                        o->posPrev = cur;
                        o->backPrev = repIndex;
                        o->prev1IsChar = 0;
                    }
                } while ( --lenTest >= 2 );
                lenTest = lenTestTemp;

                if ( repIndex == 0 ) startLen = lenTest + 1;

                /* repeat, literal, repeat of rep0 */
                {
                    uint32_t lenTest2 = lenTest + 1;
                    uint32_t limit = lenTest2 + e->numFastBytes;
                    if ( limit > numAvailFull ) limit = numAvailFull;
                    for ( ; lenTest2 < limit && data[lenTest2] == data2[lenTest2]; lenTest2++ ) ;
                    lenTest2 -= lenTest + 1;
                    if ( lenTest2 >= 2 )
                    {
                        uint32_t nextRepMatchPrice, curAndLenCharPrice, curAndLenPrice, offset;
                        uint32_t state2 = kRepNextStates[state];
                        uint32_t posStateNext = ( position + lenTest ) & e->pbMask;
                        COptimal *o;

                        curAndLenCharPrice = price +
                            e->repLenEnc.prices[posStateCur][lenTest - 2] +
                            GET_PRICE_0( e->isMatch[state2][posStateNext] ) +
                            LitGetPriceMatched( LIT_PROBS( e, position + lenTest, data[lenTest - 1] ),
                                                data[lenTest], data2[lenTest] );
                        state2 = kLiteralNextStates[state2];
                        posStateNext = ( position + lenTest + 1 ) & e->pbMask;
                        nextRepMatchPrice = curAndLenCharPrice +
                            GET_PRICE_1( e->isMatch[state2][posStateNext] ) +
                            GET_PRICE_1( e->isRep[state2] );

                        offset = cur + lenTest + 1 + lenTest2;
                        while ( lenEnd < offset ) opt[++lenEnd].price = kInfinityPrice;
                        curAndLenPrice = nextRepMatchPrice +
                                         GetRepPrice( e, 0, lenTest2, state2, posStateNext );
                        o = &opt[offset];
                        if ( curAndLenPrice < o->price )
                        {
                            o->price = curAndLenPrice;
                            o->posPrev = cur + lenTest + 1;
                            o->backPrev = 0;
                            o->prev1IsChar = 1;
                            o->prev2 = 1;
                            o->posPrev2 = cur;
                            o->backPrev2 = repIndex;
                        }
                    }
                }
            }
        }

        if ( newLen > numAvail )
        {
            newLen = numAvail;
            for ( numPairs = 0; newLen > matches[numPairs]; numPairs += 2 ) ;
            matches[numPairs] = newLen;
            numPairs += 2;
        }
        if ( newLen >= startLen )
        {
            uint32_t offs, curBack, posSlot;
            unsigned lenTest;
            normalMatchPrice = matchPrice + GET_PRICE_0( e->isRep[state] );
            while ( lenEnd < cur + newLen ) opt[++lenEnd].price = kInfinityPrice;

            offs = 0;
            while ( startLen > matches[offs] ) offs += 2;
            curBack = matches[offs + 1];
            posSlot = PosSlot( curBack );
            for ( lenTest = startLen; ; lenTest++ )
            {
                uint32_t curAndLenPrice = normalMatchPrice +
                                        e->lenEnc.prices[posStateCur][lenTest - kMatchLenMin];
                uint32_t lenToPosState = GetLenToPosState( lenTest );
                COptimal *o;
                if ( curBack < kNumFullDistances )
                    curAndLenPrice += e->distancesPrices[lenToPosState][curBack];
                else
                    curAndLenPrice += e->posSlotPrices[lenToPosState][posSlot] +
                                      e->alignPrices[curBack & kAlignMask];

                o = &opt[cur + lenTest];
                if ( curAndLenPrice < o->price )
                {
                    o->price = curAndLenPrice;
                    o->posPrev = cur;
                    o->backPrev = curBack + kNumReps;
                    o->prev1IsChar = 0;
                }

                if ( lenTest == matches[offs] )
                {
                    /* match, literal, repeat of rep0 */
                    const uint8_t *data2 = data - ( curBack + 1 );
                    uint32_t lenTest2 = lenTest + 1;
                    uint32_t limit = lenTest2 + e->numFastBytes;
                    if ( limit > numAvailFull ) limit = numAvailFull;
                    for ( ; lenTest2 < limit && data[lenTest2] == data2[lenTest2]; lenTest2++ ) ;
                    lenTest2 -= lenTest + 1;
                    if ( lenTest2 >= 2 )
                    {
                        uint32_t nextRepMatchPrice, curAndLenCharPrice, offset;
                        uint32_t state2 = kMatchNextStates[state];
                        uint32_t posStateNext = ( position + lenTest ) & e->pbMask;
                        COptimal *o2;

                        curAndLenCharPrice = curAndLenPrice +
                            GET_PRICE_0( e->isMatch[state2][posStateNext] ) +
                            LitGetPriceMatched( LIT_PROBS( e, position + lenTest, data[lenTest - 1] ),
                                                data[lenTest], data2[lenTest] );
                        state2 = kLiteralNextStates[state2];
                        posStateNext = ( posStateNext + 1 ) & e->pbMask;
                        nextRepMatchPrice = curAndLenCharPrice +
                            GET_PRICE_1( e->isMatch[state2][posStateNext] ) +
                            GET_PRICE_1( e->isRep[state2] );

                        offset = cur + lenTest + 1 + lenTest2;
                        while ( lenEnd < offset ) opt[++lenEnd].price = kInfinityPrice;
                        curAndLenPrice = nextRepMatchPrice +
                                         GetRepPrice( e, 0, lenTest2, state2, posStateNext );
                        o2 = &opt[offset];
                        if ( curAndLenPrice < o2->price )
                        {
                            o2->price = curAndLenPrice;
                            o2->posPrev = cur + lenTest + 1;
                            o2->backPrev = 0;
                            o2->prev1IsChar = 1;
                            o2->prev2 = 1;
                            o2->posPrev2 = cur;
                            o2->backPrev2 = curBack + kNumReps;
                        }
                    }
                    offs += 2;
                    if ( offs == numPairs ) break;
                    curBack = matches[offs + 1];
                    if ( curBack >= kNumFullDistances )
                        posSlot = PosSlot( curBack );
                }
            }
        }
    }
}

/*---- Initialisation ------------------------------------------------------- */
static void EncInitModel( CLzmaEnc *e )
{
    unsigned i, j, n;

    e->state = 0;
    for ( i = 0; i < kNumReps; i++ ) e->reps[i] = 0;

    n = (uint32_t)0x300 << ( e->lc + e->lp );
    for ( i = 0; i < n; i++ ) e->litProbs[i] = kProbInitValue;

    for ( i = 0; i < kNumStates; i++ )
    {
        for ( j = 0; j < kNumPosStatesMax; j++ )
        {
            e->isMatch[i][j] = kProbInitValue;
            e->isRep0Long[i][j] = kProbInitValue;
        }
        e->isRep[i] = kProbInitValue;
        e->isRepG0[i] = kProbInitValue;
        e->isRepG1[i] = kProbInitValue;
        e->isRepG2[i] = kProbInitValue;
    }
    for ( i = 0; i < kNumLenToPosStates; i++ )
        for ( j = 0; j < ( 1 << kNumPosSlotBits ); j++ )
            e->posSlotEncoder[i][j] = kProbInitValue;
    for ( i = 0; i < kNumPosEncoders; i++ )
        e->posEncoders[i] = kProbInitValue;
    for ( i = 0; i < ( 1 << kNumAlignBits ); i++ )
        e->posAlignEncoder[i] = kProbInitValue;

    LenEncInit( &e->lenEnc.p );
    LenEncInit( &e->repLenEnc.p );

    e->optimumEndIndex = 0;
    e->optimumCurrentIndex = 0;
    e->additionalOffset = 0;

    e->pbMask = ( (uint32_t)1 << e->pb ) - 1;
    e->lpMask = ( (uint32_t)1 << e->lp ) - 1;

    for ( i = 0; i < 32; i++ )
        if ( e->dictSize <= ( (uint32_t)1 << i ) ) break;
    e->distTableSize = i * 2;

    FillDistancesPrices( e );
    FillAlignPrices( e );
    e->lenEnc.tableSize = e->repLenEnc.tableSize = e->numFastBytes + 1 - kMatchLenMin;
    LenPriceUpdateTables( &e->lenEnc, (uint32_t)1 << e->pb );
    LenPriceUpdateTables( &e->repLenEnc, (uint32_t)1 << e->pb );
}

/* The hash and window sizes for a dictionary - in one place, because
 * LzmaEncMemNeeded has to agree with what is really allocated. */
static uint32_t HashMaskFor( uint32_t dictSize )
{
    uint32_t hs = dictSize - 1;
    hs |= hs >> 1;
    hs |= hs >> 2;
    hs |= hs >> 4;
    hs |= hs >> 8;
    hs |= hs >> 16;
    hs >>= 1;
    hs |= 0xFFFF;
    if ( hs > ( (uint32_t)1 << 24 ) ) hs >>= 1;
    return hs;
}

static uint32_t KeepBeforeFor( uint32_t dictSize ) { return dictSize + kNumOpts + 1; }
static uint32_t KeepAfterFor( uint32_t fastBytes ) { return fastBytes + kMatchLenMax + 1; }
static uint32_t ReserveFor( uint32_t dictSize )    { return dictSize / 2 + 65536UL; }

uint32_t LzmaEncMemNeeded( uint32_t dictSize )
{
    uint32_t hashCount = HashMaskFor( dictSize ) + 1 + kFix4HashSize;
    uint32_t window = KeepBeforeFor( dictSize ) + KeepAfterFor( kMatchLenMax ) + ReserveFor( dictSize );

    /* The literal probabilities twice over: LZMA2 keeps a copy (CSaved). */
    return (uint32_t)sizeof( CLzmaEnc ) + window + hashCount * 4
         + ( dictSize + 1 ) * 8 + (uint32_t)kNumOpts * sizeof( COptimal )
         + ( (uint32_t)0x300 << 3 ) * sizeof( CProb ) * 2 + OUT_BUFSIZE
         + (uint32_t)sizeof( CSaved );
}

uint8_t Lzma2EncPropByte( const LzmaEncProps *p )
{
    unsigned i;

    /* Property i names (2 + (i & 1)) << (i / 2 + 11) bytes. */
    for ( i = 0; i < 40; i++ )
        if ( p->dictSize <= ( (uint32_t)( 2 | ( i & 1 ) ) << ( i / 2 + 11 ) ) ) break;
    return (uint8_t)i;
}

void LzmaEncPropsInit( LzmaEncProps *p, uint32_t dictSize )
{
    p->dictSize  = dictSize;
    p->lc        = 3;
    p->lp        = 0;
    p->pb        = 2;
    p->fastBytes = 64;
    p->cutValue  = 48;
}

void LzmaEncPropsBytes( const LzmaEncProps *p, uint8_t out[5] )
{
    uint32_t d = p->dictSize;
    out[0] = (uint8_t)( ( p->pb * 5 + p->lp ) * 9 + p->lc );
    out[1] = (uint8_t)d;
    out[2] = (uint8_t)( d >> 8 );
    out[3] = (uint8_t)( d >> 16 );
    out[4] = (uint8_t)( d >> 24 );
}

static void EncFree( CLzmaEnc *e )
{
    if ( !e ) return;
    free( e->bufBase );
    free( e->hash );
    free( e->son );
    free( e->opt );
    free( e->litProbs );
    free( e->rc.buf );
    if ( e->saved ) free( e->saved->litProbs );
    free( e->saved );
    free( e );
}

/*---- Coding one decision -------------------------------------------------- *
 * Returns 0 when the decision does not match the data - see the note at the
 * top of the file. */
static int CodeOne( CLzmaEnc *e, uint32_t len, uint32_t back, uint32_t nowPos )
{
    uint32_t posState = nowPos & e->pbMask;
    CRangeEnc *rc = &e->rc;
    const uint8_t *data = e->bufBase + e->cur - e->additionalOffset;

    if ( len == 1 && back == (uint32_t)-1 )
    {
        CProb *probs;
        RcEncodeBit( rc, &e->isMatch[e->state][posState], 0 );
        probs = LIT_PROBS( e, nowPos, *( data - 1 ) );
        if ( IsCharState( e->state ) )
            LitEncode( rc, probs, *data );
        else
            LitEncodeMatched( rc, probs, *data, *( data - e->reps[0] - 1 ) );
        e->state = kLiteralNextStates[e->state];
        return 1;
    }

    {
        uint32_t dist = ( back < kNumReps ) ? e->reps[back] : back - kNumReps;
        const uint8_t *src = data - dist - 1;
        unsigned k;

        if ( dist + 1 > nowPos ) return 0;
        for ( k = 0; k < len; k++ )
            if ( src[k] != data[k] ) return 0;
    }

    RcEncodeBit( rc, &e->isMatch[e->state][posState], 1 );
    if ( back < kNumReps )
    {
        RcEncodeBit( rc, &e->isRep[e->state], 1 );
        if ( back == 0 )
        {
            RcEncodeBit( rc, &e->isRepG0[e->state], 0 );
            RcEncodeBit( rc, &e->isRep0Long[e->state][posState], ( len == 1 ) ? 0 : 1 );
        }
        else
        {
            uint32_t distance = e->reps[back];
            RcEncodeBit( rc, &e->isRepG0[e->state], 1 );
            if ( back == 1 )
                RcEncodeBit( rc, &e->isRepG1[e->state], 0 );
            else
            {
                RcEncodeBit( rc, &e->isRepG1[e->state], 1 );
                RcEncodeBit( rc, &e->isRepG2[e->state], back - 2 );
                if ( back == 3 ) e->reps[3] = e->reps[2];
                e->reps[2] = e->reps[1];
            }
            e->reps[1] = e->reps[0];
            e->reps[0] = distance;
        }
        if ( len == 1 )
            e->state = kShortRepNextStates[e->state];
        else
        {
            LenEncEncode2( &e->repLenEnc, rc, len - kMatchLenMin, posState );
            e->state = kRepNextStates[e->state];
        }
    }
    else
    {
        uint32_t posSlot, dist = back - kNumReps;

        RcEncodeBit( rc, &e->isRep[e->state], 0 );
        e->state = kMatchNextStates[e->state];
        LenEncEncode2( &e->lenEnc, rc, len - kMatchLenMin, posState );

        posSlot = PosSlot( dist );
        RcTreeEncode( rc, e->posSlotEncoder[ GetLenToPosState( len ) ], kNumPosSlotBits, posSlot );
        if ( posSlot >= kStartPosModelIndex )
        {
            uint32_t footerBits = ( posSlot >> 1 ) - 1;
            uint32_t base = ( 2 | ( posSlot & 1 ) ) << footerBits;
            uint32_t posReduced = dist - base;

            if ( posSlot < kEndPosModelIndex )
                RcTreeReverseEncode( rc, e->posEncoders + base - posSlot,
                                     (unsigned)footerBits, posReduced );
            else
            {
                RcEncodeDirectBits( rc, posReduced >> kNumAlignBits,
                                    (unsigned)( footerBits - kNumAlignBits ) );
                RcTreeReverseEncode( rc, e->posAlignEncoder, kNumAlignBits,
                                     posReduced & kAlignMask );
                e->alignPriceCount++;
            }
        }
        e->reps[3] = e->reps[2];
        e->reps[2] = e->reps[1];
        e->reps[1] = e->reps[0];
        e->reps[0] = dist;
        e->matchPriceCount++;
    }
    return 1;
}

/*===========================================================================
 * LZMA2 framing
 *
 * The same coded steps, cut into chunks.  Each chunk is a range-coded run of
 * its own - flushed at the end, started afresh - but the model, the repeat
 * distances and the window all carry on from one chunk to the next, so the
 * cost of a chunk is its header and the few bytes of the flush.  A chunk
 * ends when its output nears 64 KB or its input nears 2 MB, preferably
 * between two parses.  A chunk that ends there and did not get smaller is
 * sent as stored bytes instead, and the model goes back to how it stood
 * when the chunk began, which is where the decoder's still is.
 *===========================================================================*/
static void SaveState( CLzmaEnc *e )
{
    CSaved *s = e->saved;

    s->state = e->state;
    memcpy( s->reps, e->reps, sizeof( s->reps ) );
    memcpy( s->litProbs, e->litProbs,
            ( (uint32_t)0x300 << ( e->lc + e->lp ) ) * sizeof( CProb ) );
    memcpy( s->isMatch, e->isMatch, sizeof( s->isMatch ) );
    memcpy( s->isRep, e->isRep, sizeof( s->isRep ) );
    memcpy( s->isRepG0, e->isRepG0, sizeof( s->isRepG0 ) );
    memcpy( s->isRepG1, e->isRepG1, sizeof( s->isRepG1 ) );
    memcpy( s->isRepG2, e->isRepG2, sizeof( s->isRepG2 ) );
    memcpy( s->isRep0Long, e->isRep0Long, sizeof( s->isRep0Long ) );
    memcpy( s->posSlotEncoder, e->posSlotEncoder, sizeof( s->posSlotEncoder ) );
    memcpy( s->posEncoders, e->posEncoders, sizeof( s->posEncoders ) );
    memcpy( s->posAlignEncoder, e->posAlignEncoder, sizeof( s->posAlignEncoder ) );
    s->lenProbs    = e->lenEnc.p;
    s->repLenProbs = e->repLenEnc.p;
}

static void RestoreState( CLzmaEnc *e )
{
    const CSaved *s = e->saved;

    e->state = s->state;
    memcpy( e->reps, s->reps, sizeof( e->reps ) );
    memcpy( e->litProbs, s->litProbs,
            ( (uint32_t)0x300 << ( e->lc + e->lp ) ) * sizeof( CProb ) );
    memcpy( e->isMatch, s->isMatch, sizeof( e->isMatch ) );
    memcpy( e->isRep, s->isRep, sizeof( e->isRep ) );
    memcpy( e->isRepG0, s->isRepG0, sizeof( e->isRepG0 ) );
    memcpy( e->isRepG1, s->isRepG1, sizeof( e->isRepG1 ) );
    memcpy( e->isRepG2, s->isRepG2, sizeof( e->isRepG2 ) );
    memcpy( e->isRep0Long, s->isRep0Long, sizeof( e->isRep0Long ) );
    memcpy( e->posSlotEncoder, s->posSlotEncoder, sizeof( e->posSlotEncoder ) );
    memcpy( e->posEncoders, s->posEncoders, sizeof( e->posEncoders ) );
    memcpy( e->posAlignEncoder, s->posAlignEncoder, sizeof( e->posAlignEncoder ) );
    e->lenEnc.p    = s->lenProbs;
    e->repLenEnc.p = s->repLenProbs;

    FillDistancesPrices( e );
    FillAlignPrices( e );
    LenPriceUpdateTables( &e->lenEnc, (uint32_t)1 << e->pb );
    LenPriceUpdateTables( &e->repLenEnc, (uint32_t)1 << e->pb );
}

static int Lzma2Out( CLzmaEnc *e, const uint8_t *data, uint32_t len )
{
    if ( !e->out( e->outUser, data, len ) ) return 0;
    e->outTotal += len;
    return 1;
}

/* Close the chunk and send it.  'canStore' is 0 when the parse still has
 * steps planned: they were worked out from the repeat distances as they are
 * now, so the model cannot be wound back under them. */
static int Lzma2Flush( CLzmaEnc *e, int canStore )
{
    CRangeEnc *rc = &e->rc;
    uint32_t   in = e->chunkIn, packed;
    uint32_t   next = e->cur - e->additionalOffset;   /* first byte not coded */
    uint8_t    hdr[6];

    if ( in == 0 ) return SZ_OK;
    RcFlush( rc );
    if ( rc->overflow ) return SZ_ERR_DATA;
    packed = rc->bufPos;

    if ( canStore && packed + 2 >= in && in <= next )
    {
        const uint8_t *src = e->bufBase + next - in;
        while ( in )
        {
            uint32_t n = ( in < LZMA2_COPY_MAX ) ? in : LZMA2_COPY_MAX;
            hdr[0] = (uint8_t)( e->dictReset ? 1 : 2 );
            hdr[1] = (uint8_t)( ( n - 1 ) >> 8 );
            hdr[2] = (uint8_t)( n - 1 );
            if ( !Lzma2Out( e, hdr, 3 ) || !Lzma2Out( e, src, n ) ) return SZ_ERR_WRITE;
            e->dictReset = 0;
            src += n;
            in  -= n;
        }
        RestoreState( e );
    }
    else
    {
        uint32_t u = in - 1, p = packed - 1;
        unsigned mode = e->dictReset ? 3 : e->needProps ? 2 : e->needState ? 1 : 0;
        uint32_t n = 5;

        hdr[0] = (uint8_t)( 0x80 | ( mode << 5 ) | ( ( u >> 16 ) & 0x1F ) );
        hdr[1] = (uint8_t)( u >> 8 );
        hdr[2] = (uint8_t)u;
        hdr[3] = (uint8_t)( p >> 8 );
        hdr[4] = (uint8_t)p;
        if ( mode >= 2 ) hdr[n++] = e->propsByte;
        if ( !Lzma2Out( e, hdr, n ) || !Lzma2Out( e, rc->buf, packed ) ) return SZ_ERR_WRITE;
        e->dictReset = e->needProps = e->needState = 0;
    }

    e->chunkIn = 0;
    RcInit( rc );
    SaveState( e );
    return SZ_OK;
}

/*---- The encode loop, for both framings ----------------------------------- */
static int EncRun( const LzmaEncProps *props,
                   LzmaEncRead read, void *readUser,
                   LzmaEncWrite write, void *writeUser,
                   LzmaEncProgress prog, void *progUser,
                   uint32_t *inTotal, uint32_t *outTotal, int lzma2 )
{
    CLzmaEnc *e;
    uint32_t  nowPos = 0, nextProgress = PROGRESS_STEP;
    int       rc = SZ_OK;

    if ( inTotal )  *inTotal = 0;
    if ( outTotal ) *outTotal = 0;
    BuildTables();

    e = (CLzmaEnc *)calloc( 1, sizeof( CLzmaEnc ) );
    if ( !e ) return SZ_ERR_MEMORY;

    e->dictSize     = props->dictSize;
    e->lc           = props->lc;
    e->lp           = props->lp;
    e->pb           = props->pb;
    e->numFastBytes = props->fastBytes;
    e->cutValue     = props->cutValue;
    if ( e->numFastBytes < 5 ) e->numFastBytes = 5;
    if ( e->numFastBytes > kMatchLenMax ) e->numFastBytes = kMatchLenMax;
    if ( e->cutValue < 1 ) e->cutValue = 1;

    e->keepBefore = KeepBeforeFor( e->dictSize );
    e->keepAfter  = KeepAfterFor( e->numFastBytes );
    e->bufSize    = e->keepBefore + e->keepAfter + ReserveFor( e->dictSize );
    e->cyclicSize = e->dictSize + 1;
    e->hashMask   = HashMaskFor( e->dictSize );
    e->hashCount  = e->hashMask + 1 + kFix4HashSize;

    e->bufBase  = (uint8_t *)malloc( e->bufSize );
    e->hash     = (uint32_t *)calloc( e->hashCount, sizeof( uint32_t ) );
    e->son      = (uint32_t *)calloc( (size_t)e->cyclicSize * 2, sizeof( uint32_t ) );
    e->opt      = (COptimal *)calloc( kNumOpts, sizeof( COptimal ) );
    e->litProbs = (CProb *)malloc( ( (uint32_t)0x300 << ( e->lc + e->lp ) ) * sizeof( CProb ) );
    e->rc.buf   = (uint8_t *)malloc( OUT_BUFSIZE );
    if ( lzma2 )
    {
        e->saved = (CSaved *)calloc( 1, sizeof( CSaved ) );
        if ( e->saved )
            e->saved->litProbs =
                (CProb *)malloc( ( (uint32_t)0x300 << ( e->lc + e->lp ) ) * sizeof( CProb ) );
    }
    if ( !e->bufBase || !e->hash || !e->son || !e->opt || !e->litProbs || !e->rc.buf ||
         ( lzma2 && ( !e->saved || !e->saved->litProbs ) ) )
    {
        EncFree( e );
        return SZ_ERR_MEMORY;
    }

    e->read     = read;
    e->readUser = readUser;
    e->lzma2    = lzma2;
    if ( lzma2 )
    {
        e->out       = write;
        e->outUser   = writeUser;
        e->dictReset = e->needProps = e->needState = 1;
        e->propsByte = (uint8_t)( ( e->pb * 5 + e->lp ) * 9 + e->lc );
    }
    else
    {
        e->rc.write = write;
        e->rc.user  = writeUser;
    }
    RcInit( &e->rc );

    /* Positions start at cyclicSize, so 0 in a hash or a link means
     * "nothing" and every real position is at least that far from it. */
    e->pos       = e->cyclicSize;
    e->cyclicPos = 0;
    e->cur       = 0;
    e->dataEnd   = 0;
    e->streamEnd = 0;
    MfReadBlock( e );

    EncInitModel( e );
    if ( lzma2 ) SaveState( e );

    if ( MfAvail( e ) > 0 )
    {
        /* The first byte has no history to match against. */
        uint32_t numPairs;
        ReadMatchDistances( e, &numPairs );
        RcEncodeBit( &e->rc, &e->isMatch[0][0], 0 );
        e->state = kLiteralNextStates[e->state];
        LitEncode( &e->rc, e->litProbs, *( e->bufBase + e->cur - e->additionalOffset ) );
        e->additionalOffset--;
        nowPos++;
        e->chunkIn = 1;

        for ( ;; )
        {
            uint32_t back, len;

            if ( e->additionalOffset == 0 && MfAvail( e ) == 0 ) break;

            len = GetOptimum( e, nowPos, &back );

            /* No room for this step in the chunk: the rest of the parse
             * goes on in the next one. */
            if ( lzma2 && ( e->chunkIn + len > LZMA2_UNPACK_MAX ||
                            RcPending( &e->rc ) + LZMA2_SYMBOL_MAX > LZMA2_PACK_MAX ) )
            {
                rc = Lzma2Flush( e, 0 );
                if ( rc != SZ_OK ) break;
            }

            if ( !CodeOne( e, len, back, nowPos ) ) { rc = SZ_ERR_DATA; break; }
            e->additionalOffset -= len;
            nowPos += len;
            e->chunkIn += len;

            if ( e->additionalOffset == 0 )
            {
                if ( e->matchPriceCount >= ( 1 << 7 ) ) FillDistancesPrices( e );
                if ( e->alignPriceCount >= kAlignTableSize ) FillAlignPrices( e );
            }

            /* Between two parses: close the chunk here if it is nearly full,
             * while it can still go as stored bytes. */
            if ( lzma2 && e->optimumCurrentIndex == e->optimumEndIndex &&
                 ( RcPending( &e->rc ) + LZMA2_RESERVE > LZMA2_PACK_MAX ||
                   e->chunkIn + kNumOpts + kMatchLenMax >= LZMA2_UNPACK_MAX ) )
            {
                rc = Lzma2Flush( e, 1 );
                if ( rc != SZ_OK ) break;
            }

            if ( e->rc.failed ) { rc = SZ_ERR_WRITE; break; }
            if ( prog && nowPos >= nextProgress )
            {
                uint32_t outDone = lzma2 ? e->outTotal + e->rc.bufPos
                                       : e->rc.written + e->rc.bufPos;
                nextProgress = nowPos + PROGRESS_STEP;
                if ( !prog( progUser, nowPos, outDone ) )
                {
                    rc = SZ_ERR_CANCEL;
                    break;
                }
            }
        }
    }

    if ( rc == SZ_OK && lzma2 )
    {
        uint8_t end = 0;
        rc = Lzma2Flush( e, e->optimumCurrentIndex == e->optimumEndIndex );
        if ( rc == SZ_OK && !Lzma2Out( e, &end, 1 ) ) rc = SZ_ERR_WRITE;
    }
    else if ( rc == SZ_OK )
    {
        RcFlush( &e->rc );
        if ( e->rc.failed ) rc = SZ_ERR_WRITE;
    }
    if ( inTotal )  *inTotal = nowPos;
    if ( outTotal ) *outTotal = lzma2 ? e->outTotal : e->rc.written;
    EncFree( e );
    return rc;
}

/*---- Public --------------------------------------------------------------- */
int LzmaEncode( const LzmaEncProps *props,
                LzmaEncRead read, void *readUser,
                LzmaEncWrite write, void *writeUser,
                LzmaEncProgress prog, void *progUser,
                uint32_t *inTotal, uint32_t *outTotal )
{
    return EncRun( props, read, readUser, write, writeUser,
                   prog, progUser, inTotal, outTotal, 0 );
}

int Lzma2Encode( const LzmaEncProps *props,
                 LzmaEncRead read, void *readUser,
                 LzmaEncWrite write, void *writeUser,
                 LzmaEncProgress prog, void *progUser,
                 uint32_t *inTotal, uint32_t *outTotal )
{
    return EncRun( props, read, readUser, write, writeUser,
                   prog, progUser, inTotal, outTotal, 1 );
}
