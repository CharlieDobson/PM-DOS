/*
 * FMT.H - what a file format's code gives the rest of PMWRITE, and what
 * every format's code may use: an output file (OUT), the code pages
 * (CP), and UTF-8.
 *
 * A FORMAT is one F_*.C file.  It is handed the WHOLE FILE IN MEMORY
 * to read - the file was read by whoever asked - and an OUT to write
 * to.  It trusts nothing in the file: every length and offset it reads
 * is checked against the bytes there are before it is used.
 */
#ifndef FMT_H
#define FMT_H

#include "wdoc.h"

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

/*
 * An OUT is bytes going to a file, or into memory.  Nothing that puts
 * bytes answers with an error: the first one is kept, everything after
 * it is dropped, and out_error() says what it was - so a writer writes
 * to the end and asks once.  out_seek is for going back to fill in a
 * length; an OUT in memory grows as it is written.
 */
typedef struct OUT OUT;

OUT     *out_file( const char *path );        /* created, or emptied; NULL if it cannot be */
OUT     *out_mem( void );                     /* NULL if there is no memory */
int      out_close( OUT *out );               /* -> out_error(); an OUT in memory is freed */
int      out_error( const OUT *out );         /* 0, a DOS error code, or WE_NOMEM */
void     out_fail( OUT *out, int error );     /* a writer's own reason to stop */
void     out_put( OUT *out, const void *data, unsigned int len );
void     out_byte( OUT *out, int value );
void     out_le16( OUT *out, uint32_t value );
void     out_le32( OUT *out, uint32_t value );
void     out_be16( OUT *out, uint32_t value );
void     out_be32( OUT *out, uint32_t value );
void     out_fill( OUT *out, int value, unsigned int count );
void     out_str( OUT *out, const char *text );
void  __cdecl out_fmt( OUT *out, const char *fmt, ... );    /* sfmt's: 512 bytes at most */
uint32_t out_tell( const OUT *out );
void     out_seek( OUT *out, uint32_t pos );  /* anywhere up to the end */
uint32_t out_size( const OUT *out );          /* the end */
/* an OUT in memory: its bytes, which are the OUT's until it is closed */
uint8_t *out_data( OUT *out, uint32_t *len );
/* ...or taken away from it: the caller's to xfree, and the OUT is empty */
uint8_t *out_take( OUT *out, uint32_t *len );

/* ------------------------------------------------------------------ */
/* reading bytes that may not be there                                 */
/* ------------------------------------------------------------------ */
#define LE16( ptr )     ((uint32_t)((const uint8_t *)(ptr))[0] | ((uint32_t)((const uint8_t *)(ptr))[1] << 8))
#define LE32( ptr )     (LE16( ptr ) | (LE16( (const uint8_t *)(ptr) + 2 ) << 16))
#define BE16( ptr )     (((uint32_t)((const uint8_t *)(ptr))[0] << 8) | (uint32_t)((const uint8_t *)(ptr))[1])
#define BE32( ptr )     ((BE16( ptr ) << 16) | BE16( (const uint8_t *)(ptr) + 2 ))

/* 1 if "count" bytes at "pos" are inside "len" bytes: written so that
   no sum can wrap */
#define IN_RANGE( pos, count, len ) \
    ((uint32_t)(pos) <= (uint32_t)(len) && (uint32_t)(count) <= (uint32_t)(len) - (uint32_t)(pos))

/* ------------------------------------------------------------------ */
/* code pages and UTF-8 (CP.C)                                         */
/* ------------------------------------------------------------------ */
#define CP_ASCII        0       /* 7 bits; the rest are '?' */
#define CP_437          437     /* the PC's own */
#define CP_850          850     /* DOS, western Europe */
#define CP_852          852     /* DOS, central Europe */
#define CP_860          860
#define CP_863          863
#define CP_865          865
#define CP_866          866     /* DOS, Cyrillic */
#define CP_1250         1250    /* Windows, central Europe */
#define CP_1251         1251    /* Windows, Cyrillic */
#define CP_1252         1252    /* Windows, western: "ANSI" */
#define CP_1253         1253
#define CP_1254         1254
#define CP_MAC          10000   /* Macintosh Roman */
#define CP_LATIN1       28591   /* ISO 8859-1: a byte is its own code point */
#define CP_UTF8         65001

