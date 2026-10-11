/*
 * COLOR.C - colour spaces and the functions that go with them.
 *
 * Every colour ends as red, green and blue, eight bits each, because
 * that is what a screen is.  The device spaces and the calibrated ones
 * that stand for them (CalGray, CalRGB, ICCBased by how many
 * components it has) are taken at their word; Lab is converted; an
 * Indexed space is turned into a table of finished colours when it is
 * loaded, so an image that uses it costs a lookup a pixel; Separation
 * and DeviceN run their tint transform into the alternate space.
 *
 * A FUNCTION is one of PDF's four kinds - sampled, exponential,
 * stitching, or a small PostScript program - or an array of them.
 * They are worked in 16.16, which is ample for a colour.
 */
#include "gfx.h"

CSPACE cs_devgray = { CS_GRAY, 1 };
CSPACE cs_devrgb  = { CS_RGB, 3 };
CSPACE cs_devcmyk = { CS_CMYK, 4 };
CSPACE cs_pattern = { CS_PATTERN, 0 };

static int unit255( fx val )
{
    if ( val <= 0 ) {
        return 0;
    }
    if ( val >= FX_ONE ) {
        return 255;
    }
    return (int)((val * 255 + 0x8000) >> 16);
}

/* ------------------------------------------------------------------ */
/* functions                                                           */
/* ------------------------------------------------------------------ */

#define FN_SAMPLED  0
#define FN_EXPO     2
#define FN_STITCH   3
#define FN_CALC     4
#define FN_ARRAY    9           /* one function an output */

#define MAX_IN      4
#define CALC_STACK  100

/* the calculator's instructions */
enum {
    OP_NUM, OP_JZ, OP_JMP,
    OP_ABS, OP_ADD, OP_ATAN, OP_CEILING, OP_COS, OP_CVI, OP_CVR, OP_DIV, OP_EXP, OP_FLOOR,
    OP_IDIV, OP_LN, OP_LOG, OP_MOD, OP_MUL, OP_NEG, OP_ROUND, OP_SIN, OP_SQRT, OP_SUB,
    OP_TRUNCATE, OP_AND, OP_BITSHIFT, OP_EQ, OP_FALSE, OP_GE, OP_GT, OP_LE, OP_LT, OP_NE,
    OP_NOT, OP_OR, OP_TRUE, OP_XOR, OP_COPY, OP_DUP, OP_EXCH, OP_INDEX, OP_POP, OP_ROLL,
    OP_COUNT
};

static const char *const calc_names[OP_COUNT] = {
    "", "", "",
    "abs", "add", "atan", "ceiling", "cos", "cvi", "cvr", "div", "exp", "floor",
    "idiv", "ln", "log", "mod", "mul", "neg", "round", "sin", "sqrt", "sub",
    "truncate", "and", "bitshift", "eq", "false", "ge", "gt", "le", "lt", "ne",
    "not", "or", "true", "xor", "copy", "dup", "exch", "index", "pop", "roll"
};

typedef struct {
    u8  op;
    fx  val;                    /* OP_NUM: the number; a jump: where to */
} CALC;

struct FUNC {
    u8    type;
    u8    nin, nout;
    u8    has_range;
    fx    domain[MAX_IN * 2];
    fx    range[MAX_COMPS * 2];
    /* sampled */
    u8   *samples;
    u32   sample_len;
    int   bps;
    s32   size[MAX_IN];
    fx    encode[MAX_IN * 2];
    fx    decode[MAX_COMPS * 2];
    /* exponential */
    fx    c0[MAX_COMPS], c1[MAX_COMPS];
    fx    expo;
    /* stitching, and an array */
    int    count;
    FUNC **parts;
    fx    *bounds;
    fx    *part_encode;
    /* calculator */
    CALC  *code;
    int    code_len;
};

static void fx_array( DICT *dict, const char *key, fx *out, int max, int *got )
{
    ARR *arr = dict_arr( dict, key );
    int index;

    *got = 0;
    if ( arr == NULL ) {
        return;
    }
    for ( index = 0; index < max && (u32)index < arr->count; index++ ) {
        out[index] = obj_fx( arr_get( arr, (u32)index ) );
    }
    *got = index;
}

