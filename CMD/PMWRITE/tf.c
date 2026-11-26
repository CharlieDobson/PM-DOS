/*
 * TF.C - type faces: how wide a character is, and its shape (TF.H).
 *
 * WHAT A FACE IS HERE.  The PDF code's built-in font (PDF\SFONT.C) is
 * one drawing of each character, as the path of a pen, and a FONT
 * (PDF\GFX.H) says how it is to be drawn: slanted or not, with which
 * pen, and how wide each character is to come out.  A TFACE is such a
 * FONT made by hand - for Times, Helvetica, Courier or Symbol, in one
 * of four styles - with the widths Adobe's metric files give those
 * fonts, so that a line set here is as long as the same line in a PDF
 * that names one of them, and as long as Word would make it in Times
 * New Roman, Arial or Courier New, whose widths are the same.
 *
 * THE CHARACTERS.  A face of the first three kinds is given Unicode as
 * its codes (a composite FONT whose character map says "each code is
 * itself"), so anything SFONT.C can draw it draws, and a new drawing
 * there needs only a width here.  That is ASCII, Latin-1 and the rest
 * of Windows' code page 1252, Latin Extended-A, Greek and Cyrillic,
 * and a few more: ligatures, arrows, a check mark.  Anything else is a
 * box, as wide as a figure.  A Symbol face has Symbol's own 224 codes,
 * and takes a character three ways: as Unicode (a Greek letter), as
 * the private-use code Word gives it (F061h), or as the plain byte an
 * old file stored (61h, which in Symbol is an alpha).  Its Greek
 * letters are SFONT.C's drawings of them, as wide as Symbol makes
 * them; the figures, the signs and the bullet are drawn too, and a
 * sign SFONT.C has no drawing of is a box of the right width.
 * A picture font - Wingdings, Dingbats - has none of its pictures: a
 * bullet stands for each, a check mark or an arrow for those.
 *
 * LINES.  The built-in font has no heights of its own: it is drawn on
 * a grid of twenty to the em with capitals 14 high, ascenders 15,
 * accents over capitals to 19 and descenders 4 below.  So each face is
 * given the heights of the font it stands in for - what Times New
 * Roman, Arial, Courier New and Symbol say in their own tables - and a
 * page breaks where it would with those.
 *
 * SHAPES are kept by GLYPH.C, the same store a PDF page's glyphs are
 * in, at four positions across a pixel; tf_draw adds them into the
 * caller's block.  A size above 72 pixels is not kept, and is scanned
 * each time.
 *
 * MEMORY.  A face is about 5K from the heap, made when it is first
 * asked for.  When there is no memory for one, the plain Times face -
 * which tf_init makes - is the answer.  When there is none for a
 * shape, the program's out_of_memory is called, as for all of the PDF
 * code; if that leaves through pdf_trap (PDFVIEW.H says how), tf_draw
 * comes back with the rest of its text measured and not drawn.
 *
 * TRUETYPE FILES.  The directories tf_font_dirs names are searched
 * once for *.TTF, and of each file only its directory of tables and
 * three tables are read: its family and style, and whether its maker
 * lets it go into a document.  A font of the document whose name is a
 * family that was found gets a face that is the file: the whole file
 * is read into memory the first time, and stays there until memory is
 * short (tf_reclaim), when it is given back and read again at the next
 * call that wants it.  Its widths are the file's own, its line heights
 * the ones Windows takes from it, and its outlines go through GLYPH.C
 * as a PDF's embedded font does: filled, and not fitted to the pixels
 * (PDF\TTF.C runs no instructions).  A style the family has no file
 * for is made: the upright one sloped, the light one drawn twice a
 * little apart.  A character the file has not got is the built-in
 * face's, drawn and measured, so a Latin font still shows Greek.
 *
 * A file that cannot be read, or is damaged, or is too large for the
 * memory there is, is never an error here: the face is the built-in
 * one, or - if it stops being readable after the face was made - goes
 * on as the same TFACE, drawing and measuring as its stand-in.  A
 * collection (.TTC) and an OpenType font with CFF outlines (.OTF) are
 * not looked for.
 *
 * NOTHING POINTS INTO A FILE'S BYTES but its own record (TFILE.data
 * and the reading of it, TFILE.prog), and every use of them is between
 * file_ready and the end of the call, with "file_busy" set wherever
 * memory may be asked for in between: tf_reclaim gives no file back
 * while it is.
 */
#include "tf.h"
#include "gfx.h"
#include "sys.h"

/* the six sets of widths */
enum { SET_HELV, SET_HELV_BOLD, SET_TIMES, SET_TIMES_BOLD, SET_TIMES_ITALIC, SET_TIMES_BOLD_ITALIC };

enum { KIND_LATIN, KIND_SYMBOL, KIND_PICTURES };

#define BOX_CODE    0x25A1      /* a character SFONT.C has no drawing of: it draws its box */
#define BOX_WIDE    500
#define FIXED_WIDE  600

/* 80h to FFh of Windows' code page 1252, a row of sixteen to a line; 0
   where it has no character.  From Adobe's AFM files for Helvetica,
   Helvetica-Bold, Times-Roman, Times-Bold, Times-Italic and
   Times-BoldItalic; the oblique Helveticas are as wide as the upright. */
