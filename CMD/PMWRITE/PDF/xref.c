/*
 * XREF.C - opening a PDF file and finding its objects.
 *
 * EVERY VERSION'S WAY OF SAYING WHERE AN OBJECT IS:
 *
 *   1.0  a cross-reference table - "xref", then twenty bytes an object -
 *        and a trailer dictionary after it
 *   1.1  ...and the tables of earlier saves behind /Prev, each one
 *        overriding the objects before it
 *   1.5  a cross-reference STREAM instead: binary, compressed, and able
 *        to say that an object is the nth one inside an object stream
 *   1.5  both at once (a "hybrid" file): a table for old readers, whose
 *        trailer names a stream in /XRefStm for the objects it hides
 *   2.0  nothing new here
 *
 * and for a file whose table is wrong or missing - cut short, edited by
 * hand, or with its offsets shifted by a mail gateway - the table is
 * rebuilt by reading the file for "n g obj" from one end to the other.
 *
 * Sections are read newest first and an object's first definition is
 * the one kept, which is what makes the newest win.
 */
#include "pdf.h"

#define OC_SIZE     2048            /* objects of the page, by number */
static uint32_t *oc_num;            /* from the heap: static, they would be 16K */
static OBJ     **oc_obj;            /* of zeroes in the program's file */
static int  load_depth;

/* a decoded object stream, kept from page to page: most pages take
   their objects from one or two */
typedef struct {
    uint32_t  num;
    uint32_t *offs;                 /* where each object starts, from "first" */
    uint8_t  *data;
    uint32_t  len, count, first;
    uint32_t  stamp;
    int       in_pool;              /* the page's memory, not the heap's */
} OSTM;

#define OSTM_SLOTS  3
static OSTM     ostm[OSTM_SLOTS];
static uint32_t ostm_clock;

int pdf_count_pages( void );
int cache_reclaim( void );          /* GLYPH.C: the caches of fonts and glyphs */
void lex_reset( void );             /* LEX.C: what a page given up left behind */
void color_reset( void );           /* COLOR.C: the same, of its two counts */
void in_file_reset( void );         /* STREAM.C: where the handle is, forgotten */
int font_reclaim( int all );        /* FONT.C */
void glyph_page_begin( void );      /* GLYPH.C */
void lex_done( void );              /* the scratch memory of each of these */
void raster_done( void );
void path_dash_done( void );
void glyph_done( void );
void ttf_done( void );
void ccitt_done( void );
void interp_done( void );

/* ------------------------------------------------------------------ */
/* the page's pool and its cache                                       */
/* ------------------------------------------------------------------ */

static void ostm_drop( OSTM *slot )
{
    if ( slot->num && !slot->in_pool ) {
        xfree( slot->offs );
    }
    slot->num = 0;
}

void pdf_page_end( void )
{
    int index;

    for ( index = 0; index < OSTM_SLOTS; index++ ) {
        if ( ostm[index].in_pool ) {
            ostm[index].num = 0;
            ostm[index].in_pool = 0;
        }
    }
    if ( doc.page_pool ) {
        pool_free( doc.page_pool );
        doc.page_pool = NULL;
    }
    /* nothing is allocated here: this is what is called when a page
       has been given up for want of memory */
    if ( oc_num ) {
        mem_set( oc_num, 0, OC_SIZE * sizeof( uint32_t ) );
    }
    load_depth = 0;
    lex_reset();
    color_reset();
}

void pdf_page_begin( void )
{
    pdf_page_end();
    if ( oc_num == NULL ) {
        oc_num = (uint32_t *)xalloc( OC_SIZE * (sizeof( uint32_t ) + sizeof( OBJ * )) );
        oc_obj = (OBJ **)(oc_num + OC_SIZE);
        mem_set( oc_num, 0, OC_SIZE * sizeof( uint32_t ) );
    }
    doc.page_pool = pool_new();
}

/*
 * What can be given back while a document is open: the oldest object
 * stream that is not the one in use, and then the glyphs, the fonts
 * the page in hand has not used, and the scan converter's edges.  1 if
 * anything was.  The program's own mem_reclaim (RT.H) calls this among
 * its other caches; ACROREAD's was this and nothing else.
 */
