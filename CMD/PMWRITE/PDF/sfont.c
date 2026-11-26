/*
 * SFONT.C - the font that is built in, for text whose own font is not
 * in the file.
 *
 * A PDF file need not carry the fourteen standard fonts - Helvetica,
 * Times, Courier and the rest - and may leave out any other font if it
 * gives the widths; the reader is expected to have something to draw
 * with.  PM-DOS has no fonts at all, so this is one: every character
 * as the path of a pen, on a grid of twenty units to the em, with the
 * capitals fourteen high and the small letters ten.  A pen's path can
 * be drawn at any size, slanted for an italic and drawn with a wider
 * pen for a bold, and it is small: the whole of it is about two
 * thousand bytes.
 *
 * It is drawn to the width the file gives each character, so a line
 * set in Times or in Arial is as long as it should be and ends where
 * it should, whatever the letters look like.  The widths of the
 * standard fonts themselves are in the tables at the bottom, for the
 * files that rely on a reader knowing them.
 *
 * THE LATER DRAWINGS.  PMWRITE writes with this font as well as
 * reading with it (TF.C), and a word processor wants the rest of
 * Windows' code page 1252: the cent, the pound and the euro, the
 * section and copyright signs, the fractions, double quotation marks
 * that curl.  Those are drawn only for a font that asks, with FF_MORE.
 * A PDF's own fonts do not ask, so a page is drawn exactly as it was
 * before there were any - until sfont_as_acroread is made 0 (see
 * where(), below), and then every font has them.
 */
#include "gfx.h"

#define MV      100             /* lift the pen and move */
#define AR      101             /* a quarter of an ellipse: round this corner, to there */
#define EN      102             /* the end of the character */

#define G_BULLET    95
#define G_ENDASH    96
#define G_EMDASH    97
#define G_QLEFT     98
#define G_QRIGHT    99
#define G_BOX       100
#define G_GRAVE     101         /* the accents: grave acute circumflex tilde dieresis
                                   ring cedilla caron, in that order */
#define G_ELLIPSIS  109
#define G_CHECK     110
#define G_ARROWR    111
#define G_ARROWL    112
#define G_MORE      113         /* the later drawings: more_uni[] says which is which */
#define MORE_COUNT  48
#define G_COUNT     (G_MORE + MORE_COUNT)

/* each: its width, then the pen's path.  The first 95 are ASCII from
   '!' on. */
