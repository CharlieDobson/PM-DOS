/*
 * INTERP.C - a page's content stream, carried out.
 *
 * A content stream is operands and then an operator, over and over:
 * "10 20 m" starts a path, "(Hello) Tj" shows a string.  This reads
 * them with LEX.C, keeps the graphics state they build up, and turns
 * each thing they paint into calls on the rest of GFX.H - or, when a
 * page is being read for its text and not drawn, hands each character
 * to the text sink and paints nothing.
 *
 * WHERE THINGS ARE.  The state's matrix takes the page's own space
 * straight to the canvas's pixels, and it is kept twice: as REALs,
 * which is what one matrix is multiplied into another with, and as
 * 16.16, which is what a path's points go through.  A point too far
 * out for 16.16 goes the slow way.
 *
 * A form XObject, a Type 3 font's glyph and a tiling pattern's cell
 * are content streams too, run from inside this one; the nesting is
 * counted and stops at MAX_NEST.
 */
#include "gfx.h"

#define GS_MAX      64
#define MAX_ARGS    40
#define MAX_NEST    16

typedef struct {
    RMAT    ctm;
    FMAT    fm;
    fx      safe;               /* points smaller than this take the quick way */
    CLIP    clip;
    CSPACE *fill_cs, *stroke_cs;
    PAINT   fill, stroke;
    int     group_alpha;        /* of the transparency group this is inside */
    fx      line_width;
    int     cap, join;
    fx      miter;
    fx      dash[12];
    int     ndash;
    fx      dash_phase;
    FONT   *font;
    fx      size, char_space, word_space, hscale, leading, rise;
    int     render;
} GSTATE;

int  (*render_poll)( void );
void (*text_sink)( u16 uni, fx xpos, fx ypos, fx advance, fx size );

static CANVAS *canvas;
static GSTATE *states;              /* GS_MAX of them, from the heap */
static GSTATE *gs;
static int     gs_depth;
static PATH    path;
static int     pending_clip;        /* 1: W, 2: W* - at the next painting operator */
static DICT   *resources;
static RMAT    pattern_base;        /* the space a pattern is laid out in */
static int     reading;             /* for the text: nothing is drawn */
static int     nest;
static u32     op_tick;
static POOL   *arg_pools[MAX_NEST];

/* text */
static RMAT    text_m, line_m;
static fx      text_run;            /* how far along the line the pen has gone */
static int     text_stale;          /* the rest of these want working out again */
static fx      t_ax, t_ay, t_bx, t_by;
static fx      pen_x, pen_y;
static s32     glyph_mat[4];

static void run_stream( OBJ *stm, DICT *res );

/* ------------------------------------------------------------------ */
/* the state                                                           */
/* ------------------------------------------------------------------ */

static fx fx_abs( fx val )
{
    return val < 0 ? -val : val;
}

static void ctm_changed( void )
{
    fx across, down, most;

    rmat_to_fmat( &gs->fm, &gs->ctm );
    across = fx_abs( gs->fm.a ) + fx_abs( gs->fm.c );
    down = fx_abs( gs->fm.b ) + fx_abs( gs->fm.d );
    most = across > down ? across : down;
    /* a point this small cannot overflow on its way through */
    if ( fx_abs( gs->fm.e ) > I2FX( 8000 ) || fx_abs( gs->fm.f ) > I2FX( 8000 ) ) {
        gs->safe = 0;
    } else {
        gs->safe = most > 16 ? mul_div( I2FX( 8000 ), FX_ONE, most ) : FX_MAX;
    }
    text_stale = 1;
}

static void state_push( void )
{
    if ( gs_depth >= GS_MAX - 1 ) {
        return;
    }
    states[gs_depth + 1] = states[gs_depth];
    gs = &states[++gs_depth];
    mask_keep( gs->clip.mask );
}

static void state_pop( void )
{
    if ( gs_depth == 0 ) {
        return;
    }
    mask_drop( gs->clip.mask );
    gs = &states[--gs_depth];
    text_stale = 1;
}

/* a number small enough for the quick way through the matrix */
static int small_fx( OBJ *obj, fx *out )
{
    if ( obj->type == T_INT ) {
        if ( obj->u.ival > 32000 || obj->u.ival < -32000 ) {
            return 0;
        }
        *out = I2FX( obj->u.ival );
    } else if ( obj->type == T_REAL ) {
        if ( obj->u.real.exp + 16 > 0 ) {
            return 0;
        }
        *out = real_to_fx( obj->u.real );
    } else {
        *out = 0;
    }
    return fx_abs( *out ) < gs->safe;
}

static void user_point( OBJ *ox, OBJ *oy, fx *px, fx *py )
{
    fx ux, uy;

    if ( small_fx( ox, &ux ) && small_fx( oy, &uy ) ) {
        *px = fx_mul( gs->fm.a, ux ) + fx_mul( gs->fm.c, uy ) + gs->fm.e;
        *py = fx_mul( gs->fm.b, ux ) + fx_mul( gs->fm.d, uy ) + gs->fm.f;
    } else {
        rmat_point( &gs->ctm, obj_real( ox ), obj_real( oy ), px, py );
    }
}