int pdf_reclaim( void )
{
    int index, oldest = -1;

    for ( index = 0; index < OSTM_SLOTS; index++ ) {
        if ( ostm[index].num && !ostm[index].in_pool &&
             (oldest < 0 || ostm[index].stamp < ostm[oldest].stamp) ) {
            oldest = index;
        }
    }
    if ( oldest >= 0 && ostm[oldest].stamp != ostm_clock ) {
        ostm_drop( &ostm[oldest] );
        return 1;
    }
    return cache_reclaim();
}

/* ------------------------------------------------------------------ */
/* reading an object where it lies                                     */
/* ------------------------------------------------------------------ */

static int read_uint( IN *in, uint32_t *val )
{
    int ch = lex_skip_ws( in );
    uint32_t num = 0;

    if ( ch < '0' || ch > '9' ) {
        return 0;
    }
    while ( ch >= '0' && ch <= '9' ) {
        in->cur++;
        num = num * 10 + (uint32_t)(ch - '0');
        ch = in_peek( in );
    }
    *val = num;
    return 1;
}

/* "n g obj", the object, and where its data starts if it is a stream.
   "pos" is from the file's "%PDF".  0 if there is no object there. */
static int read_object_at( uint32_t pos, POOL *pool, OBJ *out, uint32_t *num_out )
{
    IN *in = in_file( doc.base + pos, 0xFFFFFFFFUL );
    LEX lex;
    OBJ num, gen, word;
    uint32_t at;
    int ch, good = 0;

    out->type = T_NULL;
    lex_init( &lex, in, pool, 0 );
    if ( lex_obj( &lex, &num ) == T_INT && lex_obj( &lex, &gen ) == T_INT &&
         lex_obj( &lex, &word ) == T_OP && str_cmp( lex.word, "obj" ) == 0 ) {
        good = 1;
        lex.num = (uint32_t)num.u.ival;
        lex.gen = (uint16_t)gen.u.ival;
        lex.crypt = (uint8_t)(doc.encrypted && doc.str_crypt != CRYPT_NONE);
        lex_obj( &lex, out );
        if ( out->type == T_OP || out->type == T_EOF ) {
            out->type = T_NULL;             /* "obj endobj" */
        }
        if ( num_out ) {
            *num_out = (uint32_t)num.u.ival;
        }
        if ( out->type == T_DICT ) {
            out->u.dict->num = (uint32_t)num.u.ival;
            out->u.dict->gen = (uint16_t)gen.u.ival;
            if ( lex_skip_ws( in ) == 's' && lex_obj( &lex, &word ) == T_OP &&
                 str_cmp( lex.word, "stream" ) == 0 ) {
                at = in_tell( in );
                ch = IN_GETC( in );
                if ( ch == '\r' ) {
                    at += in_peek( in ) == '\n' ? 2 : 1;
                } else if ( ch == '\n' ) {
                    at++;
                }
                out->u.dict->stm_pos = at;
                out->type = T_STREAM;
            }
        }
    }
    in_close( in );
    return good;
}

/* ------------------------------------------------------------------ */
/* object streams                                                      */
/* ------------------------------------------------------------------ */

