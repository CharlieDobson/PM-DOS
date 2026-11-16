/*
 * WDOC.H - the document, as every part of PMWRITE holds it.
 *
 * A file of any format is read INTO one of these and written FROM one,
 * so a format's code (F_*.C) knows its own file and this header and
 * nothing else; and the screen, the printer and the editing commands
 * know only this.
 *
 * WHAT A DOCUMENT IS.  Stories - the body, the running header, the
 * running footer, and one for each footnote - and a story is a list of
 * paragraphs.  A paragraph is its text, a u16 for each UTF-16 unit,
 * and beside each unit the number of its character properties (a CHP:
 * font, size, bold and so on).  Equal CHPs share one number, so the
 * numbers of two characters are equal exactly when the characters look
 * alike.  The paragraph mark is not stored: a paragraph ends where its
 * text does, and its own properties (a PAP: alignment, indents, tabs,
 * spacing, its place in a table) are in the paragraph.
 *
 * LENGTHS are twips, 1440 to the inch, 20 to the point.  Type sizes
 * are half points, as Word counts them: 24 is 12 point.
 *
 * MEMORY.  Nothing here ends the program when memory runs out.  A call
 * that cannot be done sets the document's "failed" flag and does
 * nothing, and every later call that would add to the document does
 * nothing either; so a reader need not test each call - it reads to
 * the end and asks wd_failed() once.
 */
#ifndef WDOC_H
#define WDOC_H

#include "types.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* characters that are not letters                                     */
/* ------------------------------------------------------------------ */
#define WCH_TAB         0x0009
#define WCH_LINE        0x000B  /* a new line in the same paragraph */
#define WCH_PAGE        0x000C  /* what follows starts a page */
#define WCH_NBSP        0x00A0  /* a space no line is broken at */
#define WCH_SHY         0x00AD  /* a hyphen that shows only at a line's end */
#define WCH_NBHYPHEN    0x2011  /* a hyphen no line is broken at */
#define WCH_OBJ         0xFFFC  /* an object: the paragraph's anchors say which */
/* fields, each one character whose text is made when it is shown */
#define WCH_PAGENUM     0xE000  /* the page's number */
#define WCH_NUMPAGES    0xE001  /* how many pages there are */
#define WCH_DATE        0xE002  /* today's date */
#define WCH_TIME        0xE003  /* the time now */
#define WCH_FILENAME    0xE004  /* the document's file name */
#define WCH_IS_FIELD( ch )  ((ch) >= 0xE000 && (ch) <= 0xE004)

/* ------------------------------------------------------------------ */
/* character properties                                                */
/* ------------------------------------------------------------------ */
#define CF_BOLD         0x0001
#define CF_ITALIC       0x0002
#define CF_UNDER        0x0004  /* a single line under words and spaces */
#define CF_STRIKE       0x0008
#define CF_SUPER        0x0010
#define CF_SUB          0x0020
#define CF_HIDDEN       0x0040
#define CF_SMALLCAPS    0x0080
#define CF_ALLCAPS      0x0100
#define CF_DUNDER       0x0200  /* a double line under it */
#define CF_WUNDER       0x0400  /* words underlined, the spaces not */
#define CF_OUTLINE      0x0800
#define CF_SHADOW       0x1000

#define COLOR_AUTO      0xFF000000UL    /* the window's own text colour */
#define RGB( red, green, blue ) \
    (((u32)(red) << 16) | ((u32)(green) << 8) | (u32)(blue))

typedef struct {
    u16 font;           /* which of the document's fonts */
    u16 size;           /* half points */
    u16 flags;          /* CF_* */
    u8  hilite;         /* 0 for none, or 1 + one of the sixteen colours */
    u8  spare;          /* always 0, so that equal CHPs compare equal */
    u32 color;          /* 00RRGGBB, or COLOR_AUTO */
} CHP;