static const int8_t strokes[] = {
    /* ! */ 6, 3,14, 3,4, MV, 3,0, 3,1, EN,
    /* " */ 8, 2,14, 2,10, MV, 6,14, 6,10, EN,
    /* # */ 12, 5,14, 3,0, MV, 9,14, 7,0, MV, 1,9, 11,9, MV, 1,5, 11,5, EN,
    /* $ */ 12, 10,10, AR, 10,13, 6,13, AR, 2,13, 2,10, AR, 2,7, 6,7, AR, 10,7, 10,4,
            AR, 10,1, 6,1, AR, 2,1, 2,4, MV, 6,15, 6,-1, EN,
    /* % */ 18, 4,14, AR, 7,14, 7,11, AR, 7,8, 4,8, AR, 1,8, 1,11, AR, 1,14, 4,14, MV, 13,14, 5,0,
            MV, 14,6, AR, 17,6, 17,3, AR, 17,0, 14,0, AR, 11,0, 11,3, AR, 11,6, 14,6, EN,
    /* & */ 14, 13,0, 4,10, AR, 4,14, 7,14, AR, 10,14, 10,11, 2,4, AR, 2,0, 6,0, AR, 12,0, 12,5, EN,
    /* ' */ 4, 2,14, 2,10, EN,
    /* ( */ 7, 5,15, AR, 2,15, 2,6, AR, 2,-3, 5,-3, EN,
    /* ) */ 7, 2,15, AR, 5,15, 5,6, AR, 5,-3, 2,-3, EN,
    /* * */ 10, 5,14, 5,8, MV, 2,13, 8,9, MV, 8,13, 2,9, EN,
    /* + */ 12, 6,11, 6,3, MV, 2,7, 10,7, EN,
    /* , */ 6, 3,1, 3,0, 2,-3, EN,
    /* - */ 7, 1,5, 6,5, EN,
    /* . */ 6, 3,0, 3,1, EN,
    /* / */ 6, 5,15, 1,-2, EN,
    /* 0 */ 12, 6,14, AR, 10,14, 10,7, AR, 10,0, 6,0, AR, 2,0, 2,7, AR, 2,14, 6,14, EN,
    /* 1 */ 12, 4,11, 7,14, 7,0, EN,
    /* 2 */ 12, 2,11, AR, 2,14, 6,14, AR, 10,14, 10,10, 2,0, 10,0, EN,
    /* 3 */ 12, 2,11, AR, 2,14, 6,14, AR, 10,14, 10,11, AR, 10,8, 6,8, 5,8, MV, 6,8,
            AR, 10,8, 10,4, AR, 10,0, 6,0, AR, 2,0, 2,3, EN,
    /* 4 */ 12, 8,0, 8,14, 1,4, 11,4, EN,
    /* 5 */ 12, 10,14, 3,14, 3,8, 6,8, AR, 10,8, 10,4, AR, 10,0, 6,0, AR, 2,0, 2,3, EN,
    /* 6 */ 12, 10,11, AR, 10,14, 6,14, AR, 2,14, 2,7, 2,4, AR, 2,0, 6,0, AR, 10,0, 10,4,
            AR, 10,8, 6,8, AR, 2,8, 2,4, EN,
    /* 7 */ 12, 2,14, 10,14, 4,0, EN,
    /* 8 */ 12, 6,14, AR, 9,14, 9,11, AR, 9,8, 6,8, AR, 3,8, 3,11, AR, 3,14, 6,14, MV, 6,8,
            AR, 10,8, 10,4, AR, 10,0, 6,0, AR, 2,0, 2,4, AR, 2,8, 6,8, EN,
    /* 9 */ 12, 2,3, AR, 2,0, 6,0, AR, 10,0, 10,7, 10,10, AR, 10,14, 6,14, AR, 2,14, 2,10,
            AR, 2,6, 6,6, AR, 10,6, 10,10, EN,
    /* : */ 6, 3,0, 3,1, MV, 3,9, 3,10, EN,
    /* ; */ 6, 3,1, 3,0, 2,-3, MV, 3,9, 3,10, EN,
    /* < */ 12, 10,12, 2,7, 10,2, EN,
    /* = */ 12, 2,9, 10,9, MV, 2,5, 10,5, EN,
    /* > */ 12, 2,12, 10,7, 2,2, EN,
    /* ? */ 11, 2,11, AR, 2,14, 6,14, AR, 10,14, 10,11, 6,7, 6,4, MV, 6,0, 6,1, EN,
    /* @ */ 20, 13,7, AR, 13,10, 10,10, AR, 7,10, 7,7, AR, 7,4, 10,4, AR, 13,4, 13,7, MV, 13,10, 13,5,
            AR, 13,4, 15,4, AR, 18,4, 18,8, AR, 18,15, 10,15, AR, 2,15, 2,7, AR, 2,-1, 10,-1,
            15,-1, EN,
    /* A */ 14, 1,0, 7,14, 13,0, MV, 3,5, 11,5, EN,
    /* B */ 15, 2,0, 2,14, 8,14, AR, 11,14, 11,11, AR, 11,8, 8,8, 2,8, MV, 8,8, 9,8,
            AR, 13,8, 13,4, AR, 13,0, 9,0, 2,0, EN,
    /* C */ 15, 13,10, AR, 13,14, 8,14, AR, 2,14, 2,7, AR, 2,0, 8,0, AR, 13,0, 13,4, EN,
    /* D */ 15, 2,0, 2,14, 7,14, AR, 13,14, 13,7, AR, 13,0, 7,0, 2,0, EN,
    /* E */ 13, 11,14, 2,14, 2,0, 11,0, MV, 2,7, 10,7, EN,
    /* F */ 12, 11,14, 2,14, 2,0, MV, 2,7, 9,7, EN,
    /* G */ 16, 14,10, AR, 14,14, 8,14, AR, 2,14, 2,7, AR, 2,0, 8,0, AR, 14,0, 14,5, 14,7, 9,7, EN,
    /* H */ 15, 2,0, 2,14, MV, 13,0, 13,14, MV, 2,7, 13,7, EN,
    /* I */ 6, 3,0, 3,14, EN,
    /* J */ 10, 8,14, 8,4, AR, 8,0, 5,0, AR, 2,0, 2,4, EN,
    /* K */ 14, 2,0, 2,14, MV, 12,14, 2,5, MV, 5,8, 13,0, EN,
    /* L */ 12, 2,14, 2,0, 11,0, EN,
    /* M */ 18, 2,0, 2,14, 9,2, 16,14, 16,0, EN,
    /* N */ 15, 2,0, 2,14, 13,0, 13,14, EN,
    /* O */ 16, 8,14, AR, 14,14, 14,7, AR, 14,0, 8,0, AR, 2,0, 2,7, AR, 2,14, 8,14, EN,
    /* P */ 14, 2,0, 2,14, 8,14, AR, 12,14, 12,10, AR, 12,6, 8,6, 2,6, EN,
    /* Q */ 16, 8,14, AR, 14,14, 14,7, AR, 14,0, 8,0, AR, 2,0, 2,7, AR, 2,14, 8,14, MV, 10,4, 15,-1, EN,
    /* R */ 14, 2,0, 2,14, 8,14, AR, 12,14, 12,10, AR, 12,6, 8,6, 2,6, MV, 8,6, 13,0, EN,
    /* S */ 14, 12,11, AR, 12,14, 7,14, AR, 2,14, 2,11, AR, 2,8, 7,8, AR, 12,8, 12,4,
            AR, 12,0, 7,0, AR, 2,0, 2,3, EN,
    /* T */ 12, 1,14, 11,14, MV, 6,0, 6,14, EN,
    /* U */ 14, 2,14, 2,5, AR, 2,0, 7,0, AR, 12,0, 12,5, 12,14, EN,
    /* V */ 14, 1,14, 7,0, 13,14, EN,
    /* W */ 20, 1,14, 5,0, 10,12, 15,0, 19,14, EN,
    /* X */ 14, 2,14, 12,0, MV, 12,14, 2,0, EN,
    /* Y */ 14, 1,14, 7,6, 13,14, MV, 7,6, 7,0, EN,
    /* Z */ 13, 2,14, 11,14, 2,0, 11,0, EN,
    /* [ */ 7, 6,15, 2,15, 2,-3, 6,-3, EN,
    /* \ */ 6, 1,15, 5,-2, EN,
    /* ] */ 7, 1,15, 5,15, 5,-3, 1,-3, EN,
    /* ^ */ 10, 2,8, 5,14, 8,8, EN,
    /* _ */ 11, 0,-3, 11,-3, EN,
    /* ` */ 6, 2,14, 4,11, EN,
    /* a */ 11, 3,8, AR, 3,10, 6,10, AR, 9,10, 9,7, 9,0, MV, 9,6, 5,6, AR, 2,6, 2,3, AR, 2,0, 5,0,
            6,0, AR, 9,0, 9,3, EN,
    /* b */ 12, 2,14, 2,0, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,5, AR, 10,0, 6,0, AR, 2,0, 2,4, EN,
    /* c */ 11, 9,7, AR, 9,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 9,0, 9,3, EN,
    /* d */ 12, 10,14, 10,0, MV, 10,6, AR, 10,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 10,0, 10,4, EN,
    /* e */ 12, 2,5, 10,5, AR, 10,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, 7,0, AR, 10,0, 10,2, EN,
    /* f */ 7, 7,14, 6,14, AR, 3,14, 3,11, 3,0, MV, 1,10, 6,10, EN,
    /* g */ 12, 10,10, 10,-1, AR, 10,-4, 6,-4, AR, 2,-4, 2,-2, MV, 10,6, AR, 10,10, 6,10,
            AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 10,0, 10,4, EN,
    /* h */ 12, 2,14, 2,0, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,6, 10,0, EN,
    /* i */ 4, 2,10, 2,0, MV, 2,13, 2,14, EN,
    /* j */ 5, 3,10, 3,-2, AR, 3,-4, 1,-4, 0,-4, MV, 3,13, 3,14, EN,
    /* k */ 11, 2,14, 2,0, MV, 9,10, 2,4, MV, 5,6, 10,0, EN,
    /* l */ 4, 2,14, 2,0, EN,
    /* m */ 16, 2,10, 2,0, MV, 2,6, AR, 2,10, 5,10, AR, 8,10, 8,6, 8,0, MV, 8,6, AR, 8,10, 11,10,
            AR, 14,10, 14,6, 14,0, EN,
    /* n */ 12, 2,10, 2,0, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,6, 10,0, EN,
    /* o */ 12, 6,10, AR, 10,10, 10,5, AR, 10,0, 6,0, AR, 2,0, 2,5, AR, 2,10, 6,10, EN,
    /* p */ 12, 2,10, 2,-4, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,5, AR, 10,0, 6,0, AR, 2,0, 2,4, EN,
    /* q */ 12, 10,10, 10,-4, MV, 10,6, AR, 10,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 10,0, 10,4, EN,
    /* r */ 8, 2,10, 2,0, MV, 2,6, AR, 2,10, 6,10, 7,10, EN,
    /* s */ 10, 8,8, AR, 8,10, 5,10, AR, 2,10, 2,7, AR, 2,5, 5,5, AR, 8,5, 8,3, AR, 8,0, 5,0,
            AR, 2,0, 2,2, EN,
    /* t */ 7, 3,13, 3,2, AR, 3,0, 5,0, 6,0, MV, 1,10, 6,10, EN,
    /* u */ 12, 2,10, 2,4, AR, 2,0, 6,0, AR, 10,0, 10,4, MV, 10,10, 10,0, EN,
    /* v */ 10, 1,10, 5,0, 9,10, EN,
    /* w */ 16, 1,10, 4,0, 8,9, 12,0, 15,10, EN,
    /* x */ 10, 1,10, 9,0, MV, 9,10, 1,0, EN,
    /* y */ 10, 1,10, 5,0, MV, 9,10, 3,-4, EN,
    /* z */ 10, 2,10, 8,10, 2,0, 8,0, EN,
    /* { */ 7, 6,15, AR, 4,15, 4,13, 4,8, AR, 4,6, 2,6, AR, 4,6, 4,4, 4,-1, AR, 4,-3, 6,-3, EN,
    /* | */ 6, 3,15, 3,-4, EN,
    /* } */ 7, 1,15, AR, 3,15, 3,13, 3,8, AR, 3,6, 5,6, AR, 3,6, 3,4, 3,-1, AR, 3,-3, 1,-3, EN,
    /* ~ */ 12, 2,6, AR, 2,8, 4,8, AR, 6,8, 6,7, AR, 6,6, 8,6, AR, 10,6, 10,8, EN,
    /* bullet */ 10, 5,8, AR, 6,8, 6,7, AR, 6,6, 5,6, AR, 4,6, 4,7, AR, 4,8, 5,8, EN,
    /* en dash */ 10, 0,5, 10,5, EN,
    /* em dash */ 20, 0,5, 20,5, EN,
    /* left quote */ 4, 2,14, 3,11, EN,
    /* right quote */ 4, 3,14, 2,11, EN,
    /* a box, for a character there is no drawing of */ 10, 2,0, 8,0, 8,10, 2,10, 2,0, EN,
    /* grave */ 0, -2,14, 1,12, EN,
    /* acute */ 0, -1,12, 2,14, EN,
    /* circumflex */ 0, -3,12, 0,14, 3,12, EN,
    /* tilde */ 0, -3,12, -1,14, 1,12, 3,14, EN,
    /* dieresis */ 0, -2,13, -2,13, MV, 2,13, 2,13, EN,
    /* ring */ 0, 0,15, AR, 2,15, 2,13, AR, 2,11, 0,11, AR, -2,11, -2,13, AR, -2,15, 0,15, EN,
    /* cedilla */ 0, 0,0, 0,-2, -2,-3, EN,
    /* caron */ 0, -3,14, 0,12, 3,14, EN,
    /* ellipsis */ 20, 3,0, 3,1, MV, 10,0, 10,1, MV, 17,0, 17,1, EN,
    /* check */ 12, 2,6, 5,2, 11,13, EN,
    /* arrow right */ 14, 1,5, 13,5, MV, 9,9, 13,5, 9,1, EN,
    /* arrow left */ 14, 13,5, 1,5, MV, 5,9, 1,5, 5,1, EN,
    /* the later drawings (FF_MORE), in the order of more_uni[] below */
    /* inverted ! */ 6, 3,10, 3,9, MV, 3,6, 3,-4, EN,
    /* cent */ 11, 9,7, AR, 9,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 9,0, 9,3, MV, 6,12, 6,-2, EN,
    /* pound */ 12, 10,11, AR, 10,14, 7,14, AR, 4,14, 4,11, 4,2, AR, 4,0, 2,0, 10,0, MV, 2,7, 8,7, EN,
    /* currency */ 12, 6,10, AR, 9,10, 9,7, AR, 9,4, 6,4, AR, 3,4, 3,7, AR, 3,10, 6,10, MV, 2,11, 4,9,
            MV, 10,11, 8,9, MV, 2,3, 4,5, MV, 10,3, 8,5, EN,
    /* yen */ 12, 1,14, 6,7, 11,14, MV, 6,7, 6,0, MV, 2,7, 10,7, MV, 2,4, 10,4, EN,
    /* broken bar */ 6, 3,15, 3,7, MV, 3,4, 3,-4, EN,
    /* section */ 12, 9,12, AR, 9,14, 6,14, AR, 3,14, 3,12, AR, 3,9, 6,9, AR, 9,9, 9,6, AR, 9,3, 6,3,
            MV, 3,0, AR, 3,-2, 6,-2, AR, 9,-2, 9,0, AR, 9,3, 6,3, AR, 3,3, 3,6, AR, 3,9, 6,9, EN,
    /* dieresis */ 7, 2,13, 2,13, MV, 5,13, 5,13, EN,
    /* copyright */ 16, 8,14, AR, 15,14, 15,7, AR, 15,0, 8,0, AR, 1,0, 1,7, AR, 1,14, 8,14,
            MV, 10,9, AR, 10,10, 8,10, AR, 5,10, 5,7, AR, 5,4, 8,4, AR, 10,4, 10,5, EN,
    /* feminine ordinal */ 7, 5,12, 5,8, MV, 5,10, AR, 5,12, 3,12, AR, 1,12, 1,10, AR, 1,8, 3,8,
            AR, 5,8, 5,10, EN,
    /* left guillemet */ 11, 5,9, 2,5, 5,1, MV, 9,9, 6,5, 9,1, EN,
    /* not */ 12, 2,7, 10,7, 10,3, EN,
    /* registered */ 16, 8,14, AR, 15,14, 15,7, AR, 15,0, 8,0, AR, 1,0, 1,7, AR, 1,14, 8,14,
            MV, 6,4, 6,10, 8,10, AR, 10,10, 10,9, 10,8, AR, 10,7, 8,7, 6,7, MV, 8,7, 10,4, EN,
    /* macron */ 7, 1,13, 6,13, EN,
    /* degree */ 8, 4,14, AR, 6,14, 6,12, AR, 6,10, 4,10, AR, 2,10, 2,12, AR, 2,14, 4,14, EN,
    /* plus or minus */ 12, 6,12, 6,4, MV, 2,8, 10,8, MV, 2,1, 10,1, EN,
    /* superior 2 */ 7, 1,12, AR, 1,14, 3,14, AR, 5,14, 5,12, 1,6, 5,6, EN,
    /* superior 3 */ 7, 1,13, AR, 1,14, 3,14, AR, 5,14, 5,12, AR, 5,10, 3,10, MV, 3,10, AR, 5,10, 5,8,
            AR, 5,6, 3,6, AR, 1,6, 1,7, EN,
    /* acute */ 7, 2,11, 5,14, EN,
    /* micro */ 12, 2,10, 2,-4, MV, 2,4, AR, 2,0, 6,0, AR, 10,0, 10,4, MV, 10,10, 10,0, EN,
    /* pilcrow */ 12, 10,14, 5,14, AR, 2,14, 2,11, AR, 2,8, 5,8, 6,8, MV, 6,14, 6,-3, MV, 9,14, 9,-3, EN,
    /* cedilla */ 7, 4,0, 4,-2, 2,-3, EN,
    /* superior 1 */ 7, 2,12, 4,14, 4,6, EN,
    /* masculine ordinal */ 8, 4,14, AR, 6,14, 6,11, AR, 6,8, 4,8, AR, 2,8, 2,11, AR, 2,14, 4,14, EN,
    /* right guillemet */ 11, 2,9, 5,5, 2,1, MV, 6,9, 9,5, 6,1, EN,
    /* one quarter */ 16, 1,12, 3,14, 3,7, MV, 11,14, 4,0, MV, 14,0, 14,7, 10,2, 15,2, EN,
    /* one half */ 16, 1,12, 3,14, 3,7, MV, 11,14, 4,0, MV, 10,5, AR, 10,7, 12,7, AR, 14,7, 14,5,
            10,0, 14,0, EN,
    /* three quarters */ 16, 1,13, AR, 1,14, 3,14, AR, 5,14, 5,12, AR, 5,11, 3,11, MV, 3,11,
            AR, 5,11, 5,9, AR, 5,7, 3,7, AR, 1,7, 1,8, MV, 12,14, 5,0, MV, 14,0, 14,7, 10,2, 15,2, EN,
    /* inverted ? */ 11, 10,-1, AR, 10,-4, 6,-4, AR, 2,-4, 2,-1, 6,3, 6,6, MV, 6,10, 6,9, EN,
    /* Eth */ 15, 2,0, 2,14, 7,14, AR, 13,14, 13,7, AR, 13,0, 7,0, 2,0, MV, 0,7, 6,7, EN,
    /* multiply */ 12, 2,11, 10,3, MV, 10,11, 2,3, EN,
    /* O slash */ 16, 8,14, AR, 14,14, 14,7, AR, 14,0, 8,0, AR, 2,0, 2,7, AR, 2,14, 8,14,
            MV, 13,15, 3,-1, EN,
    /* Thorn */ 14, 2,0, 2,14, MV, 2,11, 8,11, AR, 12,11, 12,7, AR, 12,3, 8,3, 2,3, EN,
    /* sharp s */ 12, 2,0, 2,11, AR, 2,14, 5,14, AR, 8,14, 8,11, AR, 8,8, 5,8, AR, 9,8, 9,4,
            AR, 9,0, 5,0, 4,0, EN,
    /* eth */ 12, 6,10, AR, 10,10, 10,5, AR, 10,0, 6,0, AR, 2,0, 2,5, AR, 2,10, 6,10,
            MV, 10,5, 10,9, 5,14, MV, 5,11, 9,13, EN,
    /* divide */ 12, 2,7, 10,7, MV, 6,11, 6,11, MV, 6,3, 6,3, EN,
    /* o slash */ 12, 6,10, AR, 10,10, 10,5, AR, 10,0, 6,0, AR, 2,0, 2,5, AR, 2,10, 6,10,
            MV, 10,11, 2,-1, EN,
    /* thorn */ 12, 2,14, 2,-4, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,5, AR, 10,0, 6,0, AR, 2,0, 2,4, EN,
    /* euro */ 12, 11,11, AR, 11,14, 7,14, AR, 3,14, 3,7, AR, 3,0, 7,0, AR, 11,0, 11,3,
            MV, 1,9, 8,9, MV, 1,5, 8,5, EN,
    /* low double quote */ 8, 3,2, 2,-1, MV, 6,2, 5,-1, EN,
    /* dagger */ 12, 6,14, 6,-3, MV, 2,9, 10,9, EN,
    /* double dagger */ 12, 6,14, 6,-3, MV, 2,10, 10,10, MV, 2,2, 10,2, EN,
    /* per thousand */ 25, 4,14, AR, 7,14, 7,11, AR, 7,8, 4,8, AR, 1,8, 1,11, AR, 1,14, 4,14, MV, 13,14, 5,0,
            MV, 14,6, AR, 17,6, 17,3, AR, 17,0, 14,0, AR, 11,0, 11,3, AR, 11,6, 14,6,
            MV, 21,6, AR, 24,6, 24,3, AR, 24,0, 21,0, AR, 18,0, 18,3, AR, 18,6, 21,6, EN,
    /* left single guillemet */ 7, 5,9, 2,5, 5,1, EN,
    /* left double quote */ 8, 2,14, 3,11, MV, 5,14, 6,11, EN,
    /* right double quote */ 8, 3,14, 2,11, MV, 6,14, 5,11, EN,
    /* trademark */ 17, 1,14, 7,14, MV, 4,14, 4,7, MV, 9,7, 9,14, 12,9, 15,14, 15,7, EN,
    /* right single guillemet */ 7, 2,9, 5,5, 2,1, EN
};

