/*
 * FONT.C - a font as a PDF file describes one: what each code in a
 * string means, how wide it is, what character it is for the text
 * view, and which glyph of which font program draws it.
 *
 * THE KINDS.  A simple font (Type1, MMType1, TrueType, Type3) has
 * codes of one byte and an encoding that names the glyph for each; a
 * composite one (Type0) has codes of one to four bytes, a CMap that
 * turns them into character IDs, and a descendant font that turns
 * those into glyphs.  The program that draws the glyphs is TrueType,
 * CFF or Type 1 if it is in the file, and SFONT.C's if it is not.
 *
 * A FONT IS KEPT FROM PAGE TO PAGE, by the number of its dictionary's
 * object, because reading a font program is most of the cost of the
 * first page and none of the cost of the second.  Each has a pool of
 * its own; the ones the page in hand has not used are what is given
 * back when memory runs short.  Type 3 fonts, whose glyphs are content
 * streams in the file, and fonts written out inside a page's own
 * dictionary, last as long as the page.
 */
#include "gfx.h"

static FONT *fonts;                 /* kept: each has its own pool */
static FONT *page_fonts;            /* the page's own */
static u32   stamp = 1;
static POOL *loading;               /* the pool of a font being read */

static POOL *words;                 /* the strings of a character map being read */

/* ------------------------------------------------------------------ */
/* character maps                                                      */
/* ------------------------------------------------------------------ */

static u32 code_of( const OBJ *str )
{
    u32 val = 0, index;

    for ( index = 0; index < str->u.str.len && index < 4; index++ ) {
        val = (val << 8) | str->u.str.ptr[index];
    }
    return val;
}

/* a string of UTF-16 into a range's value and what follows it */
static void unicode_of( const OBJ *str, CMRANGE *range )
{
    const u8 *text = str->u.str.ptr;
    u32 len = str->u.str.len / 2, index;

    range->val = len ? ((u32)text[0] << 8) | text[1] : 0xFFFD;
    if ( str->u.str.len == 1 ) {
        range->val = text[0];
    }
    if ( range->val >= 0xD800 && range->val <= 0xDFFF ) {
        range->val = 0xFFFD;                /* beyond the sixteen bits there is room for */
        return;
    }
    for ( index = 1; index < len && index < 4; index++ ) {
        range->more[index - 1] = (u16)(((u32)text[index * 2] << 8) | text[index * 2 + 1]);
    }
}

static CMRANGE *range_new( CMAP *cmap )
{
    CMRANGE *grown;

    if ( cmap->count == cmap->cap ) {
        grown = (CMRANGE *)pool_big( doc.page_pool, (cmap->cap ? cmap->cap * 2 : 128) * sizeof( CMRANGE ) );
        if ( cmap->ranges ) {
            mem_cpy( grown, cmap->ranges, cmap->count * sizeof( CMRANGE ) );
            pool_unbig( cmap->ranges );
        }
        cmap->ranges = grown;
        cmap->cap = cmap->cap ? cmap->cap * 2 : 128;
    }
    return &cmap->ranges[cmap->count++];
}