static OSTM *ostm_find( uint32_t num )
{
    OSTM *slot;
    OBJ *stm;
    DICT *dict;
    IN *in;
    uint8_t *data;
    uint32_t *offs, *block;
    uint32_t len, count, first, index, skip;
    int at, oldest = 0;

    for ( at = 0; at < OSTM_SLOTS; at++ ) {
        if ( ostm[at].num == num ) {
            ostm[at].stamp = ++ostm_clock;
            return &ostm[at];
        }
        if ( ostm[at].num == 0 || (ostm[oldest].num && ostm[at].stamp < ostm[oldest].stamp) ) {
            oldest = at;
        }
    }
    if ( num >= doc.xref_count || doc.xref[num].type != 1 ) {
        return NULL;                        /* not one inside another */
    }
    stm = pdf_load( num );
    dict = obj_dict( stm );
    if ( stm->type != T_STREAM ) {
        return NULL;
    }
    count = (uint32_t)dict_int( dict, "N", 0 );
    first = (uint32_t)dict_int( dict, "First", 0 );
    data = pdf_stream_all( stm, &len, 32UL * 1024 * 1024 );
    if ( data == NULL || first > len || count > first ) {
        return NULL;
    }
    offs = (uint32_t *)pool_big( doc.page_pool, (count + 1) * sizeof( uint32_t ) );
    in = in_mem( data, first );
    for ( index = 0; index < count; index++ ) {
        if ( !read_uint( in, &skip ) || !read_uint( in, &offs[index] ) ) {
            count = index;
            break;
        }
    }
    in_close( in );

    slot = &ostm[oldest];
    ostm_drop( slot );
    slot->num = num;
    slot->len = len;
    slot->count = count;
    slot->first = first;
    slot->stamp = ++ostm_clock;
    block = (uint32_t *)try_alloc( count * sizeof( uint32_t ) + len );
    if ( block ) {
        mem_cpy( block, offs, count * sizeof( uint32_t ) );
        mem_cpy( block + count, data, len );
        pool_unbig( offs );
        pool_unbig( data );
        slot->offs = block;
        slot->data = (uint8_t *)(block + count);
        slot->in_pool = 0;
    } else {
        slot->offs = offs;
        slot->data = data;
        slot->in_pool = 1;
    }
    return slot;
}

static void read_object_in( uint32_t stmnum, uint32_t index, uint32_t num, POOL *pool, OBJ *out )
{
    OSTM *slot = ostm_find( stmnum );
    IN *in;
    LEX lex;
    uint32_t start;

    out->type = T_NULL;
    if ( slot == NULL || index >= slot->count ) {
        return;
    }
    start = slot->first + slot->offs[index];
    if ( start >= slot->len ) {
        return;
    }
    in = in_mem( slot->data + start, slot->len - start );
    lex_init( &lex, in, pool, 0 );
    lex_obj( &lex, out );
    if ( out->type == T_OP || out->type == T_EOF ) {
        out->type = T_NULL;
    }
    if ( out->type == T_DICT ) {
        out->u.dict->num = num;
    }
    in_close( in );
}

/* ------------------------------------------------------------------ */
/* an object by number                                                 */
/* ------------------------------------------------------------------ */

static OBJ *load_into( uint32_t num, POOL *pool )
{
    OBJ *obj = (OBJ *)pool_alloc( pool, sizeof( OBJ ) );
    XENT *ent;
    uint32_t found;

    if ( num >= doc.xref_count ) {
        return obj;
    }
    ent = &doc.xref[num];
    if ( ent->type == 1 ) {
        if ( read_object_at( ent->pos, pool, obj, &found ) && found != num && !doc.repaired ) {
            obj->type = T_NULL;             /* the table points at another object */
        }
    } else if ( ent->type == 2 ) {
        read_object_in( ent->pos, ent->gen, num, pool, obj );
    }
    return obj;
}

OBJ *pdf_load( uint32_t num )
{
    uint32_t slot = num & (OC_SIZE - 1);
    OBJ *obj;
    int probe;

    if ( num == 0 ) {
        return &obj_null;
    }
    for ( probe = 0; probe < 8; probe++ ) {
        if ( oc_num[(slot + probe) & (OC_SIZE - 1)] == num ) {
            return oc_obj[(slot + probe) & (OC_SIZE - 1)];
        }
    }
    if ( load_depth >= 24 ) {
        return &obj_null;
    }
    load_depth++;
    obj = load_into( num, doc.page_pool );
    load_depth--;
    for ( probe = 0; probe < 8; probe++ ) {
        if ( oc_num[(slot + probe) & (OC_SIZE - 1)] == 0 ) {
            slot = (slot + probe) & (OC_SIZE - 1);
            break;
        }
    }
    oc_num[slot] = num;
    oc_obj[slot] = obj;
    return obj;
}

