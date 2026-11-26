/*
 * LAY.H - where the text goes: a story broken into lines, the lines
 * into pages.
 *
 * ONE LAYOUT FOR EVERY WAY OF SHOWING A DOCUMENT.  A LAY measures a
 * story for one of two devices.  The text screen: every character 144
 * twips wide and 240 high - ten to the inch, six lines to the inch, as
 * a typewriter has it - so that a twip position divided by those is a
 * column and a row.  Or type: each character as wide as its font says
 * (TF.H), which is what a graphics screen draws and what the PDF
 * writer puts on paper, from the same numbers, so that a line breaks
 * on paper where it breaks on the screen.
 *
 * All positions are twips.  Across, they count from the left edge of
 * the text area (the left margin of the page); down, from the top of
 * the story, as if it were one endless page - the pages are a list of
 * where each one starts.
 *
 * A paragraph's lines are kept with the paragraph and made again only
 * when the paragraph changes (WDOC.C drops them), so lay_update after
 * a keystroke measures one paragraph and adds up the rest.
 */
#ifndef LAY_H
#define LAY_H

#include "wdoc.h"

typedef struct LAY LAY;

/* a character cell of the text screen, in twips */
#define GRID_W          144
#define GRID_H          240

LAY  *lay_new( WDOC *doc, STORY *story, int grid );     /* NULL: no memory */
void  lay_free( LAY *lay );

/* the width the lines are broken to: 0 for the page's own (its width
   less its margins), which is where every LAY starts */
void  lay_set_width( LAY *lay, int width );
void  lay_show_hidden( LAY *lay, int show );
/* what the page-number fields say: for a header or footer, set before
   each page it is laid out for */
void  lay_set_page( LAY *lay, unsigned int page, unsigned int pages );
/* everything is to be measured again: the fonts, or the page, changed */
void  lay_stale( LAY *lay );
/* bring it up to date with the document.  0 if memory ran out: what
   could not be measured has no lines and is not shown. */
int   lay_update( LAY *lay );

int   lay_width( LAY *lay );            /* what the lines are broken to */
int   lay_height( LAY *lay );           /* of the whole story */

/* ------------------------------------------------------------------ */
/* lines                                                               */
/* ------------------------------------------------------------------ */
#define LL_PAGE         0x01    /* a page break comes after this line */
#define LL_LAST         0x02    /* the paragraph's last */

typedef struct {
    unsigned int start, len;  /* the characters on it; the lines of a paragraph
                                 take all of its text between them */
    int          x;           /* where its text starts */
    int          y;           /* its top */
    int          width;       /* of its text, without the spaces at its end */
    int          height;
    int          base;        /* the baseline, down from its top */
    uint32_t     flags;       /* LL_* */
} LINFO;

unsigned int lay_lines( LAY *lay, unsigned int para );          /* 0: not measured */
int          lay_line( LAY *lay, unsigned int para, unsigned int line, LINFO *out ); /* 0: no such line */

/* a paragraph's place: its top and how tall it is with the space above
   and below it; and of the block it is part of - itself, or the whole
   table row it is in, whose paragraphs are side by side */
void         lay_para_box( LAY *lay, unsigned int para, int *top, int *height, int *block_top, int *block_height );
/* the column it is set in: a table cell's, or the whole text area */
void         lay_para_col( LAY *lay, unsigned int para, int *col_x, int *col_width );
/* the first paragraph whose block reaches below "y": where drawing
   from a scroll position starts.  The story's count if there is none. */
unsigned int lay_para_at( LAY *lay, int y );

/* ------------------------------------------------------------------ */
/* places                                                              */
/* ------------------------------------------------------------------ */
unsigned int lay_line_of( LAY *lay, WPOS pos );
/* where the caret is for a place: the left of the character after it,
   the line's top and the line's height */
void         lay_caret( LAY *lay, WPOS pos, int *x, int *y, int *height );
/* the place nearest a point */
WPOS         lay_hit( LAY *lay, int x, int y );
/* ...and nearest "x" on a given line: how Up and Down keep to a column */
WPOS         lay_hit_line( LAY *lay, unsigned int para, unsigned int line, int x );

/* ------------------------------------------------------------------ */
/* pages                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned int para, line;  /* the first line on it */
    int          y;           /* ...and that line's top */
} LPAGE;

unsigned int lay_pages( LAY *lay );                     /* one at least */
const LPAGE *lay_page( LAY *lay, unsigned int index );  /* from 0 */
unsigned int lay_page_of( LAY *lay, WPOS pos );         /* from 0 */
int          lay_page_height( LAY *lay );               /* the text area's: page less margins */

/* ------------------------------------------------------------------ */
/* drawing a line                                                      */
/* ------------------------------------------------------------------ */
enum {
    SEG_TEXT,           /* characters of the paragraph, all of one look */
    SEG_TAB,            /* the gap a tab made; "lead" says what fills it */
    SEG_OBJECT,         /* a picture: "obj" is which, "width" by "height" */
    SEG_MADE            /* text that is not in the paragraph: a list's
                           bullet or number, a field's value, a note's
                           number, the hyphen a soft hyphen shows */
};

typedef struct {
    int             kind;     /* SEG_* */
    int             x, width;
    unsigned int    off, len; /* SEG_TEXT: which characters of the paragraph;
                                 the others: the character it stands for */
    const uint16_t *text;     /* SEG_TEXT and SEG_MADE: what to draw... */
    unsigned int    count;    /* ...and how many units */
    uint16_t        chp;      /* its look: a CHP number */
    uint8_t         lead;     /* SEG_TAB: LEAD_* */
    uint8_t         upper;    /* SEG_TEXT: draw it in capitals */
    int             size;     /* the type size to draw at, twips: smaller for
                                 superscripts, subscripts and small capitals */
    int             rise;     /* the baseline moved up by this (down if negative) */
    int             obj;      /* SEG_OBJECT */
    int             height;   /* SEG_OBJECT */
} LSEG;

typedef void (*LAYSEG)( void *ctx, const LSEG *seg );

/* every piece of a line, left to right.  A justified line comes as a
   SEG_TEXT for each word, already spread, so whoever draws need not
   know the line was justified. */
void  lay_segs( LAY *lay, unsigned int para, unsigned int line, LAYSEG fn, void *ctx );

/* a table row's ruling, for the row a paragraph is in: 0 if it is in
   none, or the row has no lines round its cells.  "edges" gets the x
   of each upright line, count + 1 of them; MAX_CELLS + 1 at most. */
int   lay_row_rules( LAY *lay, unsigned int para, int *top, int *height, int *edges, int *count );

#endif
