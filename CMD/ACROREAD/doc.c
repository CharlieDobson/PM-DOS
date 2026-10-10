/*
 * DOC.C - what is asked of an object once it has been read: a
 * dictionary's entries, a stream's data through its filters, and the
 * page tree.
 */
#include "pdf.h"

OBJ      obj_null;
PDFDOC   doc;
JMPBUF  *pdf_trap;

void pdf_fail( int why )
{
    if ( pdf_trap ) {
        rt_longjmp( pdf_trap, why );
    }
    sys_exit( 3 );
}

/* ------------------------------------------------------------------ */
/* objects                                                             */
/* ------------------------------------------------------------------ */

OBJ *obj_resolve( OBJ *obj )
{
    int hops;

    for ( hops = 0; obj->type == T_REF; hops++ ) {
        if ( hops == 8 ) {
            return &obj_null;
        }
        obj = pdf_load( (u32)obj->u.ival );
    }
    return obj;
}

OBJ *dict_raw( DICT *dict, const char *key )
{
    u32 index;

    if ( dict == NULL ) {
        return NULL;
    }
    for ( index = 0; index < dict->count; index++ ) {
        if ( str_cmp( dict->ents[index].key, key ) == 0 ) {
            return &dict->ents[index].val;
        }
    }
    return NULL;
}

OBJ *dict_get( DICT *dict, const char *key )
{
    OBJ *val = dict_raw( dict, key );

    return val ? obj_resolve( val ) : &obj_null;
}

/* an inline image may say /W for /Width */
OBJ *dict_get2( DICT *dict, const char *key, const char *abbrev )
{
    OBJ *val = dict_raw( dict, key );

    if ( val == NULL ) {
        val = dict_raw( dict, abbrev );
    }
    return val ? obj_resolve( val ) : &obj_null;
}

int obj_is_num( OBJ *obj )
{
    return obj->type == T_INT || obj->type == T_REAL;
}

s32 obj_int( OBJ *obj )
{
    if ( obj->type == T_INT || obj->type == T_BOOL ) {
        return obj->u.ival;
    }
    if ( obj->type == T_REAL ) {
        return real_to_int( obj->u.real );
    }
    return 0;
}

REAL obj_real( OBJ *obj )
{
    if ( obj->type == T_REAL ) {
        return obj->u.real;
    }
    return real_int( obj->type == T_INT ? obj->u.ival : 0 );
}

fx obj_fx( OBJ *obj )
{
    if ( obj->type == T_INT ) {
        if ( obj->u.ival > 32767 ) {
            return FX_MAX;
        }
        if ( obj->u.ival < -32767 ) {
            return -FX_MAX;
        }
        return I2FX( obj->u.ival );
    }
    if ( obj->type == T_REAL ) {
        return real_to_fx( obj->u.real );
    }
    return 0;
}

const char *obj_name( OBJ *obj )
{
    return obj->type == T_NAME ? (const char *)obj->u.str.ptr : "";
}

int obj_is_name( OBJ *obj, const char *name )
{
    return obj->type == T_NAME && str_cmp( (const char *)obj->u.str.ptr, name ) == 0;
}

DICT *obj_dict( OBJ *obj )
{
    return obj->type == T_DICT || obj->type == T_STREAM ? obj->u.dict : NULL;
}

s32 dict_int( DICT *dict, const char *key, s32 def )
{
    OBJ *val = dict_get( dict, key );

    return obj_is_num( val ) || val->type == T_BOOL ? obj_int( val ) : def;
}

fx dict_fx( DICT *dict, const char *key, fx def )
{
    OBJ *val = dict_get( dict, key );

    return obj_is_num( val ) ? obj_fx( val ) : def;
}

const char *dict_name( DICT *dict, const char *key )
{
    return obj_name( dict_get( dict, key ) );
}

DICT *dict_dict( DICT *dict, const char *key )
{
    return obj_dict( dict_get( dict, key ) );
}

ARR *dict_arr( DICT *dict, const char *key )
{
    OBJ *val = dict_get( dict, key );

    return val->type == T_ARR ? val->u.arr : NULL;
}

OBJ *arr_get( ARR *arr, u32 index )
{
    if ( arr == NULL || index >= arr->count ) {
        return &obj_null;
    }
    return obj_resolve( &arr->items[index] );
}