static int calc_compile( LEX *lex, CALC *code, int at, int max )
{
    OBJ tok;
    int type, op, jz, jmp;

    for ( ;; ) {
        type = lex_obj( lex, &tok );
        if ( type == T_EOF || at >= max - 4 ) {
            return at;
        }
        if ( type == T_INT || type == T_REAL ) {
            code[at].op = OP_NUM;
            code[at++].val = obj_fx( &tok );
            continue;
        }
        if ( type == T_BOOL ) {
            code[at++].op = (u8)(tok.u.ival ? OP_TRUE : OP_FALSE);
            continue;
        }
        if ( type != T_OP ) {
            continue;
        }
        if ( lex->word[0] == '}' ) {
            return at;
        }
        if ( lex->word[0] == '{' ) {
            /* { then } if     or     { then } { else } ifelse */
            jz = at++;
            code[jz].op = OP_JZ;
            at = calc_compile( lex, code, at, max );
            type = lex_obj( lex, &tok );
            if ( type == T_OP && lex->word[0] == '{' ) {
                jmp = at++;
                code[jmp].op = OP_JMP;
                code[jz].val = at;
                at = calc_compile( lex, code, at, max );
                lex_obj( lex, &tok );           /* ifelse */
                code[jmp].val = at;
            } else {
                code[jz].val = at;              /* if */
            }
            continue;
        }
        for ( op = OP_ABS; op < OP_COUNT; op++ ) {
            if ( str_cmp( lex->word, calc_names[op] ) == 0 ) {
                code[at++].op = (u8)op;
                break;
            }
        }
    }
}