/* ------------------------------------------------------------------ */
/* the cross-reference table                                           */
/* ------------------------------------------------------------------ */

#define XREF_MAX    8388608UL       /* object numbers PDF allows: below 2^23 */

static void xref_need( unsigned int count )
{
    XENT *grown;
    unsigned int cap;

    if ( count <= doc.xref_count ) {
        return;
    }
    if ( count > doc.size || count > XREF_MAX ) {
        pdf_fail( PE_DAMAGED );             /* more objects than bytes, or than PDF has */
    }
    cap = (count + 1023) & ~1023UL;
    grown = (XENT *)xalloc( cap * sizeof( XENT ) );
    mem_set( grown, 0, cap * sizeof( XENT ) );
    if ( doc.xref ) {
        mem_cpy( grown, doc.xref, doc.xref_count * sizeof( XENT ) );
        xfree( doc.xref );
    }
    doc.xref = grown;
    doc.xref_count = cap;
}

static void xref_set( uint32_t num, int type, uint32_t pos, uint32_t gen )
{
    XENT *ent;

    if ( num >= XREF_MAX ) {
        pdf_fail( PE_DAMAGED );
    }
    xref_need( num + 1 );
    ent = &doc.xref[num];
    if ( ent->type == 0 ) {
        ent->type = (uint8_t)type;
        ent->pos = pos;
        ent->gen = (uint16_t)gen;
    }
}

/* after "xref": the subsections, then the trailer */
static DICT *xref_table( IN *in )
{
    LEX lex;
    OBJ obj;
    uint32_t first, count;
    unsigned int index;
    uint32_t pos, gen;
    int ch;

    for ( ;; ) {
        ch = lex_skip_ws( in );
        if ( ch < '0' || ch > '9' ) {
            break;
        }
        if ( !read_uint( in, &first ) || !read_uint( in, &count ) ) {
            return NULL;
        }
        if ( count > doc.size / 8 + 16 || first > doc.size ) {
            return NULL;
        }
        for ( index = 0; index < count; index++ ) {
            if ( !read_uint( in, &pos ) || !read_uint( in, &gen ) ) {
                return NULL;
            }
            ch = lex_skip_ws( in );
            if ( ch < 0 ) {
                return NULL;
            }
            in->cur++;
            /* a table that starts at 1 with the entry that belongs to 0 */
            if ( index == 0 && first == 1 && ch == 'f' && gen == 65535 ) {
                first = 0;
            }
            if ( ch == 'n' && pos != 0 ) {
                xref_set( first + index, 1, pos, gen );
            }
        }
    }
    lex_init( &lex, in, doc.pool, 0 );
    if ( lex_obj( &lex, &obj ) != T_OP || str_cmp( lex.word, "trailer" ) != 0 ) {
        return NULL;
    }
    if ( lex_obj( &lex, &obj ) != T_DICT ) {
        return NULL;
    }
    return obj.u.dict;
}

static uint32_t be_field( const uint8_t *data, int width )
{
    uint32_t val = 0;

    while ( width-- > 0 ) {
        val = (val << 8) | *data++;
    }
    return val;
}

/* the object at "pos" is a cross-reference stream: its dictionary is
   the trailer */
