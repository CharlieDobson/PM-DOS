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
static u32  *oc_num;                /* from the heap: static, they would be 16K */
static OBJ **oc_obj;                /* of zeroes in the program's file */
static int  load_depth;

/* a decoded object stream, kept from page to page: most pages take
   their objects from one or two */
typedef struct {
    u32  num;
    u32 *offs;                      /* where each object starts, from "first" */
    u8  *data;
    u32  len, count, first;
    u32  stamp;
    int  in_pool;                   /* the page's memory, not the heap's */
} OSTM;

#define OSTM_SLOTS  3
static OSTM ostm[OSTM_SLOTS];
static u32  ostm_clock;

int pdf_count_pages( void );
int cache_reclaim( void );          /* GLYPH.C: the caches of fonts and glyphs */

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
    if ( oc_num == NULL ) {
        oc_num = (u32 *)xalloc( OC_SIZE * (sizeof( u32 ) + sizeof( OBJ * )) );
        oc_obj = (OBJ **)(oc_num + OC_SIZE);
    }
    mem_set( oc_num, 0, OC_SIZE * sizeof( u32 ) );
    load_depth = 0;
}

void pdf_page_begin( void )
{
    pdf_page_end();
    doc.page_pool = pool_new();
}

int mem_reclaim( void )
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

static int read_uint( IN *in, u32 *val )
{
    int ch = lex_skip_ws( in );
    u32 num = 0;

    if ( ch < '0' || ch > '9' ) {
        return 0;
    }
    while ( ch >= '0' && ch <= '9' ) {
        in->cur++;
        num = num * 10 + (u32)(ch - '0');
        ch = in_peek( in );
    }
    *val = num;
    return 1;
}

/* "n g obj", the object, and where its data starts if it is a stream.
   "pos" is from the file's "%PDF".  0 if there is no object there. */