/* what kind of face a font is, for when the machine has not got it */
enum { FAM_ANY, FAM_ROMAN, FAM_SWISS, FAM_MODERN, FAM_SCRIPT, FAM_DECOR, FAM_SYMBOL };

#define FONT_NAME_LEN   40

typedef struct {
    char name[FONT_NAME_LEN];   /* "Times New Roman", in ASCII */
    u8   family;                /* FAM_* */
    u8   fixed;                 /* every character the same width */
    u16  spare;
} WFONT;

/* ------------------------------------------------------------------ */
/* paragraph properties                                                */
/* ------------------------------------------------------------------ */
enum { AL_LEFT, AL_CENTER, AL_RIGHT, AL_JUSTIFY };

#define PF_KEEP         0x0001  /* its lines stay on one page */
#define PF_KEEPNEXT     0x0002  /* ...and on the next paragraph's page */
#define PF_PAGEBEFORE   0x0004  /* it starts a page */
#define PF_INTABLE      0x0008  /* it is in a table cell */
#define PF_CELLEND      0x0010  /* ...and is the last one in its cell */
#define PF_ROWEND       0x0020  /* ...and that cell is the last in its row:
                                   pap.row says where the row's cells are */
#define PF_BORDER_TOP   0x0100  /* a rule above it, below, to its left... */
#define PF_BORDER_BOT   0x0200
#define PF_BORDER_LEFT  0x0400
#define PF_BORDER_RIGHT 0x0800

enum { TAB_LEFT, TAB_CENTER, TAB_RIGHT, TAB_DECIMAL };
enum { LEAD_NONE, LEAD_DOTS, LEAD_DASHES, LEAD_LINE };

#define MAX_TABS        40

typedef struct {
    s32 pos;            /* from the left margin */
    u8  kind;           /* TAB_* */
    u8  lead;           /* LEAD_* */
    u16 spare;
} WTAB;

/* how the lines of a paragraph are spaced */
enum {
    LS_MULTIPLE,        /* "line" 240ths of the lines' own height: 240 is
                           single spacing, 360 one and a half, 480 double */
    LS_ATLEAST,         /* "line" twips, or the line's own height if more */
    LS_EXACT            /* "line" twips whatever is on the line */
};

enum { LIST_NONE, LIST_BULLET, LIST_NUMBER, LIST_LOWER, LIST_UPPER,
       LIST_ROMAN_LOWER, LIST_ROMAN_UPPER };

#define MAX_CELLS       32

/* a table row: where each cell ends, from the left margin */
typedef struct {
    u8  ncells;
    u8  borders;        /* 1 = lines round the cells */
    u8  header;         /* 1 = the row is repeated on each page */
    u8  spare;
    s32 left;           /* where the first cell starts */
    s32 edge[MAX_CELLS];
} TROW;

typedef struct {
    u8  align;          /* AL_* */
    u8  style;          /* which of the document's styles */
    u16 flags;          /* PF_* */
    s32 left, right;    /* indents, from the margins inward */
    s32 first;          /* the first line's own, added to "left": negative
                           is a hanging indent */
    u16 before, after;  /* space above and below */
    u16 line;           /* line spacing: LS_* says what it counts */
    u8  line_rule;      /* LS_* */
    u8  list;           /* LIST_* */
    u8  level;          /* a list's depth, 0 the outermost */
    u8  ntabs;
    u16 list_start;     /* a numbered list restarts with this number; 0: it
                           carries on from the paragraph before */
    WTAB *tabs;         /* the paragraph's own block, or NULL */
    TROW *row;          /* PF_ROWEND: the paragraph's own block */
} PAP;

/* ------------------------------------------------------------------ */
/* styles                                                              */
/* ------------------------------------------------------------------ */
/* the styles every format's own names are matched to */
enum { STY_NORMAL, STY_HEADING1, STY_HEADING2, STY_HEADING3, STY_HEADING4,
       STY_HEADING5, STY_HEADING6, STY_TITLE, STY_SUBTITLE, STY_HEADER,
       STY_FOOTER, STY_FOOTNOTE, STY_OTHER };