/* ------------------------------------------------------------------ */
/* streams                                                             */
/* ------------------------------------------------------------------ */

u32 pdf_stream_length( DICT *dict )
{
    s32 len = dict_int( dict, "Length", -1 );

    return len < 0 ? 0xFFFFFFFFUL : (u32)len;
}

int pdf_filter_count( OBJ *stm )
{
    OBJ *flt = dict_get2( obj_dict( stm ), "Filter", "F" );

    if ( flt->type == T_NAME ) {
        return 1;
    }
    return flt->type == T_ARR ? (int)flt->u.arr->count : 0;
}

const char *pdf_filter_name( OBJ *stm, int index )
{
    OBJ *flt = dict_get2( obj_dict( stm ), "Filter", "F" );

    if ( flt->type == T_NAME ) {
        return index == 0 ? (const char *)flt->u.str.ptr : "";
    }
    return flt->type == T_ARR ? obj_name( arr_get( flt->u.arr, (u32)index ) ) : "";
}

DICT *pdf_filter_parms( OBJ *stm, int index )
{
    OBJ *parms = dict_get2( obj_dict( stm ), "DecodeParms", "DP" );

    if ( parms->type == T_ARR ) {
        parms = arr_get( parms->u.arr, (u32)index );
    } else if ( index != 0 ) {
        return NULL;
    }
    return obj_dict( parms );
}

const char *pdf_last_filter( OBJ *stm )
{
    int count = pdf_filter_count( stm );

    return count ? pdf_filter_name( stm, count - 1 ) : "";
}

/* The data as the file holds it: from where "stream" left off for
   /Length bytes.  A length that is missing or cannot be right is made
   good by looking for "endstream". */
static u32 stream_span( DICT *dict )
{
    static const char mark[] = "endstream";
    u32 len = pdf_stream_length( dict ), pos, found = 0;
    IN *in;
    int ch;

    if ( len != 0xFFFFFFFFUL && dict->stm_pos + len <= doc.size ) {
        return len;
    }
    in = in_file( dict->stm_pos, 0xFFFFFFFFUL );
    for ( pos = 0; (ch = IN_GETC( in )) >= 0; pos++ ) {
        if ( ch == mark[found] ) {
            if ( mark[++found] == 0 ) {
                pos -= 8;               /* where the word began */
                break;
            }
        } else {
            found = ch == 'e';
        }
    }
    in_close( in );
    return pos;
}

/* The filters "holder" names, one behind another, on top of "in".
   With stop_at_image, a last filter that makes pixels (DCT, CCITT,
   JBIG2, JPX) is left for the image to apply and its index comes back
   in *last_filter; -1 there means every filter is in place.  NULL if
   a filter is one this cannot undo - "in" has been closed then. */
IN *pdf_filter_chain( IN *in, OBJ *holder, int stop_at_image, int *last_filter )
{
    DICT *parms;
    IN *next;
    const char *name;
    int count = pdf_filter_count( holder ), index;

    if ( last_filter ) {
        *last_filter = -1;
    }
    for ( index = 0; in && index < count; index++ ) {
        name = pdf_filter_name( holder, index );
        parms = pdf_filter_parms( holder, index );
        next = NULL;
        if ( str_cmp( name, "FlateDecode" ) == 0 || str_cmp( name, "Fl" ) == 0 ) {
            next = flt_inflate( in );
        } else if ( str_cmp( name, "LZWDecode" ) == 0 || str_cmp( name, "LZW" ) == 0 ) {
            next = flt_lzw( in, (int)dict_int( parms, "EarlyChange", 1 ) );
        } else if ( str_cmp( name, "ASCIIHexDecode" ) == 0 || str_cmp( name, "AHx" ) == 0 ) {
            next = flt_ahex( in );
        } else if ( str_cmp( name, "ASCII85Decode" ) == 0 || str_cmp( name, "A85" ) == 0 ) {
            next = flt_a85( in );
        } else if ( str_cmp( name, "RunLengthDecode" ) == 0 || str_cmp( name, "RL" ) == 0 ) {
            next = flt_rle( in );
        } else if ( str_cmp( name, "Crypt" ) == 0 ) {
            continue;
        } else if ( stop_at_image && index == count - 1 ) {
            if ( last_filter ) {
                *last_filter = index;
            }
            return in;
        } else if ( str_cmp( name, "CCITTFaxDecode" ) == 0 || str_cmp( name, "CCF" ) == 0 ) {
            next = flt_ccitt( in, parms );
        } else if ( str_cmp( name, "DCTDecode" ) == 0 || str_cmp( name, "DCT" ) == 0 ) {
            next = flt_dct( in, parms );
        }
        if ( next == NULL ) {
            in_close( in );
            return NULL;
        }
        in = next;
        if ( parms && dict_int( parms, "Predictor", 1 ) > 1 && (name[0] == 'F' || name[0] == 'L') ) {
            in = flt_predict( in, (int)dict_int( parms, "Predictor", 1 ),
                              (int)dict_int( parms, "Colors", 1 ),
                              (int)dict_int( parms, "BitsPerComponent", 8 ),
                              (int)dict_int( parms, "Columns", 1 ) );
        }
    }
    return in;
}