FUNC *func_load( OBJ *obj )
{
    static int depth;
    FUNC *fn;
    DICT *dict;
    ARR *arr;
    IN *in;
    LEX lex;
    OBJ tok;
    u32 total;
    int index, got, type;

    obj = obj_resolve( obj );
    if ( depth > 8 ) {
        return NULL;
    }
    fn = (FUNC *)pool_alloc( doc.page_pool, sizeof( FUNC ) );
    if ( obj->type == T_ARR ) {
        arr = obj->u.arr;
        fn->type = FN_ARRAY;
        fn->nin = 1;
        fn->count = arr->count > MAX_COMPS ? MAX_COMPS : (int)arr->count;
        fn->nout = (u8)fn->count;
        fn->parts = (FUNC **)pool_alloc( doc.page_pool, (u32)fn->count * sizeof( FUNC * ) );
        depth++;
        for ( index = 0; index < fn->count; index++ ) {
            fn->parts[index] = func_load( &arr->items[index] );
        }
        depth--;
        for ( index = 0; index < fn->count; index++ ) {
            if ( fn->parts[index] == NULL ) {
                return NULL;
            }
        }
        return fn;
    }
    dict = obj_dict( obj );
    if ( dict == NULL ) {
        return NULL;
    }
    type = (int)dict_int( dict, "FunctionType", -1 );
    fn->type = (u8)type;
    fx_array( dict, "Domain", fn->domain, MAX_IN * 2, &got );
    fn->nin = (u8)(got / 2);
    fx_array( dict, "Range", fn->range, MAX_COMPS * 2, &got );
    fn->has_range = (u8)(got >= 2);
    fn->nout = (u8)(got / 2);
    if ( fn->nin == 0 ) {
        return NULL;
    }

    switch ( type ) {
    case FN_SAMPLED:
        if ( obj->type != T_STREAM || fn->nout == 0 ) {
            return NULL;
        }
        fn->bps = (int)dict_int( dict, "BitsPerSample", 8 );
        arr = dict_arr( dict, "Size" );
        total = fn->nout;
        for ( index = 0; index < fn->nin; index++ ) {
            fn->size[index] = obj_int( arr_get( arr, (u32)index ) );
            if ( fn->size[index] < 1 || fn->size[index] > 65536L ) {
                return NULL;
            }
            total *= (u32)fn->size[index];
            if ( total > 4UL * 1024 * 1024 ) {
                return NULL;
            }
            fn->encode[index * 2] = 0;
            fn->encode[index * 2 + 1] = fn->size[index] > 32767 ? FX_MAX : I2FX( fn->size[index] - 1 );
        }
        fx_array( dict, "Encode", fn->encode, fn->nin * 2, &got );
        mem_cpy( fn->decode, fn->range, sizeof( fn->decode ) );
        fx_array( dict, "Decode", fn->decode, fn->nout * 2, &got );
        fn->samples = pdf_stream_all( obj, &fn->sample_len, total * 4 );
        if ( fn->samples == NULL || fn->sample_len < (total * (u32)fn->bps + 7) / 8 ) {
            return NULL;
        }
        break;

    case FN_EXPO:
        fn->c0[0] = 0;
        fn->c1[0] = FX_ONE;
        fx_array( dict, "C0", fn->c0, MAX_COMPS, &got );
        index = got ? got : 1;
        fx_array( dict, "C1", fn->c1, MAX_COMPS, &got );
        fn->nout = (u8)index;
        fn->expo = dict_fx( dict, "N", FX_ONE );
        break;

    case FN_STITCH:
        arr = dict_arr( dict, "Functions" );
        if ( arr == NULL || arr->count == 0 || arr->count > 256 ) {
            return NULL;
        }
        fn->count = (int)arr->count;
        fn->parts = (FUNC **)pool_alloc( doc.page_pool, (u32)fn->count * sizeof( FUNC * ) );
        fn->bounds = (fx *)pool_alloc( doc.page_pool, (u32)(fn->count + 1) * sizeof( fx ) );
        fn->part_encode = (fx *)pool_alloc( doc.page_pool, (u32)fn->count * 2 * sizeof( fx ) );
        depth++;
        for ( index = 0; index < fn->count; index++ ) {
            fn->parts[index] = func_load( &arr->items[index] );
            fn->part_encode[index * 2 + 1] = FX_ONE;
        }
        depth--;
        for ( index = 0; index < fn->count; index++ ) {
            if ( fn->parts[index] == NULL ) {
                return NULL;
            }
        }
        fx_array( dict, "Bounds", fn->bounds, fn->count - 1, &got );
        fx_array( dict, "Encode", fn->part_encode, fn->count * 2, &got );
        fn->nout = fn->parts[0]->nout;
        break;

    case FN_CALC:
        if ( obj->type != T_STREAM || fn->nout == 0 ) {
            return NULL;
        }
        in = pdf_stream( obj );
        if ( in == NULL ) {
            return NULL;
        }
        fn->code = (CALC *)pool_alloc( doc.page_pool, 1024 * sizeof( CALC ) );
        lex_init( &lex, in, doc.page_pool, 1 );
        if ( lex_obj( &lex, &tok ) == T_OP && lex.word[0] == '{' ) {
            fn->code_len = calc_compile( &lex, fn->code, 0, 1024 );
        }
        in_close( in );
        break;

    default:
        return NULL;
    }
    return fn;
}

int func_outputs( FUNC *fn )
{
    return fn ? fn->nout : 0;
}

static u32 sample_at( const FUNC *fn, u32 index )
{
    u32 bit = index * (u32)fn->bps, val = 0;
    int left = fn->bps;

    if ( fn->bps == 8 ) {
        return fn->samples[index];
    }
    if ( fn->bps == 16 ) {
        return ((u32)fn->samples[index * 2] << 8) | fn->samples[index * 2 + 1];
    }
    while ( left-- ) {
        val = (val << 1) | ((fn->samples[bit >> 3] >> (7 - (bit & 7))) & 1);
        bit++;
    }
    return val;
}

static fx interp( fx val, fx lo0, fx hi0, fx lo1, fx hi1 )
{
    if ( hi0 == lo0 ) {
        return lo1;
    }
    return lo1 + mul_div( val - lo0, hi1 - lo1, hi0 - lo0 );
}

