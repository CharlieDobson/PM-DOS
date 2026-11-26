/*
 * PDF.H - the PDF file itself: numbers, objects, the cross-reference
 * table, streams and their filters, encryption, and the page tree.
 * GFX.H is what is done with a page once it has been found.
 *
 * NOTHING HERE USES FLOATING POINT.  PM-DOS runs on a 386 with no
 * coprocessor, so a number from the file is a REAL - a mantissa and a
 * binary exponent, which is exact enough for a matrix and has the
 * range a file may use - and a position on the screen is an fx, a
 * 16.16 fixed-point number.
 *
 * MEMORY.  A page's objects come from a pool (RT.H) that is freed when
 * the page is done with, so nothing here frees an object.  A page that
 * cannot be finished - the file is damaged past reading, or memory
 * ran out - is left with pdf_fail(), which jumps back to whoever asked
 * for the page; everything it had allocated is in pools and goes with
 * them.
 */
#ifndef PDF_H
#define PDF_H

#include "types.h"
#include "rt.h"
#include "sys.h"

/* ------------------------------------------------------------------ */
/* fixed point                                                         */
/* ------------------------------------------------------------------ */

typedef int32_t fx;                     /* 16.16 */

#define FX_ONE          0x10000L
#define FX_HALF         0x8000L
#define FX_MAX          0x7FFFFFFFL
#define I2FX( val )     ((fx)((int32_t)(val) * 65536L))
#define FX_FLOOR( val ) ((int32_t)(val) >> 16)
#define FX_CEIL( val )  (((int32_t)(val) + 0xFFFFL) >> 16)
#define FX_ROUND( val ) (((int32_t)(val) + 0x8000L) >> 16)
#define FX_FRAC( val )  ((int32_t)(val) & 0xFFFFL)

/* (lhs * rhs) >> shift through the 64-bit product; shift is 0..31 */
int32_t mul_shr( int32_t lhs, int32_t rhs, uint32_t shift );
#pragma aux mul_shr =       \
    "imul edx"              \
    "shrd eax,edx,cl"       \
    parm [eax] [edx] [ecx] value [eax] modify [edx];

#define fx_mul( lhs, rhs )  mul_shr( (lhs), (rhs), 16 )

uint32_t umul_hi( uint32_t lhs, uint32_t rhs );
#pragma aux umul_hi = "mul edx" parm [eax] [edx] value [edx] modify [eax];

/* hi:lo / den; the caller has made sure hi < den */
uint32_t udiv64( uint32_t hi, uint32_t lo, uint32_t den );
#pragma aux udiv64 = "div ebx" parm [edx] [eax] [ebx] value [eax] modify [edx];

/* the highest set bit of a value that is not zero */
int bit_top( uint32_t val );
#pragma aux bit_top = "bsr eax,eax" parm [eax] value [eax];

int32_t  mul_div( int32_t lhs, int32_t rhs, int32_t den );  /* saturates */
fx       fx_div( fx num, fx den );                          /* saturates */
uint32_t isqrt( uint32_t val );
fx       fx_sqrt( fx val );
fx       fx_hypot( fx dx, fx dy );
fx       fx_sin( int deg );      /* of a whole number of degrees */
fx       fx_cos( int deg );
fx       fx_pow( fx base, fx expo );
fx       fx_log2( fx val );
fx       fx_atan2( fx dy, fx dx ); /* in degrees, 0 up to 360 */

/* a number as the file has it: man * 2^exp, man zero or normalised so
   that 2^29 <= |man| < 2^30 */
typedef struct {
    int32_t man;
    int32_t exp;
} REAL;

REAL    real_int( int32_t val );
REAL    real_fx( fx val );
REAL    real_mul( REAL lhs, REAL rhs );
REAL    real_div( REAL lhs, REAL rhs );
REAL    real_add( REAL lhs, REAL rhs );
REAL    real_neg( REAL val );
fx      real_to_fx( REAL val );                 /* saturates */
int32_t real_to_int( REAL val );                /* towards zero, saturates */
REAL    real_dec( uint32_t digits, int scale ); /* digits / 10^scale (scale may be negative) */

/* a matrix as PDF writes one: x' = a*x + c*y + e, y' = b*x + d*y + f */
typedef struct {
    REAL m[6];
} RMAT;