static IN *stream_open( OBJ *stm, int stop_at_image, int *last_filter )
{
    DICT *dict = obj_dict( stm ), *parms;
    IN *in;
    int count, index, crypt_named = 0;

    if ( stm->type != T_STREAM ) {
        return NULL;
    }
    count = pdf_filter_count( stm );
    for ( index = 0; index < count; index++ ) {
        if ( str_cmp( pdf_filter_name( stm, index ), "Crypt" ) == 0 ) {
            crypt_named = 1;            /* its own, which is Identity or the default */
            parms = pdf_filter_parms( stm, index );
            if ( parms && str_cmp( dict_name( parms, "Name" ), "Identity" ) != 0 ) {
                crypt_named = 0;
            }
        }
    }
    if ( dict->stm_mem ) {
        in = in_mem( dict->stm_mem, dict->stm_len );
    } else {
        in = in_file( dict->stm_pos, stream_span( dict ) );
        if ( doc.encrypted && !dict->crypt_off && !crypt_named &&
             doc.stm_crypt != CRYPT_NONE && !obj_is_name( dict_get( dict, "Type" ), "XRef" ) ) {
            in = crypt_stream( in, dict->num, dict->gen );
        }
    }
    return pdf_filter_chain( in, stm, stop_at_image, last_filter );
}

IN *pdf_stream( OBJ *stm )
{
    return stream_open( stm, 0, NULL );
}

/* For an image: every filter but the last when the last is one that
   makes pixels (DCT, CCITT, JBIG2, JPX), whose index comes back in
   *last_filter; -1 when every filter has been applied. */
IN *pdf_stream_raw( OBJ *stm, int *last_filter )
{
    return stream_open( stm, 1, last_filter );
}

u8 *pdf_stream_all( OBJ *stm, u32 *len, u32 max )
{
    return pdf_stream_pool( stm, doc.page_pool, len, max );
}

/* all of a stream, decoded, in a block of "pool": NULL if it cannot be
   read.  No more than "max" bytes of it. */
u8 *pdf_stream_pool( OBJ *stm, POOL *pool, u32 *len, u32 max )
{
    IN *in = pdf_stream( stm );
    u8 *buf, *grown;
    u32 cap = 4096, have = 0, got;

    *len = 0;
    if ( in == NULL ) {
        return NULL;
    }
    buf = (u8 *)pool_big( pool, cap );
    for ( ;; ) {
        got = in_read( in, buf + have, cap - have );
        have += got;
        if ( have < cap || have >= max ) {
            break;
        }
        cap *= 2;
        if ( cap > max ) {
            cap = max;
        }
        grown = (u8 *)pool_big( pool, cap );
        mem_cpy( grown, buf, have );
        pool_unbig( buf );
        buf = grown;
    }
    in_close( in );
    *len = have;
    return buf;
}

/* ------------------------------------------------------------------ */
/* the page tree                                                       */
/* ------------------------------------------------------------------ */

static int tree_count( DICT *node, int depth )
{
    ARR *kids = dict_arr( node, "Kids" );
    DICT *kid;
    u32 index;
    int total = 0;

    if ( kids == NULL ) {
        return 1;
    }
    if ( depth > 40 ) {
        return 0;
    }
    for ( index = 0; index < kids->count; index++ ) {
        kid = obj_dict( arr_get( kids, index ) );
        if ( kid ) {
            total += tree_count( kid, depth + 1 );
        }
    }
    return total;
}