#define STYLE_NAME_LEN  32
#define MAX_STYLES      64

typedef struct {
    char name[STYLE_NAME_LEN];
    u8   kind;          /* STY_* */
    u8   spare[3];
    CHP  chp;           /* what text in a paragraph of this style starts as */
    PAP  pap;           /* tabs and row are NULL */
} WSTYLE;

/* ------------------------------------------------------------------ */
/* objects                                                             */
/* ------------------------------------------------------------------ */
enum { OBJ_IMAGE = 1, OBJ_NOTE };
enum { IMG_UNKNOWN, IMG_PNG, IMG_JPEG, IMG_BMP, IMG_GIF, IMG_TIFF, IMG_WMF,
       IMG_EMF, IMG_PICT, IMG_WPG, IMG_DIB };
enum { NOTE_FOOT, NOTE_END };

typedef struct STORY STORY;

typedef struct {
    u8   kind;          /* OBJ_* */
    u8   fmt;           /* IMG_* or NOTE_* */
    u16  spare;
    u8  *data;          /* an image: the picture file's bytes as they came */
    u32  len;
    s32  width, height; /* the size it is shown at */
    STORY *story;       /* a note: its text */
} WOBJ;

/* where in a paragraph an object is */
typedef struct {
    u32 off;            /* the WCH_OBJ character */
    u32 obj;            /* which of the document's objects */
} WANCH;

/* ------------------------------------------------------------------ */
/* paragraphs and stories                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    u16  *text;         /* UTF-16 */
    u16  *attr;         /* a CHP number for each unit of text */
    u32   len, cap;
    PAP   pap;
    WANCH *anch;        /* its objects, in order */
    u32   nanch, anch_cap;
    void *lay;          /* LAY.C's: its lines, when they have been made */
} WPARA;

struct STORY {
    WPARA **para;
    u32     count, cap;
};

/* ------------------------------------------------------------------ */
/* the page                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    s32 width, height;                  /* the paper, as it is read */
    s32 left, right, top, bottom;       /* margins */
    s32 header, footer;                 /* from the paper's edge to each */
    u16 first_page;                     /* the number the first page has */
    u8  landscape;
    u8  spare;
} PAGESETUP;

/* ------------------------------------------------------------------ */
/* the document                                                        */
/* ------------------------------------------------------------------ */
#define INFO_LEN        128

/* what a file had that the document could not keep */
#define LOST_IMAGES     0x0001  /* pictures of a kind that cannot be shown */
#define LOST_TABLES     0x0002  /* a table's layout: its text was kept */
#define LOST_OBJECTS    0x0004  /* drawings, equations, embedded objects */
#define LOST_FIELDS     0x0008  /* fields kept as their last result */
#define LOST_LAYOUT     0x0010  /* columns, frames, text boxes */
#define LOST_NOTES      0x0020  /* footnotes or comments */
#define LOST_REVISIONS  0x0040  /* tracked changes: taken as accepted */
#define LOST_TEXT       0x0080  /* characters with no Unicode equivalent */
#define LOST_STYLES     0x0100  /* a style sheet in another file */

typedef struct WDOC {
    STORY   body, header, footer;
    WOBJ   *obj;
    u32     nobj, obj_cap;
    CHP    *chp;                /* chp[0] is the document's default */
    u32     nchp, chp_cap;
    u16    *chp_hash;           /* wd_chp's: numbers + 1, 0 for an empty place */
    u32     chp_hash_cap;
    WFONT  *font;
    u32     nfont, font_cap;
    WSTYLE *style;              /* style[0] is Normal */
    u32     nstyle;
    PAGESETUP page;
    s32     deftab;             /* tabs every so far, where none is set */
    char    title[INFO_LEN];    /* these four are UTF-8 */
    char    author[INFO_LEN];
    char    subject[INFO_LEN];
    char    keywords[INFO_LEN];
    int     format;             /* FMT_* (FMT.H): what it was read from */
    int     variant;            /* ...and which kind of that */
    u32     lost;               /* LOST_* */
    int     failed;             /* memory ran out while it was being made */
    void   *aux;                /* the format's own, kept for saving it back */
    void  (*aux_free)( void *aux );
    void   *undo;               /* WEDIT.C's */
    u32     serial;             /* counts the changes WEDIT.C makes */
} WDOC;

