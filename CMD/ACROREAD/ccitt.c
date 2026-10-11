/*
 * CCITT.C - CCITTFaxDecode: the fax machine's compression, Group 3
 * (one-dimensional, or mixed) and Group 4, which is what a scanned
 * page in black and white was stored with before JBIG2.
 *
 * A row is runs, white then black then white: in one dimension each
 * run's length is a code from a table (T.4's), and in two a run's end
 * is mostly said as "under the one above, or one to the left, or
 * right".  So a row is kept as the places where the colour changes,
 * which is what the next row is decoded against, and turned into bits
 * to hand over.
 *
 * The run-length codes are written out below as the bits they are and
 * made into two tables of 8192 the first time one is wanted: thirteen
 * bits of the input pick the code and how long it was.
 */
#include "gfx.h"

static const char *const white_codes[64 + 27] = {
    "00110101", "000111", "0111", "1000", "1011", "1100", "1110", "1111", "10011", "10100",
    "00111", "01000", "001000", "000011", "110100", "110101", "101010", "101011", "0100111",
    "0001100", "0001000", "0010111", "0000011", "0000100", "0101000", "0101011", "0010011",
    "0100100", "0011000", "00000010", "00000011", "00011010", "00011011", "00010010",
    "00010011", "00010100", "00010101", "00010110", "00010111", "00101000", "00101001",
    "00101010", "00101011", "00101100", "00101101", "00000100", "00000101", "00001010",
    "00001011", "01010010", "01010011", "01010100", "01010101", "00100100", "00100101",
    "01011000", "01011001", "01011010", "01011011", "01001010", "01001011", "00110010",
    "00110011", "00110100",
    /* 64, 128, ... 1728 */
    "11011", "10010", "010111", "0110111", "00110110", "00110111", "01100100", "01100101",
    "01101000", "01100111", "011001100", "011001101", "011010010", "011010011", "011010100",
    "011010101", "011010110", "011010111", "011011000", "011011001", "011011010", "011011011",
    "010011000", "010011001", "010011010", "011000", "010011011" };

static const char *const black_codes[64 + 27] = {
    "0000110111", "010", "11", "10", "011", "0011", "0010", "00011", "000101", "000100",
    "0000100", "0000101", "0000111", "00000100", "00000111", "000011000", "0000010111",
    "0000011000", "0000001000", "00001100111", "00001101000", "00001101100", "00000110111",
    "00000101000", "00000010111", "00000011000", "000011001010", "000011001011",
    "000011001100", "000011001101", "000001101000", "000001101001", "000001101010",
    "000001101011", "000011010010", "000011010011", "000011010100", "000011010101",
    "000011010110", "000011010111", "000001101100", "000001101101", "000011011010",
    "000011011011", "000001010100", "000001010101", "000001010110", "000001010111",
    "000001100100", "000001100101", "000001010010", "000001010011", "000000100100",
    "000000110111", "000000111000", "000000100111", "000000101000", "000001011000",
    "000001011001", "000000101011", "000000101100", "000001011010", "000001100110",
    "000001100111",
    /* 64, 128, ... 1728 */
    "0000001111", "000011001000", "000011001001", "000001011011", "000000110011",
    "000000110100", "000000110101", "0000001101100", "0000001101101", "0000001001010",
    "0000001001011", "0000001001100", "0000001001101", "0000001110010", "0000001110011",
    "0000001110100", "0000001110101", "0000001110110", "0000001110111", "0000001010010",
    "0000001010011", "0000001010100", "0000001010101", "0000001011010", "0000001011011",
    "0000001100100", "0000001100101" };

/* 1792, 1856, ... 2560: the same in either colour */
static const char *const long_codes[13] = {
    "00000001000", "00000001100", "00000001101", "000000010010", "000000010011",
    "000000010100", "000000010101", "000000010110", "000000010111", "000000011100",
    "000000011101", "000000011110", "000000011111" };

#define LOOK        13

/* from the heap: static, they would be 32K of zeroes in the program's file */
static u16 *white_tab, *black_tab;
static int tabs_made;

/* what the next row's first bits say, in two dimensions */
#define TD_PASS     0
#define TD_HORIZ    1
#define TD_V0       2
#define TD_VR1      3
#define TD_VR2      4
#define TD_VR3      5
#define TD_VL1      6
#define TD_VL2      7
#define TD_VL3      8
#define TD_END      9