static void eval_sampled( const FUNC *fn, const fx *in, fx *out )
{
    u32 base = 0, stride = 1, peak = fn->bps >= 31 ? 0x7FFFFFFFUL : (1UL << fn->bps) - 1;
    u32 low, high;
    fx pos, frac = 0, cell;
    s32 whole;
    int index, comp;

    /* the first input is interpolated along; the others pick the
       nearest sample */
    for ( index = 0; index < fn->nin; index++ ) {
        pos = interp( in[index], fn->domain[index * 2], fn->domain[index * 2 + 1],
                      fn->encode[index * 2], fn->encode[index * 2 + 1] );
        if ( pos < 0 ) {
            pos = 0;
        }
        if ( fn->size[index] <= 32767 && pos > I2FX( fn->size[index] - 1 ) ) {
            pos = I2FX( fn->size[index] - 1 );
        }
        if ( index == 0 ) {
            whole = FX_FLOOR( pos );
            frac = FX_FRAC( pos );
        } else {
            whole = FX_ROUND( pos );
        }
        base += (u32)whole * stride;
        stride *= (u32)fn->size[index];
    }
    for ( comp = 0; comp < fn->nout; comp++ ) {
        low = sample_at( fn, base * fn->nout + (u32)comp );
        cell = (fx)(((low >> (fn->bps > 16 ? fn->bps - 16 : 0)) << 16) /
                    (fn->bps > 16 ? 65535UL : peak));
        if ( frac && (s32)(base % (u32)fn->size[0]) < fn->size[0] - 1 ) {
            high = sample_at( fn, (base + 1) * fn->nout + (u32)comp );
            cell += fx_mul( (fx)(((high >> (fn->bps > 16 ? fn->bps - 16 : 0)) << 16) /
                                 (fn->bps > 16 ? 65535UL : peak)) - cell, frac );
        }
        out[comp] = fn->decode[comp * 2] + fx_mul( cell, fn->decode[comp * 2 + 1] - fn->decode[comp * 2] );
    }
}

