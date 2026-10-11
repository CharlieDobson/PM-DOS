/*
 * GFX.H - what is done with a page: its content stream interpreted and
 * drawn, or read for its text.
 *
 * A PAGE IS DRAWN INTO A CANVAS, which is memory, and the screen is
 * given the canvas afterwards (VIDEO.C).  A canvas is one byte a pixel
 * (grey, for the 16-colour VGA screen) or three (blue, green, red, for
 * every VBE mode), and it covers a rectangle of the page in pixels -
 * all of it when there is the memory, the part on the screen when
 * there is not.
 *
 * Everything is drawn with coverage: a shape's edges are sampled four
 * times a pixel down and exactly across (RASTER.C), so text is
 * readable at the sizes a 640 by 480 screen can show a page at.
 *
 * WHAT IS DRAWN.  Paths filled and stroked, with dashes, joins and
 * caps; clipping to any path; text in the file's own fonts - TrueType,
 * Type 1, CFF, Type 3, simple or composite - and in a built-in face
 * where the file has none (SFONT.C); every colour space but the ones
 * below; axial and radial shadings; images through Flate, LZW, DCT,
 * CCITT and the ASCII filters, with their masks; forms within forms;
 * constant alpha, and the blend modes Multiply, Screen, Darken and
 * Lighten.
 *
 * WHAT IS NOT, and what is there instead:
 *   a tiling pattern          a tint of its colour, or a pale grey
 *   a mesh shading, or one    nothing
 *     that is a function of
 *     both coordinates
 *   a soft mask in the        nothing: the thing is drawn unmasked
 *     graphics state
 *   a transparency group      its parts each made fainter (INTERP.C)
 *   a JBIG2 or JPEG 2000      a grey patch
 *     image; a progressive
 *     JPEG; an image that
 *     is tilted
 *   text as a clipping path   the text, where it would have been drawn
 *   a font keyed to one of    the built-in face's box for each character
 *     Adobe's Asian character
 *     collections that is not
 *     in the file
 */
#ifndef GFX_H
#define GFX_H

#include "pdf.h"

/* ------------------------------------------------------------------ */
/* the canvas                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    u8  *pix;
    int  width, height;
    int  bpp;                   /* 1: grey; 3: blue, green, red */
    u32  stride;
    int  org_x, org_y;          /* the page pixel at its top left corner */
} CANVAS;

void canvas_clear( CANVAS *cv, u32 rgb );

/* how a page lies on the screen */
typedef struct {
    RMAT base;                  /* the page's own space to page pixels */
    int  width, height;         /* the page, in pixels */
    fx   zoom;                  /* pixels to a point */
    int  rotate;                /* 0, 90, 180, 270: the page's and the user's */
} VIEW;

void view_setup( VIEW *view, PAGE *page, fx zoom, int turn );

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

#define BM_NORMAL       0
#define BM_MULTIPLY     1
#define BM_DARKEN       2
#define BM_SCREEN       3
#define BM_LIGHTEN      4

/* what a shape is filled with: one colour, or a colour for each pixel
   asked of "row" (a shading) */
typedef struct PAINT PAINT;
struct PAINT {
    u32   rgb;                  /* 00RRGGBB */
    u8    alpha;                /* 255 is opaque */
    u8    blend;                /* BM_* */
    u8    none;                 /* paints nothing: a /None separation */
    u8    spare;
    void (*row)( const PAINT *paint, int ypos, int xpos, int count, u8 *bgr );
    void *ctx;
};

typedef struct MASK MASK;

typedef struct {
    int   x0, y0, x1, y1;       /* a rectangle of the canvas: x1 and y1 outside */
    MASK *mask;                 /* and within it, a shape - or NULL */
} CLIP;

typedef void (*ROWFN)( void *ctx, int ypos, int x0, int x1, const u8 *cov );

void  ras_begin( void );
void  ras_line( fx x0, fx y0, fx x1, fx y1 );
int   ras_empty( void );
void  ras_scan( int cx0, int cy0, int cx1, int cy1, int evenodd, ROWFN fn, void *ctx );
void  ras_fill( CANVAS *cv, const CLIP *clip, int evenodd, const PAINT *paint );
void  paint_row( CANVAS *cv, const CLIP *clip, const PAINT *paint, int ypos, int x0, int x1,
                 const u8 *cov );
void  paint_rect( CANVAS *cv, const CLIP *clip, const PAINT *paint, int x0, int y0, int x1, int y1 );
void  paint_pixels( CANVAS *cv, const CLIP *clip, int ypos, int x0, int count, const u8 *bgr,
                    const u8 *alpha, int blend );
MASK *mask_make( const CLIP *clip, int evenodd );       /* from the edges given to ras_line */
MASK *mask_keep( MASK *mask );                          /* one more owner */
void  mask_drop( MASK *mask );                          /* one fewer */
void  mask_box( const MASK *mask, int *x0, int *y0, int *x1, int *y1 );
void  mask_row( const MASK *mask, int ypos, int x0, int x1, u8 *cov );
int   raster_reclaim( void );
u32   rgb_grey( u32 rgb );