typedef struct {
    int  kay, columns, rows, black_is_1, byte_align, end_of_line, end_of_block;
    u32  bits;
    int  nbits, src_ended;
    int  row, next_2d, ended, started;
    s32 *ref, *cur;                 /* where the colour changes */
    u8  *out;
    u32  rowbytes;
} FAX;

static void tab_add( u16 *tab, const char *code, int run )
{
    u32 val = 0, index, span;
    int len = 0;

    for ( ; code[len]; len++ ) {
        val = (val << 1) | (u32)(code[len] - '0');
    }
    span = 1UL << (LOOK - len);
    for ( index = 0; index < span; index++ ) {
        tab[(val << (LOOK - len)) + index] = (u16)((len << 12) | run);
    }
}

static void tabs_make( void )
{
    int index;

    white_tab = (u16 *)xalloc( 2 * (1UL << LOOK) * sizeof( u16 ) );
    black_tab = white_tab + (1 << LOOK);
    mem_set( white_tab, 0, 2 * (1UL << LOOK) * sizeof( u16 ) );
    for ( index = 0; index < 64 + 27; index++ ) {
        tab_add( white_tab, white_codes[index], index < 64 ? index : (index - 63) * 64 );
        tab_add( black_tab, black_codes[index], index < 64 ? index : (index - 63) * 64 );
    }
    for ( index = 0; index < 13; index++ ) {
        tab_add( white_tab, long_codes[index], 1792 + index * 64 );
        tab_add( black_tab, long_codes[index], 1792 + index * 64 );
    }
    tabs_made = 1;
}

/* ------------------------------------------------------------------ */
/* bits                                                                */
/* ------------------------------------------------------------------ */

static u32 look( FAX *fax, IN *src, int count )
{
    int ch;

    while ( fax->nbits < count ) {
        ch = IN_GETC( src );
        if ( ch < 0 ) {
            ch = 0;
            fax->src_ended++;
        }
        fax->bits = (fax->bits << 8) | (u32)ch;
        fax->nbits += 8;
    }
    return (fax->bits >> (fax->nbits - count)) & ((1UL << count) - 1);
}

static void eat( FAX *fax, int count )
{
    fax->nbits -= count;
    if ( fax->nbits < 0 ) {
        fax->nbits = 0;
    }
}

/* a run's length in one colour: the make-up codes added up, then the
   one that ends it.  -1 if the bits are no code at all. */
static int run_length( FAX *fax, IN *src, int black )
{
    const u16 *tab = black ? black_tab : white_tab;
    int total = 0, entry, run;

    for ( ;; ) {
        entry = tab[look( fax, src, LOOK )];
        if ( entry == 0 ) {
            return -1;
        }
        eat( fax, entry >> 12 );
        run = entry & 0x0FFF;
        total += run;
        if ( run < 64 ) {
            return total;
        }
        if ( total > 65536L ) {
            return -1;
        }
    }
}

static int two_dim( FAX *fax, IN *src )
{
    u32 code = look( fax, src, 7 );

    if ( code & 0x40 ) {
        eat( fax, 1 );
        return TD_V0;
    }
    switch ( code >> 4 ) {
    case 3:
        eat( fax, 3 );
        return TD_VR1;
    case 2:
        eat( fax, 3 );
        return TD_VL1;
    case 1:
        eat( fax, 3 );
        return TD_HORIZ;
    }
    if ( (code >> 3) == 1 ) {
        eat( fax, 4 );
        return TD_PASS;
    }
    switch ( code ) {
    case 6: case 7:
        eat( fax, 6 );
        return TD_VR2;
    case 4: case 5:
        eat( fax, 6 );
        return TD_VL2;
    case 3:
        eat( fax, 7 );
        return TD_VR3;
    case 2:
        eat( fax, 7 );
        return TD_VL3;
    }
    return TD_END;
}

/* ------------------------------------------------------------------ */
/* a row                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    s32 *cur;
    int  at;                        /* the change being written */
    int  columns;
} ROWOUT;

static void add_pixels( ROWOUT *ro, s32 upto, int black )
{
    if ( upto > ro->cur[ro->at] ) {
        if ( upto > ro->columns ) {
            upto = ro->columns;
        }
        if ( (ro->at & 1) ^ black ) {
            ro->at++;
        }
        ro->cur[ro->at] = upto;
    }
}