/* which character each of the later drawings is: Latin-1 in its own
   order, then the rest of what Windows' code page 1252 has */
static const uint16_t more_uni[] = {
    0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA,
    0x00AB, 0x00AC, 0x00AE, 0x00AF, 0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5,
    0x00B6, 0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF, 0x00D0,
    0x00D7, 0x00D8, 0x00DE, 0x00DF, 0x00F0, 0x00F7, 0x00F8, 0x00FE, 0x20AC, 0x201E,
    0x2020, 0x2021, 0x2030, 0x2039, 0x201C, 0x201D, 0x2122, 0x203A
};

/* ------------------------------------------------------------------ */
/* the latest drawings: Latin Extended-A, Greek and Cyrillic           */
/* ------------------------------------------------------------------ */
/*
 * PMWRITE's own, and like the later ones they are only for a
 * font that asks.  They are a list of their own so that not one of the
 * numbers above moves: a drawing here is G_COUNT and its place in the
 * list.  Eight accents come first, then the letters in the order of
 * the names below.  Most of the three alphabets is not here at all -
 * an A is an A in each of them - and the tables after the drawings
 * say, for each character, which drawing it is and which accent goes
 * on it.
 */
#define LATE_MARKS  8           /* macron breve dot ogonek double-acute comma-below
                                   apostrophe tonos: accents 9 to 16 */
enum {
    L_DBAR, L_HBAR_U, L_HBAR, L_IDOTLESS, L_KRA, L_LDOT_U, L_LDOT, L_LSLASH_U, L_LSLASH, L_ENG_U,
    L_ENG, L_TBAR_U, L_TBAR, L_LONGS, L_ETH_U,
    L_GAMMA_U, L_DELTA_U, L_THETA_U, L_LAMBDA_U, L_XI_U, L_PI_U, L_SIGMA_U, L_PHI_U, L_PSI_U, L_OMEGA_U,
    L_ALPHA, L_BETA, L_GAMMA, L_DELTA, L_EPSILON, L_ZETA, L_ETA, L_THETA, L_IOTA, L_LAMBDA,
    L_MU, L_XI, L_PI, L_RHO, L_FSIGMA, L_SIGMA, L_TAU, L_UPSILON, L_PHI, L_CHI, L_PSI, L_OMEGA,
    L_BE_U, L_DE_U, L_ZHE_U, L_ZE_U, L_I_U, L_EL_U, L_U_U, L_TSE_U, L_CHE_U, L_SHA_U,
    L_SHCHA_U, L_HARD_U, L_YERU_U, L_SOFT_U, L_E_U, L_YU_U, L_YA_U, L_YE_U, L_DJE_U, L_LJE_U,
    L_NJE_U, L_TSHE_U, L_DZHE_U, L_GHEUP_U,
    L_BE, L_VE, L_GHE, L_DE, L_ZHE, L_ZE, L_I, L_EL, L_EM, L_EN,
    L_PE, L_TE, L_EF, L_TSE, L_CHE, L_SHA, L_SHCHA, L_HARD, L_YERU, L_SOFT,
    L_E, L_YU, L_YA, L_YE, L_DJE, L_LJE, L_NJE, L_TSHE, L_DZHE, L_GHEUP,
    LATE_COUNT
};
#define LATE_ALL    (LATE_MARKS + LATE_COUNT)