static void matrix_from( RMAT *mat, OBJ *args )
{
    int index;

    for ( index = 0; index < 6; index++ ) {
        mat->m[index] = obj_real( &args[index] );
    }
}

static int matrix_of( RMAT *mat, ARR *arr )
{
    int index;

    rmat_identity( mat );
    if ( arr == NULL || arr->count < 6 ) {
        return 0;
    }
    for ( index = 0; index < 6; index++ ) {
        mat->m[index] = obj_real( arr_get( arr, (u32)index ) );
    }
    return 1;
}

/* how much longer the matrix makes a length: the root of its area */
static fx ctm_scale( void )
{
    return fx_sqrt( fx_abs( fx_mul( gs->fm.a, gs->fm.d ) - fx_mul( gs->fm.b, gs->fm.c ) ) );
}

/* ------------------------------------------------------------------ */
/* painting a path                                                     */
/* ------------------------------------------------------------------ */

static void clip_to_path( int evenodd )
{
    MASK *mask;
    int x0, y0, x1, y1;

    if ( path.count == 0 ) {
        gs->clip.x1 = gs->clip.x0;                  /* nothing is inside nothing */
        return;
    }
    if ( !path_as_rect( &path, &x0, &y0, &x1, &y1 ) ) {
        ras_begin();
        path_edges( &path );
        mask = mask_make( &gs->clip, evenodd );
        mask_drop( gs->clip.mask );
        gs->clip.mask = mask;
        mask_box( mask, &x0, &y0, &x1, &y1 );
    }
    if ( x0 > gs->clip.x0 ) {
        gs->clip.x0 = x0;
    }
    if ( y0 > gs->clip.y0 ) {
        gs->clip.y0 = y0;
    }
    if ( x1 < gs->clip.x1 ) {
        gs->clip.x1 = x1;
    }
    if ( y1 < gs->clip.y1 ) {
        gs->clip.y1 = y1;
    }
    if ( gs->clip.x1 < gs->clip.x0 ) {
        gs->clip.x1 = gs->clip.x0;
    }
    if ( gs->clip.y1 < gs->clip.y0 ) {
        gs->clip.y1 = gs->clip.y0;
    }
}

static void path_done( void )
{
    if ( pending_clip && !reading ) {
        clip_to_path( pending_clip == 2 );
    }
    pending_clip = 0;
    path_reset( &path );
}

static void do_fill( int evenodd )
{
    if ( reading || path.count == 0 ) {
        return;
    }
    ras_begin();
    path_edges( &path );
    ras_fill( canvas, &gs->clip, evenodd, &gs->fill );
}

static void do_stroke( void )
{
    STROKE stroke;
    fx scale;
    int index;

    if ( reading || path.count == 0 ) {
        return;
    }
    scale = ctm_scale();
    stroke.width = fx_mul( gs->line_width, scale );
    stroke.cap = gs->cap;
    stroke.join = gs->join;
    stroke.miter = gs->miter;
    stroke.ndash = gs->ndash;
    for ( index = 0; index < gs->ndash; index++ ) {
        stroke.dash[index] = fx_mul( gs->dash[index], scale );
    }
    stroke.phase = fx_mul( gs->dash_phase, scale );
    ras_begin();
    path_stroke_edges( &path, &stroke );
    ras_fill( canvas, &gs->clip, 0, &gs->stroke );
}

/* ------------------------------------------------------------------ */
/* colour                                                              */
/* ------------------------------------------------------------------ */

static void colour_device( int stroke, CSPACE *cs, OBJ *args, int nargs )
{
    PAINT *paint = stroke ? &gs->stroke : &gs->fill;
    fx comps[MAX_COMPS];
    int index;

    if ( stroke ) {
        gs->stroke_cs = cs;
    } else {
        gs->fill_cs = cs;
    }
    cs_initial( cs, comps );
    for ( index = 0; index < nargs && index < MAX_COMPS; index++ ) {
        comps[index] = obj_fx( &args[index] );
    }
    paint->rgb = cs_rgb( cs, comps );
    paint->row = NULL;
    paint->none = cs->none;
}

static void colour_pattern( int stroke, const char *name, OBJ *args, int nargs )
{
    PAINT *paint = stroke ? &gs->stroke : &gs->fill;
    CSPACE *cs = stroke ? gs->stroke_cs : gs->fill_cs;
    OBJ *pat = dict_get( dict_dict( resources, "Pattern" ), name );
    DICT *dict = obj_dict( pat );
    SHADE *shade;
    RMAT mat;
    fx comps[MAX_COMPS];
    int index;

    paint->row = NULL;
    paint->none = 0;
    if ( dict == NULL ) {
        return;
    }
    if ( dict_int( dict, "PatternType", 1 ) == 2 ) {
        matrix_of( &mat, dict_arr( dict, "Matrix" ) );
        rmat_mul( &mat, &mat, &pattern_base );
        shade = reading ? NULL : shade_load( dict_get( dict, "Shading" ), &mat, resources );
        if ( shade ) {
            shade_paint( shade, paint );
        } else {
            paint->rgb = 0x808080UL;
        }
        return;
    }
    /* a tiling pattern is not laid out: an uncoloured one is painted
       as its colour, thinned, and a coloured one as a pale grey */
    if ( dict_int( dict, "PaintType", 1 ) == 2 && cs->base ) {
        cs_initial( cs->base, comps );
        for ( index = 0; index < nargs && index < MAX_COMPS; index++ ) {
            comps[index] = obj_fx( &args[index] );
        }
        paint->rgb = cs_rgb( cs->base, comps );
        paint->rgb = ((paint->rgb >> 1) & 0x7F7F7FUL) + 0x808080UL;
    } else {
        paint->rgb = 0xC8C8C8UL;
    }
}