/* ------------------------------------------------------------------ */
/* paths                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    fx x, y;
} PT;

#define PF_START    1           /* the first point of a subpath */
#define PF_CLOSED   2           /* ...which was closed */

typedef struct {
    PT  *pts;
    u8  *flags;
    u32  count, cap;
    u32  sub;                   /* where the subpath being built starts */
    fx   cur_x, cur_y;
    int  has_cur;
} PATH;

typedef struct {
    fx   width;                 /* in pixels */
    int  cap, join;             /* 0 butt, 1 round, 2 square; 0 miter, 1 round, 2 bevel */
    fx   miter;
    fx   dash[12];              /* in pixels */
    int  ndash;
    fx   phase;
} STROKE;

void  path_reset( PATH *path );
void  path_move( PATH *path, fx px, fx py );
void  path_line( PATH *path, fx px, fx py );
void  path_curve( PATH *path, fx x1, fx y1, fx x2, fx y2, fx x3, fx y3 );
void  path_close( PATH *path );
void  path_edges( const PATH *path );                   /* to ras_line, each subpath closed */
void  path_stroke_edges( const PATH *path, const STROKE *stroke );
int   path_as_rect( const PATH *path, int *x0, int *y0, int *x1, int *y1 );
void  path_bounds( const PATH *path, fx *x0, fx *y0, fx *x1, fx *y1 );

/* ------------------------------------------------------------------ */
/* colour                                                              */
/* ------------------------------------------------------------------ */

#define CS_GRAY     0
#define CS_RGB      1
#define CS_CMYK     2
#define CS_LAB      3
#define CS_INDEXED  4
#define CS_SEP      5           /* Separation and DeviceN */
#define CS_PATTERN  6

#define MAX_COMPS   8

typedef struct FUNC   FUNC;
typedef struct CSPACE CSPACE;

struct CSPACE {
    u8      kind;               /* CS_* */
    u8      ncomp;
    u8      none;               /* a separation called /None: paints nothing */
    u8      spare;
    CSPACE *base;               /* Indexed: what the table was in; Separation:
                                   the alternate; Pattern: the colour's space */
    u32    *table;              /* Indexed: finished colours, 00RRGGBB */
    int     hival;
    FUNC   *tint;
    fx      range[4];           /* Lab: a and b */
};

extern CSPACE cs_devgray, cs_devrgb, cs_devcmyk, cs_pattern;

CSPACE *cs_load( OBJ *obj, DICT *res );
u32   cs_rgb( CSPACE *cs, const fx *comps );
void  cs_initial( CSPACE *cs, fx *comps );
FUNC *func_load( OBJ *obj );
int   func_outputs( FUNC *fn );
void  func_eval( FUNC *fn, const fx *in, fx *out );

/* ------------------------------------------------------------------ */
/* names and encodings                                                 */
/* ------------------------------------------------------------------ */

#define ENC_WIN     0
#define ENC_STD     1
#define ENC_MAC     2
#define ENC_SYMBOL  3
#define ENC_BUILTIN 4           /* whatever the font program says */

const char *name_std( int sid );
u16   name_to_uni( const char *name );
const char *uni_to_name( u16 uni );
void  enc_base( int which, u16 *uni );
const char *enc_std_name( int code );
int   uni_to_cp437( u16 uni, char *out );

/* ------------------------------------------------------------------ */
/* fonts                                                               */
/* ------------------------------------------------------------------ */

#define FT_BUILTIN  0           /* no program in the file: the one built in */
#define FT_TRUETYPE 1
#define FT_CFF      2
#define FT_TYPE1    3
#define FT_TYPE3    4

#define FF_FIXED    0x01
#define FF_SERIF    0x02
#define FF_SYMBOL   0x04
#define FF_ITALIC   0x08
#define FF_BOLD     0x10
#define FF_DINGBATS 0x20

/* a character map: codes to character IDs, or codes to Unicode */
typedef struct {
    u32 lo, hi;
    u32 val;                    /* what "lo" maps to; the rest follow on */
    u16 more[3];                /* Unicode: the characters after the first */
    u16 spare;
} CMRANGE;

typedef struct {
    CMRANGE *ranges;
    u32      count, cap;
    struct {
        u32 lo, hi;
        int len;
    } space[8];                 /* which codes are how many bytes */
    int      nspace;
} CMAP;

typedef struct {
    u16  first, last;
    s16  width;                 /* of every one, or... */
    s16 *widths;                /* ...of each */
} WRANGE;

typedef struct FONT FONT;
struct FONT {
    FONT  *next;
    u32    key;                 /* the font dictionary's object number; 0 for one
                                   that lasts only as long as the page */
    u32    stamp;               /* when it was last used */
    POOL  *pool;
    u8     kind;                /* FT_* */
    u8     composite;           /* a Type 0 font: codes of more than a byte */
    u8     flags;               /* FF_* */
    u8     symbolic;
    void  *prog;                /* the font program, as its reader keeps it */
    int    units;               /* to an em */
    /* a simple font */
    u16    uni[256];
    const char *gname[256];
    s16    width[256];
    u16    gid[256];
    /* a composite one */
    int    dw;
    WRANGE *wr;
    u32    nwr;
    u8    *cid2gid;
    u32    cid2gid_len;
    CMAP  *enc;                 /* its encoding, unless that is Identity */
    int    cff_cid;             /* its CFF program is keyed by character ID */
    CMAP  *tou;                 /* ToUnicode */
    /* Type 3 */
    DICT  *t3_procs, *t3_res;
    RMAT   t3_matrix;
    DICT  *ident;               /* a page's own font: the dictionary it came from */
};