typedef struct {
    fx a, b, c, d, e, f;
} FMAT;

void rmat_identity( RMAT *mat );
void rmat_mul( RMAT *out, const RMAT *first, const RMAT *then );    /* first, then "then" */
int  rmat_invert( RMAT *out, const RMAT *mat );                     /* 0 = singular */
void rmat_to_fmat( FMAT *out, const RMAT *mat );
void rmat_point( const RMAT *mat, REAL px, REAL py, fx *ox, fx *oy );
void rmat_set( RMAT *mat, REAL ma, REAL mb, REAL mc, REAL md, REAL me, REAL mf );
void fmat_point( const FMAT *mat, fx px, fx py, fx *ox, fx *oy );

/* ------------------------------------------------------------------ */
/* objects                                                             */
/* ------------------------------------------------------------------ */

#define T_NULL      0
#define T_BOOL      1
#define T_INT       2
#define T_REAL      3
#define T_STR       4
#define T_NAME      5
#define T_ARR       6
#define T_DICT      7
#define T_STREAM    8       /* a dictionary with data behind it */
#define T_REF       9
#define T_OP        10      /* a word that is none of those: an operator */
#define T_EOF       11

typedef struct OBJ  OBJ;
typedef struct DICT DICT;
typedef struct ARR  ARR;

struct OBJ {
    uint8_t  type;
    uint8_t  spare;
    uint16_t gen;               /* T_REF */
    union {
        int32_t ival;           /* T_BOOL, T_INT; T_REF: the object's number */
        REAL    real;
        struct {
            uint8_t *ptr;       /* T_STR; T_NAME and T_OP end with a NUL too */
            uint32_t len;
        } str;
        ARR  *arr;
        DICT *dict;             /* T_DICT, T_STREAM */
    } u;
};

struct ARR {
    unsigned int count;
    OBJ         *items;
};

typedef struct {
    const char *key;
    OBJ         val;
} DENT;

struct DICT {
    unsigned int count;
    DENT        *ents;
    uint32_t     num;               /* the object this is, 0 if it is inside one */
    uint16_t     gen;
    uint8_t      crypt_off;         /* its stream is not encrypted */
    uint8_t      spare;
    uint32_t     stm_pos;           /* T_STREAM: where the data is in the file... */
    const uint8_t *stm_mem;         /* ...or in memory */
    uint32_t     stm_len;           /* in memory only: /Length says for a file */
};

extern OBJ obj_null;

OBJ    *obj_resolve( OBJ *obj );                  /* through a reference; never NULL */
OBJ    *dict_raw( DICT *dict, const char *key );  /* NULL if it is not there */
OBJ    *dict_get( DICT *dict, const char *key );  /* resolved; &obj_null if not there */
OBJ    *dict_get2( DICT *dict, const char *key, const char *abbrev );
int32_t dict_int( DICT *dict, const char *key, int32_t def );
fx      dict_fx( DICT *dict, const char *key, fx def );
const char *dict_name( DICT *dict, const char *key );   /* "" if it is not a name */
DICT   *dict_dict( DICT *dict, const char *key );       /* NULL if it is not one */
ARR    *dict_arr( DICT *dict, const char *key );
OBJ    *arr_get( ARR *arr, unsigned int index );      /* resolved; &obj_null past the end */
int32_t obj_int( OBJ *obj );
fx      obj_fx( OBJ *obj );
REAL    obj_real( OBJ *obj );
int     obj_is_num( OBJ *obj );
int     obj_is_name( OBJ *obj, const char *name );
DICT   *obj_dict( OBJ *obj );                     /* of a dictionary or a stream, else NULL */
const char *obj_name( OBJ *obj );                 /* "" if it is not a name */

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

/*
 * AN IN IS BYTES TO BE READ, wherever they come from: the file, a
 * buffer, or a filter with another IN behind it.  A filter keeps its
 * state after the IN in the same block (pool_big), so closing one is
 * giving the block back.
 */