static void colour_set( int stroke, OBJ *args, int nargs )
{
    if ( nargs && args[nargs - 1].type == T_NAME ) {
        colour_pattern( stroke, (const char *)args[nargs - 1].u.str.ptr, args, nargs - 1 );
    } else {
        colour_device( stroke, stroke ? gs->stroke_cs : gs->fill_cs, args, nargs );
    }
}

static int blend_of( OBJ *obj )
{
    const char *name;

    if ( obj->type == T_ARR ) {
        obj = arr_get( obj->u.arr, 0 );
    }
    name = obj_name( obj );
    if ( str_cmp( name, "Multiply" ) == 0 || str_cmp( name, "ColorBurn" ) == 0 ) {
        return BM_MULTIPLY;
    }
    if ( str_cmp( name, "Darken" ) == 0 ) {
        return BM_DARKEN;
    }
    if ( str_cmp( name, "Screen" ) == 0 || str_cmp( name, "ColorDodge" ) == 0 ) {
        return BM_SCREEN;
    }
    if ( str_cmp( name, "Lighten" ) == 0 ) {
        return BM_LIGHTEN;
    }
    return BM_NORMAL;
}

static int alpha_of( OBJ *obj )
{
    fx val = obj_fx( obj );

    return val <= 0 ? 0 : (val >= FX_ONE ? 255 : (int)((val * 255) >> 16));
}

static void ext_gstate( const char *name )
{
    DICT *dict = dict_dict( dict_dict( resources, "ExtGState" ), name );
    OBJ *val;
    ARR *arr, *dashes;
    u32 index;

    if ( dict == NULL ) {
        return;
    }
    for ( index = 0; index < dict->count; index++ ) {
        name = dict->ents[index].key;
        val = obj_resolve( &dict->ents[index].val );
        if ( str_cmp( name, "LW" ) == 0 ) {
            gs->line_width = obj_fx( val );
        } else if ( str_cmp( name, "LC" ) == 0 ) {
            gs->cap = (int)obj_int( val );
        } else if ( str_cmp( name, "LJ" ) == 0 ) {
            gs->join = (int)obj_int( val );
        } else if ( str_cmp( name, "ML" ) == 0 ) {
            gs->miter = obj_fx( val );
        } else if ( str_cmp( name, "CA" ) == 0 ) {
            gs->stroke.alpha = (u8)(alpha_of( val ) * gs->group_alpha / 255);
        } else if ( str_cmp( name, "ca" ) == 0 ) {
            gs->fill.alpha = (u8)(alpha_of( val ) * gs->group_alpha / 255);
        } else if ( str_cmp( name, "BM" ) == 0 ) {
            gs->fill.blend = gs->stroke.blend = (u8)blend_of( val );
        } else if ( str_cmp( name, "Font" ) == 0 && val->type == T_ARR ) {
            gs->font = font_get( &val->u.arr->items[0] );
            gs->size = obj_fx( arr_get( val->u.arr, 1 ) );
            text_stale = 1;
        } else if ( str_cmp( name, "D" ) == 0 && val->type == T_ARR ) {
            arr = val->u.arr;
            dashes = arr_get( arr, 0 )->type == T_ARR ? arr_get( arr, 0 )->u.arr : NULL;
            gs->ndash = 0;
            for ( gs->ndash = 0; dashes && (u32)gs->ndash < dashes->count && gs->ndash < 12; gs->ndash++ ) {
                gs->dash[gs->ndash] = obj_fx( arr_get( dashes, (u32)gs->ndash ) );
            }
            gs->dash_phase = obj_fx( arr_get( arr, 1 ) );
        }
    }
}

/* ------------------------------------------------------------------ */
/* text                                                                */
/* ------------------------------------------------------------------ */

static void text_refresh( void )
{
    RMAT whole;
    FMAT flat;
    fx wide;
    int units;

    rmat_mul( &whole, &text_m, &gs->ctm );
    rmat_to_fmat( &flat, &whole );
    t_ax = flat.a;
    t_ay = flat.b;
    t_bx = flat.c;
    t_by = flat.d;
    pen_x = flat.e + fx_mul( t_ax, text_run ) + fx_mul( t_bx, gs->rise );
    pen_y = flat.f + fx_mul( t_ay, text_run ) + fx_mul( t_by, gs->rise );
    if ( gs->font ) {
        units = gs->font->units ? gs->font->units : 1000;
        wide = fx_mul( gs->size, gs->hscale );
        glyph_mat[0] = mul_div( fx_mul( t_ax, wide ), 256, units );
        glyph_mat[1] = mul_div( fx_mul( t_ay, wide ), 256, units );
        glyph_mat[2] = mul_div( fx_mul( t_bx, gs->size ), 256, units );
        glyph_mat[3] = mul_div( fx_mul( t_by, gs->size ), 256, units );
    }
    text_stale = 0;
}

