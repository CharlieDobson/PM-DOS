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
    uint8_t     *pix;
    int          width, height;
    int          bpp;           /* 1: grey; 3: blue, green, red */
    unsigned int stride;
    int          org_x, org_y;  /* the page pixel at its top left corner */
} CANVAS;

void canvas_clear( CANVAS *cv, uint32_t rgb );

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
    uint32_t rgb;               /* 00RRGGBB */
    uint8_t  alpha;             /* 255 is opaque */
    uint8_t  blend;             /* BM_* */
    uint8_t  none;              /* paints nothing: a /None separation */
    uint8_t  spare;
    void (*row)( const PAINT *paint, int ypos, int xpos, int count, uint8_t *bgr );
    void *ctx;
};

typedef struct MASK MASK;

typedef struct {
    int   x0, y0, x1, y1;       /* a rectangle of the canvas: x1 and y1 outside */
    MASK *mask;                 /* and within it, a shape - or NULL */
} CLIP;

typedef void (*ROWFN)( void *ctx, int ypos, int x0, int x1, const uint8_t *cov );

void  ras_begin( void );
void  ras_line( fx x0, fx y0, fx x1, fx y1 );
int   ras_empty( void );
void  ras_scan( int cx0, int cy0, int cx1, int cy1, int evenodd, ROWFN fn, void *ctx );
void  ras_fill( CANVAS *cv, const CLIP *clip, int evenodd, const PAINT *paint );
void  paint_row( CANVAS *cv, const CLIP *clip, const PAINT *paint, int ypos, int x0, int x1,
                 const uint8_t *cov );
void  paint_rect( CANVAS *cv, const CLIP *clip, const PAINT *paint, int x0, int y0, int x1, int y1 );
void  paint_pixels( CANVAS *cv, const CLIP *clip, int ypos, int x0, int count, const uint8_t *bgr,
                    const uint8_t *alpha, int blend );
MASK    *mask_make( const CLIP *clip, int evenodd );    /* from the edges given to ras_line */
MASK    *mask_keep( MASK *mask );                       /* one more owner */
void     mask_drop( MASK *mask );                       /* one fewer */
void     mask_box( const MASK *mask, int *x0, int *y0, int *x1, int *y1 );
void     mask_row( const MASK *mask, int ypos, int x0, int x1, uint8_t *cov );
int      raster_reclaim( void );
uint32_t rgb_grey( uint32_t rgb );

/* ------------------------------------------------------------------ */
/* paths                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    fx x, y;
} PT;

#define PF_START    1           /* the first point of a subpath */
#define PF_CLOSED   2           /* ...which was closed */

typedef struct {
    PT          *pts;
    uint8_t     *flags;
    unsigned int count, cap;
    unsigned int sub;           /* where the subpath being built starts */
    fx           cur_x, cur_y;
    int          has_cur;
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
void  path_free( PATH *path );                          /* its memory; it can be built again */
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
    uint8_t   kind;             /* CS_* */
    uint8_t   ncomp;
    uint8_t   none;             /* a separation called /None: paints nothing */
    uint8_t   spare;
    CSPACE   *base;             /* Indexed: what the table was in; Separation:
                                   the alternate; Pattern: the colour's space */
    uint32_t *table;            /* Indexed: finished colours, 00RRGGBB */
    int       hival;
    FUNC     *tint;
    fx        range[4];         /* Lab: a and b */
};

extern CSPACE cs_devgray, cs_devrgb, cs_devcmyk, cs_pattern;

CSPACE  *cs_load( OBJ *obj, DICT *res );
uint32_t cs_rgb( CSPACE *cs, const fx *comps );
void     cs_initial( CSPACE *cs, fx *comps );
FUNC    *func_load( OBJ *obj );
int      func_outputs( FUNC *fn );
void     func_eval( FUNC *fn, const fx *in, fx *out );

/* ------------------------------------------------------------------ */
/* names and encodings                                                 */
/* ------------------------------------------------------------------ */

#define ENC_WIN     0
#define ENC_STD     1
#define ENC_MAC     2
#define ENC_SYMBOL  3
#define ENC_BUILTIN 4           /* whatever the font program says */