static const uint16_t high_wide[6][128] = {
    /* helvetica */ {
         556,    0,  222,  556,  333, 1000,  556,  556,  333, 1000,  667,  333, 1000,    0,  611,    0,
           0,  222,  222,  333,  333,  350,  556, 1000,  333, 1000,  500,  333,  944,    0,  500,  667,
         278,  333,  556,  556,  556,  556,  260,  556,  333,  737,  370,  556,  584,  333,  737,  333,
         400,  584,  333,  333,  333,  556,  537,  278,  333,  333,  365,  556,  834,  834,  834,  611,
         667,  667,  667,  667,  667,  667, 1000,  722,  667,  667,  667,  667,  278,  278,  278,  278,
         722,  722,  778,  778,  778,  778,  778,  584,  778,  722,  722,  722,  722,  667,  667,  611,
         556,  556,  556,  556,  556,  556,  889,  500,  556,  556,  556,  556,  278,  278,  278,  278,
         556,  556,  556,  556,  556,  556,  556,  584,  611,  556,  556,  556,  556,  500,  556,  500
    },
    /* helvetica bold */ {
         556,    0,  278,  556,  500, 1000,  556,  556,  333, 1000,  667,  333, 1000,    0,  611,    0,
           0,  278,  278,  500,  500,  350,  556, 1000,  333, 1000,  556,  333,  944,    0,  500,  667,
         278,  333,  556,  556,  556,  556,  280,  556,  333,  737,  370,  556,  584,  333,  737,  333,
         400,  584,  333,  333,  333,  611,  556,  278,  333,  333,  365,  556,  834,  834,  834,  611,
         722,  722,  722,  722,  722,  722, 1000,  722,  667,  667,  667,  667,  278,  278,  278,  278,
         722,  722,  778,  778,  778,  778,  778,  584,  778,  722,  722,  722,  722,  667,  667,  611,
         556,  556,  556,  556,  556,  556,  889,  556,  556,  556,  556,  556,  278,  278,  278,  278,
         611,  611,  611,  611,  611,  611,  611,  584,  611,  611,  611,  611,  611,  556,  611,  556
    },
    /* times roman */ {
         500,    0,  333,  500,  444, 1000,  500,  500,  333, 1000,  556,  333,  889,    0,  611,    0,
           0,  333,  333,  444,  444,  350,  500, 1000,  333,  980,  389,  333,  722,    0,  444,  722,
         250,  333,  500,  500,  500,  500,  200,  500,  333,  760,  276,  500,  564,  333,  760,  333,
         400,  564,  300,  300,  333,  500,  453,  250,  333,  300,  310,  500,  750,  750,  750,  444,
         722,  722,  722,  722,  722,  722,  889,  667,  611,  611,  611,  611,  333,  333,  333,  333,
         722,  722,  722,  722,  722,  722,  722,  564,  722,  722,  722,  722,  722,  722,  556,  500,
         444,  444,  444,  444,  444,  444,  667,  444,  444,  444,  444,  444,  278,  278,  278,  278,
         500,  500,  500,  500,  500,  500,  500,  564,  500,  500,  500,  500,  500,  500,  500,  500
    },
    /* times bold */ {
         500,    0,  333,  500,  500, 1000,  500,  500,  333, 1000,  556,  333, 1000,    0,  667,    0,
           0,  333,  333,  500,  500,  350,  500, 1000,  333, 1000,  389,  333,  722,    0,  444,  722,
         250,  333,  500,  500,  500,  500,  220,  500,  333,  747,  300,  500,  570,  333,  747,  333,
         400,  570,  300,  300,  333,  556,  540,  250,  333,  300,  330,  500,  750,  750,  750,  500,
         722,  722,  722,  722,  722,  722, 1000,  722,  667,  667,  667,  667,  389,  389,  389,  389,
         722,  722,  778,  778,  778,  778,  778,  570,  778,  722,  722,  722,  722,  722,  611,  556,
         500,  500,  500,  500,  500,  500,  722,  444,  444,  444,  444,  444,  278,  278,  278,  278,
         500,  556,  500,  500,  500,  500,  500,  570,  500,  556,  556,  556,  556,  500,  556,  500
    },
    /* times italic */ {
         500,    0,  333,  500,  556,  889,  500,  500,  333, 1000,  500,  333,  944,    0,  556,    0,
           0,  333,  333,  556,  556,  350,  500,  889,  333,  980,  389,  333,  667,    0,  389,  556,
         250,  389,  500,  500,  500,  500,  275,  500,  333,  760,  276,  500,  675,  333,  760,  333,
         400,  675,  300,  300,  333,  500,  523,  250,  333,  300,  310,  500,  750,  750,  750,  500,
         611,  611,  611,  611,  611,  611,  889,  667,  611,  611,  611,  611,  333,  333,  333,  333,
         722,  667,  722,  722,  722,  722,  722,  675,  722,  722,  722,  722,  722,  556,  611,  500,
         500,  500,  500,  500,  500,  500,  667,  444,  444,  444,  444,  444,  278,  278,  278,  278,
         500,  500,  500,  500,  500,  500,  500,  675,  500,  500,  500,  500,  500,  444,  500,  444
    },
    /* times bold italic */ {
         500,    0,  333,  500,  500, 1000,  500,  500,  333, 1000,  556,  333,  944,    0,  611,    0,
           0,  333,  333,  500,  500,  350,  500, 1000,  333, 1000,  389,  333,  722,    0,  389,  611,
         250,  389,  500,  500,  500,  500,  220,  500,  333,  747,  266,  500,  606,  333,  747,  333,
         400,  570,  300,  300,  333,  576,  500,  250,  333,  300,  300,  500,  750,  750,  750,  500,
         667,  667,  667,  667,  667,  667,  944,  667,  667,  667,  667,  667,  389,  389,  389,  389,
         722,  722,  722,  722,  722,  722,  722,  570,  722,  722,  722,  722,  722,  611,  611,  500,
         500,  500,  500,  500,  500,  500,  722,  444,  444,  444,  444,  444,  278,  278,  278,  278,
         500,  556,  500,  500,  500,  500,  500,  570,  500,  556,  556,  556,  556,  444,  500,  444
    }
};

/* Times Bold Italic, 20h to 7Eh: SFONT.C has the other five */
static const uint16_t times_bold_italic[95] = {
     250,  389,  555,  500,  500,  833,  778,  333,  333,  333,  500,  570,  250,  333,  250,  278,
     500,  500,  500,  500,  500,  500,  500,  500,  500,  500,  333,  333,  570,  570,  570,  500,
     832,  667,  667,  667,  722,  667,  667,  722,  778,  389,  500,  667,  611,  889,  722,  722,
     611,  722,  667,  556,  611,  722,  667,  889,  667,  611,  611,  333,  278,  333,  570,  500,
     333,  500,  500,  444,  500,  444,  333,  500,  556,  278,  278,  500,  278,  778,  556,  500,
     500,  500,  389,  389,  278,  556,  444,  667,  500,  444,  389,  348,  220,  348,  570
};

/* Symbol, 20h to FFh in its own encoding; 0 where it has no character */
static const uint16_t symbol_wide[224] = {
     250,  333,  713,  500,  549,  833,  778,  439,  333,  333,  500,  549,  250,  549,  250,  278,
     500,  500,  500,  500,  500,  500,  500,  500,  500,  500,  278,  278,  549,  549,  549,  444,
     549,  722,  667,  722,  612,  611,  763,  603,  722,  333,  631,  722,  686,  889,  722,  722,
     768,  741,  556,  592,  611,  690,  439,  768,  645,  795,  611,  333,  863,  333,  658,  500,
     500,  631,  549,  549,  494,  439,  521,  411,  603,  329,  603,  549,  549,  576,  521,  549,
     549,  521,  549,  603,  439,  576,  713,  686,  493,  686,  494,  480,  200,  480,  549,    0,
       0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
       0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
     750,  620,  247,  549,  167,  713,  500,  753,  753,  753,  753, 1042,  987,  603,  987,  603,
     400,  549,  411,  549,  549,  713,  494,  460,  549,  549,  549,  549, 1000,  603, 1000,  658,
     823,  686,  795,  987,  768,  768,  823,  768,  768,  713,  713,  713,  713,  713,  713,  713,
     768,  713,  790,  790,  890,  823,  549,  250,  713,  603,  603, 1042,  987,  603,  987,  603,
     494,  329,  790,  790,  786,  713,  384,  384,  384,  384,  384,  384,  494,  494,  494,  494,
       0,  329,  274,  686,  686,  686,  384,  384,  384,  384,  384,  384,  494,  494,  494,    0
};

/*
 * What a Latin face draws beyond FFh, in order of code.  Each is a
 * range of one in the FONT's table of widths, which is searched by
 * halving and so must be in order.
 */
static const uint16_t extra_uni[] = {
    0x0131, 0x0141, 0x0142, 0x0152, 0x0153, 0x0160, 0x0161, 0x0178, 0x017D, 0x017E,
    0x0192, 0x02C6, 0x02DC, 0x03D1, 0x03D5, 0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D,
    0x201E, 0x2020, 0x2021, 0x2022, 0x2026, 0x2030, 0x2039, 0x203A, 0x2044, 0x20AC,
    0x2122, 0x2190, 0x2192, 0x2212, 0x2713, 0xFB00, 0xFB01, 0xFB02, 0xFB03, 0xFB04
};
#define EXTRAS      (sizeof( extra_uni ) / sizeof( extra_uni[0] ))

/*
 * The alphabets SFONT.C draws beyond Latin-1: Latin Extended-A, S
 * and T with a comma below, Greek, Cyrillic.  Each is a run of codes
 * with a width apiece, and the widths of all of them are one array in
 * the face: "slot" is where a run's first code is in it.  A width of 0
 * there is a code SFONT.C has no drawing for.
 */
static const struct {
    uint16_t first, last;
    uint16_t slot;
} late_block[] = {
    { 0x0100, 0x017F, 0 }, { 0x0218, 0x021B, 128 }, { 0x0384, 0x03CE, 132 }, { 0x0400, 0x045F, 207 },
    { 0x0490, 0x0491, 303 }
};
#define LATE_BLOCKS (sizeof( late_block ) / sizeof( late_block[0] ))
#define LATE_SLOTS  305

static int late_slot( uint32_t code )
{
    unsigned int index;

    if ( code < 0x0100 || code > 0x0491 ) {
        return -1;
    }
    for ( index = 0; index < LATE_BLOCKS; index++ ) {
        if ( code >= late_block[index].first && code <= late_block[index].last ) {
            return (int)(late_block[index].slot + (code - late_block[index].first));
        }
    }
    return -1;
}

/* those of them the AFM files have and code page 1252 has not: their
   widths in the six sets */