CMAP *cmap_parse( OBJ *stm, POOL *pool )
{
    CMAP *cmap = (CMAP *)pool_alloc( pool, sizeof( CMAP ) );
    CMRANGE *range, *final, hold;
    IN *in = pdf_stream( stm );
    LEX lex;
    OBJ lo, hi, dst;
    u32 index, gap, at, back;
    int type, mode = 0;

    if ( in == NULL ) {
        return NULL;
    }
    /* its strings are wanted only while it is read */
    if ( words ) {
        pool_free( words );                 /* one that was given up halfway */
    }
    words = pool_new();
    lex_init( &lex, in, words, 1 );
    for ( ;; ) {
        type = lex_obj( &lex, &lo );
        if ( type == T_EOF ) {
            break;
        }
        if ( type == T_OP ) {
            if ( str_cmp( lex.word, "begincodespacerange" ) == 0 ) {
                mode = 1;
            } else if ( str_cmp( lex.word, "beginbfchar" ) == 0 ) {
                mode = 2;
            } else if ( str_cmp( lex.word, "beginbfrange" ) == 0 ) {
                mode = 3;
            } else if ( str_cmp( lex.word, "begincidchar" ) == 0 ) {
                mode = 4;
            } else if ( str_cmp( lex.word, "begincidrange" ) == 0 ) {
                mode = 5;
            } else {
                mode = 0;
            }
            continue;
        }
        if ( mode == 0 || type != T_STR ) {
            continue;
        }
        if ( mode == 2 || mode == 4 ) {
            hi = lo;
        } else if ( lex_obj( &lex, &hi ) != T_STR ) {
            mode = 0;
            continue;
        }
        if ( mode == 1 ) {
            if ( cmap->nspace < 8 ) {
                cmap->space[cmap->nspace].lo = code_of( &lo );
                cmap->space[cmap->nspace].hi = code_of( &hi );
                cmap->space[cmap->nspace].len = (int)lo.u.str.len;
                cmap->nspace++;
            }
            continue;
        }
        type = lex_obj( &lex, &dst );
        if ( type == T_ARR && mode == 3 ) {
            /* a range whose characters are listed one by one */
            for ( index = 0; index < dst.u.arr->count && code_of( &lo ) + index <= code_of( &hi ); index++ ) {
                if ( dst.u.arr->items[index].type == T_STR ) {
                    range = range_new( cmap );
                    mem_set( range, 0, sizeof( *range ) );
                    range->lo = range->hi = code_of( &lo ) + index;
                    unicode_of( &dst.u.arr->items[index], range );
                }
            }
            continue;
        }
        range = range_new( cmap );
        mem_set( range, 0, sizeof( *range ) );
        range->lo = code_of( &lo );
        range->hi = code_of( &hi );
        if ( range->hi < range->lo ) {
            range->hi = range->lo;
        }
        if ( mode >= 4 ) {
            range->val = (u32)obj_int( &dst );
        } else if ( type == T_STR ) {
            unicode_of( &dst, range );
        } else {
            cmap->count--;
        }
    }
    in_close( in );
    pool_free( words );
    words = NULL;

    /* into the font's own memory, in order of code */
    for ( gap = 1; gap < cmap->count / 3; gap = gap * 3 + 1 ) {
    }
    for ( ; gap; gap /= 3 ) {
        for ( at = gap; at < cmap->count; at++ ) {
            hold = cmap->ranges[at];
            for ( back = at; back >= gap && cmap->ranges[back - gap].lo > hold.lo; back -= gap ) {
                cmap->ranges[back] = cmap->ranges[back - gap];
            }
            cmap->ranges[back] = hold;
        }
    }
    final = (CMRANGE *)pool_alloc( pool, (cmap->count + 1) * sizeof( CMRANGE ) );
    if ( cmap->ranges ) {
        mem_cpy( final, cmap->ranges, cmap->count * sizeof( CMRANGE ) );
        pool_unbig( cmap->ranges );
    }
    cmap->ranges = final;
    cmap->cap = cmap->count;
    return cmap;
}