/* a Type 3 font's glyph: a content stream of its own */
static void type3_glyph( FONT *font, u32 code )
{
    const char *name = font->gname[code & 255];
    OBJ *proc;
    RMAT mat, place, keep_text = text_m, keep_line = line_m;
    fx keep_run = text_run;
    REAL zero = real_int( 0 );

    if ( name == NULL || font->t3_procs == NULL || nest >= MAX_NEST - 1 ) {
        return;
    }
    proc = dict_get( font->t3_procs, name );
    if ( proc->type != T_STREAM ) {
        return;
    }
    state_push();
    /* glyph space, to text space at the pen, to the page */
    rmat_set( &place, real_fx( fx_mul( gs->size, gs->hscale ) ), zero, zero, real_fx( gs->size ),
              real_fx( text_run ), real_fx( gs->rise ) );
    rmat_mul( &mat, &font->t3_matrix, &place );
    rmat_mul( &mat, &mat, &keep_text );
    rmat_mul( &gs->ctm, &mat, &gs->ctm );
    ctm_changed();
    path_reset( &path );
    run_stream( proc, font->t3_res ? font->t3_res : resources );
    state_pop();
    text_m = keep_text;
    line_m = keep_line;
    text_run = keep_run;
    text_stale = 1;
}

static void show( const u8 *str, u32 len )
{
    FONT *font = gs->font;
    const u8 *before;
    u16 uni[4];
    u32 code;
    fx step;
    int count, index;

    if ( font == NULL ) {
        return;
    }
    while ( len ) {
        if ( text_stale ) {
            text_refresh();
        }
        before = str;
        code = font_next_code( font, &str, &len );
        step = mul_div( font_width( font, code ), gs->size, 1000 ) + gs->char_space;
        if ( code == 32 && str - before == 1 ) {
            step += gs->word_space;
        }
        step = fx_mul( step, gs->hscale );
        if ( reading ) {
            if ( text_sink ) {
                count = font_unicode( font, code, uni );
                for ( index = 0; index < count; index++ ) {
                    text_sink( uni[index], pen_x, pen_y,
                               index ? 0 : fx_hypot( fx_mul( t_ax, step ), fx_mul( t_ay, step ) ),
                               fx_mul( fx_hypot( t_bx, t_by ), gs->size ) );
                }
            }
        } else if ( gs->render != 3 && gs->render != 7 ) {
            if ( font->kind == FT_TYPE3 ) {
                type3_glyph( font, code );
                if ( text_stale ) {
                    text_refresh();
                }
            } else {
                glyph_draw( font, code, pen_x, pen_y, glyph_mat, canvas, &gs->clip,
                            gs->render == 1 || gs->render == 5 ? &gs->stroke : &gs->fill );
            }
        }
        text_run += step;
        pen_x += fx_mul( t_ax, step );
        pen_y += fx_mul( t_ay, step );
    }
}

static void text_move( REAL tx, REAL ty )
{
    RMAT shift;

    rmat_identity( &shift );
    shift.m[4] = tx;
    shift.m[5] = ty;
    rmat_mul( &line_m, &shift, &line_m );
    text_m = line_m;
    text_run = 0;
    text_stale = 1;
}

static void show_array( ARR *arr )
{
    OBJ *item;
    fx shift;
    u32 index;

    for ( index = 0; index < arr->count; index++ ) {
        item = &arr->items[index];
        if ( item->type == T_STR ) {
            show( item->u.str.ptr, item->u.str.len );
        } else if ( obj_is_num( item ) ) {
            /* thousandths of the font's size, back along the line */
            shift = -fx_mul( mul_div( obj_fx( item ), gs->size, 1000 * FX_ONE ), gs->hscale );
            if ( text_stale ) {
                text_refresh();
            }
            text_run += shift;
            pen_x += fx_mul( t_ax, shift );
            pen_y += fx_mul( t_ay, shift );
        }
    }
}

/* ------------------------------------------------------------------ */
/* XObjects and inline images                                          */
/* ------------------------------------------------------------------ */