static void add_pixels_back( ROWOUT *ro, s32 upto, int black )
{
    if ( upto > ro->cur[ro->at] ) {
        add_pixels( ro, upto, black );
    } else if ( upto < ro->cur[ro->at] ) {
        if ( upto < 0 ) {
            upto = 0;
        }
        while ( ro->at > 0 && upto <= ro->cur[ro->at - 1] ) {
            ro->at--;
        }
        ro->cur[ro->at] = upto;
    }
}

static void decode_row( FAX *fax, IN *src )
{
    ROWOUT ro;
    s32 *ref = fax->ref, *cur = fax->cur;
    s32 run1, run2;
    int black = 0, under = 0, code, shift, index;

    ro.cur = cur;
    ro.at = 0;
    ro.columns = fax->columns;
    cur[0] = 0;
    if ( !fax->next_2d ) {
        while ( cur[ro.at] < fax->columns ) {
            run1 = run_length( fax, src, black );
            if ( run1 < 0 || fax->src_ended > 4 ) {
                add_pixels( &ro, fax->columns, 0 );
                fax->ended = fax->src_ended > 4;
                eat( fax, 1 );
                break;
            }
            add_pixels( &ro, cur[ro.at] + run1, black );
            black ^= 1;
        }
    } else {
        while ( cur[ro.at] < fax->columns ) {
            code = two_dim( fax, src );
            if ( fax->src_ended > 4 ) {
                code = TD_END;
            }
            switch ( code ) {
            case TD_PASS:
                add_pixels( &ro, ref[under + 1], black );
                if ( ref[under + 1] < fax->columns ) {
                    under += 2;
                }
                break;
            case TD_HORIZ:
                run1 = run_length( fax, src, black );
                run2 = run_length( fax, src, !black );
                if ( run1 < 0 || run2 < 0 ) {
                    add_pixels( &ro, fax->columns, 0 );
                    break;
                }
                add_pixels( &ro, cur[ro.at] + run1, black );
                if ( cur[ro.at] < fax->columns ) {
                    add_pixels( &ro, cur[ro.at] + run2, black ^ 1 );
                }
                while ( ref[under] <= cur[ro.at] && ref[under] < fax->columns ) {
                    under += 2;
                }
                break;
            case TD_V0:
            case TD_VR1:
            case TD_VR2:
            case TD_VR3:
                shift = code == TD_V0 ? 0 : code - TD_VR1 + 1;
                add_pixels( &ro, ref[under] + shift, black );
                black ^= 1;
                if ( cur[ro.at] < fax->columns ) {
                    under++;
                    while ( ref[under] <= cur[ro.at] && ref[under] < fax->columns ) {
                        under += 2;
                    }
                }
                break;
            case TD_VL1:
            case TD_VL2:
            case TD_VL3:
                shift = code - TD_VL1 + 1;
                add_pixels_back( &ro, ref[under] - shift, black );
                black ^= 1;
                if ( cur[ro.at] < fax->columns ) {
                    if ( under > 0 ) {
                        under--;
                    } else {
                        under++;
                    }
                    while ( ref[under] <= cur[ro.at] && ref[under] < fax->columns ) {
                        under += 2;
                    }
                }
                break;
            default:
                add_pixels( &ro, fax->columns, 0 );
                fax->ended = 1;
                break;
            }
        }
    }
    /* the row as bits: black from each even change to the odd one after */
    mem_set( fax->out, fax->black_is_1 ? 0x00 : 0xFF, fax->rowbytes );
    for ( index = 0; cur[index] < fax->columns; index += 2 ) {
        for ( run1 = cur[index]; run1 < cur[index + 1] && run1 < fax->columns; run1++ ) {
            if ( (run1 & 7) == 0 && run1 + 8 <= cur[index + 1] ) {
                fax->out[run1 >> 3] = (u8)(fax->black_is_1 ? 0xFF : 0x00);
                run1 += 7;
            } else if ( fax->black_is_1 ) {
                fax->out[run1 >> 3] |= (u8)(0x80 >> (run1 & 7));
            } else {
                fax->out[run1 >> 3] &= (u8)~(0x80 >> (run1 & 7));
            }
        }
    }
    /* and as what the next row is decoded against */
    for ( index = 0; cur[index] < fax->columns; index++ ) {
    }
    cur[index + 1] = cur[index + 2] = fax->columns;
    fax->ref = cur;
    fax->cur = ref;
}