typedef struct IN IN;
struct IN {
    uint8_t *cur, *end;         /* what has been produced and not yet read */
    int  (*fill)( IN *in );     /* produce more: 0 when there is none */
    void (*done)( IN *in );     /* give back what fill needed, if anything */
    IN      *src;               /* the IN this one reads, closed with it */
    uint32_t pos;               /* the file: the offset "end" is at */
    uint8_t  at_eof;
};

#define IN_GETC( in )   ((in)->cur < (in)->end ? (int)*(in)->cur++ : in_more( in ))

int      in_more( IN *in );             /* the next byte, or -1 */
int      in_peek( IN *in );             /* ...without taking it */
uint32_t in_read( IN *in, uint8_t *buf, uint32_t len );
uint32_t in_skip( IN *in, uint32_t count );
void     in_close( IN *in );            /* and everything behind it */
IN      *in_new( unsigned int extra, int (*fill)( IN * ), IN *src );
#define IN_STATE( in, type )    ((type *)((in) + 1))

IN      *in_file( uint32_t pos, uint32_t len );  /* the PDF file; len ~0 for "to the end" */
uint32_t in_tell( IN *in );                      /* of an in_file */
IN      *in_mem( const uint8_t *data, uint32_t len );
IN      *in_borrow( IN *parent, uint32_t limit ); /* the next bytes of another, not copied */

/* FILTER.C and the decoders behind it */
IN   *flt_inflate( IN *src );
IN   *flt_lzw( IN *src, int early );
IN   *flt_ahex( IN *src );
IN   *flt_a85( IN *src );
IN   *flt_rle( IN *src );
IN   *flt_predict( IN *src, int predictor, int colors, int bpc, int columns );
IN   *flt_ccitt( IN *src, DICT *parms );
IN   *flt_dct( IN *src, DICT *parms );
IN   *flt_rc4( IN *src, const uint8_t *key, uint32_t keylen );
IN   *flt_aes( IN *src, const uint8_t *key, uint32_t keylen );

/* what a DCT stream turned out to be, for the image that asked */
extern int dct_width, dct_height, dct_comps;

/* ------------------------------------------------------------------ */
/* the lexer                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    IN      *in;
    POOL    *pool;              /* where strings, arrays and dictionaries go */
    int      content;           /* a content stream: no "n g R", and an operator
                                   is a T_OP */
    uint32_t num;               /* strings are decrypted as this object's... */
    uint16_t gen;
    uint8_t  crypt;             /* ...if this says to */
    uint8_t  has_pend;
    OBJ      pend;              /* a number read ahead while looking for R */
    char     word[64];          /* the last T_OP */
} LEX;

void  lex_init( LEX *lex, IN *in, POOL *pool, int content );
int   lex_obj( LEX *lex, OBJ *out );    /* the type: T_EOF at the end */
int   lex_skip_ws( IN *in );            /* the next byte that is not space or
                                           comment, not taken; -1 at the end */

#define IS_WS( ch )     ((ch) == ' ' || (ch) == '\n' || (ch) == '\r' || (ch) == '\t' || \
                         (ch) == '\f' || (ch) == 0)
int   is_delim( int ch );

/* ------------------------------------------------------------------ */
/* the document                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t pos;               /* the offset, or the object stream's number */
    uint16_t gen;               /* the generation, or the index in that stream */
    uint8_t  type;              /* 0 free, 1 in the file, 2 in an object stream */
    uint8_t  spare;
} XENT;

#define CRYPT_NONE  0
#define CRYPT_RC4   1
#define CRYPT_AES   2           /* AESV2: 128 bits, a key for each object */
#define CRYPT_AES256 3          /* AESV3: 256 bits, one key */

typedef struct {
    uint32_t     file;                 /* the handle; 0 when the file is in memory... */
    const uint8_t *mem;                /* ...here (pdf_open_mem) */
    uint32_t     size;
    uint32_t     base;                 /* bytes of junk before "%PDF" */
    int          ver_major, ver_minor;
    POOL        *pool;                 /* the document's own: trailer, catalogue */
    POOL        *page_pool;            /* the page being shown */
    XENT        *xref;
    unsigned int xref_count;
    DICT        *trailer;
    DICT        *root;
    int          page_count;
    int          repaired;             /* the table was rebuilt by reading the file */
    /* encryption */
    int          encrypted;
    int          stm_crypt, str_crypt; /* CRYPT_* */
    uint8_t      key[32];
    uint32_t     key_len;
    int          crypt_rev;
    DICT        *crypt_dict;
    uint8_t      file_id[64];
    uint32_t     file_id_len;
    int          crypt_meta;
} PDFDOC;