FONT *font_get( OBJ *ref );
u32   font_next_code( FONT *font, const u8 **str, u32 *left );
int   font_width( FONT *font, u32 code );           /* thousandths of the size */
int   font_unicode( FONT *font, u32 code, u16 *out );
int   font_outline( FONT *font, u32 code, fx *pen );
void  font_stamp( void );                          /* a new page */
int   font_reclaim( int all );                     /* 1 if any were given back */

CMAP *cmap_parse( OBJ *stm, POOL *pool );
int   cmap_find( const CMAP *cmap, u32 code, CMRANGE **range );

/* where a glyph's outline goes: font units in, pixels out */
typedef struct {
    s32   ma, mb, mc, md;       /* pixels to a font unit, 8.24 */
    fx    ox, oy;
    PATH *path;
    fx    last_x, last_y;       /* in font units */
} GLYPHOUT;

extern GLYPHOUT gout;

void  gl_move( fx ux, fx uy );
void  gl_line( fx ux, fx uy );
void  gl_curve( fx x1, fx y1, fx x2, fx y2, fx x3, fx y3 );
void  gl_quad( fx cx, fx cy, fx ux, fx uy );
void  gl_close( void );

void  glyph_draw( FONT *font, u32 code, fx xpos, fx ypos, const s32 *mat, CANVAS *cv,
                  const CLIP *clip, const PAINT *paint );
int   glyph_reclaim( void );
void  glyph_page_begin( void );

/* the font programs */
void *ttf_open( POOL *pool, u8 *data, u32 len );
int   ttf_units( void *prog );
int   ttf_cmap( void *prog, int platform, int encoding, u32 code );     /* a glyph, or 0 */
int   ttf_has_cmap( void *prog, int platform, int encoding );
int   ttf_outline( void *prog, u32 gid );
int   ttf_cff( void *prog, u8 **data, u32 *len );                       /* OpenType with CFF */
u16   ttf_unicode_of( void *prog, u32 gid );

void *cff_open( POOL *pool, u8 *data, u32 len );
int   cff_is_cid( void *prog );
int   cff_units( void *prog );
int   cff_glyph_named( void *prog, const char *name );      /* -1 if it has none */
int   cff_glyph_of_cid( void *prog, u32 cid );
const char *cff_builtin_name( void *prog, int code );
int   cff_outline( void *prog, u32 gid );

void *t1_open( POOL *pool, u8 *data, u32 len );
int   t1_glyph_named( void *prog, const char *name );
const char *t1_builtin_name( void *prog, int code );
int   t1_units( void *prog );
int   t1_outline( void *prog, u32 gid );

int   sfont_outline( u16 uni, int flags, int advance, fx *pen );
int   sfont_width( u16 uni, int flags );

/* ------------------------------------------------------------------ */
/* the page                                                            */
/* ------------------------------------------------------------------ */

/* IMAGE.C: an image, into the unit square that "fm" puts on the canvas */
typedef struct {
    OBJ    *stm;                /* an image XObject, or NULL for an inline one... */
    DICT   *dict;               /* ...whose dictionary this is... */
    IN     *inline_in;          /* ...and whose data is next to be read here */
    DICT   *res;
    const FMAT *fm;
    CANVAS *cv;
    const CLIP *clip;
    const PAINT *fill;          /* a stencil's colour; and any image's alpha */
} IMGREQ;

void  image_draw( IMGREQ *req );

/* SHADE.C: a shading, which is a colour for every point */
typedef struct SHADE SHADE;

SHADE *shade_load( OBJ *shading, const RMAT *ctm, DICT *res );
void  shade_paint( SHADE *shade, PAINT *paint );
void  shade_fill( SHADE *shade, CANVAS *cv, const CLIP *clip, int alpha, int blend );

/* INTERP.C: draw the page.  "poll" is called now and then, and a
   return of 1 from it gives the page up (PE_ABORT). */
void  page_render( PAGE *page, VIEW *view, CANVAS *cv );
extern int (*render_poll)( void );

/* TEXT.C: the page as lines of text, in the screen's character set */
typedef struct {
    int    count;
    char **lines;
} TEXTPAGE;

TEXTPAGE *text_extract( int index );

/* what INTERP.C hands each character of a page to, when it is reading
   and not drawing: where it is in points from the page's top left
   corner, how far it advances and how tall its font is */
extern void (*text_sink)( u16 uni, fx xpos, fx ypos, fx advance, fx size );
void  page_text( PAGE *page, VIEW *view );

int   cache_reclaim( void );

#endif