static void do_form( OBJ *stm )
{
    DICT *dict = stm->u.dict, *own = dict_dict( dict, "Resources" );
    ARR *box = dict_arr( dict, "BBox" );
    RMAT mat, keep_text = text_m, keep_line = line_m, keep_base = pattern_base;
    fx keep_run = text_run, px, py;
    int corner;

    if ( nest >= MAX_NEST - 1 ) {
        return;
    }
    state_push();
    if ( matrix_of( &mat, dict_arr( dict, "Matrix" ) ) ) {
        rmat_mul( &gs->ctm, &mat, &gs->ctm );
        ctm_changed();
    }
    pattern_base = gs->ctm;
    if ( dict_dict( dict, "Group" ) ) {
        /* A transparency group is painted as a whole with the alpha
           in force where it is used.  The whole is not made here: each
           thing in it is painted that much fainter instead, which is
           the same wherever they do not overlap. */
        gs->group_alpha = gs->fill.alpha;
        gs->stroke.alpha = gs->fill.alpha;
    }
    if ( box && box->count >= 4 && !reading ) {
        path_reset( &path );
        for ( corner = 0; corner < 4; corner++ ) {
            user_point( arr_get( box, corner == 0 || corner == 3 ? 0 : 2 ),
                        arr_get( box, corner < 2 ? 1 : 3 ), &px, &py );
            if ( corner == 0 ) {
                path_move( &path, px, py );
            } else {
                path_line( &path, px, py );
            }
        }
        path_close( &path );
        clip_to_path( 0 );
    }
    path_reset( &path );
    run_stream( stm, own ? own : resources );
    state_pop();
    path_reset( &path );
    pattern_base = keep_base;
    text_m = keep_text;
    line_m = keep_line;
    text_run = keep_run;
    text_stale = 1;
}

static void do_image( OBJ *stm, DICT *dict, IN *inline_in )
{
    IMGREQ req;

    req.stm = stm;
    req.dict = dict;
    req.inline_in = inline_in;
    req.res = resources;
    req.fm = &gs->fm;
    req.cv = reading ? NULL : canvas;
    req.clip = &gs->clip;
    req.fill = &gs->fill;
    image_draw( &req );
}

static void do_xobject( const char *name )
{
    OBJ *obj = dict_get( dict_dict( resources, "XObject" ), name );
    const char *subtype;

    if ( obj->type != T_STREAM ) {
        return;
    }
    subtype = dict_name( obj->u.dict, "Subtype" );
    if ( str_cmp( subtype, "Form" ) == 0 ) {
        do_form( obj );
    } else if ( str_cmp( subtype, "Image" ) == 0 && !reading ) {
        do_image( obj, obj->u.dict, NULL );
    }
}