static const struct {
    uint16_t uni;
    uint16_t wide[6];
} afm_more[] = {
    { 0x0131, { 278, 278, 278, 278, 278, 278 } },   /* dotless i */
    { 0x0141, { 556, 611, 611, 667, 556, 611 } },   /* L with a stroke */
    { 0x0142, { 222, 278, 278, 278, 278, 278 } },
    { 0x2044, { 167, 167, 167, 167, 167, 167 } },   /* the fraction's slash */
    { 0x2212, { 584, 584, 564, 570, 675, 606 } },   /* minus */
    { 0xFB01, { 500, 611, 556, 556, 500, 556 } },   /* fi */
    { 0xFB02, { 500, 611, 556, 556, 500, 556 } }    /* fl */
};

/* characters that are drawn as another is, and are as wide */
static const uint16_t same_as[][2] = {
    { 0x00A0, 0x0020 }, { 0x01FC, 0x00C6 }, { 0x2010, 0x002D }, { 0x2011, 0x002D },
    { 0x2012, 0x2013 }, { 0x2015, 0x2014 }, { 0x201B, 0x2018 }, { 0x2024, 0x002E },
    { 0x2027, 0x00B7 }, { 0x2032, 0x0027 }, { 0x2033, 0x0022 }, { 0x21D0, 0x2190 },
    { 0x21D2, 0x2192 }, { 0x2215, 0x2044 }, { 0x2217, 0x002A }, { 0x2219, 0x00B7 },
    { 0x221A, 0x2713 }, { 0x223C, 0x007E }, { 0x22C5, 0x00B7 }, { 0x25BA, 0x2192 },
    { 0x25C4, 0x2190 }, { 0x25CB, 0x2022 }, { 0x25CF, 0x2022 }, { 0x25E6, 0x2022 },
    { 0x2714, 0x2713 }
};

/* above the baseline, below it, and between lines, in thousandths of
   an em: Times New Roman's, Arial's, Courier New's and Symbol's own */
static const int16_t heights[4][3] = {
    {  891, 216, 42 },
    {  905, 212, 33 },
    {  833, 300,  0 },
    { 1005, 220,  0 }
};

typedef struct TFILE TFILE;

struct TFACE {
    TFACE   *next;
    uint8_t  base;              /* TFB_* */
    uint8_t  style;             /* CF_BOLD | CF_ITALIC */
    uint8_t  kind;              /* KIND_* */
    uint8_t  made;              /* a file's face: the CF_BOLD | CF_ITALIC the file has not got */
    int16_t  wide[224];         /* 20h to FFh: of a Symbol face, in Symbol's codes */
    int16_t  extra[EXTRAS];     /* of extra_uni's, each */
    int16_t  late[LATE_SLOTS];  /* of late_block's runs */
    WRANGE   ranges[EXTRAS + 2 + LATE_BLOCKS];
    CMAP     same;              /* "each code is itself" */
    CMRANGE  all;
    FONT     font;
    /* a file's face: "wide" is the file's widths, and the built-in
       face's tables above are not used */
    TFILE   *file;              /* NULL for a built-in face */
    TFACE   *standin;           /* the built-in face of its base, style and kind */
    uint16_t gid[224];          /* the file's glyphs for 20h to FFh: 0 where it has none */
    int16_t  high[3];           /* above the baseline, below it, between lines; all 0 if
                                   the file's own are not to be believed */
};

static TFACE *faces;
static TFACE *plain;            /* Times, upright and light: the last resort */

/* tf_draw's own, where a jump out of the PDF code cannot lose them */
static JMPBUF       draw_trap;
static unsigned int draw_at;
static int32_t      draw_pen;
static int          draw_dry;   /* measuring only: there is no block, or no memory */

/* ------------------------------------------------------------------ */
/* the TrueType files that were found                                  */
/* ------------------------------------------------------------------ */

#define FILES_MOST  256
#define FILE_BYTES  (32UL * 1024 * 1024)    /* a font file larger than this is left alone */
#define NAME_BYTES  (64UL * 1024)           /* as much of a file's table of names as is read */
#define DIR_TABLES  96                      /* as many of a file's tables as are looked through */
#define ROOM_BYTES  (DIR_TABLES * 16)

#define FS_NEVER    0x0202      /* fsType: not to be put in a document; bitmaps only */
#define FS_WHOLE    0x0100      /* ...and: not as a subset */

struct TFILE {
    TFILE   *next;              /* in order of family, and of style within one */
    char     family[FONT_NAME_LEN];
    uint8_t  style;             /* the CF_BOLD | CF_ITALIC it is */
    uint8_t  dead;              /* it could not be read, and is not tried again */
    uint8_t  known;             /* "facts" has been filled */
    uint8_t  spare;
    uint32_t fstype;            /* what its maker allows */
    uint8_t *data;              /* the whole file, while it is in memory */
    uint32_t len;
    void    *prog;              /* PDF\TTF.C's reading of "data" */
    TTFACTS  facts;
    char     path[1];           /* as long as it has to be */
};

static TFILE       *files;
static unsigned int nfiles;
static char        *more_dirs;  /* tf_font_dirs' */
static int          usual_dirs;
static int          dirs_given;
static int          looked;     /* the directories have been searched */
static int          file_busy;  /* a file's bytes are in use: none is given back */
static int          ink_cut;    /* tf_screen's: 0 on a screen that shows shades */
static uint32_t     face_serial;

static uint32_t be16( const uint8_t *at )
{
    return ((uint32_t)at[0] << 8) | at[1];
}

static uint32_t be32( const uint8_t *at )
{
    return (be16( at ) << 16) | be16( at + 2 );
}

static int read_at( uint32_t handle, uint32_t pos, void *buf, uint32_t len )
{
    uint32_t done = 0;

    return sys_seek( handle, pos ) == 0 && sys_read( handle, buf, len, &done ) == 0 && done == len;
}

/* a file into the list, where its family and style put it.  The first
   one found of a family and a style is the one that is used. */
static void file_add( const char *path, const char *family, int style, uint32_t fstype )
{
    TFILE *file, **link;
    int order;

    for ( link = &files; *link != NULL; link = &(*link)->next ) {
        order = str_icmp( (*link)->family, family );
        if ( order == 0 ) {
            order = (int)(*link)->style - style;
        }
        if ( order == 0 ) {
            return;
        }
        if ( order > 0 ) {
            break;
        }
    }
    file = (TFILE *)try_alloc( sizeof( TFILE ) + str_len( path ) );
    if ( file == NULL ) {
        return;
    }
    mem_set( file, 0, sizeof( TFILE ) );
    str_cpyt( file->family, family, sizeof( file->family ) );
    file->style = (uint8_t)style;
    file->fstype = fstype;
    str_cpy( file->path, path );
    file->next = *link;
    *link = file;
    nfiles++;
}

/* One file: its directory of tables, and from three of them its
   family, its style and what its maker allows.  "room" is ROOM_BYTES. */