static const int8_t late_strokes[] = {
    /* the accents, drawn about the middle of a small letter */
    /* macron */ 0, -3,13, 3,13, EN,
    /* breve */ 0, -3,14, AR, -3,12, 0,12, AR, 3,12, 3,14, EN,
    /* dot */ 0, 0,13, 0,13, EN,
    /* ogonek */ 0, 1,0, AR, -1,-1, -1,-2, AR, -1,-3, 1,-3, EN,
    /* double acute */ 0, -2,12, 0,14, MV, 1,12, 3,14, EN,
    /* comma below */ 0, 0,-2, -1,-4, EN,
    /* apostrophe, at the right of the letter */ 0, 0,14, -1,11, EN,
    /* tonos, at the left of a capital */ 0, 0,12, 1,15, EN,
    /* Latin */
    /* d bar */ 12, 10,14, 10,0, MV, 10,6, AR, 10,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 10,0, 10,4,
            MV, 6,12, 12,12, EN,
    /* H bar */ 15, 2,0, 2,14, MV, 13,0, 13,14, MV, 2,7, 13,7, MV, 0,11, 15,11, EN,
    /* h bar */ 12, 2,14, 2,0, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,6, 10,0, MV, 0,12, 6,12, EN,
    /* dotless i */ 4, 2,10, 2,0, EN,
    /* kra */ 11, 2,0, 2,10, MV, 9,10, 2,4, MV, 5,6, 10,0, EN,
    /* L dot */ 12, 2,14, 2,0, 11,0, MV, 7,7, 7,7, EN,
    /* l dot */ 7, 2,14, 2,0, MV, 5,7, 5,7, EN,
    /* L slash */ 12, 3,14, 3,0, 11,0, MV, 1,5, 6,9, EN,
    /* l slash */ 6, 3,14, 3,0, MV, 1,5, 5,9, EN,
    /* Eng */ 15, 2,0, 2,14, 13,0, MV, 13,14, 13,-1, AR, 13,-4, 10,-4, EN,
    /* eng */ 12, 2,10, 2,0, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,6, 10,-1, AR, 10,-4, 7,-4, EN,
    /* T bar */ 12, 1,14, 11,14, MV, 6,0, 6,14, MV, 3,7, 9,7, EN,
    /* t bar */ 7, 3,13, 3,2, AR, 3,0, 5,0, 6,0, MV, 1,10, 6,10, MV, 1,6, 6,6, EN,
    /* long s */ 6, 6,14, AR, 3,14, 3,11, 3,0, EN,
    /* D stroke */ 15, 2,0, 2,14, 7,14, AR, 13,14, 13,7, AR, 13,0, 7,0, 2,0, MV, 0,7, 6,7, EN,
    /* Greek capitals */
    /* Gamma */ 12, 11,14, 2,14, 2,0, EN,
    /* Delta */ 14, 1,0, 7,14, 13,0, 1,0, EN,
    /* Theta */ 16, 8,14, AR, 14,14, 14,7, AR, 14,0, 8,0, AR, 2,0, 2,7, AR, 2,14, 8,14, MV, 5,7, 11,7, EN,
    /* Lambda */ 14, 1,0, 7,14, 13,0, EN,
    /* Xi */ 13, 2,14, 11,14, MV, 3,7, 10,7, MV, 2,0, 11,0, EN,
    /* Pi */ 15, 2,0, 2,14, 13,14, 13,0, EN,
    /* Sigma */ 13, 11,14, 2,14, 7,7, 2,0, 11,0, EN,
    /* Phi */ 16, 8,14, 8,0, MV, 8,11, AR, 14,11, 14,7, AR, 14,3, 8,3, AR, 2,3, 2,7, AR, 2,11, 8,11, EN,
    /* Psi */ 16, 8,14, 8,0, MV, 2,14, 2,9, AR, 2,4, 8,4, AR, 14,4, 14,9, 14,14, EN,
    /* Omega */ 16, 2,0, 6,0, 6,3, AR, 2,3, 2,8, AR, 2,14, 8,14, AR, 14,14, 14,8, AR, 14,3, 10,3,
            10,0, 14,0, EN,
    /* Greek small letters */
    /* alpha */ 12, 10,10, 10,2, AR, 10,0, 11,0, MV, 10,6, AR, 10,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0,
            AR, 10,0, 10,4, EN,
    /* beta */ 12, 2,-4, 2,11, AR, 2,14, 5,14, AR, 9,14, 9,11, AR, 9,8, 5,8, AR, 10,8, 10,4,
            AR, 10,0, 6,0, AR, 2,0, 2,2, EN,
    /* gamma */ 10, 1,10, 5,0, 5,-4, MV, 9,10, 5,0, EN,
    /* delta */ 12, 9,13, AR, 9,14, 6,14, AR, 3,14, 3,12, AR, 3,10, 6,9, AR, 10,8, 10,4, AR, 10,0, 6,0,
            AR, 2,0, 2,4, AR, 2,9, 6,9, EN,
    /* epsilon */ 10, 8,8, AR, 8,10, 5,10, AR, 2,10, 2,8, AR, 2,5, 5,5, MV, 5,5, AR, 2,5, 2,3,
            AR, 2,0, 5,0, AR, 8,0, 8,2, EN,
    /* zeta */ 10, 2,14, 8,14, AR, 2,9, 2,4, AR, 2,0, 6,0, AR, 8,0, 8,-2, AR, 8,-4, 6,-4, EN,
    /* eta */ 12, 2,10, 2,0, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,6, 10,-4, EN,
    /* theta */ 12, 6,14, AR, 10,14, 10,7, AR, 10,0, 6,0, AR, 2,0, 2,7, AR, 2,14, 6,14, MV, 2,7, 10,7, EN,
    /* iota */ 5, 2,10, 2,2, AR, 2,0, 4,0, EN,
    /* lambda */ 10, 2,14, 9,0, MV, 5,8, 1,0, EN,
    /* mu */ 12, 2,10, 2,-4, MV, 2,4, AR, 2,0, 6,0, AR, 10,0, 10,4, MV, 10,10, 10,0, EN,
    /* xi */ 10, 8,14, 4,14, AR, 2,14, 2,11, AR, 2,8, 5,8, MV, 5,8, AR, 2,8, 2,4, AR, 2,0, 6,0,
            AR, 8,0, 8,-2, AR, 8,-4, 6,-4, EN,
    /* pi */ 12, 1,10, 11,10, MV, 4,10, 4,0, MV, 8,10, 8,0, EN,
    /* rho */ 12, 2,-4, 2,5, AR, 2,10, 6,10, AR, 10,10, 10,5, AR, 10,0, 6,0, AR, 2,0, 2,5, EN,
    /* final sigma */ 10, 9,8, AR, 9,10, 6,10, AR, 2,10, 2,5, AR, 2,1, 6,1, AR, 8,1, 8,-1, AR, 8,-4, 5,-4, EN,
    /* sigma */ 12, 11,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 10,0, 10,5, AR, 10,10, 6,10, EN,
    /* tau */ 10, 1,10, 9,10, MV, 5,10, 5,2, AR, 5,0, 7,0, EN,
    /* upsilon */ 11, 2,10, 2,4, AR, 2,0, 5,0, AR, 9,0, 9,5, 9,10, EN,
    /* phi */ 14, 7,12, 7,-4, MV, 7,10, AR, 12,10, 12,5, AR, 12,0, 7,0, AR, 2,0, 2,5, AR, 2,10, 7,10, EN,
    /* chi */ 10, 1,10, 9,-4, MV, 9,10, 1,-4, EN,
    /* psi */ 14, 7,12, 7,-4, MV, 2,10, 2,5, AR, 2,0, 7,0, AR, 12,0, 12,5, 12,10, EN,
    /* omega */ 16, 3,10, AR, 2,8, 2,4, AR, 2,0, 5,0, AR, 8,0, 8,4, 8,6, MV, 8,4, AR, 8,0, 11,0,
            AR, 14,0, 14,4, AR, 14,8, 13,10, EN,
    /* Cyrillic capitals */
    /* Be */ 14, 11,14, 2,14, 2,0, 8,0, AR, 12,0, 12,4, AR, 12,8, 8,8, 2,8, EN,
    /* De */ 15, 3,0, 6,14, 12,14, 12,0, MV, 1,-3, 1,0, 14,0, 14,-3, EN,
    /* Zhe */ 18, 9,0, 9,14, MV, 1,14, 9,7, 17,14, MV, 1,0, 9,7, 17,0, EN,
    /* Ze */ 13, 2,11, AR, 2,14, 6,14, AR, 11,14, 11,11, AR, 11,8, 6,8, 5,8, MV, 6,8, AR, 11,8, 11,4,
            AR, 11,0, 6,0, AR, 2,0, 2,3, EN,
    /* I */ 15, 2,14, 2,0, 13,14, 13,0, EN,
    /* El */ 14, 1,0, AR, 4,0, 4,5, 4,14, 12,14, 12,0, EN,
    /* U */ 13, 1,14, 7,4, MV, 12,14, 6,2, AR, 5,0, 2,0, EN,
    /* Tse */ 15, 2,14, 2,0, 13,0, 13,-3, MV, 11,14, 11,0, EN,
    /* Che */ 13, 2,14, 2,9, AR, 2,6, 6,6, 11,6, MV, 11,14, 11,0, EN,
    /* Sha */ 18, 2,14, 2,0, 16,0, 16,14, MV, 9,14, 9,0, EN,
    /* Shcha */ 20, 2,14, 2,0, 18,0, 18,-3, MV, 9,14, 9,0, MV, 16,14, 16,0, EN,
    /* hard sign */ 15, 1,14, 4,14, 4,0, 9,0, AR, 13,0, 13,4, AR, 13,8, 9,8, 4,8, EN,
    /* Yeru */ 18, 2,14, 2,0, 6,0, AR, 10,0, 10,4, AR, 10,8, 6,8, 2,8, MV, 15,14, 15,0, EN,
    /* soft sign */ 13, 2,14, 2,0, 7,0, AR, 11,0, 11,4, AR, 11,8, 7,8, 2,8, EN,
    /* E */ 15, 2,10, AR, 2,14, 7,14, AR, 13,14, 13,7, AR, 13,0, 7,0, AR, 2,0, 2,4, MV, 6,7, 13,7, EN,
    /* Yu */ 20, 2,14, 2,0, MV, 2,7, 6,7, MV, 12,14, AR, 18,14, 18,7, AR, 18,0, 12,0, AR, 6,0, 6,7,
            AR, 6,14, 12,14, EN,
    /* Ya */ 14, 12,0, 12,14, 6,14, AR, 2,14, 2,10, AR, 2,6, 6,6, 12,6, MV, 6,6, 1,0, EN,
    /* Ukrainian Ye */ 15, 13,10, AR, 13,14, 8,14, AR, 2,14, 2,7, AR, 2,0, 8,0, AR, 13,0, 13,4,
            MV, 2,7, 9,7, EN,
    /* Dje */ 16, 1,14, 11,14, MV, 5,14, 5,0, MV, 5,8, 10,8, AR, 14,8, 14,4, 14,1, AR, 14,-2, 11,-2, EN,
    /* Lje */ 20, 1,0, AR, 4,0, 4,5, 4,14, 10,14, 10,0, 14,0, AR, 18,0, 18,4, AR, 18,8, 14,8, 10,8, EN,
    /* Nje */ 20, 2,0, 2,14, MV, 2,7, 10,7, MV, 10,14, 10,0, 14,0, AR, 18,0, 18,4, AR, 18,8, 14,8,
            10,8, EN,
    /* Tshe */ 16, 1,14, 11,14, MV, 5,14, 5,0, MV, 5,8, 10,8, AR, 14,8, 14,4, 14,0, EN,
    /* Dzhe */ 15, 2,14, 2,0, 13,0, 13,14, MV, 7,0, 7,-3, EN,
    /* Ghe with upturn */ 12, 11,16, 11,14, 2,14, 2,0, EN,
    /* Cyrillic small letters */
    /* be */ 12, 10,14, AR, 2,13, 2,5, AR, 2,0, 6,0, AR, 10,0, 10,5, AR, 10,9, 6,9, AR, 2,9, 2,5, EN,
    /* ve */ 11, 2,0, 2,10, 6,10, AR, 9,10, 9,8, AR, 9,6, 6,6, 2,6, MV, 6,6, AR, 10,6, 10,3,
            AR, 10,0, 6,0, 2,0, EN,
    /* ghe */ 9, 8,10, 2,10, 2,0, EN,
    /* de */ 13, 3,0, 5,10, 10,10, 10,0, MV, 1,-3, 1,0, 12,0, 12,-3, EN,
    /* zhe */ 14, 7,0, 7,10, MV, 1,10, 7,5, 13,10, MV, 1,0, 7,5, 13,0, EN,
    /* ze */ 10, 2,8, AR, 2,10, 5,10, AR, 8,10, 8,8, AR, 8,6, 5,6, 4,6, MV, 5,6, AR, 8,6, 8,3,
            AR, 8,0, 5,0, AR, 2,0, 2,2, EN,
    /* i */ 12, 2,10, 2,0, 10,10, 10,0, EN,
    /* el */ 11, 1,0, AR, 3,0, 3,3, 3,10, 9,10, 9,0, EN,
    /* em */ 14, 2,0, 2,10, 7,2, 12,10, 12,0, EN,
    /* en */ 12, 2,0, 2,10, MV, 10,0, 10,10, MV, 2,5, 10,5, EN,
    /* pe */ 12, 2,0, 2,10, 10,10, 10,0, EN,
    /* te */ 10, 1,10, 9,10, MV, 5,10, 5,0, EN,
    /* ef */ 14, 7,14, 7,-4, MV, 7,10, AR, 12,10, 12,5, AR, 12,0, 7,0, AR, 2,0, 2,5, AR, 2,10, 7,10, EN,
    /* tse */ 12, 2,10, 2,0, 11,0, 11,-3, MV, 9,10, 9,0, EN,
    /* che */ 11, 2,10, 2,7, AR, 2,4, 5,4, 9,4, MV, 9,10, 9,0, EN,
    /* sha */ 16, 2,10, 2,0, 14,0, 14,10, MV, 8,10, 8,0, EN,
    /* shcha */ 17, 2,10, 2,0, 16,0, 16,-3, MV, 8,10, 8,0, MV, 14,10, 14,0, EN,
    /* hard sign */ 12, 1,10, 4,10, 4,0, 7,0, AR, 10,0, 10,3, AR, 10,6, 7,6, 4,6, EN,
    /* yeru */ 15, 2,10, 2,0, 5,0, AR, 8,0, 8,3, AR, 8,6, 5,6, 2,6, MV, 13,10, 13,0, EN,
    /* soft sign */ 10, 2,10, 2,0, 5,0, AR, 8,0, 8,3, AR, 8,6, 5,6, 2,6, EN,
    /* e */ 11, 2,8, AR, 2,10, 5,10, AR, 9,10, 9,5, AR, 9,0, 5,0, AR, 2,0, 2,2, MV, 4,5, 9,5, EN,
    /* yu */ 16, 2,10, 2,0, MV, 2,5, 6,5, MV, 10,10, AR, 14,10, 14,5, AR, 14,0, 10,0, AR, 6,0, 6,5,
            AR, 6,10, 10,10, EN,
    /* ya */ 11, 9,0, 9,10, 5,10, AR, 2,10, 2,7, AR, 2,4, 5,4, 9,4, MV, 5,4, 1,0, EN,
    /* Ukrainian ye */ 11, 9,7, AR, 9,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 9,0, 9,3, MV, 2,5, 7,5, EN,
    /* dje */ 12, 2,14, 2,0, MV, 0,12, 6,12, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,6, 10,-1,
            AR, 10,-4, 7,-4, EN,
    /* lje */ 16, 1,0, AR, 3,0, 3,3, 3,10, 8,10, 8,0, 11,0, AR, 14,0, 14,3, AR, 14,6, 11,6, 8,6, EN,
    /* nje */ 16, 2,0, 2,10, MV, 2,5, 8,5, MV, 8,10, 8,0, 11,0, AR, 14,0, 14,3, AR, 14,6, 11,6, 8,6, EN,
    /* tshe */ 12, 2,14, 2,0, MV, 0,12, 6,12, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,6, 10,0, EN,
    /* dzhe */ 12, 2,10, 2,0, 10,0, 10,10, MV, 6,0, 6,-3, EN,
    /* ghe with upturn */ 9, 8,12, 8,10, 2,10, 2,0, EN
};