static DICT *xref_stream( uint32_t pos )
{
    OBJ *stm = (OBJ *)pool_alloc( doc.pool, sizeof( OBJ ) );
    DICT *dict;
    ARR *widths, *index;
    IN *in;
    uint8_t rec[24];
    uint32_t first, count;
    unsigned int at, pair;
    uint32_t type, size;
    int w0, w1, w2;

    if ( !read_object_at( pos, doc.pool, stm, NULL ) || stm->type != T_STREAM ) {
        return NULL;
    }
    dict = stm->u.dict;
    dict->crypt_off = 1;
    widths = dict_arr( dict, "W" );
    if ( widths == NULL || widths->count < 3 ) {
        return NULL;
    }
    w0 = (int)obj_int( arr_get( widths, 0 ) );
    w1 = (int)obj_int( arr_get( widths, 1 ) );
    w2 = (int)obj_int( arr_get( widths, 2 ) );
    if ( w0 < 0 || w1 < 0 || w2 < 0 || w0 > 8 || w1 > 8 || w2 > 8 || w0 + w1 + w2 == 0 ) {
        return NULL;
    }
    size = (uint32_t)dict_int( dict, "Size", 0 );
    index = dict_arr( dict, "Index" );
    in = pdf_stream( stm );
    if ( in == NULL ) {
        return NULL;
    }
    for ( pair = 0; ; pair += 2 ) {
        if ( index ) {
            if ( pair + 1 >= index->count ) {
                break;
            }
            first = (uint32_t)obj_int( arr_get( index, pair ) );
            count = (uint32_t)obj_int( arr_get( index, pair + 1 ) );
        } else {
            if ( pair ) {
                break;
            }
            first = 0;
            count = size;
        }
        if ( count > doc.size || first > doc.size ) {
            break;
        }
        for ( at = 0; at < count; at++ ) {
            if ( in_read( in, rec, (uint32_t)(w0 + w1 + w2) ) != (uint32_t)(w0 + w1 + w2) ) {
                break;
            }
            type = w0 ? be_field( rec, w0 ) : 1;
            if ( type == 1 ) {
                xref_set( first + at, 1, be_field( rec + w0, w1 ), be_field( rec + w0 + w1, w2 ) );
            } else if ( type == 2 ) {
                xref_set( first + at, 2, be_field( rec + w0, w1 ), be_field( rec + w0 + w1, w2 ) );
            }
        }
    }
    in_close( in );
    return dict;
}

/* one section, table or stream, at "pos": its trailer, or NULL */
static DICT *xref_section( uint32_t pos )
{
    IN *in;
    DICT *trailer = NULL;
    int ch;

    if ( pos >= doc.size ) {
        return NULL;
    }
    in = in_file( doc.base + pos, 0xFFFFFFFFUL );
    ch = lex_skip_ws( in );
    if ( ch == 'x' ) {
        in_skip( in, 4 );
        trailer = xref_table( in );
        in_close( in );
    } else {
        in_close( in );
        if ( ch >= '0' && ch <= '9' ) {
            trailer = xref_stream( pos );
        }
    }
    return trailer;
}

static int xref_chain( uint32_t start )
{
    DICT *trailer;
    uint32_t pos = start, seen[64];
    int32_t other;
    int rounds, at;

    for ( rounds = 0; rounds < 64; rounds++ ) {
        for ( at = 0; at < rounds; at++ ) {
            if ( seen[at] == pos ) {
                return doc.trailer != NULL;     /* a loop: what there is, is all */
            }
        }
        seen[rounds] = pos;
        trailer = xref_section( pos );
        if ( trailer == NULL ) {
            return 0;
        }
        if ( doc.trailer == NULL ) {
            doc.trailer = trailer;
        }
        other = dict_int( trailer, "XRefStm", -1 );
        if ( other > 0 ) {
            xref_section( (uint32_t)other );
        }
        other = dict_int( trailer, "Prev", -1 );
        if ( other <= 0 ) {
            break;
        }
        pos = (uint32_t)other;
    }
    return doc.trailer != NULL;
}

/* ------------------------------------------------------------------ */
/* rebuilding it                                                       */
/* ------------------------------------------------------------------ */

#define RB_TRAILERS 8

static DICT *dict_one( const char *key, uint32_t refnum )
{
    DICT *dict = (DICT *)pool_alloc( doc.pool, sizeof( DICT ) + sizeof( DENT ) );

    dict->count = 1;
    dict->ents = (DENT *)(dict + 1);
    dict->ents[0].key = key;
    dict->ents[0].val.type = T_REF;
    dict->ents[0].val.u.ival = (int32_t)refnum;
    return dict;
}

/* The objects that say nothing of themselves in the file's text: the
   ones inside object streams, and a trailer that is a stream. */
