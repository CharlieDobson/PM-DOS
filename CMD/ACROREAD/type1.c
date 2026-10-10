/*
 * TYPE1.C - a Type 1 font program: PostScript's own font format, as a
 * PDF file embeds it (/FontFile).
 *
 * The program is PostScript text, most of it enciphered: a readable
 * part that names the font and gives its encoding, then "eexec" and
 * the part that holds the outlines, which is deciphered here and read
 * for the two tables in it - the subroutines and the charstrings, each
 * of which is enciphered again.  Nothing is interpreted as PostScript:
 * every Type 1 font is written to the same pattern, and the pattern is
 * what is read.
 *
 * A charstring is the Type 1 kind: the ancestor of CFF.C's, with its
 * own way of doing flex (through "other subroutines", which are
 * PostScript procedures an interpreter is expected to know by number)
 * and accented letters built from two glyphs (seac).
 */
#include "gfx.h"

typedef struct {
    u32 off, len;
} T1SPAN;

typedef struct {
    const char *name;
    T1SPAN      span;
} T1CHAR;

typedef struct {
    POOL   *pool;
    u8     *plain;              /* the private part, deciphered */
    u32     plain_len;
    T1SPAN *subrs;
    u32     nsubrs;
    T1CHAR *chars;
    u32     nchars;
    int     len_iv;
    int     units;
    const char *builtin[256];   /* its own encoding; NULL means the standard one */
    int     has_builtin;
} T1;

typedef struct {
    T1  *font;
    fx   stack[32];
    int  sp;
    fx   ps[8];                 /* what an other-subroutine left to be popped */
    int  ps_count;
    fx   xpos, ypos;
    fx   off_x, off_y;          /* where an accent goes */
    fx   side;                  /* the left side bearing */
    fx   flex[14];
    int  flexing, flex_count;
    int  depth, done;
    u32  budget;
} T1RUN;

static void t1_exec( T1RUN *run, const T1SPAN *span );

/* ------------------------------------------------------------------ */
/* reading the program                                                 */
/* ------------------------------------------------------------------ */

static int is_space( int ch )
{
    return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '\f' || ch == 0;
}

/* the next word at *pos, up to "end": where it starts and its length */
static int word_next( const u8 *text, u32 end, u32 *pos, u32 *start, u32 *len )
{
    u32 at = *pos;

    while ( at < end && is_space( text[at] ) ) {
        at++;
    }
    if ( at >= end ) {
        *pos = at;
        return 0;
    }
    *start = at;
    if ( text[at] == '[' || text[at] == ']' || text[at] == '{' || text[at] == '}' ) {
        at++;
    } else {
        at++;
        while ( at < end && !is_space( text[at] ) && text[at] != '/' && text[at] != '[' &&
                text[at] != ']' && text[at] != '{' && text[at] != '}' ) {
            at++;
        }
    }
    *len = at - *start;
    *pos = at;
    return 1;
}

static int word_is( const u8 *text, u32 start, u32 len, const char *want )
{
    return str_len( want ) == len && mem_cmp( text + start, want, len ) == 0;
}

static s32 word_int( const u8 *text, u32 start, u32 len )
{
    s32 val = 0;
    u32 at = start;
    int neg = 0;

    if ( at < start + len && text[at] == '-' ) {
        neg = 1;
        at++;
    }
    for ( ; at < start + len && text[at] >= '0' && text[at] <= '9'; at++ ) {
        val = val * 10 + (text[at] - '0');
    }
    return neg ? -val : val;
}

static const char *word_keep( T1 *font, const u8 *text, u32 start, u32 len )
{
    char *copy = (char *)pool_alloc( font->pool, len + 1 );

    mem_cpy( copy, text + start, len );
    copy[len] = 0;
    return copy;
}