/* a place in a story: between characters, 0 before the first */
typedef struct {
    u32 para;
    u32 off;
} WPOS;

/* ------------------------------------------------------------------ */
/* making and reading one (WDOC.C)                                     */
/* ------------------------------------------------------------------ */

/*
 * A new document: US Letter, margins of an inch (1.25 at the sides),
 * tabs every half inch, one font ("Times New Roman"), the style Normal
 * at 12 point, and NO paragraphs - wd_finish gives the body, and each
 * note, an empty one if it has none.  (A header or footer with no
 * paragraphs is a document without one.)  NULL if there is no memory
 * even for that.
 */
WDOC *wd_new( void );
void  wd_free( WDOC *doc );
int   wd_failed( const WDOC *doc );
void  wd_finish( WDOC *doc );           /* after a reader is done with it */

/* a font by name, added if the document has not got it -> its number.
   "family" is FAM_* and may be FAM_ANY; "fixed" 1, 0, or -1 for "to be
   guessed from the name". */
int   wd_font( WDOC *doc, const char *name, int family, int fixed );

/* the number of a CHP, added if it is new.  0, the default's, if there
   is no memory (and the document is marked failed). */
u16   wd_chp( WDOC *doc, const CHP *chp );
const CHP *wd_chp_get( const WDOC *doc, u16 id );

/* a style by name, added with "kind" if it is not there -> its number.
   A new one starts as a copy of Normal.  STY_NORMAL..STY_FOOTNOTE are
   found by kind whatever the file called them. */
int   wd_style( WDOC *doc, const char *name, int kind );

void  pap_init( PAP *pap );             /* left-aligned, single spaced, no tabs */
/* a copy that owns its own tabs and row; 0 if there was no memory */
int   pap_copy( PAP *dst, const PAP *src );
void  pap_free( PAP *pap );             /* its tabs and row */
int   pap_set_tabs( PAP *pap, const WTAB *tabs, int count );    /* sorted, copied */
int   pap_set_row( PAP *pap, const TROW *row );                 /* copied */

/* a new paragraph at the end of a story (or NULL): its PAP is a copy of
   "pap", or pap_init's when that is NULL */
WPARA *wd_para_add( WDOC *doc, STORY *story, const PAP *pap );
/* ...and anywhere: before the paragraph that is "index" now */
WPARA *wd_para_insert( WDOC *doc, STORY *story, u32 index, const PAP *pap );
void  wd_para_remove( WDOC *doc, STORY *story, u32 index );

/* text put into a paragraph at "off", all of it with one CHP */
int   wd_text_insert( WDOC *doc, WPARA *para, u32 off, const u16 *text, u32 len, u16 chp );
/* ...or each unit with its own */
int   wd_text_insert2( WDOC *doc, WPARA *para, u32 off, const u16 *text, const u16 *attr, u32 len );
void  wd_text_delete( WDOC *doc, WPARA *para, u32 off, u32 len );
#define wd_text_add( doc, para, text, count, chp ) \
    wd_text_insert( (doc), (para), (para)->len, (text), (count), (chp) )

/* an object: wd_obj_new makes one and returns its number (-1 if it
   could not); wd_obj_put sets it in a paragraph as a WCH_OBJ character */