static void rebuild_streams( void )
{
    OBJ *obj, *type;
    OSTM *slot;
    IN *in;
    uint32_t num, index, inner, skip, limit = doc.xref_count, catalog = 0;

    for ( num = 1; num < limit; num++ ) {
        if ( doc.xref[num].type != 1 ) {
            continue;
        }
        if ( (num & 127) == 0 ) {
            pdf_page_begin();
        }
        obj = pdf_load( num );
        if ( obj_dict( obj ) == NULL ) {
            continue;
        }
        type = dict_get( obj_dict( obj ), "Type" );
        if ( obj_is_name( type, "Catalog" ) ) {
            catalog = num;
        } else if ( obj_is_name( type, "XRef" ) && dict_raw( obj_dict( obj ), "Root" ) ) {
            obj = (OBJ *)pool_alloc( doc.pool, sizeof( OBJ ) );
            read_object_at( doc.xref[num].pos, doc.pool, obj, NULL );
            if ( obj_dict( obj ) ) {
                doc.trailer = obj_dict( obj );
            }
        } else if ( obj_is_name( type, "ObjStm" ) && obj->type == T_STREAM ) {
            slot = ostm_find( num );
            if ( slot == NULL ) {
                continue;
            }
            in = in_mem( slot->data, slot->first );
            for ( index = 0; index < slot->count; index++ ) {
                if ( !read_uint( in, &inner ) || !read_uint( in, &skip ) ) {
                    break;
                }
                if ( inner < doc.size && inner < XREF_MAX ) {
                    xref_set( inner, 2, num, index );
                }
            }
            in_close( in );
        }
    }
    if ( doc.trailer == NULL && catalog ) {
        doc.trailer = dict_one( "Root", catalog );
    }
}

static int xref_rebuild( void )
{
    IN *in = in_file( 0, 0xFFFFFFFFUL );
    LEX lex;
    OBJ obj;
    char word[8];
    uint32_t pos, tok_start = 0, tok_val = 0, tok_len = 0;
    uint32_t num1 = 0, num2 = 0, pos1 = 0, pos2 = 0;
    uint32_t trailers[RB_TRAILERS];
    int ch, tok_int = 0, have = 0, tcount = 0, at;

    if ( doc.xref ) {
        mem_set( doc.xref, 0, doc.xref_count * sizeof( XENT ) );
    }
    doc.trailer = NULL;
    doc.repaired = 1;
    for ( pos = 0; ; pos++ ) {
        ch = IN_GETC( in );
        if ( ch < 0 || IS_WS( ch ) || is_delim( ch ) ) {
            if ( tok_len ) {
                word[tok_len < 7 ? tok_len : 7] = 0;
                if ( tok_int ) {
                    num1 = num2;
                    pos1 = pos2;
                    num2 = tok_val;
                    pos2 = tok_start;
                    if ( have < 2 ) {
                        have++;
                    }
                } else {
                    if ( have == 2 && tok_len == 3 && str_cmp( word, "obj" ) == 0 &&
                         pos1 >= doc.base && num1 < doc.size && num1 < XREF_MAX ) {
                        /* the last one in the file is the one in force */
                        xref_need( num1 + 1 );
                        doc.xref[num1].type = 1;
                        doc.xref[num1].pos = pos1 - doc.base;
                        doc.xref[num1].gen = (uint16_t)num2;
                    } else if ( tok_len == 7 && str_cmp( word, "trailer" ) == 0 ) {
                        if ( tcount == RB_TRAILERS ) {
                            mem_cpy( trailers, trailers + 1, sizeof( trailers ) - sizeof( uint32_t ) );
                            tcount--;
                        }
                        trailers[tcount++] = pos;
                    }
                    have = 0;
                }
                tok_len = 0;
            }
            if ( ch < 0 ) {
                break;
            }
            if ( !IS_WS( ch ) ) {
                have = 0;                       /* "1 0 <<" is not an object */
            }
            continue;
        }
        if ( tok_len == 0 ) {
            tok_start = pos;
            tok_int = 1;
            tok_val = 0;
        }
        if ( ch >= '0' && ch <= '9' && tok_int && tok_len < 10 ) {
            tok_val = tok_val * 10 + (uint32_t)(ch - '0');
        } else {
            tok_int = 0;
        }
        if ( tok_len < 7 ) {
            word[tok_len] = (char)ch;
        }
        tok_len++;
    }
    in_close( in );

    for ( at = tcount - 1; at >= 0 && doc.trailer == NULL; at-- ) {
        in = in_file( trailers[at], 0xFFFFFFFFUL );
        lex_init( &lex, in, doc.pool, 0 );
        if ( lex_obj( &lex, &obj ) == T_DICT && dict_raw( obj.u.dict, "Root" ) ) {
            doc.trailer = obj.u.dict;
        }
        in_close( in );
    }
    if ( doc.trailer == NULL || doc.ver_minor >= 5 || doc.ver_major >= 2 ) {
        rebuild_streams();
    }
    return doc.trailer != NULL;
}