static int hex_digit( int ch )
{
    if ( ch >= '0' && ch <= '9' ) {
        return ch - '0';
    }
    if ( ch >= 'a' && ch <= 'f' ) {
        return ch - 'a' + 10;
    }
    if ( ch >= 'A' && ch <= 'F' ) {
        return ch - 'A' + 10;
    }
    return -1;
}

/* the part before eexec: the encoding, and how big an em is */
static void read_clear( T1 *font, const u8 *text, u32 end )
{
    u32 pos = 0, start, len, digits, at;
    int code, scale, dot;
    REAL first;

    while ( word_next( text, end, &pos, &start, &len ) ) {
        if ( word_is( text, start, len, "/FontMatrix" ) ) {
            if ( word_next( text, end, &pos, &start, &len ) && text[start] == '[' &&
                 word_next( text, end, &pos, &start, &len ) ) {
                digits = 0;
                scale = dot = 0;
                for ( at = start; at < start + len; at++ ) {
                    if ( text[at] == '.' ) {
                        dot = 1;
                    } else if ( text[at] >= '0' && text[at] <= '9' && digits < 100000000UL ) {
                        digits = digits * 10 + (u32)(text[at] - '0');
                        scale += dot;
                    }
                }
                if ( digits ) {
                    first = real_div( real_int( 1 ), real_dec( digits, scale ) );
                    font->units = (int)real_to_int( real_add( first, real_fx( FX_HALF ) ) );
                }
            }
        } else if ( word_is( text, start, len, "/Encoding" ) ) {
            if ( !word_next( text, end, &pos, &start, &len ) ||
                 word_is( text, start, len, "StandardEncoding" ) ) {
                continue;
            }
            /* 256 array ... dup 65 /A put ... readonly def */
            font->has_builtin = 1;
            while ( word_next( text, end, &pos, &start, &len ) ) {
                if ( word_is( text, start, len, "def" ) || word_is( text, start, len, "readonly" ) ) {
                    break;
                }
                if ( !word_is( text, start, len, "dup" ) ) {
                    continue;
                }
                if ( !word_next( text, end, &pos, &start, &len ) ) {
                    break;
                }
                code = (int)word_int( text, start, len );
                if ( !word_next( text, end, &pos, &start, &len ) ) {
                    break;
                }
                if ( text[start] == '/' && code >= 0 && code < 256 ) {
                    font->builtin[code] = word_keep( font, text, start + 1, len - 1 );
                }
            }
        }
    }
    if ( font->units < 16 || font->units > 16384 ) {
        font->units = 1000;
    }
}