extern PDFDOC doc;

/* why a page, or the file, could not be read */
#define PE_NONE     0
#define PE_NOMEM    1
#define PE_DAMAGED  2
#define PE_NOTPDF   3
#define PE_PASSWORD 4           /* it needs one */
#define PE_CRYPT    5           /* encrypted in a way this does not know */
#define PE_IO       6
#define PE_ABORT    7           /* a key was pressed */

extern JMPBUF *pdf_trap;        /* where pdf_fail goes */
void  pdf_fail( int why );      /* does not return */

/* One document is open at a time.  An open that fails leaves nothing
   open, except PE_PASSWORD, which waits for pdf_password and
   pdf_open_finish - or for pdf_close. */
int      pdf_open( const char *path );                       /* PE_* */
int      pdf_open_mem( const uint8_t *data, uint32_t len );  /* the same, of a file that is in memory:
                                                                the bytes stay the caller's, and stay put */
int      pdf_password( const char *pw, uint32_t len );       /* 1 = it opens the file */
void     pdf_close( void );                                  /* the file, its fonts and its glyphs */
int      pdf_reclaim( void );                                /* a cache given back: 1 if one was.  For
                                                                the program's mem_reclaim (RT.H) */
void     pdf_done( void );                                   /* pdf_close, and the work space too */
OBJ     *pdf_load( uint32_t num );                           /* an object by number; never NULL */
void     pdf_page_begin( void );                             /* a new pool, an empty cache */
void     pdf_page_end( void );
IN      *pdf_stream( OBJ *stm );                             /* its data, decoded; NULL if it
                                                                cannot be */
IN      *pdf_stream_raw( OBJ *stm, int *last_filter );             /* up to an image's own filter */
uint8_t *pdf_stream_all( OBJ *stm, uint32_t *len, uint32_t max );  /* all of it, in the page pool */
uint8_t *pdf_stream_pool( OBJ *stm, POOL *pool, uint32_t *len, uint32_t max );
uint32_t pdf_stream_length( DICT *dict );
IN      *pdf_filter_chain( IN *in, OBJ *holder, int stop_at_image, int *last_filter );
const char *pdf_last_filter( OBJ *stm );                     /* the name of its last filter, or "" */
DICT    *pdf_filter_parms( OBJ *stm, int index );
int      pdf_filter_count( OBJ *stm );
const char *pdf_filter_name( OBJ *stm, int index );

/* a page */
typedef struct {
    DICT *dict;
    DICT *res;                  /* its resources, inherited if need be */
    REAL  box[4];               /* the crop box: x0 y0 x1 y1, x0 < x1 and y0 < y1 */
    int   rotate;               /* 0, 90, 180 or 270 */
} PAGE;

int   pdf_page( int index, PAGE *page );        /* from 0; 0 = there is no such page */
int   pdf_count_pages( void );
int   pdf_open_finish( void );                  /* after a password was taken: PE_* */

/* CRYPT.C */
int   crypt_setup( DICT *enc );                 /* PE_* */
void  crypt_object_key( uint32_t num, uint32_t gen, int aes, uint8_t *key, uint32_t *keylen );
void  crypt_string( uint32_t num, uint32_t gen, uint8_t *data, uint32_t *len );
IN   *crypt_stream( IN *src, uint32_t num, uint32_t gen );

void  md5( const uint8_t *data, uint32_t len, uint8_t *digest );
void  sha256( const uint8_t *data, uint32_t len, uint8_t *digest );
void  sha512( const uint8_t *data, uint32_t len, uint8_t *digest, int bytes ); /* 48: SHA-384 */
int   crypt_selftest( void );
typedef struct {
    uint32_t state[4];
    uint32_t count;
    uint8_t  buf[64];
} MD5CTX;
void  md5_init( MD5CTX *ctx );
void  md5_update( MD5CTX *ctx, const uint8_t *data, uint32_t len );
void  md5_final( MD5CTX *ctx, uint8_t *digest );

#endif