static void eval_calc( const FUNC *fn, const fx *in, fx *out )
{
    fx stack[CALC_STACK], lhs, rhs, hold;
    int top = 0, pc, count, shift, index, at;

    for ( index = 0; index < fn->nin; index++ ) {
        stack[top++] = in[index];
    }
    for ( pc = 0; pc < fn->code_len; pc++ ) {
        if ( top < 0 || top > CALC_STACK - 4 ) {
            break;
        }
        lhs = top >= 2 ? stack[top - 2] : 0;
        rhs = top >= 1 ? stack[top - 1] : 0;
        switch ( fn->code[pc].op ) {
        case OP_NUM:      stack[top++] = fn->code[pc].val; break;
        case OP_JZ:
            top--;
            if ( rhs == 0 ) {
                pc = (int)fn->code[pc].val - 1;
            }
            break;
        case OP_JMP:      pc = (int)fn->code[pc].val - 1; break;
        case OP_ABS:      stack[top - 1] = rhs < 0 ? -rhs : rhs; break;
        case OP_ADD:      stack[--top - 1] = lhs + rhs; break;
        case OP_SUB:      stack[--top - 1] = lhs - rhs; break;
        case OP_MUL:      stack[--top - 1] = fx_mul( lhs, rhs ); break;
        case OP_DIV:      stack[--top - 1] = rhs ? fx_div( lhs, rhs ) : 0; break;
        case OP_IDIV:     stack[--top - 1] = FX_FLOOR( rhs ) ? I2FX( FX_FLOOR( lhs ) / FX_FLOOR( rhs ) ) : 0; break;
        case OP_MOD:      stack[--top - 1] = FX_FLOOR( rhs ) ? I2FX( FX_FLOOR( lhs ) % FX_FLOOR( rhs ) ) : 0; break;
        case OP_NEG:      stack[top - 1] = -rhs; break;
        case OP_CEILING:  stack[top - 1] = (rhs + 0xFFFF) & ~0xFFFFL; break;
        case OP_FLOOR:    stack[top - 1] = rhs & ~0xFFFFL; break;
        case OP_ROUND:    stack[top - 1] = (rhs + 0x8000) & ~0xFFFFL; break;
        case OP_TRUNCATE:
        case OP_CVI:      stack[top - 1] = rhs < 0 ? -((-rhs) & ~0xFFFFL) : rhs & ~0xFFFFL; break;
        case OP_CVR:      break;
        case OP_SQRT:     stack[top - 1] = fx_sqrt( rhs ); break;
        case OP_SIN:
        case OP_COS:
            /* between whole degrees, in a straight line */
            index = (int)FX_FLOOR( rhs );
            hold = fn->code[pc].op == OP_SIN ? fx_sin( index ) : fx_cos( index );
            stack[top - 1] = hold + fx_mul( (fn->code[pc].op == OP_SIN ? fx_sin( index + 1 )
                                                                       : fx_cos( index + 1 )) - hold,
                                            FX_FRAC( rhs ) );
            break;
        case OP_ATAN:     stack[--top - 1] = fx_atan2( lhs, rhs ); break;
        case OP_EXP:      stack[--top - 1] = fx_pow( lhs, rhs ); break;
        case OP_LN:       stack[top - 1] = fx_mul( fx_log2( rhs ), 45426L ); break;
        case OP_LOG:      stack[top - 1] = fx_mul( fx_log2( rhs ), 19728L ); break;
        case OP_EQ:       stack[--top - 1] = lhs == rhs ? FX_ONE : 0; break;
        case OP_NE:       stack[--top - 1] = lhs != rhs ? FX_ONE : 0; break;
        case OP_GT:       stack[--top - 1] = lhs > rhs ? FX_ONE : 0; break;
        case OP_GE:       stack[--top - 1] = lhs >= rhs ? FX_ONE : 0; break;
        case OP_LT:       stack[--top - 1] = lhs < rhs ? FX_ONE : 0; break;
        case OP_LE:       stack[--top - 1] = lhs <= rhs ? FX_ONE : 0; break;
        case OP_AND:      stack[--top - 1] = lhs & rhs; break;
        case OP_OR:       stack[--top - 1] = lhs | rhs; break;
        case OP_XOR:      stack[--top - 1] = lhs ^ rhs; break;
        case OP_NOT:      stack[top - 1] = rhs == FX_ONE ? 0 : (rhs == 0 ? FX_ONE : ~rhs & ~0xFFFFL); break;
        case OP_BITSHIFT:
            shift = (int)FX_FLOOR( rhs );
            top--;
            if ( shift > 15 || shift < -15 ) {
                stack[top - 1] = 0;
            } else {
                stack[top - 1] = shift >= 0 ? I2FX( FX_FLOOR( lhs ) << shift )
                                            : I2FX( FX_FLOOR( lhs ) >> -shift );
            }
            break;
        case OP_TRUE:     stack[top++] = FX_ONE; break;
        case OP_FALSE:    stack[top++] = 0; break;
        case OP_POP:      top--; break;
        case OP_DUP:      stack[top] = rhs; top++; break;
        case OP_EXCH:
            if ( top >= 2 ) {
                stack[top - 2] = rhs;
                stack[top - 1] = lhs;
            }
            break;
        case OP_COPY:
            count = (int)FX_FLOOR( rhs );
            top--;
            if ( count > 0 && count <= top && top + count < CALC_STACK ) {
                mem_cpy( stack + top, stack + top - count, (u32)count * sizeof( fx ) );
                top += count;
            }
            break;
        case OP_INDEX:
            count = (int)FX_FLOOR( rhs );
            if ( count >= 0 && count < top - 1 ) {
                stack[top - 1] = stack[top - 2 - count];
            }
            break;
        case OP_ROLL:
            /* n j roll: the top n, turned j places towards the top */
            shift = (int)FX_FLOOR( rhs );
            count = (int)FX_FLOOR( lhs );
            top -= 2;
            if ( count > 0 && count <= top ) {
                shift %= count;
                if ( shift < 0 ) {
                    shift += count;
                }
                for ( ; shift > 0; shift-- ) {
                    hold = stack[top - 1];
                    for ( at = top - 1; at > top - count; at-- ) {
                        stack[at] = stack[at - 1];
                    }
                    stack[top - count] = hold;
                }
            }
            break;
        }
    }
    for ( index = 0; index < fn->nout; index++ ) {
        at = top - fn->nout + index;
        out[index] = at >= 0 && at < CALC_STACK ? stack[at] : 0;
    }
}