/* the private part, deciphered: the subroutines and the charstrings */
static void read_private( T1 *font )
{
    const u8 *text = font->plain;
    u32 end = font->plain_len, pos = 0, start, len, count, index, size;
    int mode = 0;
    T1SPAN span;

    while ( word_next( text, end, &pos, &start, &len ) ) {
        if ( word_is( text, start, len, "/lenIV" ) ) {
            if ( word_next( text, end, &pos, &start, &len ) ) {
                font->len_iv = (int)word_int( text, start, len );
            }
        } else if ( word_is( text, start, len, "/Subrs" ) && font->subrs == NULL ) {
            if ( word_next( text, end, &pos, &start, &len ) ) {
                count = (u32)word_int( text, start, len );
                if ( count > 0 && count < 65536UL ) {
                    font->subrs = (T1SPAN *)pool_alloc( font->pool, count * sizeof( T1SPAN ) );
                    font->nsubrs = count;
                    mode = 1;
                }
            }
        } else if ( word_is( text, start, len, "/CharStrings" ) && font->chars == NULL ) {
            if ( word_next( text, end, &pos, &start, &len ) ) {
                count = (u32)word_int( text, start, len );
                if ( count > 0 && count < 65536UL ) {
                    font->chars = (T1CHAR *)pool_alloc( font->pool, count * sizeof( T1CHAR ) );
                    font->nchars = 0;
                    mode = 2;
                    index = count;          /* how many there is room for */
                    while ( word_next( text, end, &pos, &start, &len ) ) {
                        if ( word_is( text, start, len, "end" ) ) {
                            break;
                        }
                        if ( text[start] != '/' ) {
                            continue;
                        }
                        span.off = start + 1;       /* the name, for now */
                        span.len = len - 1;
                        if ( !word_next( text, end, &pos, &start, &len ) ) {
                            break;
                        }
                        size = (u32)word_int( text, start, len );
                        if ( !word_next( text, end, &pos, &start, &len ) ) {
                            break;                  /* RD, or -| */
                        }
                        pos++;                      /* and the one space after it */
                        if ( pos + size > end || font->nchars >= index ) {
                            break;
                        }
                        font->chars[font->nchars].name = word_keep( font, text, span.off, span.len );
                        font->chars[font->nchars].span.off = pos;
                        font->chars[font->nchars].span.len = size;
                        font->nchars++;
                        pos += size;
                    }
                    return;
                }
            }
        } else if ( mode == 1 && word_is( text, start, len, "dup" ) ) {
            /* dup 5 23 RD <23 bytes> NP */
            if ( !word_next( text, end, &pos, &start, &len ) ) {
                break;
            }
            index = (u32)word_int( text, start, len );
            if ( !word_next( text, end, &pos, &start, &len ) ) {
                break;
            }
            size = (u32)word_int( text, start, len );
            if ( !word_next( text, end, &pos, &start, &len ) ) {
                break;
            }
            pos++;
            if ( pos + size > end ) {
                break;
            }
            if ( index < font->nsubrs ) {
                font->subrs[index].off = pos;
                font->subrs[index].len = size;
            }
            pos += size;
        }
    }
}

void *t1_open( POOL *pool, u8 *data, u32 len )
{
    T1 *font = (T1 *)pool_alloc( pool, sizeof( T1 ) );
    u32 at, start = 0, count = 0, out;
    u16 key = 55665U;
    int found, hex = 1, hi = -1, val, ch;

    font->pool = pool;
    font->len_iv = 4;
    font->units = 1000;
    /* a file in the PC's wrapping (PFB): take the wrappers off */
    if ( len > 6 && data[0] == 0x80 && data[1] == 1 ) {
        out = 0;
        for ( at = 0; at + 6 <= len && data[at] == 0x80 && data[at + 1] != 3; ) {
            count = (u32)data[at + 2] | ((u32)data[at + 3] << 8) | ((u32)data[at + 4] << 16) |
                    ((u32)data[at + 5] << 24);
            at += 6;
            if ( count > len - at ) {
                count = len - at;
            }
            mem_cpy( data + out, data + at, count );
            out += count;
            at += count;
        }
        len = out;
    }
    found = mem_find( data, len, "eexec" );
    if ( found < 0 ) {
        return NULL;
    }
    read_clear( font, data, (u32)found );
    start = (u32)found + 5;
    while ( start < len && (data[start] == ' ' || data[start] == '\t' || data[start] == '\r' ||
                            data[start] == '\n') ) {
        start++;
    }
    /* binary, or hexadecimal text: text if the first four are digits */
    for ( at = start; at < start + 4 && at < len; at++ ) {
        if ( hex_digit( data[at] ) < 0 ) {
            hex = 0;
        }
    }
    font->plain = (u8 *)pool_alloc( pool, len - start + 1 );
    out = 0;
    for ( at = start; at < len; at++ ) {
        ch = data[at];
        if ( hex ) {
            val = hex_digit( ch );
            if ( val < 0 ) {
                continue;
            }
            if ( hi < 0 ) {
                hi = val;
                continue;
            }
            ch = hi * 16 + val;
            hi = -1;
        }
        font->plain[out++] = (u8)(ch ^ (key >> 8));
        key = (u16)((ch + key) * 52845U + 22719U);
    }
    if ( out < 4 ) {
        return NULL;
    }
    font->plain += 4;                       /* four bytes of nothing lead it */
    font->plain_len = out - 4;
    read_private( font );
    if ( font->nchars == 0 ) {
        return NULL;
    }
    return font;
}