int      cp_known( int codepage );
/* a byte of a code page as Unicode: 0xFFFD for one that is nothing */
uint16_t cp_to_uni( int codepage, int byte );
/* ...and back: the byte, or -1 when the code page has no such character */
int      cp_from_uni( int codepage, uint32_t code );
/* the nearest thing in ASCII to a character no code page had: "(c)",
   "...", a letter without its accent.  -> how many bytes, 0 for none;
   "out" has room for 8. */
int      uni_fallback( uint32_t code, char *out );
/* ...so: a character written in a code page, as itself or as the
   nearest thing, or as '?'.  -> bytes written (1 to 8). */
int      cp_put( int codepage, uint32_t code, char *out );

/* the next character of UTF-8 at *cur, which moves past it; bad bytes
   are 0xFFFD each.  "end" is where the text stops. */
uint32_t utf8_get( const uint8_t **cur, const uint8_t *end );
int      utf8_put( uint8_t *out, uint32_t code ); /* -> 1 to 4 bytes */
/* UTF-16 at text[*pos], which moves past it: a whole character */
uint32_t utf16_get( const uint16_t *text, unsigned int len, unsigned int *pos );
/* a document's own text as UTF-8, to an OUT */
void     out_utf8( OUT *out, const uint16_t *text, unsigned int len );
/* UTF-8 into a fixed buffer, cut at a character if it does not fit */
void     utf8_copy( char *dst, unsigned int max, const uint16_t *text, unsigned int len );

/* ------------------------------------------------------------------ */
/* the formats                                                         */
/* ------------------------------------------------------------------ */
enum {
    FMT_NONE,
    FMT_TXT,            /* plain text */
    FMT_RTF,            /* Rich Text Format */
    FMT_DOCX,           /* Word 2007 and later */
    FMT_ODT,            /* OpenDocument Text */
    FMT_DOC,            /* Word 6.0, 95, 97-2003 */
    FMT_WINWORD,        /* Word for Windows 1.x and 2.x */
    FMT_WORDDOS,        /* Word for DOS */
    FMT_WRITE,          /* Windows Write */
    FMT_WORDSTAR,
    FMT_WORDPERFECT,
    FMT_AMIPRO,
    FMT_WORDPRO,
    FMT_PDF,
    FMT_COUNT
};

typedef struct {
    int   id;                   /* FMT_* */
    const char *name;           /* as the Open and Save As dialogs list it */
    const char *exts;           /* "DOC;DOT": the first is what Save As adds */
    /*
     * How sure it is that the file is one of its own, 0 to 100: 0 "no",
     * 100 "it has my signature".  "ext" is the name's extension in
     * capitals, without the dot ("" if it has none).  A format with no
     * signature of its own answers from the bytes' look and the
     * extension, and never above 60.
     */
    int (*detect)( const uint8_t *data, uint32_t len, const char *ext );
    /* the file into an empty document (wd_new's): WE_* */
    int (*load)( const uint8_t *data, uint32_t len, WDOC *doc );
    /* the document to a file, as the kind "variant" says: WE_*.  NULL
       for a format that is only read. */
    int (*save)( WDOC *doc, OUT *out, int variant );
    /* the kinds it can write, as Save As lists them, a NULL after the
       last; and the one it writes when nobody says */
    const char *const *variants;
    int   def_variant;
} FORMAT;

extern const FORMAT *const formats[];   /* FMTTAB.C: FMT_COUNT entries, [0] NULL */

const FORMAT *fmt_get( int id );
/* which format a file is: the one that is surest, FMT_NONE for none.
   Plain text is the answer for anything that looks like text. */
int   fmt_detect( const uint8_t *data, uint32_t len, const char *name );
/* a format by its name on a command line ("DOCX", "WORDSTAR"), and a
   variant by the start of its name; -1 for neither */
int   fmt_by_tag( const char *tag );
const char *fmt_tag( int id );
int   fmt_variant( const FORMAT *fmt, const char *text );

/* a file's extension in capitals, without the dot: at most 7 letters */
void  fmt_ext_of( const char *name, char *ext );

/* the document described line by line: what the tests compare */
void  wd_dump( WDOC *doc, OUT *out );

#endif