int cmap_find( const CMAP *cmap, u32 code, CMRANGE **range )
{
    u32 lo = 0, hi = cmap->count, mid;

    while ( lo < hi ) {                     /* the last range that starts at or before it */
        mid = (lo + hi) / 2;
        if ( cmap->ranges[mid].lo <= code ) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if ( lo == 0 || cmap->ranges[lo - 1].hi < code ) {
        return 0;
    }
    *range = &cmap->ranges[lo - 1];
    return 1;
}

/* ------------------------------------------------------------------ */
/* what kind of face                                                   */
/* ------------------------------------------------------------------ */

static int has_word( const char *name, const char *word )
{
    return mem_ifind( (const u8 *)name, str_len( name ), word ) >= 0;
}

static void face_flags( FONT *font, const char *name, DICT *desc )
{
    s32 flags = dict_int( desc, "Flags", 0 );
    int out = 0;

    if ( name[0] && name[6] == '+' ) {
        name += 7;                          /* a subset's six letters */
    }
    if ( (flags & 1) || has_word( name, "Courier" ) || has_word( name, "Mono" ) ||
         has_word( name, "Consol" ) || has_word( name, "Typewriter" ) ) {
        out |= FF_FIXED;
    }
    if ( ((flags & 2) || has_word( name, "Times" ) || has_word( name, "Roman" ) ||
          has_word( name, "Serif" ) || has_word( name, "Georgia" ) || has_word( name, "Garamond" ) ||
          has_word( name, "Palatino" ) || has_word( name, "Bookman" ) || has_word( name, "Century" ) ||
          has_word( name, "Cambria" ) || has_word( name, "Minion" )) && !has_word( name, "Sans" ) ) {
        out |= FF_SERIF;
    }
    if ( (flags & 64) || has_word( name, "Italic" ) || has_word( name, "Oblique" ) ||
         dict_int( desc, "ItalicAngle", 0 ) != 0 ) {
        out |= FF_ITALIC;
    }
    if ( (flags & 0x40000L) || has_word( name, "Bold" ) || has_word( name, "Black" ) ||
         has_word( name, "Heavy" ) || has_word( name, "Demi" ) || dict_int( desc, "FontWeight", 400 ) >= 600 ) {
        out |= FF_BOLD;
    }
    if ( str_cmp( name, "Symbol" ) == 0 ) {
        out |= FF_SYMBOL;
    }
    if ( str_cmp( name, "ZapfDingbats" ) == 0 || has_word( name, "Dingbats" ) || has_word( name, "Wingdings" ) ) {
        out |= FF_DINGBATS;
    }
    font->flags = (u8)out;
    font->symbolic = (u8)((flags & 4) != 0 && (flags & 32) == 0);
}

/* the font program in the descriptor, read; FT_BUILTIN if there is
   none or it cannot be */
static void program_load( FONT *font, DICT *desc )
{
    OBJ *file;
    u8 *data, *inner;
    u32 len, inner_len;
    const char *subtype;

    font->kind = FT_BUILTIN;
    font->units = 1000;
    if ( desc == NULL ) {
        return;
    }
    file = dict_get( desc, "FontFile2" );
    if ( file->type == T_STREAM ) {
        data = pdf_stream_pool( file, font->pool, &len, 16UL * 1024 * 1024 );
        if ( data && (font->prog = ttf_open( font->pool, data, len )) != NULL ) {
            font->kind = FT_TRUETYPE;
            font->units = ttf_units( font->prog );
            if ( ttf_cff( font->prog, &inner, &inner_len ) ) {
                font->prog = cff_open( font->pool, inner, inner_len );
                font->kind = font->prog ? FT_CFF : FT_BUILTIN;
            }
        }
        return;
    }
    file = dict_get( desc, "FontFile3" );
    if ( file->type == T_STREAM ) {
        subtype = dict_name( file->u.dict, "Subtype" );
        data = pdf_stream_pool( file, font->pool, &len, 16UL * 1024 * 1024 );
        if ( data == NULL ) {
            return;
        }
        if ( str_cmp( subtype, "OpenType" ) == 0 || (len > 4 && data[0] == 'O' && data[1] == 'T') ||
             (len > 4 && data[0] == 0 && data[1] == 1 && data[2] == 0 && data[3] == 0) ) {
            font->prog = ttf_open( font->pool, data, len );
            if ( font->prog == NULL ) {
                return;
            }
            if ( ttf_cff( font->prog, &inner, &inner_len ) ) {
                font->prog = cff_open( font->pool, inner, inner_len );
                font->kind = font->prog ? FT_CFF : FT_BUILTIN;
                font->units = font->prog ? cff_units( font->prog ) : 1000;
            } else {
                font->kind = FT_TRUETYPE;
                font->units = ttf_units( font->prog );
            }
            return;
        }
        font->prog = cff_open( font->pool, data, len );
        if ( font->prog ) {
            font->kind = FT_CFF;
            font->units = cff_units( font->prog );
        }
        return;
    }
    file = dict_get( desc, "FontFile" );
    if ( file->type == T_STREAM ) {
        data = pdf_stream_pool( file, font->pool, &len, 16UL * 1024 * 1024 );
        if ( data && (font->prog = t1_open( font->pool, data, len )) != NULL ) {
            font->kind = FT_TYPE1;
            font->units = t1_units( font->prog );
        }
    }
}

/* ------------------------------------------------------------------ */
/* a simple font                                                       */
/* ------------------------------------------------------------------ */

static void simple_load( FONT *font, DICT *dict, const char *subtype )
{
    DICT *desc = dict_dict( dict, "FontDescriptor" ), *enc_dict = NULL;
    OBJ *enc = dict_get( dict, "Encoding" ), *item;
    ARR *diffs, *widths;
    const char *name, *base_name = "";
    s32 first, last, missing;
    int base = -1, code, has31, has30, has10, gid;

    face_flags( font, dict_name( dict, "BaseFont" ), desc );
    if ( str_cmp( subtype, "Type3" ) == 0 ) {
        font->kind = FT_TYPE3;
        font->units = 1000;
    } else {
        program_load( font, desc );
    }

    /* the encoding: a base, and what is different from it */
    if ( enc->type == T_NAME ) {
        base_name = (const char *)enc->u.str.ptr;
    } else if ( obj_dict( enc ) ) {
        enc_dict = obj_dict( enc );
        base_name = dict_name( enc_dict, "BaseEncoding" );
    }
    if ( str_cmp( base_name, "WinAnsiEncoding" ) == 0 ) {
        base = ENC_WIN;
    } else if ( str_cmp( base_name, "MacRomanEncoding" ) == 0 ) {
        base = ENC_MAC;
    } else if ( base_name[0] ) {
        base = ENC_STD;
    } else if ( font->kind == FT_TYPE1 || font->kind == FT_CFF ) {
        base = ENC_BUILTIN;
    } else if ( font->flags & FF_SYMBOL ) {
        base = ENC_SYMBOL;
    } else if ( font->kind == FT_TRUETYPE && font->symbolic ) {
        base = -1;                          /* the codes are the font's own */
    } else if ( !(font->flags & FF_DINGBATS) ) {
        base = font->kind == FT_TRUETYPE ? ENC_WIN : ENC_STD;
    }
    if ( base == ENC_BUILTIN ) {
        for ( code = 0; code < 256; code++ ) {
            name = font->kind == FT_TYPE1 ? t1_builtin_name( font->prog, code )
                                          : cff_builtin_name( font->prog, code );
            font->gname[code] = name;
            font->uni[code] = name ? name_to_uni( name ) : 0;
        }
    } else if ( base >= 0 ) {
        enc_base( base, font->uni );
        for ( code = 0; code < 256; code++ ) {
            font->gname[code] = font->uni[code] ? uni_to_name( font->uni[code] ) : NULL;
        }
    }
    diffs = enc_dict ? dict_arr( enc_dict, "Differences" ) : NULL;
    if ( diffs ) {
        code = 0;
        for ( first = 0; (u32)first < diffs->count; first++ ) {
            item = arr_get( diffs, (u32)first );
            if ( obj_is_num( item ) ) {
                code = (int)obj_int( item );
            } else if ( item->type == T_NAME && code >= 0 && code < 256 ) {
                font->gname[code] = pool_str( font->pool, (const char *)item->u.str.ptr );
                font->uni[code] = name_to_uni( font->gname[code] );
                code++;
            }
        }
    }
    if ( font->flags & FF_DINGBATS ) {
        /* Zapf's: no names worth having.  The few a document uses for
           its bullets and ticks; a bullet for the rest. */
        for ( code = 33; code < 256; code++ ) {
            if ( font->uni[code] == 0 ) {
                font->uni[code] = code == 0x33 || code == 0x34 ? 0x2713
                                : code == 0x6E || code == 0x6F || code == 0x71 || code == 0x72 ? 0x25A0
                                : code >= 0xD4 && code <= 0xEF ? 0x2192 : 0x2022;
            }
        }
    }

    /* the widths */
    for ( code = 0; code < 256; code++ ) {
        font->width[code] = -1;
        font->gid[code] = 0xFFFF;
    }
    widths = dict_arr( dict, "Widths" );
    first = dict_int( dict, "FirstChar", 0 );
    last = dict_int( dict, "LastChar", 255 );
    missing = dict_int( desc, "MissingWidth", 0 );
    font->dw = -1;
    if ( widths ) {
        font->dw = (int)missing;
        for ( code = (int)first; code <= last && code < 256; code++ ) {
            if ( code >= 0 && (u32)(code - first) < widths->count ) {
                font->width[code] = (s16)obj_int( arr_get( widths, (u32)(code - first) ) );
            }
        }
    }

    /* which glyph of the program each code is */
    if ( font->kind == FT_TYPE1 || (font->kind == FT_CFF && !cff_is_cid( font->prog )) ) {
        for ( code = 0; code < 256; code++ ) {
            if ( font->gname[code] ) {
                gid = font->kind == FT_TYPE1 ? t1_glyph_named( font->prog, font->gname[code] )
                                             : cff_glyph_named( font->prog, font->gname[code] );
                if ( gid >= 0 ) {
                    font->gid[code] = (u16)gid;
                }
            }
        }
    } else if ( font->kind == FT_CFF ) {
        for ( code = 0; code < 256; code++ ) {
            font->gid[code] = (u16)code;
        }
    } else if ( font->kind == FT_TRUETYPE ) {
        has31 = ttf_has_cmap( font->prog, 3, 1 );
        has30 = ttf_has_cmap( font->prog, 3, 0 );
        has10 = ttf_has_cmap( font->prog, 1, 0 );
        for ( code = 0; code < 256; code++ ) {
            gid = 0;
            if ( has31 && font->uni[code] && !(font->symbolic && has30) ) {
                gid = ttf_cmap( font->prog, 3, 1, font->uni[code] );
            }
            if ( gid == 0 && has30 ) {
                gid = ttf_cmap( font->prog, 3, 0, (u32)code );
                if ( gid == 0 ) {
                    gid = ttf_cmap( font->prog, 3, 0, 0xF000UL + (u32)code );
                }
            }
            if ( gid == 0 && has10 ) {
                gid = ttf_cmap( font->prog, 1, 0, (u32)code );
            }
            if ( gid == 0 && has31 ) {
                gid = ttf_cmap( font->prog, 3, 1, font->uni[code] ? font->uni[code] : (u32)code );
                if ( gid == 0 ) {
                    gid = ttf_cmap( font->prog, 3, 1, 0xF000UL + (u32)code );
                }
            }
            if ( gid == 0 && !has31 && !has30 && !has10 ) {
                gid = code;                 /* no map at all: the code is the glyph */
            }
            font->gid[code] = (u16)gid;
        }
    }

    if ( font->kind == FT_TYPE3 ) {
        font->t3_procs = dict_dict( dict, "CharProcs" );
        font->t3_res = dict_dict( dict, "Resources" );
        rmat_identity( &font->t3_matrix );
        widths = dict_arr( dict, "FontMatrix" );
        if ( widths && widths->count >= 6 ) {
            for ( code = 0; code < 6; code++ ) {
                font->t3_matrix.m[code] = obj_real( arr_get( widths, (u32)code ) );
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* a composite font                                                    */
/* ------------------------------------------------------------------ */

static void composite_load( FONT *font, DICT *dict )
{
    DICT *child = obj_dict( arr_get( dict_arr( dict, "DescendantFonts" ), 0 ) );
    DICT *desc = dict_dict( child, "FontDescriptor" );
    OBJ *enc = dict_get( dict, "Encoding" ), *map, *item;
    ARR *warr = dict_arr( child, "W" );
    WRANGE *range, hold;
    u32 index, at, gap, back, count;
    s32 first, last;

    font->composite = 1;
    face_flags( font, dict_name( child, "BaseFont" ), desc );
    program_load( font, desc );
    if ( font->kind == FT_TYPE1 ) {
        font->kind = FT_BUILTIN;            /* a CID-keyed Type 1: not read */
    }
    font->cff_cid = font->kind == FT_CFF && cff_is_cid( font->prog );
    font->dw = (int)dict_int( child, "DW", 1000 );
    if ( enc->type == T_STREAM ) {
        font->enc = cmap_parse( enc, font->pool );
    }
    map = dict_get( child, "CIDToGIDMap" );
    if ( map->type == T_STREAM && font->kind == FT_TRUETYPE ) {
        font->cid2gid = pdf_stream_pool( map, font->pool, &font->cid2gid_len, 131072UL );
    }

    if ( warr ) {
        font->wr = (WRANGE *)pool_alloc( font->pool, (warr->count / 2 + 1) * sizeof( WRANGE ) );
        for ( index = 0; index + 1 < warr->count; ) {
            first = obj_int( arr_get( warr, index ) );
            item = arr_get( warr, index + 1 );
            range = &font->wr[font->nwr];
            if ( item->type == T_ARR ) {
                count = item->u.arr->count;
                if ( count && first >= 0 && first + count <= 65536UL ) {
                    range->first = (u16)first;
                    range->last = (u16)(first + count - 1);
                    range->widths = (s16 *)pool_alloc( font->pool, count * sizeof( s16 ) );
                    for ( at = 0; at < count; at++ ) {
                        range->widths[at] = (s16)obj_int( arr_get( item->u.arr, at ) );
                    }
                    font->nwr++;
                }
                index += 2;
            } else {
                last = obj_int( item );
                if ( index + 2 < warr->count && first >= 0 && last >= first && last < 65536L ) {
                    range->first = (u16)first;
                    range->last = (u16)last;
                    range->width = (s16)obj_int( arr_get( warr, index + 2 ) );
                    font->nwr++;
                }
                index += 3;
            }
        }
        for ( gap = 1; gap < font->nwr / 3; gap = gap * 3 + 1 ) {
        }
        for ( ; gap; gap /= 3 ) {
            for ( at = gap; at < font->nwr; at++ ) {
                hold = font->wr[at];
                for ( back = at; back >= gap && font->wr[back - gap].first > hold.first; back -= gap ) {
                    font->wr[back] = font->wr[back - gap];
                }
                font->wr[back] = hold;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* finding and keeping                                                 */
/* ------------------------------------------------------------------ */

FONT *font_get( OBJ *ref )
{
    OBJ *obj = obj_resolve( ref ), *tou;
    DICT *dict = obj_dict( obj );
    FONT *font;
    POOL *pool;
    const char *subtype;
    int keep;

    if ( dict == NULL ) {
        return NULL;
    }
    for ( font = fonts; font; font = font->next ) {
        if ( dict->num && font->key == dict->num ) {
            font->stamp = stamp;
            return font;
        }
    }
    for ( font = page_fonts; font; font = font->next ) {
        if ( font->ident == dict ) {
            return font;
        }
    }
    if ( loading ) {
        pool_free( loading );               /* one that was given up halfway */
        loading = NULL;
    }
    subtype = dict_name( dict, "Subtype" );
    keep = dict->num != 0 && str_cmp( subtype, "Type3" ) != 0;
    pool = keep ? pool_new() : doc.page_pool;
    if ( keep ) {
        loading = pool;
    }
    font = (FONT *)pool_alloc( pool, sizeof( FONT ) );
    font->pool = pool;
    font->key = keep ? dict->num : 0;
    font->stamp = stamp;
    if ( str_cmp( subtype, "Type0" ) == 0 ) {
        composite_load( font, dict );
    } else {
        simple_load( font, dict, subtype );
    }
    tou = dict_get( dict, "ToUnicode" );
    if ( tou->type == T_STREAM ) {
        font->tou = cmap_parse( tou, pool );
    }
    if ( keep ) {
        loading = NULL;
        font->next = fonts;
        fonts = font;
    } else {
        font->ident = dict;                 /* known again, while the page lasts */
        font->next = page_fonts;
        page_fonts = font;
    }
    return font;
}

/* a new page: the page's own fonts went with its pool */
void font_stamp( void )
{
    stamp++;
    page_fonts = NULL;
}

int font_reclaim( int all )
{
    FONT **link = &fonts, *font;
    int freed = 0;

    while ( (font = *link) != NULL ) {
        if ( all || font->stamp != stamp ) {
            if ( !freed ) {
                glyph_reclaim();            /* its glyphs point at it */
            }
            *link = font->next;
            pool_free( font->pool );
            freed = 1;
        } else {
            link = &font->next;
        }
    }
    if ( all && loading ) {
        pool_free( loading );
        loading = NULL;
    }
    return freed;
}

/* ------------------------------------------------------------------ */
/* using one                                                           */
/* ------------------------------------------------------------------ */

u32 font_next_code( FONT *font, const u8 **str, u32 *left )
{
    const u8 *text = *str;
    u32 code = 0, len, take = 2;
    int index;

    if ( *left == 0 ) {
        return 0;
    }
    if ( !font->composite ) {
        (*str)++;
        (*left)--;
        return text[0];
    }
    if ( font->enc && font->enc->nspace ) {
        take = 0;
        for ( len = 1; len <= 4 && len <= *left && !take; len++ ) {
            code = (code << 8) | text[len - 1];
            for ( index = 0; index < font->enc->nspace; index++ ) {
                if ( (u32)font->enc->space[index].len == len && code >= font->enc->space[index].lo &&
                     code <= font->enc->space[index].hi ) {
                    take = len;
                    break;
                }
            }
        }
        if ( !take ) {
            take = (u32)font->enc->space[0].len;    /* in none of them: step over it */
        }
    }
    if ( take > *left ) {
        take = *left;
    }
    code = 0;
    for ( len = 0; len < take; len++ ) {
        code = (code << 8) | text[len];
    }
    *str += take;
    *left -= take;
    return code;
}

/* a composite font's code as a character ID */
static u32 cid_of( FONT *font, u32 code )
{
    CMRANGE *range;

    if ( font->enc ) {
        return cmap_find( font->enc, code, &range ) ? range->val + (code - range->lo) : 0;
    }
    return code;
}

int font_width( FONT *font, u32 code )
{
    u32 cid, lo, hi, mid;

    if ( font->composite ) {
        cid = cid_of( font, code );
        lo = 0;
        hi = font->nwr;
        while ( lo < hi ) {
            mid = (lo + hi) / 2;
            if ( font->wr[mid].last < cid ) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if ( lo < font->nwr && font->wr[lo].first <= cid ) {
            return font->wr[lo].widths ? font->wr[lo].widths[cid - font->wr[lo].first] : font->wr[lo].width;
        }
        return font->dw;
    }
    code &= 255;
    if ( font->width[code] >= 0 ) {
        return font->width[code];
    }
    if ( font->dw >= 0 ) {
        return font->dw;                    /* there are widths, and this is not among them */
    }
    return sfont_width( font->uni[code], font->flags );
}

int font_unicode( FONT *font, u32 code, u16 *out )
{
    CMRANGE *range;
    int count = 1;
    u32 gid;

    if ( font->tou && cmap_find( font->tou, code, &range ) ) {
        out[0] = (u16)(range->val + (code - range->lo));
        if ( code == range->lo ) {
            for ( ; count < 4 && range->more[count - 1]; count++ ) {
                out[count] = range->more[count - 1];
            }
        }
        return count;
    }
    if ( !font->composite ) {
        out[0] = font->uni[code & 255];
        if ( out[0] == 0 && font->kind == FT_TRUETYPE && font->gid[code & 255] != 0xFFFF ) {
            out[0] = ttf_unicode_of( font->prog, font->gid[code & 255] );
        }
        if ( out[0] == 0 && (code & 255) >= 32 && (code & 255) < 127 && !font->symbolic ) {
            out[0] = (u16)(code & 255);
        }
        return out[0] != 0;
    }
    /* no ToUnicode: a TrueType program may still say, backwards */
    if ( font->kind == FT_TRUETYPE ) {
        gid = cid_of( font, code );
        if ( font->cid2gid && gid * 2 + 1 < font->cid2gid_len ) {
            gid = ((u32)font->cid2gid[gid * 2] << 8) | font->cid2gid[gid * 2 + 1];
        }
        out[0] = ttf_unicode_of( font->prog, gid );
        if ( out[0] ) {
            return 1;
        }
    }
    out[0] = 0xFFFD;
    return 1;
}

/* The outline of a code's glyph, to gl_move and the rest.  1 if it is
   to be filled, 2 if it is a pen's path "pen" wide (in the font's
   units), 0 if there is nothing to draw. */
int font_outline( FONT *font, u32 code, fx *pen )
{
    u16 uni[4];
    u32 gid;

    if ( font->composite ) {
        gid = cid_of( font, code );
        switch ( font->kind ) {
        case FT_TRUETYPE:
            if ( font->cid2gid ) {
                if ( gid * 2 + 1 >= font->cid2gid_len ) {
                    return 0;
                }
                gid = ((u32)font->cid2gid[gid * 2] << 8) | font->cid2gid[gid * 2 + 1];
            }
            return ttf_outline( font->prog, gid );
        case FT_CFF:
            return cff_outline( font->prog, (u32)cff_glyph_of_cid( font->prog, gid ) );
        }
        if ( !font_unicode( font, code, uni ) ) {
            return 0;
        }
        return sfont_outline( uni[0], font->flags, font_width( font, code ), pen );
    }
    code &= 255;
    gid = font->gid[code];
    switch ( font->kind ) {
    case FT_TRUETYPE:
        return gid != 0xFFFF && gid != 0 ? ttf_outline( font->prog, gid ) : 0;
    case FT_CFF:
        return gid != 0xFFFF ? cff_outline( font->prog, gid ) : 0;
    case FT_TYPE1:
        return gid != 0xFFFF ? t1_outline( font->prog, gid ) : 0;
    case FT_BUILTIN:
        return sfont_outline( font->uni[code], font->flags, font_width( font, code ), pen );
    }
    return 0;
}