int t1_units( void *prog )
{
    return ((T1 *)prog)->units;
}

int t1_glyph_named( void *prog, const char *name )
{
    T1 *font = (T1 *)prog;
    u32 index;

    for ( index = 0; index < font->nchars; index++ ) {
        if ( font->chars[index].name[0] == name[0] && str_cmp( font->chars[index].name, name ) == 0 ) {
            return (int)index;
        }
    }
    return -1;
}

const char *t1_builtin_name( void *prog, int code )
{
    T1 *font = (T1 *)prog;

    if ( code < 0 || code > 255 ) {
        return NULL;
    }
    return font->has_builtin ? font->builtin[code] : enc_std_name( code );
}

/* ------------------------------------------------------------------ */
/* Type 1 charstrings                                                  */
/* ------------------------------------------------------------------ */

static void t1_seac( T1RUN *run, fx asb, fx adx, fx ady, int bchar, int achar )
{
    T1 *font = run->font;
    const char *name;
    fx side = run->side, base_x = run->off_x, base_y = run->off_y;
    int gid;

    name = enc_std_name( bchar );
    gid = name ? t1_glyph_named( font, name ) : -1;
    if ( gid >= 0 ) {
        run->sp = 0;
        run->done = 0;
        t1_exec( run, &font->chars[gid].span );
    }
    name = enc_std_name( achar );
    gid = name ? t1_glyph_named( font, name ) : -1;
    if ( gid >= 0 ) {
        run->sp = 0;
        run->done = 0;
        run->off_x = base_x + side - asb + adx;
        run->off_y = base_y + ady;
        t1_exec( run, &font->chars[gid].span );
    }
    run->off_x = base_x;
    run->off_y = base_y;
    run->done = 1;
}

static void t1_moved( T1RUN *run )
{
    if ( run->flexing ) {
        /* inside a flex the moves are its points, not moves */
        if ( run->flex_count < 14 ) {
            run->flex[run->flex_count++] = run->xpos;
            run->flex[run->flex_count++] = run->ypos;
        }
    } else {
        gl_move( run->xpos, run->ypos );
    }
}

static void t1_curve( T1RUN *run, fx dx1, fx dy1, fx dx2, fx dy2, fx dx3, fx dy3 )
{
    fx x1 = run->xpos + dx1, y1 = run->ypos + dy1;
    fx x2 = x1 + dx2, y2 = y1 + dy2;

    run->xpos = x2 + dx3;
    run->ypos = y2 + dy3;
    gl_curve( x1, y1, x2, y2, run->xpos, run->ypos );
}