/* ------------------------------------------------------------------ */
/* opening                                                             */
/* ------------------------------------------------------------------ */

static uint32_t find_startxref( void )
{
    uint8_t tail[2048];
    uint32_t len = doc.size < sizeof( tail ) ? doc.size : sizeof( tail ), at, val = 0;
    IN *in = in_file( doc.size - len, len );
    int found = -1;

    len = in_read( in, tail, len );
    in_close( in );
    for ( at = 0; at + 9 <= len; at++ ) {
        if ( tail[at] == 's' && mem_cmp( tail + at, "startxref", 9 ) == 0 ) {
            found = (int)at;
        }
    }
    if ( found < 0 ) {
        return 0;
    }
    for ( at = (uint32_t)found + 9; at < len && IS_WS( tail[at] ); at++ ) {
    }
    for ( ; at < len && tail[at] >= '0' && tail[at] <= '9'; at++ ) {
        val = val * 10 + (uint32_t)(tail[at] - '0');
    }
    return val;
}

static int root_good( void )
{
    OBJ *ref = dict_raw( doc.trailer, "Root" );
    OBJ *root;

    if ( ref == NULL ) {
        return 0;
    }
    if ( ref->type == T_REF ) {
        root = load_into( (uint32_t)ref->u.ival, doc.pool );
    } else {
        root = ref;
    }
    doc.root = obj_dict( root );
    return doc.root != NULL && dict_dict( doc.root, "Pages" ) != NULL;
}

static int open_inner( void )
{
    uint8_t head[1024];
    IN *in = in_file( 0, sizeof( head ) );
    uint32_t len = in_read( in, head, sizeof( head ) ), start;
    OBJ *enc, *ident;
    const char *version;
    int at, why;

    in_close( in );
    at = mem_find( head, len, "%PDF-" );
    if ( at < 0 ) {
        return PE_NOTPDF;
    }
    doc.base = (uint32_t)at;
    doc.ver_major = 1;
    if ( (uint32_t)at + 7 < len && head[at + 5] >= '0' && head[at + 5] <= '9' ) {
        doc.ver_major = head[at + 5] - '0';
        if ( head[at + 6] == '.' && head[at + 7] >= '0' && head[at + 7] <= '9' ) {
            doc.ver_minor = head[at + 7] - '0';
        }
    }

    start = find_startxref();
    if ( start == 0 || !xref_chain( start ) || !root_good() ) {
        if ( !xref_rebuild() || !root_good() ) {
            return PE_DAMAGED;
        }
    }
    /* a later save may say a later version than the first line does */
    version = dict_name( doc.root, "Version" );
    if ( version[0] >= '1' && version[0] <= '9' && version[1] == '.' &&
         version[2] >= '0' && version[2] <= '9' ) {
        if ( version[0] - '0' > doc.ver_major ||
             (version[0] - '0' == doc.ver_major && version[2] - '0' > doc.ver_minor) ) {
            doc.ver_major = version[0] - '0';
            doc.ver_minor = version[2] - '0';
        }
    }

    ident = dict_get( doc.trailer, "ID" );
    if ( ident->type == T_ARR ) {
        ident = arr_get( ident->u.arr, 0 );
        if ( ident->type == T_STR ) {
            doc.file_id_len = ident->u.str.len < sizeof( doc.file_id ) ? ident->u.str.len
                                                                       : sizeof( doc.file_id );
            mem_cpy( doc.file_id, ident->u.str.ptr, doc.file_id_len );
        }
    }
    enc = dict_raw( doc.trailer, "Encrypt" );
    if ( enc && enc->type != T_NULL ) {
        if ( enc->type == T_REF ) {
            enc = load_into( (uint32_t)enc->u.ival, doc.pool );
        }
        if ( obj_dict( enc ) == NULL ) {
            return PE_CRYPT;
        }
        doc.crypt_dict = obj_dict( enc );
        why = crypt_setup( doc.crypt_dict );
        if ( why != PE_NONE ) {
            return why;
        }
    }
    doc.page_count = pdf_count_pages();
    return PE_NONE;
}