/* the letter each of them is as wide as, in a font whose widths are
   known for ASCII only: the drawings in the order of their names */
static const char late_like[LATE_COUNT + 1] =
    "dHhikLlLlNnTtfD"
    "LEOAEHEOOO"
    "dbyoccnoikuc" "npcocudxww"
    "BRWSHRYHRWWAMPCWRCDWWDHL"
    "bacbwsnnwnncwnnmmbwacwachwwhnc";

/* where an accent from 9 on goes: 0 over the middle of the letter, 1 at
   its right edge, 2 at its left; and 1 if it is drawn higher over a
   tall letter */
static const uint8_t mark_at[LATE_MARKS][2] = {
    { 0, 1 }, { 0, 1 }, { 0, 1 }, { 0, 0 }, { 0, 1 }, { 0, 0 }, { 1, 0 }, { 2, 0 }
};

#define LD( name )  (0x80 + L_##name)

/* Latin Extended-A, 0100h to 017Fh: the letter - one of ASCII's, or a
   drawing above - and the accent on it.  0 0 is a character made some
   other way, or not at all. */
static const uint8_t pair_latin[128][2] = {
    { 'A', 9 }, { 'a', 9 }, { 'A', 10 }, { 'a', 10 }, { 'A', 12 }, { 'a', 12 }, { 'C', 2 }, { 'c', 2 },
    { 'C', 3 }, { 'c', 3 }, { 'C', 11 }, { 'c', 11 }, { 'C', 8 }, { 'c', 8 }, { 'D', 8 }, { 'd', 15 },
    { LD( ETH_U ), 0 }, { LD( DBAR ), 0 }, { 'E', 9 }, { 'e', 9 }, { 'E', 10 }, { 'e', 10 }, { 'E', 11 }, { 'e', 11 },
    { 'E', 12 }, { 'e', 12 }, { 'E', 8 }, { 'e', 8 }, { 'G', 3 }, { 'g', 3 }, { 'G', 10 }, { 'g', 10 },
    { 'G', 11 }, { 'g', 11 }, { 'G', 14 }, { 'g', 2 }, { 'H', 3 }, { 'h', 3 }, { LD( HBAR_U ), 0 }, { LD( HBAR ), 0 },
    { 'I', 4 }, { 'i', 4 }, { 'I', 9 }, { 'i', 9 }, { 'I', 10 }, { 'i', 10 }, { 'I', 12 }, { 'i', 12 },
    { 'I', 11 }, { LD( IDOTLESS ), 0 }, { 0, 0 }, { 0, 0 }, { 'J', 3 }, { 'j', 3 }, { 'K', 14 }, { 'k', 14 },
    { LD( KRA ), 0 }, { 'L', 2 }, { 'l', 2 }, { 'L', 14 }, { 'l', 14 }, { 'L', 15 }, { 'l', 15 }, { LD( LDOT_U ), 0 },
    { LD( LDOT ), 0 }, { LD( LSLASH_U ), 0 }, { LD( LSLASH ), 0 }, { 'N', 2 }, { 'n', 2 }, { 'N', 14 }, { 'n', 14 }, { 'N', 8 },
    { 'n', 8 }, { 0, 0 }, { LD( ENG_U ), 0 }, { LD( ENG ), 0 }, { 'O', 9 }, { 'o', 9 }, { 'O', 10 }, { 'o', 10 },
    { 'O', 13 }, { 'o', 13 }, { 0, 0 }, { 0, 0 }, { 'R', 2 }, { 'r', 2 }, { 'R', 14 }, { 'r', 14 },
    { 'R', 8 }, { 'r', 8 }, { 'S', 2 }, { 's', 2 }, { 'S', 3 }, { 's', 3 }, { 'S', 7 }, { 's', 7 },
    { 'S', 8 }, { 's', 8 }, { 'T', 7 }, { 't', 7 }, { 'T', 8 }, { 't', 15 }, { LD( TBAR_U ), 0 }, { LD( TBAR ), 0 },
    { 'U', 4 }, { 'u', 4 }, { 'U', 9 }, { 'u', 9 }, { 'U', 10 }, { 'u', 10 }, { 'U', 6 }, { 'u', 6 },
    { 'U', 13 }, { 'u', 13 }, { 'U', 12 }, { 'u', 12 }, { 'W', 3 }, { 'w', 3 }, { 'Y', 3 }, { 'y', 3 },
    { 'Y', 5 }, { 'Z', 2 }, { 'z', 2 }, { 'Z', 11 }, { 'z', 11 }, { 'Z', 8 }, { 'z', 8 }, { LD( LONGS ), 0 }
};