static void file_look( const char *path, uint8_t *room )
{
    char family[FONT_NAME_LEN];
    uint8_t *names;
    uint32_t handle, size = 0, tables;
    unsigned int index;
    uint32_t tag, off, len;
    uint32_t name_at = 0, name_len = 0, head_at = 0, os2_at = 0, os2_len = 0, mac, sel = 0, fstype = 0;
    int outlines = 0;

    if ( sys_open_read( path, &handle ) != 0 ) {
        return;
    }
    if ( sys_file_size( handle, &size ) == 0 && size >= 12 && size <= FILE_BYTES && read_at( handle, 0, room, 12 ) &&
         (be32( room ) == 0x00010000UL || be32( room ) == 0x74727565UL) ) {
        tables = be16( room + 4 );
        if ( tables > DIR_TABLES ) {
            tables = DIR_TABLES;
        }
        if ( tables != 0 && tables * 16 <= size - 12 && read_at( handle, 12, room, tables * 16 ) ) {
            for ( index = 0; index < tables; index++ ) {
                tag = be32( room + index * 16 );
                off = be32( room + index * 16 + 8 );
                len = be32( room + index * 16 + 12 );
                if ( off >= size || len > size - off ) {
                    continue;
                }
                if ( tag == 0x6E616D65UL ) {                    /* name */
                    name_at = off;
                    name_len = len;
                } else if ( tag == 0x68656164UL && len >= 54 ) {        /* head */
                    head_at = off;
                } else if ( tag == 0x4F532F32UL ) {             /* OS/2 */
                    os2_at = off;
                    os2_len = len;
                } else if ( tag == 0x676C7966UL ) {             /* glyf */
                    outlines = 1;
                }
            }
        }
        if ( outlines && name_len >= 6 && head_at != 0 && read_at( handle, head_at, room, 54 ) ) {
            mac = be16( room + 44 );
            if ( os2_len >= 64 && read_at( handle, os2_at, room, 64 ) ) {
                fstype = be16( room + 8 );
                sel = be16( room + 62 );
            }
            if ( name_len > NAME_BYTES ) {
                name_len = NAME_BYTES;
            }
            names = (uint8_t *)try_alloc( name_len );
            if ( names != NULL ) {
                if ( read_at( handle, name_at, names, name_len ) &&
                     ttf_name( names, name_len, 1, family, sizeof( family ) ) ) {
                    file_add( path, family, (((mac & 1) || (sel & 0x20)) ? CF_BOLD : 0) |
                              (((mac & 2) || (sel & 1)) ? CF_ITALIC : 0), fstype );
                }
                xfree( names );
            }
        }
    }
    sys_close( handle );
}

/* every *.TTF in a directory, whose name is "dir_len" long */
static void look_in( const char *dir, unsigned int dir_len, FINDREC *rec, uint8_t *room )
{
    char path[PATH_LEN];
    uint32_t handle;
    unsigned int base;

    while ( dir_len && dir[0] == ' ' ) {
        dir++;
        dir_len--;
    }
    while ( dir_len && dir[dir_len - 1] == ' ' ) {
        dir_len--;
    }
    if ( dir_len == 0 || dir_len + 7 > PATH_LEN ) {
        return;
    }
    mem_cpy( path, dir, dir_len );
    if ( path[dir_len - 1] != '\\' && path[dir_len - 1] != '/' && path[dir_len - 1] != ':' ) {
        path[dir_len++] = '\\';
    }
    base = dir_len;
    str_cpy( path + base, "*.TTF" );
    if ( sys_find_first( path, ATTR_READONLY | ATTR_ARCHIVE, rec, &handle ) != 0 ) {
        return;
    }
    do {
        if ( nfiles < FILES_MOST && !(rec->attr & ATTR_DIR) && base + str_len( rec->name ) < PATH_LEN ) {
            str_cpy( path + base, rec->name );
            file_look( path, room );
        }
    } while ( nfiles < FILES_MOST && sys_find_next( handle, rec ) == 0 );
    sys_find_close( handle );
}

/* the search, the first time anything asks what fonts there are */
static void look_all( void )
{
    const char *prog, *cur, *end;
    char path[PATH_LEN];
    FINDREC *rec;
    uint8_t *room;
    int drive, letter, pass;

    if ( looked || !dirs_given ) {
        return;
    }
    looked = 1;
    rec = (FINDREC *)try_alloc( sizeof( FINDREC ) + ROOM_BYTES );
    if ( rec == NULL ) {
        return;
    }
    room = (uint8_t *)(rec + 1);
    prog = sys_program_dir();
    if ( usual_dirs && str_len( prog ) + 6 <= PATH_LEN ) {
        sfmt( path, sizeof( path ), "%sFONTS", prog );
        look_in( path, str_len( path ), rec, room );
    }
    for ( cur = more_dirs; cur != NULL && *cur; cur = *end ? end + 1 : end ) {
        for ( end = cur; *end && *end != ';'; end++ ) {
        }
        look_in( cur, (unsigned int)(end - cur), rec, room );
    }
    if ( usual_dirs ) {
        drive = prog[0] && prog[1] == ':' ? ch_upper( prog[0] ) : 'A' + sys_get_drive();
        for ( pass = 0; pass < 2; pass++ ) {
            letter = pass == 0 ? 'C' : drive;
            if ( (pass == 1 && drive == 'C') || letter < 'A' || letter > 'Z' || !sys_drive_valid( letter - 'A' ) ) {
                continue;
            }
            sfmt( path, sizeof( path ), "%c:\\WINDOWS\\SYSTEM", letter );
            look_in( path, str_len( path ), rec, room );
            sfmt( path, sizeof( path ), "%c:\\WINDOWS\\FONTS", letter );
            look_in( path, str_len( path ), rec, room );
        }
    }
    xfree( rec );
    sys_crit_seen();            /* a drive that was not ready is nobody else's news */
}

/*
 * The file in memory: read now if it is not.  0, and the file is not
 * tried again, if it cannot be - no memory, no file, not a font.  The
 * caller asks for no memory between this and its last use of the
 * file's bytes, or sets "file_busy" while it does.
 */
static int file_ready( TFILE *file )
{
    uint32_t handle, size = 0, done = 0;
    uint8_t *data;
    void *prog;

    if ( file->data != NULL ) {
        return 1;
    }
    if ( file->dead ) {
        return 0;
    }
    file->dead = 1;
    if ( sys_open_read( file->path, &handle ) != 0 ) {
        return 0;
    }
    if ( sys_file_size( handle, &size ) != 0 || size < 12 || size > FILE_BYTES ) {
        sys_close( handle );
        return 0;
    }
    data = (uint8_t *)try_alloc( size );
    if ( data == NULL || sys_read( handle, data, size, &done ) != 0 || done != size ) {
        xfree( data );
        sys_close( handle );
        return 0;
    }
    sys_close( handle );
    prog = ttf_open_heap( data, size );
    if ( prog == NULL ) {
        xfree( data );
        return 0;
    }
    file->data = data;
    file->len = size;
    file->prog = prog;
    file->dead = 0;
    if ( !file->known ) {
        ttf_facts( prog, &file->facts );
        file->known = 1;
    }
    return 1;
}

/* every file that is in memory given back: 1 if any was.  No face is
   left with the address of a reading that is gone. */
static int files_drop( void )
{
    TFILE *file;
    TFACE *face;
    int any = 0;

    for ( file = files; file != NULL; file = file->next ) {
        if ( file->data != NULL ) {
            xfree( file->data );
            xfree( file->prog );
            file->data = NULL;
            file->prog = NULL;
            file->len = 0;
            any = 1;
        }
    }
    for ( face = faces; face != NULL; face = face->next ) {
        if ( face->file != NULL ) {
            face->font.prog = NULL;
        }
    }
    return any;
}

/* the file for a font's name in a style: the family's file of that
   style, or the nearest to it - one without a style, which can be made,
   before one with a style that was not asked for */
static TFILE *file_pick( const char *name, int style )
{
    TFILE *file, *best = NULL;
    int score, least = 100;

    for ( file = files; file != NULL; file = file->next ) {
        if ( str_icmp( file->family, name ) != 0 ) {
            continue;
        }
        score = 0;
        if ( (file->style ^ style) & CF_ITALIC ) {
            score += (file->style & CF_ITALIC) ? 8 : 1;
        }
        if ( (file->style ^ style) & CF_BOLD ) {
            score += (file->style & CF_BOLD) ? 4 : 2;
        }
        if ( score < least ) {
            least = score;
            best = file;
        }
    }
    return best;
}

void tf_font_dirs( const char *dirs, int usual )
{
    unsigned int len = dirs != NULL ? str_len( dirs ) : 0;

    if ( looked ) {
        return;                 /* too late: the list is made */
    }
    xfree( more_dirs );
    more_dirs = len ? (char *)try_alloc( len + 1 ) : NULL;
    if ( more_dirs != NULL ) {
        str_cpy( more_dirs, dirs );
    }
    usual_dirs = usual;
    dirs_given = 1;
}