int   wd_image_new( WDOC *doc, const u8 *data, u32 len, int fmt, s32 width, s32 height );
int   wd_note_new( WDOC *doc, int kind );           /* doc->obj[n].story is empty */
int   wd_obj_put( WDOC *doc, WPARA *para, u32 off, int obj, u16 chp );
/* the object at a WCH_OBJ character, or -1 */
int   wd_obj_at( const WPARA *para, u32 off );

/* the story a note's text is in */
STORY *wd_note_story( WDOC *doc, int obj );

void  wd_set_aux( WDOC *doc, void *aux, void (*aux_free)( void *aux ) );

/* LAY.C's: how a paragraph's lines are given back when the paragraph
   goes or its text changes.  NULL until LAY.C sets it. */
extern void (*wd_lay_drop)( WPARA *para );
/* WEDIT.C's: how a document's undo history is given back */
extern void (*wd_undo_drop)( struct WDOC *doc );

/* text put back as it was taken out: WCH_OBJ characters kept, with
   "anch" saying which object each is ("off" counted from the start of
   the text being put back).  WEDIT.C's undo is what this is for. */
int   wd_text_restore( WDOC *doc, WPARA *para, u32 off, const u16 *text, const u16 *attr,
                       u32 len, const WANCH *anch, u32 nanch );
/* a paragraph's look changed some other way: its lines are stale */
void  wd_para_touch( WPARA *para );

/* ------------------------------------------------------------------ */
/* a reader's helper: text as it comes                                 */
/* ------------------------------------------------------------------ */

/*
 * Most files are text with changes of property in between, so a reader
 * keeps one of these: it sets "chp" and "pap" as the file says, hands
 * over characters, and says where each paragraph ends.  The paragraph
 * is given the PAP that is current WHEN IT ENDS, which is how every
 * word processor's file works.
 */
typedef struct {
    WDOC  *doc;
    STORY *story;
    WPARA *para;        /* the paragraph being filled, or NULL */
    CHP    chp;         /* what the next character will have */
    PAP    pap;         /* what the paragraph will have (tabs and row are
                           the builder's own blocks) */
    u16    chp_id;      /* wb_'s own: the number of "chp" when it was */
    CHP    chp_was;     /* last asked for */
    int    chp_known;
} WBUILD;

void  wb_init( WBUILD *wb, WDOC *doc, STORY *story );   /* Normal's CHP and PAP */
void  wb_done( WBUILD *wb );            /* ends a paragraph that has text; frees */
void  wb_char( WBUILD *wb, u32 code );  /* a Unicode character, of any plane */
void  wb_text( WBUILD *wb, const u16 *text, u32 len );
void  wb_bytes( WBUILD *wb, const u8 *text, u32 len, int codepage );   /* CP.H */
void  wb_utf8( WBUILD *wb, const u8 *text, u32 len );
void  wb_par( WBUILD *wb );             /* the paragraph ends here */
void  wb_object( WBUILD *wb, int obj ); /* an object, as the next character */
/* text goes to another story from now on (a header, a note); the
   paragraph in hand is ended first.  The CHP and PAP carry on. */
void  wb_story( WBUILD *wb, STORY *story );

/* ------------------------------------------------------------------ */
/* a writer's helper: a paragraph as runs                              */
/* ------------------------------------------------------------------ */

/* the run that starts at "off": how many units share one CHP number.
   0 at the paragraph's end. */
u32   wd_run( const WPARA *para, u32 off );

/* ------------------------------------------------------------------ */
/* errors a reader or writer answers with (FMT.H)                      */
/* ------------------------------------------------------------------ */
#define WE_OK           0
#define WE_NOMEM        (-1)    /* no memory */
#define WE_FORMAT       (-2)    /* not this kind of file, or damaged */
#define WE_UNSUPPORTED  (-3)    /* a version or feature that is not read */
#define WE_PASSWORD     (-4)    /* it is protected with a password */
#define WE_IO           (-5)    /* the disk: out_error() has DOS's code */

#endif