/* S and T with a comma below, 0218h to 021Bh */
static const uint8_t pair_comma[4][2] = { { 'S', 14 }, { 's', 14 }, { 'T', 14 }, { 't', 14 } };

/* Greek, 0384h to 03CEh */
static const uint8_t pair_greek[75][2] = {
    { 0, 0 }, { 0, 0 }, { 'A', 16 }, { 0, 0 }, { 'E', 16 }, { 'H', 16 }, { 'I', 16 }, { 0, 0 },
    { 'O', 16 }, { 0, 0 }, { 'Y', 16 }, { LD( OMEGA_U ), 16 },
    { LD( IOTA ), 5 }, { 'A', 0 }, { 'B', 0 }, { LD( GAMMA_U ), 0 }, { LD( DELTA_U ), 0 }, { 'E', 0 }, { 'Z', 0 }, { 'H', 0 },
    { LD( THETA_U ), 0 }, { 'I', 0 }, { 'K', 0 }, { LD( LAMBDA_U ), 0 }, { 'M', 0 }, { 'N', 0 }, { LD( XI_U ), 0 }, { 'O', 0 },
    { LD( PI_U ), 0 }, { 'P', 0 }, { 0, 0 }, { LD( SIGMA_U ), 0 }, { 'T', 0 }, { 'Y', 0 }, { LD( PHI_U ), 0 }, { 'X', 0 },
    { LD( PSI_U ), 0 }, { LD( OMEGA_U ), 0 }, { 'I', 5 }, { 'Y', 5 }, { LD( ALPHA ), 2 }, { LD( EPSILON ), 2 }, { LD( ETA ), 2 }, { LD( IOTA ), 2 },
    { LD( UPSILON ), 5 }, { LD( ALPHA ), 0 }, { LD( BETA ), 0 }, { LD( GAMMA ), 0 }, { LD( DELTA ), 0 }, { LD( EPSILON ), 0 }, { LD( ZETA ), 0 }, { LD( ETA ), 0 },
    { LD( THETA ), 0 }, { LD( IOTA ), 0 }, { LD( KRA ), 0 }, { LD( LAMBDA ), 0 }, { LD( MU ), 0 }, { 'v', 0 }, { LD( XI ), 0 }, { 'o', 0 },
    { LD( PI ), 0 }, { LD( RHO ), 0 }, { LD( FSIGMA ), 0 }, { LD( SIGMA ), 0 }, { LD( TAU ), 0 }, { LD( UPSILON ), 0 }, { LD( PHI ), 0 }, { LD( CHI ), 0 },
    { LD( PSI ), 0 }, { LD( OMEGA ), 0 }, { LD( IOTA ), 5 }, { LD( UPSILON ), 5 }, { 'o', 2 }, { LD( UPSILON ), 2 }, { LD( OMEGA ), 2 }
};

/* Cyrillic, 0400h to 045Fh */
static const uint8_t pair_cyrillic[96][2] = {
    { 'E', 1 }, { 'E', 5 }, { LD( DJE_U ), 0 }, { LD( GAMMA_U ), 2 }, { LD( YE_U ), 0 }, { 'S', 0 }, { 'I', 0 }, { 'I', 5 },
    { 'J', 0 }, { LD( LJE_U ), 0 }, { LD( NJE_U ), 0 }, { LD( TSHE_U ), 0 }, { 'K', 2 }, { LD( I_U ), 1 }, { LD( U_U ), 10 }, { LD( DZHE_U ), 0 },
    { 'A', 0 }, { LD( BE_U ), 0 }, { 'B', 0 }, { LD( GAMMA_U ), 0 }, { LD( DE_U ), 0 }, { 'E', 0 }, { LD( ZHE_U ), 0 }, { LD( ZE_U ), 0 },
    { LD( I_U ), 0 }, { LD( I_U ), 10 }, { 'K', 0 }, { LD( EL_U ), 0 }, { 'M', 0 }, { 'H', 0 }, { 'O', 0 }, { LD( PI_U ), 0 },
    { 'P', 0 }, { 'C', 0 }, { 'T', 0 }, { LD( U_U ), 0 }, { LD( PHI_U ), 0 }, { 'X', 0 }, { LD( TSE_U ), 0 }, { LD( CHE_U ), 0 },
    { LD( SHA_U ), 0 }, { LD( SHCHA_U ), 0 }, { LD( HARD_U ), 0 }, { LD( YERU_U ), 0 }, { LD( SOFT_U ), 0 }, { LD( E_U ), 0 }, { LD( YU_U ), 0 }, { LD( YA_U ), 0 },
    { 'a', 0 }, { LD( BE ), 0 }, { LD( VE ), 0 }, { LD( GHE ), 0 }, { LD( DE ), 0 }, { 'e', 0 }, { LD( ZHE ), 0 }, { LD( ZE ), 0 },
    { LD( I ), 0 }, { LD( I ), 10 }, { LD( KRA ), 0 }, { LD( EL ), 0 }, { LD( EM ), 0 }, { LD( EN ), 0 }, { 'o', 0 }, { LD( PE ), 0 },
    { 'p', 0 }, { 'c', 0 }, { LD( TE ), 0 }, { 'y', 0 }, { LD( EF ), 0 }, { 'x', 0 }, { LD( TSE ), 0 }, { LD( CHE ), 0 },
    { LD( SHA ), 0 }, { LD( SHCHA ), 0 }, { LD( HARD ), 0 }, { LD( YERU ), 0 }, { LD( SOFT ), 0 }, { LD( E ), 0 }, { LD( YU ), 0 }, { LD( YA ), 0 },
    { 'e', 1 }, { 'e', 5 }, { LD( DJE ), 0 }, { LD( GHE ), 2 }, { LD( YE ), 0 }, { 's', 0 }, { 'i', 0 }, { 'i', 5 },
    { 'j', 0 }, { LD( LJE ), 0 }, { LD( NJE ), 0 }, { LD( TSHE ), 0 }, { LD( KRA ), 2 }, { LD( I ), 1 }, { 'y', 10 }, { LD( DZHE ), 0 }
};

static const uint8_t pair_ghe[2][2] = { { LD( GHEUP_U ), 0 }, { LD( GHEUP ), 0 } };
static const uint8_t pair_theta[2] = { LD( THETA ), 0 };
static const uint8_t pair_phi[2] = { LD( PHI ), 0 };

/* which drawing and accent a character past Latin-1 is: NULL if it is
   in none of the tables */
