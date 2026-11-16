/*
 * TF.H - type faces: how wide a character is in a font, and its shape.
 *
 * PM-DOS has no fonts.  What PMWRITE has is the PDF code's built-in
 * face (PDF\SFONT.C: one drawing of each letter, stood up or slanted,
 * light or heavy, spaced as Times, Helvetica or Courier are) and, when
 * the user has put TrueType files where PMWRITE looks for them, those.
 * A TFACE is one of them chosen for a document's font and a style; the
 * layout asks it for widths, the screen and the PDF writer for shapes.
 *
 * Nothing here is floating point.  Widths are thousandths of an em -
 * of the type size - which is how the fonts' own tables count them.
 */
#ifndef TF_H
#define TF_H

#include "wdoc.h"

typedef struct TFACE TFACE;

/* which of the fourteen fonts every PDF reader has a face is nearest
   to: what the PDF writer names, and whose widths the built-in face
   uses */
enum { TFB_TIMES, TFB_HELVETICA, TFB_COURIER, TFB_SYMBOL };

void  tf_init( void );
void  tf_done( void );                  /* every face, font file and cached shape given back */

/*
 * Where TrueType files (*.TTF) are looked for.  Nothing is looked for
 * until this has been called; the looking is done once, when a face or
 * a family is first asked for after it.  "dirs" is directories with a
 * semicolon between them, or NULL.  With "usual", these are searched as
 * well: FONTS in the program's own directory before the ones named,
 * and after them \WINDOWS\SYSTEM and \WINDOWS\FONTS on drive C: and on
 * the program's drive.  The first 256 files found are used.
 */
void  tf_font_dirs( const char *dirs, int usual );

/* the families of the files that were found, in order of name: NULL
   past the last */
const char *tf_family( u32 index );

/*
 * The face for a font of the document in a style (CF_BOLD, CF_ITALIC;
 * other bits are ignored).  Never NULL.  A font whose name is the
 * family of a TrueType file that was found is that file's face - the
 * file of the style asked for, or else another of the family's with the
 * style made: sloped for italic, drawn twice for bold.  A font the
 * machine has not got is the built-in face spaced as its family is
 * (FAM_ROMAN and unknown as Times, FAM_SWISS as Helvetica, fixed-pitch
 * as Courier).  The same arguments give the same TFACE, so two can be
 * compared.
 */
TFACE *tf_face( const WFONT *font, int flags );

int   tf_base( const TFACE *face );     /* TFB_*: for a file's face, what it is nearest to */
int   tf_is_builtin( const TFACE *face );       /* 0 for a face that is a file's */
int   tf_flags( const TFACE *face );    /* the CF_BOLD | CF_ITALIC it was asked with */

/* how far a character moves the pen: thousandths of an em.  A
   character the face has no shape for is as wide as its stand-in. */
u32   tf_advance( TFACE *face, u32 code );

/* 1 if the face has a shape of its own for the character: not a space,
   and not the box that stands for a character it has not got */
int   tf_draws( TFACE *face, u32 code );

/*
 * The shape of a character of a built-in face as the path of its pen,
 * in thousandths of an em: "sink" is called for each point, with "move"
 * 1 where the pen is lifted to it.  *pen is how wide the pen is.  0 if
 * there is nothing to draw, or the character is one a font file draws:
 * of a file's face only the characters its stand-in draws have a path.
 */
int   tf_path( TFACE *face, u32 code, void (*sink)( void *ctx, int move, s32 x, s32 y ), void *ctx, s32 *pen );

/* above and below the baseline, and the extra between lines, in
   thousandths of an em: a line of 12 point type is
   (ascent + descent + gap) * 12 / 1000 points high */
void  tf_metrics( TFACE *face, int *ascent, int *descent, int *gap );

/*
 * Text drawn into a block of coverage: one byte a pixel, 0 where the
 * paper shows and 255 where a pixel is all ink, ADDED to what is there
 * (and held at 255).  "em" is the type size in pixels and "pen_x" and
 * "base_y" the start of the baseline, all 16.16.  Returns where the
 * pen ends.  What falls outside the block is not drawn.
 */
typedef struct {
    u8  *pix;
    int  width, height;
    u32  stride;
} COVER;

s32   tf_draw( TFACE *face, s32 em, s32 pen_x, s32 base_y, const u16 *text, u32 len, COVER *cover );

/*
 * What the coverage tf_draw makes is shown on.  "cut" is 0 for a
 * screen that shows shades of it, which is how it starts.  On a screen
 * where a pixel is ink or paper - sixteen colors - it is the coverage
 * from which a pixel is ink (GFX.C's gfx_cover_cut), and tf_draw then
 * keeps the strokes of a file's face that are too thin to reach it,
 * one pixel wide: an outline from a file is not fitted to the pixels,
 * and without this the bar of an e is there in one word and gone in
 * the next.  The built-in face is drawn as it always was.
 */
void  tf_screen( int cut );

/* when memory is short: cached shapes given back, and if there were
   none, the font files that are in memory (each is read again when it
   is next wanted).  1 if anything was. */
int   tf_reclaim( void );

/* ------------------------------------------------------------------ */
/* a file's face, for the PDF writer                                   */
/* ------------------------------------------------------------------ */

/* The built-in face that stands in for a file's face - the one that
   draws and measures the characters the file has not got - and for a
   built-in face, itself. */
TFACE *tf_standin( TFACE *face );

/* which font file a face is drawn from: NULL for a built-in face, and
   the same for two faces that are one file (a face and its made bold) */
void *tf_file( TFACE *face );

/* 1 if the file may go into a PDF: its maker allows it, and it has the
   tables a subset is made from */
int   tf_embed( TFACE *face );

/* the CF_BOLD | CF_ITALIC the file has not got and the face makes */
int   tf_made( TFACE *face );

/* the file's glyph for a character, which is also how far it moves the
   pen for tf_advance: 0 if the file has none, and the stand-in is used */
u32   tf_glyph( TFACE *face, u32 code );

/* a glyph's width in thousandths of an em */
u32   tf_glyph_advance( TFACE *face, u32 glyph );

/* what a PDF says about a font it holds: thousandths of an em */
typedef struct {
    char name[64];              /* what PostScript knows it by; never empty */
    u32  glyphs;
    s32  box[4];                /* left, bottom, right, top of every glyph */
    s32  ascent, descent, cap;
    s32  angle;                 /* degrees it leans, anticlockwise */
    int  fixed, serif, symbol;
} TFINFO;

int   tf_file_info( TFACE *face, TFINFO *info );        /* 0 for a built-in face */

/*
 * The file with the outlines of only the glyphs wanted - "keep" is a
 * byte a glyph, "count" of them; every glyph keeps its number - as a
 * block for xfree, *len long.  A file whose maker forbids a subset
 * comes back whole.  NULL if there is no memory, or the face is not a
 * file's that tf_embed allows.
 */
u8   *tf_subset( TFACE *face, const u8 *keep, u32 count, u32 *len );

#endif