static int read_object_at( u32 pos, POOL *pool, OBJ *out, u32 *num_out )
{
    IN *in = in_file( doc.base + pos, 0xFFFFFFFFUL );
    LEX lex;
    OBJ num, gen, word;
    u32 at;
    int ch, good = 0;

    out->type = T_NULL;
    lex_init( &lex, in, pool, 0 );
    if ( lex_obj( &lex, &num ) == T_INT && lex_obj( &lex, &gen ) == T_INT &&
         lex_obj( &lex, &word ) == T_OP && str_cmp( lex.word, "obj" ) == 0 ) {
        good = 1;
        lex.num = (u32)num.u.ival;
        lex.gen = (u16)gen.u.ival;
        lex.crypt = (u8)(doc.encrypted && doc.str_crypt != CRYPT_NONE);
        lex_obj( &lex, out );
        if ( out->type == T_OP || out->type == T_EOF ) {
            out->type = T_NULL;             /* "obj endobj" */
        }
        if ( num_out ) {
            *num_out = (u32)num.u.ival;
        }
        if ( out->type == T_DICT ) {
            out->u.dict->num = (u32)num.u.ival;
            out->u.dict->gen = (u16)gen.u.ival;
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

static OSTM *ostm_find( u32 num )
{
    OSTM *slot;
    OBJ *stm;
    DICT *dict;
    IN *in;
    u8 *data;
    u32 *offs, *block;
    u32 len, count, first, index, skip;
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
    count = (u32)dict_int( dict, "N", 0 );
    first = (u32)dict_int( dict, "First", 0 );
    data = pdf_stream_all( stm, &len, 32UL * 1024 * 1024 );
    if ( data == NULL || first > len || count > first ) {
        return NULL;
    }
    offs = (u32 *)pool_big( doc.page_pool, (count + 1) * sizeof( u32 ) );
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
    block = (u32 *)try_alloc( count * sizeof( u32 ) + len );
    if ( block ) {
        mem_cpy( block, offs, count * sizeof( u32 ) );
        mem_cpy( block + count, data, len );
        pool_unbig( offs );
        pool_unbig( data );
        slot->offs = block;
        slot->data = (u8 *)(block + count);
        slot->in_pool = 0;
    } else {
        slot->offs = offs;
        slot->data = data;
        slot->in_pool = 1;
    }
    return slot;
}

static void read_object_in( u32 stmnum, u32 index, u32 num, POOL *pool, OBJ *out )
{
    OSTM *slot = ostm_find( stmnum );
    IN *in;
    LEX lex;
    u32 start;

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

static OBJ *load_into( u32 num, POOL *pool )
{
    OBJ *obj = (OBJ *)pool_alloc( pool, sizeof( OBJ ) );
    XENT *ent;
    u32 found;

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

OBJ *pdf_load( u32 num )
{
    u32 slot = num & (OC_SIZE - 1);
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

static void xref_need( u32 count )
{
    XENT *grown;
    u32 cap;

    if ( count <= doc.xref_count ) {
        return;
    }
    if ( count > doc.size ) {
        pdf_fail( PE_DAMAGED );             /* more objects than bytes */
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

static void xref_set( u32 num, int type, u32 pos, u32 gen )
{
    XENT *ent;

    xref_need( num + 1 );
    ent = &doc.xref[num];
    if ( ent->type == 0 ) {
        ent->type = (u8)type;
        ent->pos = pos;
        ent->gen = (u16)gen;
    }
}

/* after "xref": the subsections, then the trailer */
static DICT *xref_table( IN *in )
{
    LEX lex;
    OBJ obj;
    u32 first, count, index, pos, gen;
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

static u32 be_field( const u8 *data, int width )
{
    u32 val = 0;

    while ( width-- > 0 ) {
        val = (val << 8) | *data++;
    }
    return val;
}

/* the object at "pos" is a cross-reference stream: its dictionary is
   the trailer */
static DICT *xref_stream( u32 pos )
{
    OBJ *stm = (OBJ *)pool_alloc( doc.pool, sizeof( OBJ ) );
    DICT *dict;
    ARR *widths, *index;
    IN *in;
    u8 rec[24];
    u32 first, count, at, pair, type, size;
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
    size = (u32)dict_int( dict, "Size", 0 );
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
            first = (u32)obj_int( arr_get( index, pair ) );
            count = (u32)obj_int( arr_get( index, pair + 1 ) );
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
            if ( in_read( in, rec, (u32)(w0 + w1 + w2) ) != (u32)(w0 + w1 + w2) ) {
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
static DICT *xref_section( u32 pos )
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

static int xref_chain( u32 start )
{
    DICT *trailer;
    u32 pos = start, seen[64];
    s32 other;
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
            xref_section( (u32)other );
        }
        other = dict_int( trailer, "Prev", -1 );
        if ( other <= 0 ) {
            break;
        }
        pos = (u32)other;
    }
    return doc.trailer != NULL;
}

/* ------------------------------------------------------------------ */
/* rebuilding it                                                       */
/* ------------------------------------------------------------------ */

#define RB_TRAILERS 8

static DICT *dict_one( const char *key, u32 refnum )
{
    DICT *dict = (DICT *)pool_alloc( doc.pool, sizeof( DICT ) + sizeof( DENT ) );

    dict->count = 1;
    dict->ents = (DENT *)(dict + 1);
    dict->ents[0].key = key;
    dict->ents[0].val.type = T_REF;
    dict->ents[0].val.u.ival = (s32)refnum;
    return dict;
}

/* The objects that say nothing of themselves in the file's text: the
   ones inside object streams, and a trailer that is a stream. */
static void rebuild_streams( void )
{
    OBJ *obj, *type;
    OSTM *slot;
    IN *in;
    u32 num, index, inner, skip, limit = doc.xref_count, catalog = 0;

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
                if ( inner < doc.size ) {
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
    u32 pos, tok_start = 0, tok_val = 0, tok_len = 0;
    u32 num1 = 0, num2 = 0, pos1 = 0, pos2 = 0;
    u32 trailers[RB_TRAILERS];
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
                         pos1 >= doc.base && num1 < doc.size ) {
                        /* the last one in the file is the one in force */
                        xref_need( num1 + 1 );
                        doc.xref[num1].type = 1;
                        doc.xref[num1].pos = pos1 - doc.base;
                        doc.xref[num1].gen = (u16)num2;
                    } else if ( tok_len == 7 && str_cmp( word, "trailer" ) == 0 ) {
                        if ( tcount == RB_TRAILERS ) {
                            mem_cpy( trailers, trailers + 1, sizeof( trailers ) - sizeof( u32 ) );
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
            tok_val = tok_val * 10 + (u32)(ch - '0');
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

static u32 find_startxref( void )
{
    u8 tail[2048];
    u32 len = doc.size < sizeof( tail ) ? doc.size : sizeof( tail ), at, val = 0;
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
    for ( at = (u32)found + 9; at < len && IS_WS( tail[at] ); at++ ) {
    }
    for ( ; at < len && tail[at] >= '0' && tail[at] <= '9'; at++ ) {
        val = val * 10 + (u32)(tail[at] - '0');
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
        root = load_into( (u32)ref->u.ival, doc.pool );
    } else {
        root = ref;
    }
    doc.root = obj_dict( root );
    return doc.root != NULL && dict_dict( doc.root, "Pages" ) != NULL;
}

static int open_inner( void )
{
    u8 head[1024];
    IN *in = in_file( 0, sizeof( head ) );
    u32 len = in_read( in, head, sizeof( head ) ), start;
    OBJ *enc, *ident;
    const char *version;
    int at, why;

    in_close( in );
    at = mem_find( head, len, "%PDF-" );
    if ( at < 0 ) {
        return PE_NOTPDF;
    }
    doc.base = (u32)at;
    doc.ver_major = 1;
    if ( (u32)at + 7 < len && head[at + 5] >= '0' && head[at + 5] <= '9' ) {
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
            enc = load_into( (u32)enc->u.ival, doc.pool );
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

int pdf_open( const char *path )
{
    static JMPBUF trap;
    int why;

    mem_set( &doc, 0, sizeof( doc ) );
    if ( sys_open_read( path, &doc.file ) ) {
        return PE_IO;
    }
    if ( sys_file_size( doc.file, &doc.size ) ) {
        sys_close( doc.file );
        return PE_IO;
    }
    doc.pool = pool_new();
    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        pdf_page_begin();
        why = open_inner();
    }
    pdf_page_end();
    pdf_trap = NULL;
    return why;
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
    return why;
}

void pdf_close( void )
{
    int index;

    pdf_page_end();
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
    mem_set( &doc, 0, sizeof( doc ) );
}