/* The part of opening that is the same wherever the bytes are.  A file
   that cannot be opened is closed again here, so that nothing is left
   open behind an error - but one that only wants its password stays as
   it is, for pdf_password and pdf_open_finish. */
static int open_common( void )
{
    static JMPBUF trap;
    int why;

    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        doc.pool = pool_new();
        pdf_page_begin();
        why = open_inner();
    }
    pdf_page_end();
    pdf_trap = NULL;
    if ( why != PE_NONE && why != PE_PASSWORD ) {
        pdf_close();
    }
    return why;
}

/* One document is open at a time: opening another closes the one that
   was. */
int pdf_open( const char *path )
{
    pdf_close();
    if ( sys_open_read( path, &doc.file ) ) {
        doc.file = 0;
        return PE_IO;
    }
    if ( sys_file_size( doc.file, &doc.size ) ) {
        sys_close( doc.file );
        doc.file = 0;
        return PE_IO;
    }
    return open_common();
}

/* A file that is in memory already, all of it.  The bytes are read
   where they lie, so they must stay there until pdf_close. */
int pdf_open_mem( const uint8_t *data, uint32_t len )
{
    pdf_close();
    if ( data == NULL || len == 0 ) {
        return PE_NOTPDF;
    }
    doc.mem = data;
    doc.size = len;
    return open_common();
}

/* after PE_PASSWORD and a password that crypt_password took: the part
   of opening that could not be done without it */
int pdf_open_finish( void )
{
    static JMPBUF trap;
    int why;

    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        pdf_page_begin();
        doc.page_count = pdf_count_pages();
    }
    pdf_page_end();
    pdf_trap = NULL;
    if ( why != PE_NONE ) {
        pdf_close();
    }
    return why;
}

/* Safe on a document that is half open, or not open at all.  The fonts
   go with the file: they are known by the numbers of its objects, and
   the next file's objects have the same numbers. */
void pdf_close( void )
{
    int index;

    pdf_page_end();
    glyph_page_begin();                 /* a glyph a page given up left in hand */
    font_reclaim( 1 );
    for ( index = 0; index < OSTM_SLOTS; index++ ) {
        ostm_drop( &ostm[index] );
    }
    if ( doc.xref ) {
        xfree( doc.xref );
    }
    if ( doc.pool ) {
        pool_free( doc.pool );
    }
    if ( doc.file ) {
        sys_close( doc.file );
    }
    in_file_reset();
    mem_set( &doc, 0, sizeof( doc ) );
}

/*
 * The memory that is kept from one document to the next because it is
 * wanted again at once - the page's table of objects, the lexer's and
 * the scan converter's work space, the interpreter's states - given
 * back as well.  For the program's end, and for a program that has
 * closed its last PDF and wants the room for something else.  Whatever
 * is wanted again is made again.
 */
void pdf_done( void )
{
    pdf_close();
    if ( oc_num ) {
        xfree( oc_num );
        oc_num = NULL;
        oc_obj = NULL;
    }
    lex_done();
    raster_done();
    path_dash_done();
    glyph_done();
    ttf_done();
    ccitt_done();
    interp_done();
}