static int fax_fill( IN *in )
{
    FAX *fax = IN_STATE( in, FAX );
    IN *src = in->src;
    u32 code;
    int got_eol = 0;

    if ( fax->ended || (fax->rows > 0 && fax->row >= fax->rows) ) {
        return 0;
    }
    if ( !fax->started ) {
        /* any zeroes and a first end-of-line, and which way the first row is */
        fax->started = 1;
        while ( (code = look( fax, src, 12 )) == 0 && fax->src_ended < 4 ) {
            eat( fax, 1 );
        }
        if ( code == 1 ) {
            eat( fax, 12 );
            fax->end_of_line = 1;
        }
        if ( fax->kay > 0 ) {
            fax->next_2d = !look( fax, src, 1 );
            eat( fax, 1 );
        }
    }
    decode_row( fax, src );
    fax->row++;

    /* what follows a row: an end-of-line, padding to a byte, the tag
       for the next row, or the end of everything */
    if ( !fax->end_of_block && fax->rows > 0 && fax->row >= fax->rows ) {
        fax->ended = 1;
    } else if ( !fax->ended && (fax->end_of_line || !fax->byte_align) && fax->kay >= 0 ) {
        code = look( fax, src, 12 );
        if ( fax->end_of_line ) {
            while ( code != 1 && fax->src_ended < 4 ) {
                eat( fax, 1 );
                code = look( fax, src, 12 );
            }
        } else {
            while ( code == 0 && fax->src_ended < 4 ) {
                eat( fax, 1 );
                code = look( fax, src, 12 );
            }
        }
        if ( code == 1 ) {
            eat( fax, 12 );
            got_eol = 1;
        }
    }
    if ( fax->byte_align && !got_eol ) {
        fax->nbits &= ~7;
    }
    if ( fax->src_ended > 4 ) {
        fax->ended = 1;
    }
    if ( !fax->ended && fax->kay > 0 ) {
        fax->next_2d = !look( fax, src, 1 );
        eat( fax, 1 );
    }
    if ( fax->end_of_block && got_eol && look( fax, src, 12 ) == 1 ) {
        fax->ended = 1;                             /* two together: the end of the page */
    }
    if ( fax->kay < 0 && !fax->ended && look( fax, src, 24 ) == 0x001001UL ) {
        fax->ended = 1;                             /* Group 4's */
    }
    in->cur = fax->out;
    in->end = fax->out + fax->rowbytes;
    return 1;
}

static void fax_done( IN *in )
{
    FAX *fax = IN_STATE( in, FAX );

    pool_unbig( fax->ref < fax->cur ? fax->ref : fax->cur );
    pool_unbig( fax->out );
}

IN *flt_ccitt( IN *src, DICT *parms )
{
    IN *in = in_new( sizeof( FAX ), fax_fill, src );
    FAX *fax = IN_STATE( in, FAX );
    s32 *lines;

    if ( !tabs_made ) {
        tabs_make();
    }
    in->done = fax_done;
    fax->kay = (int)dict_int( parms, "K", 0 );
    fax->columns = (int)dict_int( parms, "Columns", 1728 );
    fax->rows = (int)dict_int( parms, "Rows", 0 );
    fax->black_is_1 = (int)dict_int( parms, "BlackIs1", 0 );
    fax->byte_align = (int)dict_int( parms, "EncodedByteAlign", 0 );
    fax->end_of_line = (int)dict_int( parms, "EndOfLine", 0 );
    fax->end_of_block = (int)dict_int( parms, "EndOfBlock", 1 );
    if ( fax->columns < 1 || fax->columns > 32000 ) {
        in_close( in );
        return NULL;
    }
    fax->rowbytes = ((u32)fax->columns + 7) / 8;
    lines = (s32 *)pool_big( doc.page_pool, ((u32)fax->columns + 4) * 2 * sizeof( s32 ) );
    fax->ref = lines;
    fax->cur = lines + fax->columns + 4;
    fax->ref[0] = fax->ref[1] = fax->ref[2] = fax->columns;     /* above the first row: white */
    fax->out = (u8 *)pool_big( doc.page_pool, fax->rowbytes + 4 );
    fax->next_2d = fax->kay < 0;
    return in;
}