/* what a node says of its pages: /Count if it is believable */
static int node_count( DICT *node )
{
    s32 count;

    if ( dict_arr( node, "Kids" ) == NULL ) {
        return 1;
    }
    count = dict_int( node, "Count", -1 );
    if ( count < 0 || doc.repaired ) {
        count = tree_count( node, 0 );
    }
    return (int)count;
}

int pdf_count_pages( void )
{
    DICT *pages = dict_dict( doc.root, "Pages" );

    return pages ? node_count( pages ) : 0;
}

static void box_from( OBJ *obj, REAL *box, int *have )
{
    REAL swap;
    int index;

    if ( obj->type != T_ARR || obj->u.arr->count < 4 ) {
        return;
    }
    for ( index = 0; index < 4; index++ ) {
        box[index] = obj_real( arr_get( obj->u.arr, (u32)index ) );
    }
    if ( real_to_fx( box[0] ) > real_to_fx( box[2] ) ) {
        swap = box[0];
        box[0] = box[2];
        box[2] = swap;
    }
    if ( real_to_fx( box[1] ) > real_to_fx( box[3] ) ) {
        swap = box[1];
        box[1] = box[3];
        box[3] = swap;
    }
    *have = 1;
}

int pdf_page( int index, PAGE *page )
{
    DICT *node = dict_dict( doc.root, "Pages" ), *kid;
    ARR *kids;
    OBJ *val;
    u32 at;
    int depth, count, have_crop = 0, have_media = 0, found;
    REAL media[4];

    mem_set( page, 0, sizeof( *page ) );
    if ( node == NULL || index < 0 ) {
        return 0;
    }
    for ( depth = 0; depth < 64; depth++ ) {
        /* what this node gives the pages under it */
        val = dict_get( node, "Resources" );
        if ( obj_dict( val ) ) {
            page->res = obj_dict( val );
        }
        box_from( dict_get( node, "MediaBox" ), media, &have_media );
        box_from( dict_get( node, "CropBox" ), page->box, &have_crop );
        val = dict_get( node, "Rotate" );
        if ( obj_is_num( val ) ) {
            page->rotate = (int)obj_int( val );
        }
        kids = dict_arr( node, "Kids" );
        if ( kids == NULL ) {
            break;                      /* a page */
        }
        found = 0;
        for ( at = 0; at < kids->count; at++ ) {
            kid = obj_dict( arr_get( kids, at ) );
            if ( kid == NULL ) {
                continue;
            }
            count = node_count( kid );
            if ( index < count ) {
                node = kid;
                found = 1;
                break;
            }
            index -= count;
        }
        if ( !found ) {
            return 0;
        }
    }
    if ( depth == 64 || index != 0 ) {
        return 0;
    }
    page->dict = node;
    if ( !have_crop ) {
        if ( have_media ) {
            mem_cpy( page->box, media, sizeof( media ) );
        } else {
            page->box[0] = page->box[1] = real_int( 0 );
            page->box[2] = real_int( 612 );
            page->box[3] = real_int( 792 );
        }
    } else if ( have_media ) {
        /* the crop box is no larger than the media box */
        if ( real_to_fx( page->box[0] ) < real_to_fx( media[0] ) ) {
            page->box[0] = media[0];
        }
        if ( real_to_fx( page->box[1] ) < real_to_fx( media[1] ) ) {
            page->box[1] = media[1];
        }
        if ( real_to_fx( page->box[2] ) > real_to_fx( media[2] ) ) {
            page->box[2] = media[2];
        }
        if ( real_to_fx( page->box[3] ) > real_to_fx( media[3] ) ) {
            page->box[3] = media[3];
        }
    }
    if ( real_to_fx( page->box[2] ) - real_to_fx( page->box[0] ) < FX_ONE ||
         real_to_fx( page->box[3] ) - real_to_fx( page->box[1] ) < FX_ONE ) {
        page->box[0] = page->box[1] = real_int( 0 );
        page->box[2] = real_int( 612 );
        page->box[3] = real_int( 792 );
    }
    page->rotate %= 360;
    if ( page->rotate < 0 ) {
        page->rotate += 360;
    }
    page->rotate = (page->rotate + 45) / 90 * 90 % 360;
    return 1;
}