const char *tf_family( unsigned int index )
{
    TFILE *file, *last = NULL;

    look_all();
    for ( file = files; file != NULL; file = file->next ) {
        if ( last != NULL && str_icmp( last->family, file->family ) == 0 ) {
            continue;
        }
        if ( index-- == 0 ) {
            return file->family;
        }
        last = file;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* making a face                                                       */
/* ------------------------------------------------------------------ */

static int set_of( int base, int style )
{
    if ( base == TFB_HELVETICA ) {
        return (style & CF_BOLD) ? SET_HELV_BOLD : SET_HELV;
    }
    if ( style & CF_BOLD ) {
        return (style & CF_ITALIC) ? SET_TIMES_BOLD_ITALIC : SET_TIMES_BOLD;
    }
    return (style & CF_ITALIC) ? SET_TIMES_ITALIC : SET_TIMES;
}

/* how wide one of extra_uni's is in a set: from the AFM's figures if
   it has any, else as SFONT.C draws it */
static int extra_wide( uint32_t uni, int set, int flags, const uint16_t *ansi )
{
    unsigned int index;

    for ( index = 0x80; index < 0xA0; index++ ) {
        if ( ansi[index] == uni && high_wide[set][index - 0x80] ) {
            return high_wide[set][index - 0x80];
        }
    }
    for ( index = 0; index < sizeof( afm_more ) / sizeof( afm_more[0] ); index++ ) {
        if ( afm_more[index].uni == uni ) {
            return afm_more[index].wide[set];
        }
    }
    return sfont_width( (uint16_t)uni, flags );
}

/* how wide a character of late_block's is: as the AFM files say if they
   have it, else as wide as the letter SFONT.C makes it from.  0 if
   there is no drawing of it. */
static int late_wide( const TFACE *face, uint32_t code )
{
    unsigned int index;
    uint16_t like;

    for ( index = 0; index < EXTRAS; index++ ) {
        if ( extra_uni[index] == code ) {
            return face->extra[index];
        }
    }
    switch ( code ) {
    case 0x0132: return face->wide['I' - 0x20] + face->wide['J' - 0x20];
    case 0x0133: return face->wide['i' - 0x20] + face->wide['j' - 0x20];
    case 0x0149: return face->wide['\'' - 0x20] + face->wide['n' - 0x20];
    case 0x0384: return face->wide['\'' - 0x20];
    case 0x0385: return face->wide['"' - 0x20];
    case 0x0387: return face->wide[0xB7 - 0x20];
    }
    like = sfont_like( (uint16_t)code );
    return like >= 0x20 && like < 0x7F ? face->wide[like - 0x20] : 0;
}

static void face_fill( TFACE *face, int base, int style, int kind )
{
    FONT *font = &face->font;
    uint16_t ansi[256];
    uint32_t index;
    unsigned int block, count;
    uint32_t limit;
    int set = set_of( base, style ), flags = FF_MORE;

    mem_set( face, 0, sizeof( *face ) );
    face->base = (uint8_t)base;
    face->style = (uint8_t)style;
    face->kind = (uint8_t)kind;
    if ( base == TFB_COURIER ) {
        flags |= FF_FIXED;
    } else if ( base != TFB_HELVETICA ) {
        flags |= FF_SERIF;
    }
    if ( style & CF_BOLD ) {
        flags |= FF_BOLD;
    }
    if ( style & CF_ITALIC ) {
        flags |= FF_ITALIC;
    }
    font->kind = FT_BUILTIN;
    font->units = 1000;
    font->flags = (uint8_t)flags;
    /* part of what GLYPH.C keeps a shape under; any number will do */
    font->key = 0xFFFF0000UL + (uint32_t)base * 16 + (uint32_t)style * 4 + (uint32_t)kind;

    if ( kind == KIND_SYMBOL ) {
        /* a simple font in Symbol's encoding: a code is a byte */
        enc_base( ENC_SYMBOL, font->uni );
        font->dw = -1;
        for ( index = 0; index < 256; index++ ) {
            font->width[index] = (int16_t)(index >= 0x20 ? symbol_wide[index - 0x20] : 0);
            font->gid[index] = 0xFFFF;
        }
        for ( index = 0; index < 224; index++ ) {
            face->wide[index] = (int16_t)symbol_wide[index];
        }
        return;
    }

    enc_base( ENC_WIN, ansi );
    for ( index = 0x20; index < 0x100; index++ ) {
        if ( flags & FF_FIXED ) {
            face->wide[index - 0x20] = FIXED_WIDE;
        } else if ( index < 0x7F ) {
            face->wide[index - 0x20] = (int16_t)(set == SET_TIMES_BOLD_ITALIC ? times_bold_italic[index - 0x20]
                                                                           : sfont_width( (uint16_t)index, flags ));
        } else if ( index >= 0xA0 ) {
            face->wide[index - 0x20] = (int16_t)high_wide[set][index - 0x80];
        }
    }
    for ( index = 0; index < EXTRAS; index++ ) {
        face->extra[index] = (int16_t)((flags & FF_FIXED) ? FIXED_WIDE
                                                      : extra_wide( extra_uni[index], set, flags, ansi ));
    }
    /* the FONT: codes that are Unicode, and the widths as ranges */
    font->composite = 1;
    font->dw = (flags & FF_FIXED) ? FIXED_WIDE : BOX_WIDE;
    face->ranges[0].first = 0x20;
    face->ranges[0].last = 0x7E;
    face->ranges[0].widths = face->wide;
    face->ranges[1].first = 0xA0;
    face->ranges[1].last = 0xFF;
    face->ranges[1].widths = face->wide + 0x80;
    for ( block = 0; block < LATE_BLOCKS; block++ ) {
        for ( index = late_block[block].first; index <= late_block[block].last; index++ ) {
            limit = (uint32_t)late_wide( face, index );
            face->late[late_block[block].slot + (index - late_block[block].first)] =
                (int16_t)(limit != 0 && (flags & FF_FIXED) ? FIXED_WIDE : limit);
        }
    }
    /* the runs and the single ones, in order of code: one of extra_uni's
       that is inside a run has its width there */
    count = 2;
    block = 0;
    for ( index = 0; index <= EXTRAS; index++ ) {
        limit = index < EXTRAS ? extra_uni[index] : 0x10000UL;
        while ( block < LATE_BLOCKS && late_block[block].first < limit ) {
            face->ranges[count].first = late_block[block].first;
            face->ranges[count].last = late_block[block].last;
            face->ranges[count].widths = face->late + late_block[block].slot;
            count++;
            block++;
        }
        if ( index < EXTRAS && late_slot( extra_uni[index] ) < 0 ) {
            face->ranges[count].first = face->ranges[count].last = extra_uni[index];
            face->ranges[count].width = face->extra[index];
            count++;
        }
    }
    font->wr = face->ranges;
    font->nwr = count;
    face->all.lo = 0;
    face->all.hi = 0xFFFF;
    face->all.val = 0;
    face->same.ranges = &face->all;
    face->same.count = face->same.cap = 1;
    font->tou = &face->same;
}

static TFACE *face_find( int base, int style, int kind )
{
    TFACE *face;

    for ( face = faces; face; face = face->next ) {
        if ( face->file == NULL && face->base == base && face->style == style && face->kind == kind ) {
            return face;
        }
    }
    return NULL;
}

/* the face, made if it has to be: NULL when there is no memory, unless
   "must", which is how the plain face is asked for */
static TFACE *face_make( int base, int style, int kind, int must )
{
    TFACE *face = face_find( base, style, kind );

    if ( face == NULL ) {
        face = (TFACE *)(must ? xalloc( sizeof( TFACE ) ) : try_alloc( sizeof( TFACE ) ));
        if ( face == NULL ) {
            return NULL;
        }
        face_fill( face, base, style, kind );
        face->next = faces;
        faces = face;
    }
    return face;
}

/* a file's units as thousandths of an em */
static int units_wide( const TFACE *face, int32_t units )
{
    int32_t per = face->font.units;

    if ( units > 0x200000L ) {
        units = 0x200000L;
    } else if ( units < -0x200000L ) {
        units = -0x200000L;
    }
    units = units >= 0 ? (units * 1000 + per / 2) / per : -((-units * 1000 + per / 2) / per);
    return (int)(units > 30000 ? 30000 : units < -30000 ? -30000 : units);
}

/* The file's glyph for a character.  The file is in memory.  A Symbol
   font is asked for a Greek letter or a sign by the code Symbol gives
   it, which the stand-in's table of them says. */
static uint32_t file_gid( const TFACE *face, uint32_t code )
{
    void *prog = face->file->prog;
    uint32_t gid = ttf_glyph_of( prog, code ), sym;

    if ( gid == 0 && face->kind == KIND_SYMBOL && code >= 0x100 && (code < 0xF000 || code > 0xF0FF) ) {
        for ( sym = 0x20; sym < 0x100; sym++ ) {
            if ( face->standin->font.uni[sym] == code ) {
                return ttf_glyph_of( prog, 0xF000 + sym );
            }
        }
    }
    return gid;
}

/* a face that is a file, which is in memory: nothing here asks for any */
static void file_fill( TFACE *face, TFILE *file, TFACE *standin, int base, int style, int kind )
{
    FONT *font = &face->font;
    const TTFACTS *facts = &file->facts;
    uint32_t code, gid;
    int rise, fall, lead;

    mem_set( face, 0, sizeof( *face ) );
    face->base = (uint8_t)base;
    face->style = (uint8_t)style;
    face->kind = (uint8_t)kind;
    face->made = (uint8_t)(style & ~file->style);
    face->file = file;
    face->standin = standin;
    /* a composite font with no map from codes to glyphs: FONT.C takes
       each code to be the glyph's number */
    font->kind = FT_TRUETYPE;
    font->composite = 1;
    font->units = ttf_units( file->prog );
    font->prog = file->prog;
    font->key = 0xFFFE0000UL + (face_serial++ & 0xFFFF);
    for ( code = 0x20; code < 0x100; code++ ) {
        gid = file_gid( face, code );
        face->gid[code - 0x20] = (uint16_t)gid;
        face->wide[code - 0x20] = (int16_t)(gid ? units_wide( face, ttf_advance( file->prog, gid ) ) : 0);
    }
    rise = units_wide( face, facts->ascent );
    fall = units_wide( face, facts->descent );
    lead = units_wide( face, facts->gap );
    if ( rise > 0 && fall >= 0 && rise + fall >= 500 && rise + fall <= 5000 && lead <= 2000 ) {
        face->high[0] = (int16_t)rise;
        face->high[1] = (int16_t)fall;
        face->high[2] = (int16_t)lead;
    }
}

/* the face of a file for a base, a style and a kind, made if it has to
   be: NULL if the file is not to be had, or there is no memory */
static TFACE *face_of_file( TFILE *file, int base, int style, int kind )
{
    TFACE *face, *standin;

    for ( face = faces; face != NULL; face = face->next ) {
        if ( face->file == file && face->base == base && face->style == style && face->kind == kind ) {
            return face;
        }
    }
    if ( file->dead ) {
        return NULL;
    }
    /* the memory first, the file after it: asking for memory can take
       a file that is not in use out of memory again */
    standin = face_make( base, style, kind, 0 );
    face = (TFACE *)try_alloc( sizeof( TFACE ) );
    if ( standin == NULL || face == NULL || !file_ready( file ) ) {
        xfree( face );
        return NULL;
    }
    file_fill( face, file, standin, base, style, kind );
    face->next = faces;
    faces = face;
    return face;
}

void tf_init( void )
{
    if ( plain == NULL ) {
        plain = face_make( TFB_TIMES, 0, KIND_LATIN, 1 );
    }
}

void tf_done( void )
{
    TFACE *face, *next;
    TFILE *file, *after;

    /* the shapes first: each is kept under its face's address */
    glyph_page_begin();
    glyph_reclaim();
    for ( face = faces; face; face = next ) {
        next = face->next;
        xfree( face );
    }
    faces = NULL;
    plain = NULL;
    for ( file = files; file != NULL; file = after ) {
        after = file->next;
        xfree( file->data );
        xfree( file->prog );
        xfree( file );
    }
    files = NULL;
    nfiles = 0;
    xfree( more_dirs );
    more_dirs = NULL;
    looked = dirs_given = usual_dirs = 0;
    file_busy = 0;
}

int tf_reclaim( void )
{
    if ( glyph_reclaim() ) {
        return 1;
    }
    return file_busy ? 0 : files_drop();
}

TFACE *tf_face( const WFONT *font, int flags )
{
    TFACE *face;
    TFILE *file;
    int base = TFB_TIMES, kind = KIND_LATIN, style = flags & (CF_BOLD | CF_ITALIC);

    if ( font != NULL ) {
        if ( font->family == FAM_SYMBOL ) {
            base = TFB_SYMBOL;
            kind = mem_ifind( (const uint8_t *)font->name, str_len( font->name ), "Symbol" ) >= 0 ? KIND_SYMBOL
                                                                                           : KIND_PICTURES;
        } else if ( font->fixed || font->family == FAM_MODERN ) {
            base = TFB_COURIER;
        } else if ( font->family == FAM_SWISS ) {
            base = TFB_HELVETICA;
        }
    }
    tf_init();
    look_all();
    if ( font != NULL && files != NULL ) {
        file = file_pick( font->name, style );
        if ( file != NULL ) {
            face = face_of_file( file, base, style, kind );
            if ( face != NULL ) {
                return face;
            }
        }
    }
    face = face_make( base, style, kind, 0 );
    return face ? face : plain;
}

int tf_base( const TFACE *face )
{
    return face->base;
}

int tf_is_builtin( const TFACE *face )
{
    return face->file == NULL || face->file->dead;
}

int tf_flags( const TFACE *face )
{
    return face->style;
}

void tf_metrics( TFACE *face, int *ascent, int *descent, int *gap )
{
    if ( face->file != NULL && !face->file->dead && face->high[0] != 0 ) {
        *ascent = face->high[0];
        *descent = face->high[1];
        *gap = face->high[2];
        return;
    }
    *ascent = heights[face->base][0];
    *descent = heights[face->base][1];
    *gap = heights[face->base][2];
}

/* ------------------------------------------------------------------ */
/* a character in a face                                               */
/* ------------------------------------------------------------------ */

/* a space that has a width of its own, in thousandths of an em: -1 if
   "code" is not one */
static int space_wide( TFACE *face, uint32_t code )
{
    switch ( code ) {
    case 0x2000: case 0x2002: return 500;
    case 0x2001: case 0x2003: case 0x3000: return 1000;
    case 0x2004: return 333;
    case 0x2005: return 250;
    case 0x2006: return 167;
    case 0x2007: return face->kind == KIND_SYMBOL ? 500 : face->wide['0' - 0x20];
    case 0x2008: return face->kind == KIND_SYMBOL ? 250 : face->wide['.' - 0x20];
    case 0x2009: case 0x202F: return 200;
    case 0x200A: return 100;
    case 0x205F: return 222;
    }
    return -1;
}

/* 1 if it takes no room and shows nothing: a control code, a mark that
   sits on the letter before it, a hyphen that is not showing */
static int no_wide( uint32_t code )
{
    return code < 0x20 || (code >= 0x7F && code < 0xA0) || code == 0x00AD ||
           (code >= 0x0300 && code <= 0x036F) || (code >= 0x200B && code <= 0x200F) ||
           (code >= 0x2028 && code <= 0x202E) || (code >= 0x2060 && code <= 0x2064) ||
           code == 0xFEFF || (code >= 0xFE00 && code <= 0xFE0F);
}

/*
 * What a face makes of a character: the code GLYPH.C is to be asked
 * for - 0 when nothing is drawn - and in *wide how far the pen moves.
 */
static uint32_t face_glyph( TFACE *face, uint32_t code, int *wide )
{
    uint32_t lo, hi, mid;
    int gap;

    if ( code == 0x20 ) {
        *wide = face->wide[0];
        return 0;
    }
    if ( face->kind == KIND_LATIN && ((code > 0x20 && code <= 0x7E) || (code > 0xA0 && code <= 0xFF)) &&
         code != 0x00AD ) {
        *wide = face->wide[code - 0x20];        /* nearly all of them are these */
        return code;
    }
    if ( no_wide( code ) ) {
        *wide = 0;
        return 0;
    }
    gap = space_wide( face, code );
    if ( gap >= 0 ) {
        *wide = (face->font.flags & FF_FIXED) ? FIXED_WIDE : gap;
        return 0;
    }

    if ( face->kind == KIND_SYMBOL ) {
        if ( code >= 0xF020 && code <= 0xF0FF ) {
            code -= 0xF000;                     /* as Word keeps a symbol */
        } else if ( code >= 0x100 || face->font.uni[code] == 0 ) {
            /* as Unicode: which of Symbol's codes is it? */
            for ( mid = 0x20; mid < 0x100; mid++ ) {
                if ( face->font.uni[mid] == code ) {
                    break;
                }
            }
            code = mid;
        }
        if ( code < 0x20 || code > 0xFF || face->font.uni[code] == 0 ) {
            *wide = BOX_WIDE;
            return 0;                           /* Symbol has no such character */
        }
        *wide = face->wide[code - 0x20];
        return code == 0x20 || code == 0xA0 ? 0 : code;
    }

    if ( face->kind == KIND_PICTURES ) {
        /* a picture font's character, whichever way it is kept */
        if ( code >= 0xF020 && code <= 0xF0FF ) {
            code -= 0xF000;
        }
        if ( code == 0x20 || code == 0xA0 ) {
            *wide = face->wide[0];
            return 0;
        }
        code = code == 0xFB || code == 0xFC || code == 0x2713 || code == 0x2714 ? 0x2713
             : (code >= 0xD8 && code <= 0xF0) || code == 0x2192 ? 0x2192 : 0x2022;
    }

    gap = late_slot( code );
    if ( gap >= 0 ) {
        if ( face->late[gap] == 0 ) {
            *wide = face->font.dw;
            return BOX_CODE;
        }
        *wide = face->late[gap];
        return code;
    }
    for ( mid = 0; mid < sizeof( same_as ) / sizeof( same_as[0] ); mid++ ) {
        if ( same_as[mid][0] == code ) {
            code = same_as[mid][1];
            break;
        }
    }
    if ( code >= 0x20 && code <= 0xFF ) {
        *wide = face->wide[code - 0x20];
        return code == 0x20 ? 0 : code;
    }
    lo = 0;
    hi = EXTRAS;
    while ( lo < hi ) {
        mid = (lo + hi) / 2;
        if ( extra_uni[mid] < code ) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if ( lo < EXTRAS && extra_uni[lo] == code ) {
        *wide = face->extra[lo];
        return code;
    }
    *wide = face->font.dw;
    return BOX_CODE;
}

/*
 * What a face that is a file makes of a character: the file's glyph
 * for it and, in *wide, how far the pen moves.  0 with *wide at 0 or
 * more is a character that shows nothing.  0 with *wide below 0 is one
 * the file has not got - or the file is not to be had - and the face
 * that stands in is to be asked.  It may read the file into memory.
 */
static uint32_t file_glyph( TFACE *face, uint32_t code, int *wide )
{
    TFILE *file = face->file;
    uint32_t gid;

    *wide = -1;
    if ( file->dead ) {
        return 0;
    }
    if ( no_wide( code ) ) {
        *wide = 0;
        return 0;
    }
    if ( code >= 0x20 && code <= 0xFF ) {
        gid = face->gid[code - 0x20];           /* nearly all of them are these */
        if ( gid ) {
            *wide = face->wide[code - 0x20];
        }
        return gid;
    }
    if ( !file_ready( file ) ) {
        return 0;
    }
    gid = file_gid( face, code );
    if ( gid ) {
        *wide = units_wide( face, ttf_advance( file->prog, gid ) );
    }
    return gid;
}

unsigned int tf_advance( TFACE *face, uint32_t code )
{
    int wide;

    if ( face->file != NULL ) {
        file_glyph( face, code, &wide );
        if ( wide >= 0 ) {
            return (unsigned int)wide;
        }
        face = face->standin;
    }
    face_glyph( face, code, &wide );
    return (unsigned int)wide;
}

int tf_draws( TFACE *face, uint32_t code )
{
    int wide;
    uint32_t glyph;

    if ( face->file != NULL ) {
        glyph = file_glyph( face, code, &wide );
        if ( wide >= 0 ) {
            return glyph != 0 && code != 0x20 && code != 0xA0 && space_wide( face->standin, code ) < 0;
        }
        face = face->standin;
    }
    glyph = face_glyph( face, code, &wide );
    return glyph != 0 && glyph != BOX_CODE;
}

/* tf_path's caller, for the while SFONT.C is drawing */
static void (*path_to)( void *ctx, int move, int x, int y );

static void path_sink( void *ctx, int move, fx ux, fx uy )
{
    path_to( ctx, move, FX_ROUND( ux ), FX_ROUND( uy ) );
}

int tf_path( TFACE *face, uint32_t code, void (*sink)( void *ctx, int move, int x, int y ), void *ctx, int *pen )
{
    fx width = 0;
    int wide, kind;
    uint32_t glyph;

    *pen = 0;
    if ( face->file != NULL ) {
        /* a file's outlines are not a pen's path; a character the file
           has not got is the stand-in's, and that is one */
        file_glyph( face, code, &wide );
        if ( wide >= 0 ) {
            return 0;
        }
        face = face->standin;
    }
    glyph = face_glyph( face, code, &wide );
    if ( glyph == 0 || face->font.kind != FT_BUILTIN ) {
        return 0;
    }
    path_to = sink;
    sfont_sink_ctx = ctx;
    sfont_sink = path_sink;
    kind = font_outline( &face->font, glyph, &width );
    sfont_sink = NULL;
    *pen = FX_ROUND( width );
    return kind == 2;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

/* a row of a glyph, added into the block: GLYPH.C has cut it to fit */
static void cover_row( void *ctx, int ypos, int x0, int x1, const uint8_t *cov )
{
    COVER *cover = (COVER *)ctx;
    uint8_t *dst = cover->pix + (unsigned int)ypos * cover->stride + (unsigned int)x0;
    int count = x1 - x0, index, sum;

    for ( index = 0; index < count; index++ ) {
        if ( cov[index] ) {
            sum = dst[index] + cov[index];
            dst[index] = (uint8_t)(sum > 255 ? 255 : sum);
        }
    }
}

void tf_screen( int cut )
{
    ink_cut = cut < 0 ? 0 : cut > 255 ? 255 : cut;
}

/*
 * On a screen where a pixel is ink or paper, a stroke thinner than a
 * pixel is all there or all gone by where it happens to fall: lying
 * across two pixels, part in each, it reaches the cut in neither.  A
 * file's outlines are full of such strokes at text sizes - the bar of
 * an e, every stroke of Courier New - because nothing fits them to the
 * pixels.  So where coverage is below the cut but is the most of
 * three pixels in a row, or of three in a column, that between them
 * hold a stroke's worth of ink, and neither neighbor is ink already,
 * that pixel is made ink: the stroke is there, one pixel wide.  Columns
 * x0 to x1 of the block are looked at.
 */
static void thin_keep( COVER *cover, int x0, int x1 )
{
    uint8_t *row;
    int ypos, xpos, here, before, after, cut = ink_cut, least = ink_cut / 3;

    if ( x0 < 0 ) {
        x0 = 0;
    }
    if ( x1 > cover->width ) {
        x1 = cover->width;
    }
    for ( ypos = 0; ypos < cover->height; ypos++ ) {
        row = cover->pix + (unsigned int)ypos * cover->stride;
        for ( xpos = x0; xpos < x1; xpos++ ) {
            here = row[xpos];
            if ( here >= cut || here < least ) {
                continue;
            }
            /* an upright stroke: across the row */
            before = xpos > 0 ? row[xpos - 1] : 0;
            after = xpos + 1 < cover->width ? row[xpos + 1] : 0;
            if ( before < cut && after < cut && here >= before && here > after && before + here + after >= cut ) {
                row[xpos] = 255;
                continue;
            }
            /* a level one: down the column */
            before = ypos > 0 ? row[xpos - (int)cover->stride] : 0;
            after = ypos + 1 < cover->height ? row[xpos + (int)cover->stride] : 0;
            if ( before < cut && after < cut && here >= before && here > after && before + here + after >= cut ) {
                row[xpos] = 255;
            }
        }
    }
}

int32_t tf_draw( TFACE *face, int32_t em, int32_t pen_x, int32_t base_y, const uint16_t *text, unsigned int len, COVER *cover )
{
    TFACE *alt = NULL;
    FONT *font;
    const int32_t *use;
    int32_t mat[4], own_mat[4], step, at;
    int reach, pixel;
    int32_t smear = 0;
    uint32_t code, glyph;
    int wide, own;

    if ( em <= 0 || text == NULL ) {
        return pen_x;
    }
    /* pixels to a thousandth of an em, 8.24; y grows downward */
    mat[0] = mul_div( em, 256, 1000 );
    mat[1] = mat[2] = 0;
    mat[3] = -mat[0];
    /* how many pixels from the pen a character's ink can be */
    reach = FX_CEIL( em ) * 2 + 4;
    if ( face->file != NULL ) {
        if ( file_ready( face->file ) ) {
            /* the file's own units; a slope that is made leans a fifth
               of its height, and a bold that is made is the shape twice,
               a twenty-fourth of an em apart and never less than half a
               pixel */
            alt = face->standin;
            face->font.prog = face->file->prog;
            own_mat[0] = mul_div( em, 256, face->font.units );
            own_mat[1] = 0;
            own_mat[2] = (face->made & CF_ITALIC) ? own_mat[0] / 5 : 0;
            own_mat[3] = -own_mat[0];
            if ( face->made & CF_BOLD ) {
                smear = em / 24 > FX_HALF ? em / 24 : FX_HALF;
            }
        } else {
            face = face->standin;
        }
    }

    draw_at = 0;
    draw_pen = pen_x;
    draw_dry = cover == NULL || cover->pix == NULL || cover->width <= 0 || cover->height <= 0 ||
               cover->stride < (unsigned int)cover->width;
    /* from here to the end the file's bytes are read, and the shapes
       ask for memory */
    file_busy = alt != NULL;
    /* no memory for a shape leaves the PDF code through pdf_trap, if
       the program's out_of_memory is so written: it comes back here,
       and the rest of the text is measured */
    own = pdf_trap == NULL;
    if ( own ) {
        glyph_page_begin();
        pdf_trap = &draw_trap;
        if ( rt_setjmp( &draw_trap ) ) {
            draw_dry = 1;
        }
    }
    while ( draw_at < len ) {
        code = text[draw_at++];
        if ( code >= 0xD800 && code <= 0xDBFF && draw_at < len && text[draw_at] >= 0xDC00 &&
             text[draw_at] <= 0xDFFF ) {
            code = 0x10000UL + ((code - 0xD800) << 10) + (text[draw_at++] - 0xDC00);
        }
        font = &face->font;
        use = mat;
        wide = -1;
        glyph = 0;
        if ( alt != NULL ) {
            glyph = file_glyph( face, code, &wide );
            use = own_mat;
        }
        if ( wide < 0 ) {
            /* a built-in face's character, or one the file has not got */
            font = alt != NULL ? &alt->font : font;
            use = mat;
            glyph = face_glyph( alt != NULL ? alt : face, code, &wide );
        }
        step = mul_div( em, wide, 1000 );
        at = draw_pen;
        /* the pen moves before the shape is asked for, so that a jump
           out of the asking finds it moved */
        draw_pen = step > 0 && at > FX_MAX - step ? FX_MAX : at + step;
        pixel = FX_FLOOR( at );
        if ( glyph && !draw_dry && pixel > -reach && pixel - reach < cover->width ) {
            glyph_cover( font, glyph, at, base_y, use, 0, 0, cover->width, cover->height,
                         cover_row, cover );
            if ( smear && use == own_mat ) {
                glyph_cover( font, glyph, at + smear, base_y, use, 0, 0, cover->width, cover->height,
                             cover_row, cover );
            }
        }
    }
    file_busy = 0;
    if ( own ) {
        pdf_trap = NULL;
    }
    if ( alt != NULL && ink_cut != 0 && !draw_dry ) {
        /* the columns this text can have ink in: to the block's edge if
           the pen ran off the end of what a number holds */
        thin_keep( cover, (int)FX_FLOOR( pen_x ) - (int)reach,
                   draw_pen > FX_MAX - 2 * FX_ONE ? cover->width : (int)FX_CEIL( draw_pen ) + (int)reach );
    }
    return draw_pen;
}

/* ------------------------------------------------------------------ */
/* a file's face, for the PDF writer                                   */
/* ------------------------------------------------------------------ */

TFACE *tf_standin( TFACE *face )
{
    return face->file != NULL ? face->standin : face;
}

void *tf_file( TFACE *face )
{
    return face->file != NULL && !face->file->dead ? face->file : NULL;
}

int tf_embed( TFACE *face )
{
    TFILE *file = face->file;

    return file != NULL && !file->dead && file->known && file->facts.whole && !(file->fstype & FS_NEVER);
}

int tf_made( TFACE *face )
{
    return face->file != NULL ? face->made : 0;
}

uint32_t tf_glyph( TFACE *face, uint32_t code )
{
    int wide;
    uint32_t gid;

    if ( face->file == NULL ) {
        return 0;
    }
    gid = file_glyph( face, code, &wide );
    return wide >= 0 ? gid : 0;
}

unsigned int tf_glyph_advance( TFACE *face, uint32_t glyph )
{
    if ( face->file == NULL || !file_ready( face->file ) ) {
        return 0;
    }
    return (unsigned int)units_wide( face, ttf_advance( face->file->prog, glyph ) );
}

int tf_file_info( TFACE *face, TFINFO *info )
{
    const TTFACTS *facts;
    const char *from;
    unsigned int used = 0, index;

    mem_set( info, 0, sizeof( *info ) );
    if ( face->file == NULL || !face->file->known ) {
        return 0;
    }
    facts = &face->file->facts;
    /* the name PostScript has for it, or its family: letters, digits
       and a hyphen are all a name in a PDF can hold without escapes */
    from = facts->name[0] ? facts->name : face->file->family;
    for ( ; *from && used + 1 < sizeof( info->name ); from++ ) {
        if ( (*from >= 'A' && *from <= 'Z') || (*from >= 'a' && *from <= 'z') || (*from >= '0' && *from <= '9') ||
             *from == '-' ) {
            info->name[used++] = *from;
        }
    }
    if ( used == 0 ) {
        str_cpy( info->name, "Font" );
    }
    info->glyphs = (uint32_t)facts->glyphs;
    for ( index = 0; index < 4; index++ ) {
        info->box[index] = units_wide( face, facts->box[index] );
    }
    info->ascent = units_wide( face, facts->ascent );
    info->descent = units_wide( face, facts->descent );
    info->cap = units_wide( face, facts->cap );
    info->angle = facts->angle > 90 ? 90 : facts->angle < -90 ? -90 : facts->angle;
    info->fixed = facts->fixed;
    info->serif = face->base == TFB_TIMES;
    info->symbol = face->kind != KIND_LATIN;
    return 1;
}

uint8_t *tf_subset( TFACE *face, const uint8_t *keep, unsigned int count, uint32_t *len )
{
    TFILE *file = face->file;
    uint8_t *out;

    *len = 0;
    if ( !tf_embed( face ) || !file_ready( file ) ) {
        return NULL;
    }
    file_busy = 1;
    if ( file->fstype & FS_WHOLE ) {
        out = (uint8_t *)try_alloc( file->len );
        if ( out != NULL ) {
            mem_cpy( out, file->data, file->len );
            *len = file->len;
        }
    } else {
        out = ttf_subset( file->prog, keep, count, len );
    }
    file_busy = 0;
    return out;
}