static void t1_exec( T1RUN *run, const T1SPAN *span )
{
    static u8 code[4096];           /* one level's charstring, deciphered */
    T1 *font = run->font;
    u8 *prog;
    fx *st = run->stack;
    u32 len = span->len, at, skip = font->len_iv < 0 ? 0 : (u32)font->len_iv;
    u16 key = 4330U;
    s32 big, below;
    int b0, other, count, index;
    T1SPAN sub;

    if ( run->depth > 8 || len <= skip || span->off + len > font->plain_len ) {
        return;
    }
    /* a subroutine's caller is still running its own, so each level
       has its own copy: the first in the static buffer, the rest in
       the page's memory */
    len -= skip;
    prog = run->depth == 0 && len <= sizeof( code ) ? code : (u8 *)pool_big( doc.page_pool, len );
    if ( font->len_iv < 0 ) {
        mem_cpy( prog, font->plain + span->off, len );
    } else {
        for ( at = 0; at < span->len; at++ ) {
            b0 = font->plain[span->off + at];
            if ( at >= skip ) {
                prog[at - skip] = (u8)(b0 ^ (key >> 8));
            }
            key = (u16)((b0 + key) * 52845U + 22719U);
        }
    }

    for ( at = 0; at < len && !run->done; ) {
        if ( run->budget-- == 0 ) {
            run->done = 1;
            break;
        }
        b0 = prog[at++];
        if ( b0 >= 32 ) {
            if ( run->sp >= 30 ) {
                run->sp = 0;
            }
            if ( b0 <= 246 ) {
                st[run->sp++] = I2FX( b0 - 139 );
            } else if ( b0 <= 250 ) {
                st[run->sp++] = I2FX( (b0 - 247) * 256 + (at < len ? prog[at] : 0) + 108 );
                at++;
            } else if ( b0 <= 254 ) {
                st[run->sp++] = I2FX( -(b0 - 251) * 256 - (at < len ? prog[at] : 0) - 108 );
                at++;
            } else {
                /* four bytes.  A number too large for the stack is
                   always the top of a fraction - "100000 1000 div" - so
                   the fraction is taken whole when it is */
                big = at + 4 <= len ? (s32)(((u32)prog[at] << 24) | ((u32)prog[at + 1] << 16) |
                                            ((u32)prog[at + 2] << 8) | prog[at + 3]) : 0;
                at += 4;
                if ( big > 32767 || big < -32767 ) {
                    below = 0;
                    if ( at + 2 < len && prog[at] >= 32 && prog[at] <= 246 &&
                         prog[at + 1] == 12 && prog[at + 2] == 12 ) {
                        below = prog[at] - 139;
                        at += 3;
                    } else if ( at + 3 < len && prog[at] >= 247 && prog[at] <= 250 &&
                                prog[at + 2] == 12 && prog[at + 3] == 12 ) {
                        below = (prog[at] - 247) * 256 + prog[at + 1] + 108;
                        at += 4;
                    }
                    st[run->sp++] = below ? mul_div( big, FX_ONE, below )
                                          : (big < 0 ? -FX_MAX : FX_MAX);
                } else {
                    st[run->sp++] = I2FX( big );
                }
            }
            continue;
        }
        switch ( b0 ) {
        case 13:                                    /* hsbw */
            if ( run->sp >= 2 ) {
                run->side = st[0];
                run->xpos = run->off_x + st[0];
                run->ypos = run->off_y;
            }
            break;
        case 21:                                    /* rmoveto */
            if ( run->sp >= 2 ) {
                run->xpos += st[0];
                run->ypos += st[1];
                t1_moved( run );
            }
            break;
        case 22:                                    /* hmoveto */
            if ( run->sp >= 1 ) {
                run->xpos += st[0];
                t1_moved( run );
            }
            break;
        case 4:                                     /* vmoveto */
            if ( run->sp >= 1 ) {
                run->ypos += st[0];
                t1_moved( run );
            }
            break;
        case 5:                                     /* rlineto */
            if ( run->sp >= 2 ) {
                run->xpos += st[0];
                run->ypos += st[1];
                gl_line( run->xpos, run->ypos );
            }
            break;
        case 6:                                     /* hlineto */
            if ( run->sp >= 1 ) {
                run->xpos += st[0];
                gl_line( run->xpos, run->ypos );
            }
            break;
        case 7:                                     /* vlineto */
            if ( run->sp >= 1 ) {
                run->ypos += st[0];
                gl_line( run->xpos, run->ypos );
            }
            break;
        case 8:                                     /* rrcurveto */
            if ( run->sp >= 6 ) {
                t1_curve( run, st[0], st[1], st[2], st[3], st[4], st[5] );
            }
            break;
        case 30:                                    /* vhcurveto */
            if ( run->sp >= 4 ) {
                t1_curve( run, 0, st[0], st[1], st[2], st[3], 0 );
            }
            break;
        case 31:                                    /* hvcurveto */
            if ( run->sp >= 4 ) {
                t1_curve( run, st[0], 0, st[1], st[2], 0, st[3] );
            }
            break;
        case 9:                                     /* closepath */
            gl_close();
            break;
        case 10:                                    /* callsubr */
            if ( run->sp < 1 ) {
                break;
            }
            index = (int)FX_FLOOR( st[--run->sp] );
            if ( index >= 0 && (u32)index < font->nsubrs && font->subrs[index].len ) {
                sub = font->subrs[index];
                run->depth++;
                t1_exec( run, &sub );
                run->depth--;
            }
            continue;
        case 11:                                    /* return */
            goto out;
        case 14:                                    /* endchar */
            run->done = 1;
            break;
        case 12:
            b0 = at < len ? prog[at++] : 0;
            switch ( b0 ) {
            case 6:                                 /* seac */
                if ( run->sp >= 5 ) {
                    run->depth++;
                    t1_seac( run, st[0], st[1], st[2], (int)FX_FLOOR( st[3] ), (int)FX_FLOOR( st[4] ) );
                    run->depth--;
                }
                break;
            case 7:                                 /* sbw */
                if ( run->sp >= 4 ) {
                    run->side = st[0];
                    run->xpos = run->off_x + st[0];
                    run->ypos = run->off_y + st[1];
                }
                break;
            case 12:                                /* div */
                if ( run->sp >= 2 ) {
                    st[run->sp - 2] = st[run->sp - 1] ? fx_div( st[run->sp - 2], st[run->sp - 1] ) : 0;
                    run->sp--;
                }
                continue;
            case 16:                                /* callothersubr */
                if ( run->sp < 2 ) {
                    break;
                }
                other = (int)FX_FLOOR( st[--run->sp] );
                count = (int)FX_FLOOR( st[--run->sp] );
                if ( count < 0 || count > run->sp ) {
                    count = run->sp;
                }
                run->ps_count = 0;
                if ( other == 1 ) {
                    run->flexing = 1;
                    run->flex_count = 0;
                } else if ( other == 0 && count >= 3 ) {
                    /* the end of a flex: seven points were collected,
                       the first only a reference; two curves through
                       the other six */
                    run->flexing = 0;
                    if ( run->flex_count >= 14 ) {
                        gl_curve( run->flex[2], run->flex[3], run->flex[4], run->flex[5],
                                  run->flex[6], run->flex[7] );
                        gl_curve( run->flex[8], run->flex[9], run->flex[10], run->flex[11],
                                  run->flex[12], run->flex[13] );
                    }
                    run->ps[run->ps_count++] = st[run->sp - 1];     /* y, then x, to be popped */
                    run->ps[run->ps_count++] = st[run->sp - 2];
                } else if ( other != 2 ) {
                    /* its arguments come back as they went */
                    for ( index = 0; index < count && run->ps_count < 8; index++ ) {
                        run->ps[run->ps_count++] = st[run->sp - 1 - index];
                    }
                }
                run->sp -= count;
                continue;
            case 17:                                /* pop */
                if ( run->ps_count > 0 && run->sp < 30 ) {
                    st[run->sp++] = run->ps[--run->ps_count];
                }
                continue;
            case 33:                                /* setcurrentpoint */
                /* only ever said after a flex, of where the flex ended -
                   which is where the point already is */
                break;
            default:                                /* dotsection, the stems in threes */
                break;
            }
            break;
        default:                                    /* hstem, vstem */
            break;
        }
        run->sp = 0;
    }
out:
    if ( prog != code ) {
        pool_unbig( prog );
    }
}

int t1_outline( void *prog, u32 gid )
{
    static T1RUN run;
    T1 *font = (T1 *)prog;

    if ( gid >= font->nchars ) {
        return 0;
    }
    mem_set( &run, 0, sizeof( run ) );
    run.font = font;
    run.budget = 50000UL;
    t1_exec( &run, &font->chars[gid].span );
    return 1;
}