const char *name_std( int sid );
uint16_t name_to_uni( const char *name );
const char *uni_to_name( uint16_t uni );
void  enc_base( int which, uint16_t *uni );
const char *enc_std_name( int code );
int   uni_to_cp437( uint16_t uni, char *out );

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
#define FF_MORE     0x40        /* the built-in face's later drawings too (SFONT.C):
                                   TF.C's faces ask, a PDF's fonts never do */

/* a character map: codes to character IDs, or codes to Unicode */
typedef struct {
    uint32_t lo, hi;
    uint32_t val;               /* what "lo" maps to; the rest follow on */
    uint16_t more[3];           /* Unicode: the characters after the first */
    uint16_t spare;
} CMRANGE;

typedef struct {
    CMRANGE     *ranges;
    unsigned int count, cap;
    struct {
        uint32_t lo, hi;
        int len;
    } space[8];                 /* which codes are how many bytes */
    int          nspace;
} CMAP;

typedef struct {
    uint16_t first, last;
    int16_t  width;             /* of every one, or... */
    int16_t *widths;            /* ...of each */
} WRANGE;

typedef struct FONT FONT;
struct FONT {
    FONT        *next;
    uint32_t     key;           /* the font dictionary's object number; 0 for one
                                   that lasts only as long as the page */
    uint32_t     stamp;         /* when it was last used */
    POOL        *pool;
    uint8_t      kind;          /* FT_* */
    uint8_t      composite;     /* a Type 0 font: codes of more than a byte */
    uint8_t      flags;         /* FF_* */
    uint8_t      symbolic;
    void        *prog;          /* the font program, as its reader keeps it */
    int          units;         /* to an em */
    /* a simple font */
    uint16_t     uni[256];
    const char  *gname[256];
    int16_t      width[256];
    uint16_t     gid[256];
    /* a composite one */
    int          dw;
    WRANGE      *wr;
    unsigned int nwr;
    uint8_t     *cid2gid;
    uint32_t     cid2gid_len;
    CMAP        *enc;           /* its encoding, unless that is Identity */
    int          cff_cid;       /* its CFF program is keyed by character ID */
    CMAP        *tou;           /* ToUnicode */
    /* Type 3 */
    DICT        *t3_procs, *t3_res;
    RMAT         t3_matrix;
    DICT        *ident;         /* a page's own font: the dictionary it came from */
    const char  *name;          /* its BaseFont, without a subset's six letters and
                                   plus sign; NULL for a font made by hand */
};

FONT    *font_get( OBJ *ref );
uint32_t font_next_code( FONT *font, const uint8_t **str, unsigned int *left );
int      font_width( FONT *font, uint32_t code );   /* thousandths of the size */
int      font_unicode( FONT *font, uint32_t code, uint16_t *out );
int      font_outline( FONT *font, uint32_t code, fx *pen );
void     font_stamp( void );                       /* a new page */
int      font_reclaim( int all );                  /* 1 if any were given back */

CMAP *cmap_parse( OBJ *stm, POOL *pool );
int   cmap_find( const CMAP *cmap, uint32_t code, CMRANGE **range );

/* where a glyph's outline goes: font units in, pixels out */
typedef struct {
    int32_t ma, mb, mc, md;     /* pixels to a font unit, 8.24 */
    fx      ox, oy;
    PATH   *path;
    fx      last_x, last_y;     /* in font units */
} GLYPHOUT;

extern GLYPHOUT gout;

void  gl_move( fx ux, fx uy );
void  gl_line( fx ux, fx uy );
void  gl_curve( fx x1, fx y1, fx x2, fx y2, fx x3, fx y3 );
void  gl_quad( fx cx, fx cy, fx ux, fx uy );
void  gl_close( void );

void  glyph_draw( FONT *font, uint32_t code, fx xpos, fx ypos, const int32_t *mat, CANVAS *cv,
                  const CLIP *clip, const PAINT *paint );
/* the same glyph, from the same cache, as coverage and not as paint:
   "fn" is handed each row of it that lies inside x0, y0 to x1, y1.
   For a caller that keeps its own kind of picture (TF.C). */
void  glyph_cover( FONT *font, uint32_t code, fx xpos, fx ypos, const int32_t *mat, int x0, int y0,
                   int x1, int y1, ROWFN fn, void *ctx );
int   glyph_reclaim( void );
void  glyph_page_begin( void );