static const uint8_t *late_pair( uint16_t uni )
{
    if ( uni >= 0x0100 && uni <= 0x017F ) {
        return pair_latin[uni - 0x0100];
    }
    if ( uni >= 0x0218 && uni <= 0x021B ) {
        return pair_comma[uni - 0x0218];
    }
    if ( uni >= 0x0384 && uni <= 0x03CE ) {
        return pair_greek[uni - 0x0384];
    }
    if ( uni >= 0x0400 && uni <= 0x045F ) {
        return pair_cyrillic[uni - 0x0400];
    }
    if ( uni == 0x0490 || uni == 0x0491 ) {
        return pair_ghe[uni - 0x0490];
    }
    if ( uni == 0x03D1 ) {
        return pair_theta;
    }
    if ( uni == 0x03D5 ) {
        return pair_phi;
    }
    return NULL;
}

static uint16_t starts[G_COUNT];
static uint16_t late_starts[LATE_ALL];
static int starts_made;

int sfont_as_acroread = 1;          /* where(), below, says what this is */

static void starts_make( void )
{
    unsigned int pos = 0;
    int index;

    for ( index = 0; index < G_COUNT && pos < sizeof( strokes ); index++ ) {
        starts[index] = (uint16_t)pos;
        while ( pos < sizeof( strokes ) && strokes[pos] != EN ) {
            pos++;
        }
        pos++;
    }
    pos = 0;
    for ( index = 0; index < LATE_ALL && pos < sizeof( late_strokes ); index++ ) {
        late_starts[index] = (uint16_t)pos;
        while ( pos < sizeof( late_strokes ) && late_strokes[pos] != EN ) {
            pos++;
        }
        pos++;
    }
    starts_made = 1;
}

/*
 * Where a drawing's bytes start.
 *
 * ACROREAD NUMBERED THE DRAWINGS AFTER '~' FROM 95, and there are 94
 * before them.  So it drew each of them as the one after: a bullet as a
 * dash, a dash as a longer dash, an ellipsis as a check mark, a grave
 * accent as an acute - "A" with a grave came out with an acute - and
 * the last number of all, the left arrow, as the first drawing, the
 * exclamation mark.  The numbers are right as their names say; it is
 * the looking up that was one out, and here it is put right for a font
 * that asks for the later drawings (PMWRITE's own text), and for every
 * font once sfont_as_acroread is 0.  While it is 1, a PDF's page is
 * drawn as ACROREAD drew it, mistake and all, so that the two can be
 * compared byte for byte.
 */
static const int8_t *where( int glyph, int flags )
{
    if ( glyph >= G_COUNT ) {
        /* one of the latest drawings, which were never numbered wrongly */
        return late_strokes + late_starts[glyph - G_COUNT < LATE_ALL ? glyph - G_COUNT : 0];
    }
    if ( glyph >= G_BULLET ) {
        if ( (flags & FF_MORE) || !sfont_as_acroread ) {
            glyph--;
        } else if ( glyph >= G_ARROWL ) {
            glyph = 0;
        }
    }
    return strokes + starts[glyph];
}

/* Latin-1 C0h to FFh: the letter, and the accent on it (0 none, then
   grave acute circumflex tilde dieresis ring cedilla) */
static const char latin_base[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPBaaaaaaaceeeeiiiidnooooo/ouuuuypy";
static const char latin_mark[] =
    "1234560712351235041234500123520012345607123512350412345001235205";

/* what to draw for a character: up to three drawings side by side, and
   an accent over the first.  0 if it is drawn as nothing. */
static int pick( uint16_t uni, int flags, int *parts, int *mark )
{
    const char *seq = NULL;
    int count = 0;

    *mark = 0;
    if ( uni >= 0x21 && uni <= 0x7E ) {
        parts[0] = uni - 0x21;
        return 1;
    }
    if ( (flags & FF_MORE) || !sfont_as_acroread ) {
        const uint8_t *pair = late_pair( uni );

        if ( pair != NULL && pair[0] != 0 ) {
            parts[0] = pair[0] >= 0x80 ? G_COUNT + LATE_MARKS + (pair[0] - 0x80) : pair[0] - 0x21;
            *mark = pair[1];
            if ( pair[0] == 'i' && *mark != 0 && *mark != 12 ) {
                parts[0] = G_COUNT + LATE_MARKS + L_IDOTLESS;   /* the accent takes the dot's place */
            }
            return 1;
        }
        if ( uni >= 0xEC && uni <= 0xEF ) {
            parts[0] = G_COUNT + LATE_MARKS + L_IDOTLESS;
            *mark = latin_mark[uni - 0xC0] - '0';
            return 1;
        }
        for ( count = 0; count < MORE_COUNT; count++ ) {
            if ( more_uni[count] == uni ) {
                parts[0] = G_MORE + count;
                return 1;
            }
        }
        count = 0;
    }
    if ( uni >= 0xC0 && uni <= 0xFF && uni != 0xC6 && uni != 0xE6 ) {
        parts[0] = latin_base[uni - 0xC0] - 0x21;
        *mark = latin_mark[uni - 0xC0] - '0';
        return 1;
    }
    switch ( uni ) {
    case 0x0020: case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x00AD:
        return 0;
    case 0x00A1: seq = "!"; break;
    case 0x00A6: seq = "|"; break;
    case 0x00AB: case 0x2039: seq = "<"; break;
    case 0x00BB: case 0x203A: seq = ">"; break;
    case 0x00B1: seq = "+"; break;
    case 0x00B4: case 0x2032: seq = "'"; break;
    case 0x00A8: case 0x2033: case 0x201C: case 0x201D: case 0x201E: seq = "\""; break;
    case 0x00BF: seq = "?"; break;
    case 0x00C6: case 0x01FC: seq = "AE"; break;
    case 0x00E6: seq = "ae"; break;
    case 0x0132: seq = "IJ"; break;
    case 0x0133: seq = "ij"; break;
    case 0x0149: seq = "'n"; break;
    case 0x0384: seq = "'"; break;
    case 0x0385: seq = "\""; break;
    case 0x0152: seq = "OE"; break;
    case 0x0153: seq = "oe"; break;
    case 0x0131: seq = "i"; break;
    case 0x0141: seq = "L"; break;
    case 0x0142: seq = "l"; break;
    case 0x0192: seq = "f"; break;
    case 0x02C6: seq = "^"; break;
    case 0x02DC: case 0x223C: seq = "~"; break;
    case 0x201A: seq = ","; break;
    case 0x2044: case 0x2215: seq = "/"; break;
    case 0x2217: seq = "*"; break;
    case 0xFB00: seq = "ff"; break;
    case 0xFB01: seq = "fi"; break;
    case 0xFB02: seq = "fl"; break;
    case 0xFB03: seq = "ffi"; break;
    case 0xFB04: seq = "ffl"; break;
    case 0x0160: seq = "S"; *mark = 8; break;
    case 0x0161: seq = "s"; *mark = 8; break;
    case 0x017D: seq = "Z"; *mark = 8; break;
    case 0x017E: seq = "z"; *mark = 8; break;
    case 0x0178: seq = "Y"; *mark = 5; break;
    case 0x00B7: case 0x0387: case 0x2022: case 0x2219: case 0x22C5: case 0x25CF: case 0x25CB: case 0x25E6:
        parts[0] = G_BULLET;
        return 1;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: case 0x00AF:
        parts[0] = G_ENDASH;
        return 1;
    case 0x2014: case 0x2015:
        parts[0] = G_EMDASH;
        return 1;
    case 0x2018: case 0x201B:
        parts[0] = G_QLEFT;
        return 1;
    case 0x2019:
        parts[0] = G_QRIGHT;
        return 1;
    case 0x2026:
        parts[0] = G_ELLIPSIS;
        return 1;
    case 0x2713: case 0x2714: case 0x221A:
        parts[0] = G_CHECK;
        return 1;
    case 0x2192: case 0x21D2: case 0x25BA:
        parts[0] = G_ARROWR;
        return 1;
    case 0x2190: case 0x21D0: case 0x25C4:
        parts[0] = G_ARROWL;
        return 1;
    }
    if ( seq == NULL ) {
        if ( uni < 0x21 ) {
            return 0;
        }
        parts[0] = G_BOX;
        return 1;
    }
    for ( ; seq[count] && count < 3; count++ ) {
        parts[count] = seq[count] - 0x21;
    }
    return count;
}

/* when not NULL, a character's path goes here and not to GLYPH.C: the
   PDF writer asks, to put the drawing itself in a file */
void (*sfont_sink)( void *ctx, int move, fx ux, fx uy );
void  *sfont_sink_ctx;

typedef struct {
    fx scale_x;                 /* font units to a grid unit, across */
    fx shear;                   /* how far a unit of height leans */
    fx left;                    /* where the drawing starts, in font units */
    int flags;                  /* the font's, for where() */
} SLANT;

static void put( const SLANT *sl, int first, fx gx, fx gy )
{
    fx ux = sl->left + fx_mul( gx, sl->scale_x ) + fx_mul( gy * 50, sl->shear );
    fx uy = gy * 50;

    if ( sfont_sink != NULL ) {
        sfont_sink( sfont_sink_ctx, first, ux, uy );
    } else if ( first ) {
        gl_move( ux, uy );
    } else {
        gl_line( ux, uy );
    }
}

/* does a drawing reach the height of a capital?  An accent over it is
   drawn that much higher. */
static int is_tall( int glyph, int flags )
{
    const int8_t *cur = where( glyph, flags ) + 1;

    while ( *cur != EN ) {
        if ( *cur == MV ) {
            cur++;
        } else if ( *cur == AR ) {
            if ( cur[2] > 11 || cur[4] > 11 ) {
                return 1;
            }
            cur += 5;
        } else {
            if ( cur[1] > 11 ) {
                return 1;
            }
            cur += 2;
        }
    }
    return 0;
}

/* one drawing, its grid shifted by (dx, dy) */
static void draw( const SLANT *sl, int glyph, fx dx, fx dy )
{
    const int8_t *cur = where( glyph, sl->flags ) + 1;
    fx px = 0, py = 0, cx, cy, ex, ey, sine, cosine;
    int first = 1, step;

    while ( *cur != EN ) {
        if ( *cur == MV ) {
            first = 1;
            cur++;
            continue;
        }
        if ( *cur == AR ) {
            cx = I2FX( cur[1] ) + dx;
            cy = I2FX( cur[2] ) + dy;
            ex = I2FX( cur[3] ) + dx;
            ey = I2FX( cur[4] ) + dy;
            for ( step = 1; step <= 6; step++ ) {
                sine = fx_sin( step * 15 );
                cosine = fx_cos( step * 15 );
                put( sl, 0, cx + fx_mul( px - cx, FX_ONE - sine ) + fx_mul( ex - cx, FX_ONE - cosine ),
                     cy + fx_mul( py - cy, FX_ONE - sine ) + fx_mul( ey - cy, FX_ONE - cosine ) );
            }
            px = ex;
            py = ey;
            cur += 5;
            continue;
        }
        px = I2FX( cur[0] ) + dx;
        py = I2FX( cur[1] ) + dy;
        put( sl, first, px, py );
        first = 0;
        cur += 2;
    }
}

/* The character, as a pen's path in thousandths of an em, drawn to be
   "advance" wide (0 for as wide as it is).  2 and the pen's width if
   there is something to stroke, 0 if the character is a space. */
int sfont_outline( uint16_t uni, int flags, int advance, fx *pen )
{
    SLANT sl;
    int parts[3], mark, count, index, natural = 0, upper;
    fx across, place;

    if ( !starts_made ) {
        starts_make();
    }
    count = pick( uni, flags, parts, &mark );
    if ( count == 0 ) {
        return 0;
    }
    for ( index = 0; index < count; index++ ) {
        natural += *where( parts[index], flags );
    }
    if ( natural == 0 ) {
        natural = 10;
    }
    /* the grid stretched or squeezed to the width asked for, within
       reason; what cannot be made up is left as space either side */
    sl.scale_x = 50 * FX_ONE;
    sl.left = 0;
    sl.flags = flags;
    if ( advance > 0 ) {
        sl.scale_x = (fx)(((int32_t)advance << 16) / natural);
        if ( sl.scale_x > 70 * FX_ONE ) {
            sl.scale_x = 70 * FX_ONE;
        }
        if ( sl.scale_x < 30 * FX_ONE ) {
            sl.scale_x = 30 * FX_ONE;
        }
        sl.left = (I2FX( advance ) - natural * sl.scale_x) / 2;
    }
    sl.shear = (flags & FF_ITALIC) ? 13107L : 0;        /* a fifth */
    across = 0;
    for ( index = 0; index < count; index++ ) {
        draw( &sl, parts[index], across, 0 );
        if ( index == 0 && mark >= 9 && mark < 9 + LATE_MARKS ) {
            /* one of the latest accents: mark_at[] says where each goes */
            upper = mark_at[mark - 9][1] && is_tall( parts[0], flags );
            place = I2FX( *where( parts[0], flags ) );
            if ( mark_at[mark - 9][0] == 0 ) {
                place = place / 2 + (mark == 12 ? 2 * FX_ONE : 0);      /* an ogonek hangs to the right */
            } else if ( mark_at[mark - 9][0] == 2 ) {
                place = 0;
            }
            draw( &sl, G_COUNT + mark - 9, place, upper ? 4 * FX_ONE : 0 );
        } else if ( index == 0 && mark ) {
            /* over the middle of the letter; higher over a capital */
            upper = parts[0] >= G_COUNT ? is_tall( parts[0], flags )
                                        : (parts[0] + 0x21 >= 'A' && parts[0] + 0x21 <= 'Z');
            draw( &sl, G_GRAVE + mark - 1, I2FX( *where( parts[0], flags ) ) / 2,
                  mark == 7 ? 0 : (upper ? 4 * FX_ONE : 0) );
        }
        across += I2FX( *where( parts[index], flags ) );
    }
    *pen = (flags & FF_BOLD) ? 135 * FX_ONE : ((flags & FF_FIXED) ? 70 * FX_ONE : 82 * FX_ONE);
    return 2;
}

/* ------------------------------------------------------------------ */
/* the widths of the standard fonts                                    */
/* ------------------------------------------------------------------ */

/* ASCII 32 to 126, in thousandths of an em */
static const uint16_t helvetica[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015,
    667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667,
    611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333,
    556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500,
    278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584 };
static const uint16_t helvetica_bold[95] = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611, 975,
    722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778, 667, 778, 722, 667,
    611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556, 333,
    556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611, 611, 611, 389, 556,
    333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584 };