/* BI has been read: the dictionary up to ID, the data, and EI */
static void inline_image( LEX *lex )
{
    IN *in = lex->in;
    DICT *dict;
    OBJ key, val, pairs[64];
    int count = 0, ch, state = 0, index;

    for ( ;; ) {
        if ( lex_obj( lex, &key ) == T_EOF ) {
            return;
        }
        if ( key.type == T_OP ) {
            break;                                  /* ID */
        }
        if ( lex_obj( lex, &val ) == T_EOF ) {
            return;
        }
        if ( key.type == T_NAME && val.type != T_OP && count < 31 ) {
            pairs[count * 2] = key;
            pairs[count * 2 + 1] = val;
            count++;
        }
    }
    dict = (DICT *)pool_alloc( lex->pool, sizeof( DICT ) + (u32)count * sizeof( DENT ) );
    dict->count = (u32)count;
    dict->ents = (DENT *)(dict + 1);
    for ( index = 0; index < count; index++ ) {
        dict->ents[index].key = (const char *)pairs[index * 2].u.str.ptr;
        dict->ents[index].val = pairs[index * 2 + 1];
    }
    ch = IN_GETC( in );                             /* the one white space after ID */
    if ( ch == '\r' && in_peek( in ) == '\n' ) {
        in->cur++;
    }
    do_image( NULL, dict, in );
    /* on to EI, which stands alone */
    while ( (ch = IN_GETC( in )) >= 0 ) {
        if ( state == 2 ) {
            if ( IS_WS( ch ) || ch == 'Q' || ch == 'q' ) {
                if ( !IS_WS( ch ) ) {
                    in->cur--;
                }
                return;
            }
            state = 0;
        }
        if ( state == 1 ) {
            state = ch == 'I' ? 2 : 0;
        }
        if ( state == 0 && ch == 'E' ) {
            state = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the operators                                                       */
/* ------------------------------------------------------------------ */

#define IS( name )      (str_cmp( op, name ) == 0)
#define NUM( index )    obj_fx( &args[index] )
#define NEED( count )   if ( nargs < (count) ) { return; }

static void operate( LEX *lex, const char *op, OBJ *args, int nargs )
{
    RMAT mat;
    REAL rx, ry, rw, rh;
    OBJ *obj;
    fx x1, y1, x2, y2, x3, y3;
    int index;

    switch ( op[0] ) {
    case 'q':
        if ( op[1] == 0 ) {
            state_push();
        }
        return;
    case 'Q':
        if ( op[1] == 0 ) {
            state_pop();
        }
        return;
    case 'c':
        if ( op[1] == 0 ) {                         /* c: a curve */
            NEED( 6 );
            user_point( &args[0], &args[1], &x1, &y1 );
            user_point( &args[2], &args[3], &x2, &y2 );
            user_point( &args[4], &args[5], &x3, &y3 );
            path_curve( &path, x1, y1, x2, y2, x3, y3 );
        } else if ( IS( "cm" ) ) {
            NEED( 6 );
            matrix_from( &mat, args );
            rmat_mul( &gs->ctm, &mat, &gs->ctm );
            ctm_changed();
        } else if ( IS( "cs" ) ) {
            NEED( 1 );
            colour_device( 0, cs_load( &args[0], resources ), args, 0 );
        }
        return;
    case 'm':
        NEED( 2 );
        user_point( &args[0], &args[1], &x1, &y1 );
        path_move( &path, x1, y1 );
        return;
    case 'l':
        NEED( 2 );
        user_point( &args[0], &args[1], &x1, &y1 );
        path_line( &path, x1, y1 );
        return;
    case 'v':                                       /* the first control point is where we are */
        NEED( 4 );
        user_point( &args[0], &args[1], &x2, &y2 );
        user_point( &args[2], &args[3], &x3, &y3 );
        path_curve( &path, path.cur_x, path.cur_y, x2, y2, x3, y3 );
        return;
    case 'y':                                       /* the second is where it ends */
        NEED( 4 );
        user_point( &args[0], &args[1], &x1, &y1 );
        user_point( &args[2], &args[3], &x3, &y3 );
        path_curve( &path, x1, y1, x3, y3, x3, y3 );
        return;
    case 'h':
        path_close( &path );
        return;
    case 'r':
        if ( IS( "re" ) ) {
            NEED( 4 );
            rx = obj_real( &args[0] );
            ry = obj_real( &args[1] );
            rw = real_add( rx, obj_real( &args[2] ) );
            rh = real_add( ry, obj_real( &args[3] ) );
            rmat_point( &gs->ctm, rx, ry, &x1, &y1 );
            path_move( &path, x1, y1 );
            rmat_point( &gs->ctm, rw, ry, &x1, &y1 );
            path_line( &path, x1, y1 );
            rmat_point( &gs->ctm, rw, rh, &x1, &y1 );
            path_line( &path, x1, y1 );
            rmat_point( &gs->ctm, rx, rh, &x1, &y1 );
            path_line( &path, x1, y1 );
            path_close( &path );
        } else if ( IS( "rg" ) ) {
            colour_device( 0, &cs_devrgb, args, nargs );
        }
        return;
    case 'S':
        if ( op[1] == 0 ) {
            do_stroke();
            path_done();
        } else if ( IS( "SC" ) || IS( "SCN" ) ) {
            colour_set( 1, args, nargs );
        }
        return;
    case 's':
        if ( op[1] == 0 ) {
            path_close( &path );
            do_stroke();
            path_done();
        } else if ( IS( "sc" ) || IS( "scn" ) ) {
            colour_set( 0, args, nargs );
        } else if ( IS( "sh" ) ) {
            NEED( 1 );
            if ( !reading && args[0].type == T_NAME ) {
                SHADE *shade = shade_load( dict_get( dict_dict( resources, "Shading" ),
                                                     (const char *)args[0].u.str.ptr ),
                                           &gs->ctm, resources );
                if ( shade ) {
                    shade_fill( shade, canvas, &gs->clip, gs->fill.alpha, gs->fill.blend );
                }
            }
        }
        return;
    case 'f':
    case 'F':
        do_fill( op[1] == '*' );
        path_done();
        return;
    case 'B':
        if ( op[1] == 0 || IS( "B*" ) ) {
            do_fill( op[1] == '*' );
            do_stroke();
            path_done();
        } else if ( IS( "BT" ) ) {
            rmat_identity( &text_m );
            line_m = text_m;
            text_run = 0;
            text_stale = 1;
        } else if ( IS( "BI" ) ) {
            inline_image( lex );
        }
        return;
    case 'b':
        if ( op[1] == 0 || IS( "b*" ) ) {
            path_close( &path );
            do_fill( op[1] == '*' );
            do_stroke();
            path_done();
        }
        return;
    case 'n':
        path_done();
        return;
    case 'W':
        pending_clip = op[1] == '*' ? 2 : 1;
        return;
    case 'w':
        NEED( 1 );
        gs->line_width = NUM( 0 );
        return;
    case 'J':
        NEED( 1 );
        gs->cap = (int)obj_int( &args[0] );
        return;
    case 'j':
        NEED( 1 );
        gs->join = (int)obj_int( &args[0] );
        return;
    case 'M':
        if ( op[1] == 0 ) {
            NEED( 1 );
            gs->miter = NUM( 0 );
        }
        return;
    case 'd':
        if ( op[1] == 0 ) {
            NEED( 2 );
            gs->ndash = 0;
            if ( args[0].type == T_ARR ) {
                for ( index = 0; (u32)index < args[0].u.arr->count && index < 12; index++ ) {
                    gs->dash[index] = obj_fx( &args[0].u.arr->items[index] );
                }
                gs->ndash = index;
            }
            gs->dash_phase = NUM( 1 );
        }
        return;                                     /* d0 and d1 say nothing needed here */
    case 'g':
        if ( op[1] == 0 ) {
            colour_device( 0, &cs_devgray, args, nargs );
        } else if ( IS( "gs" ) ) {
            NEED( 1 );
            if ( args[0].type == T_NAME ) {
                ext_gstate( (const char *)args[0].u.str.ptr );
            }
        }
        return;
    case 'G':
        colour_device( 1, &cs_devgray, args, nargs );
        return;
    case 'R':
        if ( IS( "RG" ) ) {
            colour_device( 1, &cs_devrgb, args, nargs );
        }
        return;
    case 'k':
        colour_device( 0, &cs_devcmyk, args, nargs );
        return;
    case 'K':
        colour_device( 1, &cs_devcmyk, args, nargs );
        return;
    case 'C':
        if ( IS( "CS" ) ) {
            NEED( 1 );
            colour_device( 1, cs_load( &args[0], resources ), args, 0 );
        }
        return;
    case 'D':
        if ( IS( "Do" ) ) {
            NEED( 1 );
            if ( args[0].type == T_NAME ) {
                do_xobject( (const char *)args[0].u.str.ptr );
            }
        }
        return;
    case 'T':
        break;
    case '\'':
        NEED( 1 );
        text_move( real_int( 0 ), real_neg( real_fx( gs->leading ) ) );
        if ( args[0].type == T_STR ) {
            show( args[0].u.str.ptr, args[0].u.str.len );
        }
        return;
    case '"':
        NEED( 3 );
        gs->word_space = NUM( 0 );
        gs->char_space = NUM( 1 );
        text_move( real_int( 0 ), real_neg( real_fx( gs->leading ) ) );
        if ( args[2].type == T_STR ) {
            show( args[2].u.str.ptr, args[2].u.str.len );
        }
        return;
    default:
        return;
    }

    /* the text operators */
    switch ( op[1] ) {
    case 'j':
        NEED( 1 );
        if ( args[0].type == T_STR ) {
            show( args[0].u.str.ptr, args[0].u.str.len );
        }
        break;
    case 'J':
        NEED( 1 );
        if ( args[0].type == T_ARR ) {
            show_array( args[0].u.arr );
        }
        break;
    case 'f':
        NEED( 2 );
        if ( args[0].type == T_NAME ) {
            obj = dict_raw( dict_dict( resources, "Font" ), (const char *)args[0].u.str.ptr );
            gs->font = obj ? font_get( obj ) : NULL;
        }
        gs->size = NUM( 1 );
        text_stale = 1;
        break;
    case 'd':
    case 'D':
        NEED( 2 );
        if ( op[1] == 'D' ) {
            gs->leading = -NUM( 1 );
        }
        text_move( obj_real( &args[0] ), obj_real( &args[1] ) );
        break;
    case 'm':
        NEED( 6 );
        matrix_from( &text_m, args );
        line_m = text_m;
        text_run = 0;
        text_stale = 1;
        break;
    case '*':
        text_move( real_int( 0 ), real_neg( real_fx( gs->leading ) ) );
        break;
    case 'c':
        NEED( 1 );
        gs->char_space = NUM( 0 );
        break;
    case 'w':
        NEED( 1 );
        gs->word_space = NUM( 0 );
        break;
    case 'z':
        NEED( 1 );
        gs->hscale = NUM( 0 ) / 100;
        text_stale = 1;
        break;
    case 'L':
        NEED( 1 );
        gs->leading = NUM( 0 );
        break;
    case 'r':
        NEED( 1 );
        gs->render = (int)obj_int( &args[0] );
        break;
    case 's':
        NEED( 1 );
        gs->rise = NUM( 0 );
        text_stale = 1;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* running a stream                                                    */
/* ------------------------------------------------------------------ */

static void run( IN *in, DICT *res )
{
    LEX lex;
    OBJ args[MAX_ARGS], tok;
    DICT *keep_res = resources;
    POOL *pool;
    int nargs = 0, type, base = gs_depth, used = 0;

    if ( nest >= MAX_NEST ) {
        return;
    }
    if ( arg_pools[nest] == NULL ) {
        arg_pools[nest] = pool_new();
    }
    pool = arg_pools[nest];
    pool_reset( pool );
    nest++;
    resources = res;
    lex_init( &lex, in, pool, 1 );
    for ( ;; ) {
        type = lex_obj( &lex, &tok );
        if ( type == T_EOF ) {
            break;
        }
        if ( type != T_OP ) {
            if ( nargs == MAX_ARGS ) {
                nargs = 0;                          /* more than any operator takes */
            }
            args[nargs++] = tok;
            used |= type >= T_STR;
            continue;
        }
        operate( &lex, lex.word, args, nargs );
        nargs = 0;
        if ( used ) {
            pool_reset( pool );
            used = 0;
        }
        if ( (++op_tick & 127) == 0 && render_poll && render_poll() ) {
            pdf_fail( PE_ABORT );
        }
    }
    while ( gs_depth > base ) {
        state_pop();                                /* a q with no Q */
    }
    resources = keep_res;
    nest--;
}

static void run_stream( OBJ *stm, DICT *res )
{
    IN *in = pdf_stream( stm );

    if ( in ) {
        run( in, res );
        in_close( in );
    }
}

/* a page's /Contents may be several streams, to be read as one */
typedef struct {
    ARR *arr;
    u32  next;
    IN  *cur;
    u8   buf[512];
} CONCAT;

static int concat_fill( IN *in )
{
    CONCAT *st = IN_STATE( in, CONCAT );
    u32 got;

    for ( ;; ) {
        if ( st->cur ) {
            got = in_read( st->cur, st->buf, sizeof( st->buf ) );
            if ( got == 0 ) {
                in_close( st->cur );
                st->cur = NULL;
                st->buf[0] = '\n';                  /* a word does not run on into the next */
                got = 1;
            }
            in->cur = st->buf;
            in->end = st->buf + got;
            return 1;
        }
        if ( st->next >= st->arr->count ) {
            return 0;
        }
        st->cur = pdf_stream( arr_get( st->arr, st->next++ ) );
    }
}

static void concat_done( IN *in )
{
    CONCAT *st = IN_STATE( in, CONCAT );

    if ( st->cur ) {
        in_close( st->cur );
    }
}

/* ------------------------------------------------------------------ */
/* a page                                                              */
/* ------------------------------------------------------------------ */

void view_setup( VIEW *view, PAGE *page, fx zoom, int turn )
{
    REAL zz = real_fx( zoom ), nz = real_neg( zz ), zero = real_int( 0 );
    REAL *box = page->box;
    fx wide = real_to_fx( box[2] ) - real_to_fx( box[0] );
    fx high = real_to_fx( box[3] ) - real_to_fx( box[1] );
    fx swap;

    view->zoom = zoom;
    view->rotate = ((page->rotate + turn) % 360 + 360) % 360;
    switch ( view->rotate ) {
    case 90:
        rmat_set( &view->base, zero, zz, zz, zero, real_mul( real_neg( box[1] ), zz ),
                  real_mul( real_neg( box[0] ), zz ) );
        break;
    case 180:
        rmat_set( &view->base, nz, zero, zero, zz, real_mul( box[2], zz ),
                  real_mul( real_neg( box[1] ), zz ) );
        break;
    case 270:
        rmat_set( &view->base, zero, nz, nz, zero, real_mul( box[3], zz ), real_mul( box[2], zz ) );
        break;
    default:
        rmat_set( &view->base, zz, zero, zero, nz, real_mul( real_neg( box[0] ), zz ),
                  real_mul( box[3], zz ) );
        break;
    }
    if ( view->rotate == 90 || view->rotate == 270 ) {
        swap = wide;
        wide = high;
        high = swap;
    }
    view->width = (int)FX_CEIL( fx_mul( wide, zoom ) );
    view->height = (int)FX_CEIL( fx_mul( high, zoom ) );
    if ( view->width < 1 ) {
        view->width = 1;
    }
    if ( view->height < 1 ) {
        view->height = 1;
    }
}

static void page_run( PAGE *page, VIEW *view, CANVAS *cv, int text_only )
{
    OBJ *contents;
    IN *in;
    CONCAT *st;
    RMAT shift;

    canvas = cv;
    reading = text_only;
    if ( states == NULL ) {
        states = (GSTATE *)xalloc( GS_MAX * sizeof( GSTATE ) );
    }
    gs_depth = 0;
    nest = 0;
    op_tick = 0;
    gs = &states[0];
    mem_set( gs, 0, sizeof( *gs ) );
    gs->ctm = view->base;
    if ( cv ) {
        rmat_identity( &shift );
        shift.m[4] = real_int( -cv->org_x );
        shift.m[5] = real_int( -cv->org_y );
        rmat_mul( &gs->ctm, &gs->ctm, &shift );
        gs->clip.x1 = cv->width;
        gs->clip.y1 = cv->height;
    } else {
        gs->clip.x1 = gs->clip.y1 = 32000;
    }
    ctm_changed();
    pattern_base = gs->ctm;
    gs->fill_cs = gs->stroke_cs = &cs_devgray;
    gs->fill.alpha = gs->stroke.alpha = 255;
    gs->group_alpha = 255;
    gs->line_width = FX_ONE;
    gs->miter = 10 * FX_ONE;
    gs->hscale = FX_ONE;
    rmat_identity( &text_m );
    line_m = text_m;
    text_run = 0;
    path_reset( &path );
    pending_clip = 0;
    font_stamp();
    glyph_page_begin();

    contents = dict_get( page->dict, "Contents" );
    if ( contents->type == T_ARR ) {
        in = in_new( sizeof( CONCAT ), concat_fill, NULL );
        in->done = concat_done;
        st = IN_STATE( in, CONCAT );
        st->arr = contents->u.arr;
    } else {
        in = pdf_stream( contents );
    }
    if ( in ) {
        run( in, page->res );
        in_close( in );
    }
}

void page_render( PAGE *page, VIEW *view, CANVAS *cv )
{
    page_run( page, view, cv, 0 );
}

void page_text( PAGE *page, VIEW *view )
{
    page_run( page, view, NULL, 1 );
}