/* the font programs */
void    *ttf_open( POOL *pool, uint8_t *data, uint32_t len );
int      ttf_units( void *prog );
int      ttf_cmap( void *prog, int platform, int encoding, uint32_t code );  /* a glyph, or 0 */
int      ttf_has_cmap( void *prog, int platform, int encoding );
int      ttf_outline( void *prog, uint32_t gid );
int      ttf_cff( void *prog, uint8_t **data, uint32_t *len );               /* OpenType with CFF */
uint16_t ttf_unicode_of( void *prog, uint32_t gid );

/* a font file of the user's, whole and in memory (TF.C): TTF.C says
   what each of these is */
typedef struct {
    int      units;                 /* to an em */
    int      glyphs;
    int32_t  box[4];                /* left, bottom, right, top: these and the rest in units */
    int32_t  ascent, descent, gap;  /* a line's, as Windows spaces lines */
    int32_t  cap;                   /* how tall a capital is */
    int32_t  angle;                 /* degrees its uprights lean, anticlockwise */
    uint16_t fstype;                /* what its maker allows: the OS/2 table's fsType */
    uint8_t  fixed;                 /* every character as wide as every other */
    uint8_t  whole;                 /* every table a subset is made from is there */
    char     name[64];              /* what PostScript knows it by; "" if it does not say */
} TTFACTS;

void    *ttf_open_heap( uint8_t *data, uint32_t len );
int      ttf_glyph_count( void *prog );
uint32_t ttf_glyph_of( void *prog, uint32_t code );
int      ttf_advance( void *prog, uint32_t gid );
int      ttf_name( const uint8_t *table, uint32_t len, int id, char *out, unsigned int max );
void     ttf_facts( void *prog, TTFACTS *facts );
uint8_t *ttf_subset( void *prog, const uint8_t *keep, unsigned int count, uint32_t *out_len );

void *cff_open( POOL *pool, uint8_t *data, uint32_t len );
int   cff_is_cid( void *prog );
int   cff_units( void *prog );
int   cff_glyph_named( void *prog, const char *name );      /* -1 if it has none */
int   cff_glyph_of_cid( void *prog, uint32_t cid );
const char *cff_builtin_name( void *prog, int code );
int   cff_outline( void *prog, uint32_t gid );

void *t1_open( POOL *pool, uint8_t *data, uint32_t len );
int   t1_glyph_named( void *prog, const char *name );
const char *t1_builtin_name( void *prog, int code );
int   t1_units( void *prog );
int   t1_outline( void *prog, uint32_t gid );

int      sfont_outline( uint16_t uni, int flags, int advance, fx *pen );
int      sfont_width( uint16_t uni, int flags );
uint16_t sfont_like( uint16_t uni );            /* SFONT.C says what these are */
extern   void (*sfont_sink)( void *ctx, int move, fx ux, fx uy );
extern void  *sfont_sink_ctx;
/* 1, as it starts: a PDF's fonts get the drawings ACROREAD gave them,
   which for the accents, the dashes, the bullet and the quotation marks
   were each the wrong one (SFONT.C says how).  0: the right ones, and
   the later drawings too - the copyright sign, the pound, the euro. */
extern int sfont_as_acroread;

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
extern void (*text_sink)( uint16_t uni, fx xpos, fx ypos, fx advance, fx size );
void  page_text( PAGE *page, VIEW *view );

/* ...and a second sink, for a reader that keeps the look of the text
   as well as the text (F_PDF.C).  Either may be set, or both.  "name"
   is the font's own string and goes when the font does: it is good
   until the page ends, and whoever wants it longer copies it. */
typedef struct {
    uint16_t uni;
    uint8_t  flags;             /* FF_* of its font */
    uint8_t  render;            /* the text rendering mode: 3 does not show */
    fx       xpos, ypos;        /* as text_sink's */
    fx       advance, size;
    fx       dir_x, dir_y;      /* which way the line runs: (FX_ONE, 0) is across */
    uint32_t rgb;               /* the color it is drawn in, 00RRGGBB */
    const char *name;           /* its font's BaseFont without a subset's prefix; ""
                                   when the file gives it none */
} TEXTCH;
extern void (*text_sink2)( const TEXTCH *ch );
extern int text_images;         /* how many images page_text went past */

int   cache_reclaim( void );

#endif