static const uint16_t times_roman[95] = {
    250, 333, 408, 500, 500, 833, 778, 180, 333, 333, 500, 564, 250, 333, 250, 278,
    500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 278, 278, 564, 564, 564, 444, 921,
    722, 667, 667, 722, 611, 556, 722, 722, 333, 389, 722, 611, 889, 722, 722, 556, 722, 667, 556,
    611, 722, 722, 944, 722, 722, 611, 333, 278, 333, 469, 500, 333,
    444, 500, 444, 500, 444, 333, 500, 500, 278, 278, 500, 278, 778, 500, 500, 500, 500, 333, 389,
    278, 500, 500, 722, 500, 500, 444, 480, 200, 480, 541 };
static const uint16_t times_bold[95] = {
    250, 333, 555, 500, 500, 1000, 833, 278, 333, 333, 500, 570, 250, 333, 250, 278,
    500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 333, 333, 570, 570, 570, 500, 930,
    722, 667, 722, 722, 667, 611, 778, 778, 389, 500, 778, 667, 944, 722, 778, 611, 778, 722, 556,
    667, 722, 722, 1000, 722, 722, 667, 333, 278, 333, 581, 500, 333,
    500, 556, 444, 556, 444, 333, 500, 556, 278, 333, 556, 278, 833, 556, 500, 556, 556, 444, 389,
    333, 556, 500, 722, 500, 500, 444, 394, 220, 394, 520 };
static const uint16_t times_italic[95] = {
    250, 333, 420, 500, 500, 833, 778, 214, 333, 333, 500, 675, 250, 333, 250, 278,
    500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 333, 333, 675, 675, 675, 500, 920,
    611, 611, 667, 722, 611, 611, 722, 722, 333, 444, 667, 556, 833, 667, 722, 611, 722, 611, 500,
    556, 722, 611, 833, 611, 556, 556, 389, 278, 389, 422, 500, 333,
    500, 500, 444, 500, 444, 278, 500, 500, 278, 278, 444, 278, 722, 500, 500, 500, 500, 389, 389,
    278, 500, 444, 667, 444, 444, 389, 400, 275, 400, 541 };

/* how wide the standard font of these flags makes a character, for a
   file that leaves the widths to the reader */
int sfont_width( uint16_t uni, int flags )
{
    const uint16_t *table;
    int parts[3], mark, count, index, total = 0;

    if ( flags & FF_FIXED ) {
        return 600;
    }
    if ( flags & FF_SERIF ) {
        table = (flags & FF_BOLD) ? times_bold : ((flags & FF_ITALIC) ? times_italic : times_roman);
    } else {
        table = (flags & FF_BOLD) ? helvetica_bold : helvetica;
    }
    if ( uni == 0x00A0 ) {
        uni = 0x20;
    }
    if ( uni >= 0x20 && uni <= 0x7E ) {
        return table[uni - 0x20];
    }
    if ( uni >= 0xC0 && uni <= 0xFF && uni != 0xC6 && uni != 0xE6 ) {
        return table[latin_base[uni - 0xC0] - 0x20];
    }
    switch ( uni ) {
    case 0x2018: case 0x2019: case 0x201A:
        return table[','  - 0x20];
    case 0x201C: case 0x201D: case 0x201E:
        return table['"' - 0x20];
    case 0x2013:
        return 500;
    case 0x2014: case 0x2026:
        return 1000;
    case 0x2022:
        return 350;
    }
    if ( !starts_made ) {
        starts_make();
    }
    count = pick( uni, flags, parts, &mark );
    for ( index = 0; index < count; index++ ) {
        total += parts[index] < 95 ? table[parts[index] + 1] : *where( parts[index], flags ) * 50;
    }
    return count ? total : table[0];
}

/*
 * The character of ASCII that one of the latest drawings is as wide as:
 * the letter under an accent, or for a letter with a drawing of its own
 * the one late_like[] names.  0 for a character that is not one of
 * them, or is made of several drawings.  TF.C fills its widths with
 * this: a face's own figures for ASCII then serve for every alphabet.
 */
uint16_t sfont_like( uint16_t uni )
{
    const uint8_t *pair = late_pair( uni );

    if ( pair == NULL || pair[0] == 0 ) {
        return 0;
    }
    return (uint16_t)(pair[0] >= 0x80 ? late_like[pair[0] - 0x80] : pair[0]);
}