void func_eval( FUNC *fn, const fx *in, fx *out )
{
    fx arg[MAX_IN], val, lo, hi;
    int index, part;

    for ( index = 0; index < fn->nin && index < MAX_IN; index++ ) {
        arg[index] = in[index];
        if ( arg[index] < fn->domain[index * 2] ) {
            arg[index] = fn->domain[index * 2];
        }
        if ( arg[index] > fn->domain[index * 2 + 1] ) {
            arg[index] = fn->domain[index * 2 + 1];
        }
    }
    switch ( fn->type ) {
    case FN_SAMPLED:
        eval_sampled( fn, arg, out );
        break;
    case FN_EXPO:
        val = fx_pow( arg[0], fn->expo );
        for ( index = 0; index < fn->nout; index++ ) {
            out[index] = fn->c0[index] + fx_mul( val, fn->c1[index] - fn->c0[index] );
        }
        break;
    case FN_STITCH:
        for ( part = 0; part < fn->count - 1 && arg[0] >= fn->bounds[part]; part++ ) {
        }
        lo = part == 0 ? fn->domain[0] : fn->bounds[part - 1];
        hi = part == fn->count - 1 ? fn->domain[1] : fn->bounds[part];
        val = interp( arg[0], lo, hi, fn->part_encode[part * 2], fn->part_encode[part * 2 + 1] );
        func_eval( fn->parts[part], &val, out );
        return;                         /* the part has clipped it to its own range */
    case FN_CALC:
        eval_calc( fn, arg, out );
        break;
    case FN_ARRAY:
        for ( index = 0; index < fn->count; index++ ) {
            func_eval( fn->parts[index], arg, &val );
            out[index] = val;
        }
        return;
    }
    if ( fn->has_range ) {
        for ( index = 0; index < fn->nout; index++ ) {
            if ( out[index] < fn->range[index * 2] ) {
                out[index] = fn->range[index * 2];
            }
            if ( out[index] > fn->range[index * 2 + 1] ) {
                out[index] = fn->range[index * 2 + 1];
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* colour spaces                                                       */
/* ------------------------------------------------------------------ */

static u32 pack_rgb( int red, int green, int blue )
{
    return ((u32)red << 16) | ((u32)green << 8) | (u32)blue;
}

static u32 cmyk_rgb( fx cyan, fx magenta, fx yellow, fx black )
{
    int keep = 255 - unit255( black );

    return pack_rgb( (255 - unit255( cyan )) * keep / 255, (255 - unit255( magenta )) * keep / 255,
                     (255 - unit255( yellow )) * keep / 255 );
}

/* the cube of f, or the straight piece of the Lab curve below it */
static fx lab_inv( fx val )
{
    if ( val > 13563L ) {                           /* 6/29 */
        return fx_mul( fx_mul( val, val ), val );
    }
    return fx_mul( 8418L, val - 9040L );            /* 3 (6/29)^2 (f - 4/29) */
}

static int gamma255( fx lin )
{
    if ( lin <= 0 ) {
        return 0;
    }
    if ( lin >= FX_ONE ) {
        return 255;
    }
    return unit255( fx_pow( lin, 29789L ) );        /* 1 / 2.2 */
}

static u32 lab_rgb( fx light, fx aa, fx bb )
{
    fx fy = (light + 16 * FX_ONE) / 116, fxx = fy + aa / 500, fz = fy - bb / 200;
    fx xx = fx_mul( 63190L, lab_inv( fxx ) );       /* the D50 white */
    fx yy = lab_inv( fy );
    fx zz = fx_mul( 54061L, lab_inv( fz ) );

    return pack_rgb( gamma255( fx_mul( 205383L, xx ) - fx_mul( 105965L, yy ) - fx_mul( 32152L, zz ) ),
                     gamma255( fx_mul( -64147L, xx ) + fx_mul( 125573L, yy ) + fx_mul( 2195L, zz ) ),
                     gamma255( fx_mul( 4712L, xx ) - fx_mul( 15008L, yy ) + fx_mul( 92091L, zz ) ) );
}

u32 cs_rgb( CSPACE *cs, const fx *comps )
{
    fx alt[MAX_COMPS];
    s32 index;
    int grey;

    switch ( cs->kind ) {
    case CS_GRAY:
        grey = unit255( comps[0] );
        return pack_rgb( grey, grey, grey );
    case CS_RGB:
        return pack_rgb( unit255( comps[0] ), unit255( comps[1] ), unit255( comps[2] ) );
    case CS_CMYK:
        return cmyk_rgb( comps[0], comps[1], comps[2], comps[3] );
    case CS_LAB:
        return lab_rgb( comps[0], comps[1], comps[2] );
    case CS_INDEXED:
        index = FX_ROUND( comps[0] );
        if ( index < 0 ) {
            index = 0;
        }
        if ( index > cs->hival ) {
            index = cs->hival;
        }
        return cs->table ? cs->table[index] : 0;
    case CS_SEP:
        if ( cs->tint && cs->base ) {
            mem_set( alt, 0, sizeof( alt ) );
            func_eval( cs->tint, comps, alt );
            return cs_rgb( cs->base, alt );
        }
        grey = 255 - unit255( comps[0] );
        return pack_rgb( grey, grey, grey );
    }
    return 0;
}

void cs_initial( CSPACE *cs, fx *comps )
{
    int index;

    mem_set( comps, 0, MAX_COMPS * sizeof( fx ) );
    if ( cs->kind == CS_CMYK ) {
        comps[3] = FX_ONE;
    } else if ( cs->kind == CS_SEP ) {
        for ( index = 0; index < cs->ncomp && index < MAX_COMPS; index++ ) {
            comps[index] = FX_ONE;
        }
    }
}

/* an Indexed space's table, as finished colours */
static void index_table( CSPACE *cs, const u8 *data, u32 len )
{
    CSPACE *base = cs->base;
    fx comps[MAX_COMPS];
    u32 need = (u32)base->ncomp;
    int entry, comp;

    cs->table = (u32 *)pool_alloc( doc.page_pool, (u32)(cs->hival + 1) * sizeof( u32 ) );
    for ( entry = 0; entry <= cs->hival; entry++ ) {
        if ( ((u32)entry + 1) * need > len ) {
            break;
        }
        for ( comp = 0; comp < base->ncomp; comp++ ) {
            comps[comp] = (fx)(((u32)data[(u32)entry * need + (u32)comp] << 16) / 255);
        }
        if ( base->kind == CS_LAB ) {
            comps[0] *= 100;
            comps[1] = base->range[0] + fx_mul( comps[1], base->range[1] - base->range[0] );
            comps[2] = base->range[2] + fx_mul( comps[2], base->range[3] - base->range[2] );
        }
        cs->table[entry] = cs_rgb( base, comps );
    }
}

static CSPACE *by_count( int count )
{
    return count == 1 ? &cs_devgray : (count == 4 ? &cs_devcmyk : &cs_devrgb);
}

CSPACE *cs_load( OBJ *obj, DICT *res )
{
    static int depth;
    CSPACE *cs, *found = NULL;
    ARR *arr, *names;
    OBJ *item, *named;
    const char *name;
    u8 *data;
    u32 len, index;
    int got, all_none;

    obj = obj_resolve( obj );
    if ( obj->type == T_NAME ) {
        name = (const char *)obj->u.str.ptr;
        if ( str_cmp( name, "DeviceGray" ) == 0 || str_cmp( name, "G" ) == 0 ||
             str_cmp( name, "CalGray" ) == 0 ) {
            return &cs_devgray;
        }
        if ( str_cmp( name, "DeviceRGB" ) == 0 || str_cmp( name, "RGB" ) == 0 ||
             str_cmp( name, "CalRGB" ) == 0 ) {
            return &cs_devrgb;
        }
        if ( str_cmp( name, "DeviceCMYK" ) == 0 || str_cmp( name, "CMYK" ) == 0 ) {
            return &cs_devcmyk;
        }
        if ( str_cmp( name, "Pattern" ) == 0 ) {
            return &cs_pattern;
        }
        named = dict_get( dict_dict( res, "ColorSpace" ), name );
        if ( named->type == T_NULL || depth > 6 ) {
            return &cs_devgray;
        }
        depth++;
        found = cs_load( named, NULL );
        depth--;
        return found;
    }
    if ( obj->type != T_ARR || obj->u.arr->count == 0 || depth > 6 ) {
        return &cs_devgray;
    }
    arr = obj->u.arr;
    name = obj_name( arr_get( arr, 0 ) );
    if ( arr->count == 1 ) {
        depth++;
        found = cs_load( arr_get( arr, 0 ), res );
        depth--;
        return found;
    }
    if ( str_cmp( name, "CalGray" ) == 0 ) {
        return &cs_devgray;
    }
    if ( str_cmp( name, "CalRGB" ) == 0 ) {
        return &cs_devrgb;
    }
    if ( str_cmp( name, "ICCBased" ) == 0 ) {
        item = arr_get( arr, 1 );
        return by_count( (int)dict_int( obj_dict( item ), "N", 3 ) );
    }
    cs = (CSPACE *)pool_alloc( doc.page_pool, sizeof( CSPACE ) );
    depth++;
    if ( str_cmp( name, "Lab" ) == 0 ) {
        cs->kind = CS_LAB;
        cs->ncomp = 3;
        cs->range[0] = cs->range[2] = -100 * FX_ONE;
        cs->range[1] = cs->range[3] = 100 * FX_ONE;
        fx_array( obj_dict( arr_get( arr, 1 ) ), "Range", cs->range, 4, &got );
    } else if ( str_cmp( name, "Indexed" ) == 0 || str_cmp( name, "I" ) == 0 ) {
        cs->kind = CS_INDEXED;
        cs->ncomp = 1;
        cs->base = cs_load( arr_get( arr, 1 ), res );
        cs->hival = (int)obj_int( arr_get( arr, 2 ) );
        if ( cs->hival < 0 ) {
            cs->hival = 0;
        }
        if ( cs->hival > 255 ) {
            cs->hival = 255;
        }
        item = arr_get( arr, 3 );
        if ( cs->base->kind == CS_INDEXED || cs->base->kind == CS_PATTERN ) {
            cs->base = &cs_devgray;
        }
        if ( item->type == T_STR ) {
            index_table( cs, item->u.str.ptr, item->u.str.len );
        } else if ( item->type == T_STREAM ) {
            data = pdf_stream_all( item, &len, 256 * MAX_COMPS );
            if ( data ) {
                index_table( cs, data, len );
                pool_unbig( data );
            }
        }
    } else if ( str_cmp( name, "Separation" ) == 0 || str_cmp( name, "DeviceN" ) == 0 ) {
        cs->kind = CS_SEP;
        cs->ncomp = 1;
        item = arr_get( arr, 1 );
        if ( item->type == T_ARR ) {
            names = item->u.arr;
            cs->ncomp = (u8)(names->count > MAX_COMPS ? MAX_COMPS : names->count);
            all_none = names->count != 0;
            for ( index = 0; index < names->count; index++ ) {
                if ( !obj_is_name( arr_get( names, index ), "None" ) ) {
                    all_none = 0;
                }
            }
            cs->none = (u8)all_none;
        } else {
            cs->none = (u8)obj_is_name( item, "None" );
        }
        cs->base = cs_load( arr_get( arr, 2 ), res );
        cs->tint = func_load( arr_get( arr, 3 ) );
        if ( cs->base->kind == CS_SEP || cs->base->kind == CS_PATTERN ||
             (cs->tint && func_outputs( cs->tint ) < cs->base->ncomp) ) {
            cs->tint = NULL;
        }
    } else if ( str_cmp( name, "Pattern" ) == 0 ) {
        cs->kind = CS_PATTERN;
        cs->base = cs_load( arr_get( arr, 1 ), res );
        cs->ncomp = cs->base->ncomp;
    } else {
        depth--;
        return cs_load( arr_get( arr, 0 ), res );       /* [ /DeviceRGB ] and the like */
    }
    depth--;
    return cs;
}
