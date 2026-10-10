/*
 * CFF.C - a Compact Font Format program (PDF's Type1C, CIDFontType0C,
 * and the outlines of an OpenType font that says "OTTO"): its tables,
 * and the Type 2 charstrings that draw its glyphs.
 *
 * A glyph here is found by name (a simple font, through the charset)
 * or by character ID (a font keyed by them, whose charset is the IDs),
 * and drawn by running its charstring: a small stack machine whose
 * operators are all "so much further, in this direction".  The hints -
 * which stems to align to the pixel grid - are counted, because the
 * mask operators that follow them are as long as the count says, and
 * otherwise ignored.
 */
#include "gfx.h"

typedef struct {
    u32 count;
    u32 offsize;
    u32 offs;                   /* where the offsets are */
    u32 base;                   /* what an offset is from */
    u32 end;                    /* the byte after the whole INDEX */
} CIDX;

typedef struct {
    CIDX subrs;
    int  has_subrs;
} CFD;

typedef struct {
    POOL *pool;
    u8   *data;
    u32   len;
    CIDX  gsubrs, chars, strings, fdarray;
    u32   charset, encoding, fdselect;
    int   glyphs, is_cid, units;
    u16  *sids;                 /* each glyph's string, or character ID */
    CFD  *fds;
    int   nfds;
} CFF;

#define T2_STACK    48

typedef struct {
    CFF *cff;
    CFD *fd;
    fx   stack[T2_STACK + 2];
    int  sp;
    fx   xpos, ypos;
    int  stems, have_width, open, depth, done;
    u32  budget;                /* operators left, against a loop */
    fx   trans[32];
} T2;

static u32 rd( const CFF *cff, u32 off, u32 size )
{
    u32 val = 0;

    if ( off + size > cff->len ) {
        return 0;
    }
    while ( size-- ) {
        val = (val << 8) | cff->data[off++];
    }
    return val;
}

/* an INDEX at "off": 0 if it is not all there */
static int index_at( const CFF *cff, u32 off, CIDX *idx )
{
    u32 last;

    mem_set( idx, 0, sizeof( *idx ) );
    if ( off + 2 > cff->len ) {
        return 0;
    }
    idx->count = rd( cff, off, 2 );
    if ( idx->count == 0 ) {
        idx->end = off + 2;
        return 1;
    }
    idx->offsize = rd( cff, off + 2, 1 );
    if ( idx->offsize < 1 || idx->offsize > 4 ) {
        return 0;
    }
    idx->offs = off + 3;
    idx->base = off + 3 + (idx->count + 1) * idx->offsize - 1;
    last = rd( cff, idx->offs + idx->count * idx->offsize, idx->offsize );
    idx->end = idx->base + last;
    return idx->end <= cff->len && idx->base < cff->len;
}

/* entry "which" of an INDEX: where it is and how long */
static int index_get( const CFF *cff, const CIDX *idx, u32 which, u32 *start, u32 *size )
{
    u32 from, to;

    if ( which >= idx->count ) {
        return 0;
    }
    from = rd( cff, idx->offs + which * idx->offsize, idx->offsize );
    to = rd( cff, idx->offs + (which + 1) * idx->offsize, idx->offsize );
    if ( to < from || idx->base + to > cff->len ) {
        return 0;
    }
    *start = idx->base + from;
    *size = to - from;
    return 1;
}

/* ------------------------------------------------------------------ */
/* dictionaries                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    s32  vals[16];              /* the operands of the entry found, as integers */
    REAL first;                 /* ...and the first as it was written */
    int  count;
} DICTVAL;

/* a real, in the packed decimal a DICT writes one in */
static REAL dict_real( const CFF *cff, u32 *pos, u32 end )
{
    u32 digits = 0;
    int scale = 0, kept = 0, dot = 0, neg = 0, expo = 0, eneg = 0, in_exp = 0, half = 0, nib, byte = 0;
    REAL val;

    for ( ;; ) {
        if ( !half ) {
            if ( *pos >= end ) {
                break;
            }
            byte = cff->data[(*pos)++];
            nib = byte >> 4;
        } else {
            nib = byte & 15;
        }
        half = !half;
        if ( nib == 15 ) {
            break;
        }
        if ( nib <= 9 ) {
            if ( in_exp ) {
                expo = expo * 10 + nib;
            } else if ( kept < 9 ) {
                digits = digits * 10 + (u32)nib;
                if ( digits ) {
                    kept++;
                }
                if ( dot ) {
                    scale++;
                }
            } else if ( !dot ) {
                scale--;
            }
        } else if ( nib == 10 ) {
            dot = 1;
        } else if ( nib == 11 ) {
            in_exp = 1;
        } else if ( nib == 12 ) {
            in_exp = 1;
            eneg = 1;
        } else if ( nib == 14 ) {
            neg = 1;
        }
    }
    if ( expo > 30 ) {
        expo = 30;
    }
    val = real_dec( digits, scale + (eneg ? expo : -expo) );
    if ( neg ) {
        val.man = -val.man;
    }
    return val;
}

/* look for operator "want" (an escaped one is 0x0C00 + its second byte)
   in the DICT at start..start+size */
static int dict_find( const CFF *cff, u32 start, u32 size, u32 want, DICTVAL *out )
{
    u32 pos = start, end = start + size, op;
    int b0;
    REAL real;

    out->count = 0;
    while ( pos < end ) {
        b0 = cff->data[pos++];
        if ( b0 <= 21 ) {
            op = (u32)b0;
            if ( b0 == 12 && pos < end ) {
                op = 0x0C00 + cff->data[pos++];
            }
            if ( op == want ) {
                return 1;
            }
            out->count = 0;
            continue;
        }
        real = real_int( 0 );
        if ( b0 == 28 ) {
            real = real_int( (s16)rd( cff, pos, 2 ) );
            pos += 2;
        } else if ( b0 == 29 ) {
            real = real_int( (s32)rd( cff, pos, 4 ) );
            pos += 4;
        } else if ( b0 == 30 ) {
            real = dict_real( cff, &pos, end );
        } else if ( b0 >= 32 && b0 <= 246 ) {
            real = real_int( b0 - 139 );
        } else if ( b0 >= 247 && b0 <= 250 ) {
            real = real_int( (b0 - 247) * 256 + (int)rd( cff, pos, 1 ) + 108 );
            pos++;
        } else if ( b0 >= 251 && b0 <= 254 ) {
            real = real_int( -(b0 - 251) * 256 - (int)rd( cff, pos, 1 ) - 108 );
            pos++;
        }
        if ( out->count < 16 ) {
            if ( out->count == 0 ) {
                out->first = real;
            }
            out->vals[out->count++] = real_to_int( real );
        }
    }
    return 0;
}

/* a font DICT's Private DICT, for its local subroutines */
static void private_of( CFF *cff, u32 start, u32 size, CFD *fd )
{
    DICTVAL val;
    u32 poff, psize;

    fd->has_subrs = 0;
    if ( !dict_find( cff, start, size, 18, &val ) || val.count < 2 ) {
        return;
    }
    psize = (u32)val.vals[0];
    poff = (u32)val.vals[1];
    if ( poff >= cff->len || psize > cff->len - poff ) {
        return;
    }
    if ( dict_find( cff, poff, psize, 19, &val ) && val.count >= 1 ) {
        fd->has_subrs = index_at( cff, poff + (u32)val.vals[0], &fd->subrs );
    }
}

static void charset_read( CFF *cff )
{
    u32 pos = cff->charset, first, left;
    int gid = 1, format, index;

    cff->sids = (u16 *)pool_alloc( cff->pool, (u32)(cff->glyphs + 1) * sizeof( u16 ) );
    if ( cff->charset <= 2 ) {
        /* a predefined one: the standard strings in order */
        for ( index = 0; index < cff->glyphs; index++ ) {
            cff->sids[index] = (u16)index;
        }
        return;
    }
    format = (int)rd( cff, pos++, 1 );
    if ( format == 0 ) {
        for ( ; gid < cff->glyphs; gid++, pos += 2 ) {
            cff->sids[gid] = (u16)rd( cff, pos, 2 );
        }
        return;
    }
    while ( gid < cff->glyphs && pos < cff->len ) {
        first = rd( cff, pos, 2 );
        left = rd( cff, pos + 2, format == 1 ? 1 : 2 );
        pos += format == 1 ? 3 : 4;
        for ( index = 0; index <= (int)left && gid < cff->glyphs; index++ ) {
            cff->sids[gid++] = (u16)(first + (u32)index);
        }
    }
}

void *cff_open( POOL *pool, u8 *data, u32 len )
{
    CFF *cff = (CFF *)pool_alloc( pool, sizeof( CFF ) );
    CIDX names, tops;
    DICTVAL val;
    REAL scale;
    u32 top, top_size, start, size;
    int index;

    cff->pool = pool;
    cff->data = data;
    cff->len = len;
    if ( len < 4 || !index_at( cff, rd( cff, 2, 1 ), &names ) || !index_at( cff, names.end, &tops ) ||
         !index_at( cff, tops.end, &cff->strings ) || !index_at( cff, cff->strings.end, &cff->gsubrs ) ||
         !index_get( cff, &tops, 0, &top, &top_size ) ) {
        return NULL;
    }
    if ( !dict_find( cff, top, top_size, 17, &val ) || val.count < 1 ||
         !index_at( cff, (u32)val.vals[0], &cff->chars ) || cff->chars.count == 0 ) {
        return NULL;
    }
    cff->glyphs = (int)cff->chars.count;
    cff->charset = dict_find( cff, top, top_size, 15, &val ) && val.count ? (u32)val.vals[0] : 0;
    cff->encoding = dict_find( cff, top, top_size, 16, &val ) && val.count ? (u32)val.vals[0] : 0;
    cff->is_cid = dict_find( cff, top, top_size, 0x0C1E, &val );
    cff->units = 1000;
    if ( dict_find( cff, top, top_size, 0x0C07, &val ) && val.count >= 1 && val.first.man > 0 ) {
        scale = real_div( real_int( 1 ), val.first );
        cff->units = (int)real_to_int( real_add( scale, real_fx( FX_HALF ) ) );
        if ( cff->units < 16 || cff->units > 16384 ) {
            cff->units = 1000;
        }
    }
    charset_read( cff );

    if ( dict_find( cff, top, top_size, 0x0C24, &val ) && val.count &&
         index_at( cff, (u32)val.vals[0], &cff->fdarray ) && cff->fdarray.count ) {
        /* a font of several font DICTs, each glyph in one of them */
        cff->nfds = (int)cff->fdarray.count;
        cff->fds = (CFD *)pool_alloc( pool, (u32)cff->nfds * sizeof( CFD ) );
        for ( index = 0; index < cff->nfds; index++ ) {
            if ( index_get( cff, &cff->fdarray, (u32)index, &start, &size ) ) {
                private_of( cff, start, size, &cff->fds[index] );
            }
        }
        cff->fdselect = dict_find( cff, top, top_size, 0x0C25, &val ) && val.count ? (u32)val.vals[0] : 0;
    } else {
        cff->nfds = 1;
        cff->fds = (CFD *)pool_alloc( pool, sizeof( CFD ) );
        private_of( cff, top, top_size, &cff->fds[0] );
    }
    return cff;
}

int cff_is_cid( void *prog )
{
    return ((CFF *)prog)->is_cid;
}

int cff_units( void *prog )
{
    return ((CFF *)prog)->units;
}

/* a string by number, as text that lasts: the standard ones are
   NAMES.C's, the font's own are copied */
static const char *string_of( CFF *cff, u32 sid )
{
    u32 start, size;
    char *copy;

    if ( sid < 391 ) {
        return name_std( (int)sid );
    }
    if ( !index_get( cff, &cff->strings, sid - 391, &start, &size ) || size > 127 ) {
        return NULL;
    }
    copy = (char *)pool_alloc( cff->pool, size + 1 );
    mem_cpy( copy, cff->data + start, size );
    copy[size] = 0;
    return copy;
}

int cff_glyph_named( void *prog, const char *name )
{
    CFF *cff = (CFF *)prog;
    const char *known;
    u32 len = str_len( name ), start, size;
    int gid;

    for ( gid = 0; gid < cff->glyphs; gid++ ) {
        if ( cff->sids[gid] < 391 ) {
            known = name_std( cff->sids[gid] );
            if ( known && str_cmp( known, name ) == 0 ) {
                return gid;
            }
        } else if ( index_get( cff, &cff->strings, (u32)cff->sids[gid] - 391, &start, &size ) &&
                    size == len && mem_cmp( cff->data + start, name, len ) == 0 ) {
            return gid;
        }
    }
    return -1;
}

int cff_glyph_of_cid( void *prog, u32 cid )
{
    CFF *cff = (CFF *)prog;
    int gid;

    if ( !cff->is_cid ) {
        return cid < (u32)cff->glyphs ? (int)cid : 0;
    }
    if ( cid < (u32)cff->glyphs && cff->sids[cid] == cid ) {
        return (int)cid;
    }
    for ( gid = 0; gid < cff->glyphs; gid++ ) {
        if ( cff->sids[gid] == cid ) {
            return gid;
        }
    }
    return 0;
}

/* the name of the glyph the font's own encoding gives a code, or NULL */
const char *cff_builtin_name( void *prog, int code )
{
    CFF *cff = (CFF *)prog;
    u32 pos = cff->encoding, count, index, first, left;
    int format, gid = 0, found = 0;

    if ( cff->encoding <= 1 ) {
        return enc_std_name( code );
    }
    format = (int)rd( cff, pos, 1 );
    count = rd( cff, pos + 1, 1 );
    pos += 2;
    if ( (format & 0x7F) == 0 ) {
        for ( index = 0; index < count; index++ ) {
            if ( (int)rd( cff, pos + index, 1 ) == code ) {
                gid = (int)index + 1;
                found = 1;
            }
        }
        pos += count;
    } else {
        gid = 1;
        for ( index = 0; index < count; index++, pos += 2 ) {
            first = rd( cff, pos, 1 );
            left = rd( cff, pos + 1, 1 );
            if ( (u32)code >= first && (u32)code <= first + left ) {
                gid += code - (int)first;
                found = 1;
                break;
            }
            gid += (int)left + 1;
        }
        if ( !found ) {
            pos += (count - index) * 2;
        } else {
            pos = 0;
        }
    }
    if ( !found && (format & 0x80) && pos ) {
        /* the codes given a second glyph each */
        count = rd( cff, pos, 1 );
        for ( index = 0; index < count; index++ ) {
            if ( (int)rd( cff, pos + 1 + index * 3, 1 ) == code ) {
                return string_of( cff, rd( cff, pos + 2 + index * 3, 2 ) );
            }
        }
    }
    if ( !found || gid >= cff->glyphs ) {
        return NULL;
    }
    return string_of( cff, cff->sids[gid] );
}

/* ------------------------------------------------------------------ */
/* Type 2 charstrings                                                  */
/* ------------------------------------------------------------------ */

static int subr_bias( u32 count )
{
    return count < 1240 ? 107 : (count < 33900UL ? 1131 : 32768L);
}

static void t2_move( T2 *run )
{
    if ( run->open ) {
        gl_close();
    }
    gl_move( run->xpos, run->ypos );
    run->open = 1;
}

static void t2_line( T2 *run, fx dx, fx dy )
{
    run->xpos += dx;
    run->ypos += dy;
    gl_line( run->xpos, run->ypos );
}

static void t2_curve( T2 *run, fx dx1, fx dy1, fx dx2, fx dy2, fx dx3, fx dy3 )
{
    fx x1 = run->xpos + dx1, y1 = run->ypos + dy1;
    fx x2 = x1 + dx2, y2 = y1 + dy2;

    run->xpos = x2 + dx3;
    run->ypos = y2 + dy3;
    gl_curve( x1, y1, x2, y2, run->xpos, run->ypos );
}

/* the width, if this operator's operands start with one: it is there
   when there is one operand more than the operator takes */
static void t2_width( T2 *run, int extra )
{
    if ( !run->have_width ) {
        run->have_width = 1;
        if ( extra && run->sp > 0 ) {
            mem_cpy( run->stack, run->stack + 1, (u32)(run->sp - 1) * sizeof( fx ) );
            run->sp--;
        }
    }
}

static void t2_run( T2 *run, u32 start, u32 size )
{
    CFF *cff = run->cff;
    const u8 *data = cff->data;
    fx *st = run->stack;
    u32 pos = start, end = start + size, sub_start, sub_size;
    fx hold, dx, dy;
    int b0, index, count, horiz, num;

    if ( run->depth > 10 ) {
        return;
    }
    while ( pos < end && !run->done ) {
        if ( run->budget-- == 0 ) {
            run->done = 1;
            break;
        }
        b0 = data[pos++];
        if ( b0 >= 32 || b0 == 28 ) {
            if ( run->sp >= T2_STACK ) {
                run->sp = 0;                        /* more than can be: start over */
            }
            if ( b0 == 28 ) {
                st[run->sp++] = I2FX( (s16)rd( cff, pos, 2 ) );
                pos += 2;
            } else if ( b0 <= 246 ) {
                st[run->sp++] = I2FX( b0 - 139 );
            } else if ( b0 <= 250 ) {
                st[run->sp++] = I2FX( (b0 - 247) * 256 + (int)rd( cff, pos++, 1 ) + 108 );
            } else if ( b0 <= 254 ) {
                st[run->sp++] = I2FX( -(b0 - 251) * 256 - (int)rd( cff, pos++, 1 ) - 108 );
            } else {
                st[run->sp++] = (fx)rd( cff, pos, 4 );
                pos += 4;
            }
            continue;
        }
        switch ( b0 ) {
        case 1:                                     /* hstem */
        case 3:                                     /* vstem */
        case 18:                                    /* hstemhm */
        case 23:                                    /* vstemhm */
            t2_width( run, run->sp & 1 );
            run->stems += run->sp / 2;
            break;
        case 19:                                    /* hintmask */
        case 20:                                    /* cntrmask */
            t2_width( run, run->sp & 1 );
            run->stems += run->sp / 2;
            pos += (u32)(run->stems + 7) / 8;
            break;
        case 21:                                    /* rmoveto */
            t2_width( run, run->sp > 2 );
            if ( run->sp >= 2 ) {
                run->xpos += st[0];
                run->ypos += st[1];
            }
            t2_move( run );
            break;
        case 22:                                    /* hmoveto */
            t2_width( run, run->sp > 1 );
            if ( run->sp >= 1 ) {
                run->xpos += st[0];
            }
            t2_move( run );
            break;
        case 4:                                     /* vmoveto */
            t2_width( run, run->sp > 1 );
            if ( run->sp >= 1 ) {
                run->ypos += st[0];
            }
            t2_move( run );
            break;
        case 5:                                     /* rlineto */
            for ( index = 0; index + 1 < run->sp; index += 2 ) {
                t2_line( run, st[index], st[index + 1] );
            }
            break;
        case 6:                                     /* hlineto */
        case 7:                                     /* vlineto */
            horiz = b0 == 6;
            for ( index = 0; index < run->sp; index++, horiz = !horiz ) {
                t2_line( run, horiz ? st[index] : 0, horiz ? 0 : st[index] );
            }
            break;
        case 8:                                     /* rrcurveto */
            for ( index = 0; index + 5 < run->sp; index += 6 ) {
                t2_curve( run, st[index], st[index + 1], st[index + 2], st[index + 3],
                          st[index + 4], st[index + 5] );
            }
            break;
        case 24:                                    /* rcurveline */
            for ( index = 0; index + 7 < run->sp; index += 6 ) {
                t2_curve( run, st[index], st[index + 1], st[index + 2], st[index + 3],
                          st[index + 4], st[index + 5] );
            }
            if ( index + 1 < run->sp ) {
                t2_line( run, st[index], st[index + 1] );
            }
            break;
        case 25:                                    /* rlinecurve */
            for ( index = 0; index + 7 < run->sp; index += 2 ) {
                t2_line( run, st[index], st[index + 1] );
            }
            if ( index + 5 < run->sp ) {
                t2_curve( run, st[index], st[index + 1], st[index + 2], st[index + 3],
                          st[index + 4], st[index + 5] );
            }
            break;
        case 26:                                    /* vvcurveto */
            index = 0;
            dx = 0;
            if ( run->sp & 1 ) {
                dx = st[index++];
            }
            for ( ; index + 3 < run->sp; index += 4 ) {
                t2_curve( run, dx, st[index], st[index + 1], st[index + 2], 0, st[index + 3] );
                dx = 0;
            }
            break;
        case 27:                                    /* hhcurveto */
            index = 0;
            dy = 0;
            if ( run->sp & 1 ) {
                dy = st[index++];
            }
            for ( ; index + 3 < run->sp; index += 4 ) {
                t2_curve( run, st[index], dy, st[index + 1], st[index + 2], st[index + 3], 0 );
                dy = 0;
            }
            break;
        case 30:                                    /* vhcurveto */
        case 31:                                    /* hvcurveto */
            horiz = b0 == 31;
            for ( index = 0; index + 3 < run->sp; index += 4, horiz = !horiz ) {
                /* the last curve of an odd count ends where its fifth
                   number says, not straight along */
                hold = run->sp - index == 5 ? st[index + 4] : 0;
                if ( horiz ) {
                    t2_curve( run, st[index], 0, st[index + 1], st[index + 2], hold, st[index + 3] );
                } else {
                    t2_curve( run, 0, st[index], st[index + 1], st[index + 2], st[index + 3], hold );
                }
            }
            break;
        case 10:                                    /* callsubr */
        case 29:                                    /* callgsubr */
            if ( run->sp < 1 ) {
                break;
            }
            num = (int)FX_FLOOR( st[--run->sp] );
            if ( b0 == 10 ) {
                if ( !run->fd->has_subrs ||
                     !index_get( cff, &run->fd->subrs, (u32)(num + subr_bias( run->fd->subrs.count )),
                                 &sub_start, &sub_size ) ) {
                    continue;
                }
            } else if ( !index_get( cff, &cff->gsubrs, (u32)(num + subr_bias( cff->gsubrs.count )),
                                    &sub_start, &sub_size ) ) {
                continue;
            }
            run->depth++;
            t2_run( run, sub_start, sub_size );
            run->depth--;
            continue;                               /* the stack is the subroutine's to leave */
        case 11:                                    /* return */
            return;
        case 14:                                    /* endchar */
            run->done = 1;
            break;
        case 12:
            b0 = pos < end ? data[pos++] : 0;
            count = run->sp;
            switch ( b0 ) {
            case 34:                                /* hflex */
                if ( count >= 7 ) {
                    t2_curve( run, st[0], 0, st[1], st[2], st[3], 0 );
                    t2_curve( run, st[4], 0, st[5], -st[2], st[6], 0 );
                }
                break;
            case 35:                                /* flex */
                if ( count >= 12 ) {
                    t2_curve( run, st[0], st[1], st[2], st[3], st[4], st[5] );
                    t2_curve( run, st[6], st[7], st[8], st[9], st[10], st[11] );
                }
                break;
            case 36:                                /* hflex1 */
                if ( count >= 9 ) {
                    t2_curve( run, st[0], st[1], st[2], st[3], st[4], 0 );
                    t2_curve( run, st[5], 0, st[6], st[7], st[8], -(st[1] + st[3] + st[7]) );
                }
                break;
            case 37:                                /* flex1 */
                if ( count >= 11 ) {
                    dx = st[0] + st[2] + st[4] + st[6] + st[8];
                    dy = st[1] + st[3] + st[5] + st[7] + st[9];
                    t2_curve( run, st[0], st[1], st[2], st[3], st[4], st[5] );
                    if ( (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ) {
                        t2_curve( run, st[6], st[7], st[8], st[9], st[10], -dy );
                    } else {
                        t2_curve( run, st[6], st[7], st[8], st[9], -dx, st[10] );
                    }
                }
                break;
            /* the arithmetic: it leaves its answer, so the stack stays */
            case 9:  if ( count >= 1 ) { st[count - 1] = st[count - 1] < 0 ? -st[count - 1] : st[count - 1]; } continue;
            case 10: if ( count >= 2 ) { st[count - 2] += st[count - 1]; run->sp--; } continue;
            case 11: if ( count >= 2 ) { st[count - 2] -= st[count - 1]; run->sp--; } continue;
            case 12: if ( count >= 2 ) { st[count - 2] = st[count - 1] ? fx_div( st[count - 2], st[count - 1] ) : 0; run->sp--; } continue;
            case 14: if ( count >= 1 ) { st[count - 1] = -st[count - 1]; } continue;
            case 18: if ( count >= 1 ) { run->sp--; } continue;
            case 24: if ( count >= 2 ) { st[count - 2] = fx_mul( st[count - 2], st[count - 1] ); run->sp--; } continue;
            case 26: if ( count >= 1 ) { st[count - 1] = fx_sqrt( st[count - 1] ); } continue;
            case 27: if ( count >= 1 && count < T2_STACK ) { st[count] = st[count - 1]; run->sp++; } continue;
            case 28:
                if ( count >= 2 ) {
                    hold = st[count - 1];
                    st[count - 1] = st[count - 2];
                    st[count - 2] = hold;
                }
                continue;
            case 20:                                /* put */
                if ( count >= 2 ) {
                    index = (int)FX_FLOOR( st[count - 1] );
                    if ( index >= 0 && index < 32 ) {
                        run->trans[index] = st[count - 2];
                    }
                    run->sp -= 2;
                }
                continue;
            case 21:                                /* get */
                if ( count >= 1 ) {
                    index = (int)FX_FLOOR( st[count - 1] );
                    st[count - 1] = index >= 0 && index < 32 ? run->trans[index] : 0;
                }
                continue;
            case 22:                                /* ifelse */
                if ( count >= 4 ) {
                    if ( st[count - 2] > st[count - 1] ) {
                        st[count - 4] = st[count - 3];
                    }
                    run->sp -= 3;
                }
                continue;
            case 23:                                /* random */
                if ( count < T2_STACK ) {
                    st[run->sp++] = FX_HALF;
                }
                continue;
            default:
                break;
            }
            break;
        default:
            break;
        }
        run->sp = 0;
    }
}

int cff_outline( void *prog, u32 gid )
{
    static T2 run;
    CFF *cff = (CFF *)prog;
    u32 start, size, pos, count, index, first, fd = 0;
    int format;

    if ( !index_get( cff, &cff->chars, gid, &start, &size ) ) {
        return 0;
    }
    if ( cff->nfds > 1 && cff->fdselect ) {
        format = (int)rd( cff, cff->fdselect, 1 );
        if ( format == 0 ) {
            fd = rd( cff, cff->fdselect + 1 + gid, 1 );
        } else if ( format == 3 ) {
            count = rd( cff, cff->fdselect + 1, 2 );
            pos = cff->fdselect + 3;
            for ( index = 0; index < count; index++, pos += 3 ) {
                first = rd( cff, pos, 2 );
                if ( gid >= first && gid < rd( cff, pos + 3, 2 ) ) {
                    fd = rd( cff, pos + 2, 1 );
                    break;
                }
            }
        }
        if ( fd >= (u32)cff->nfds ) {
            fd = 0;
        }
    }
    mem_set( &run, 0, sizeof( run ) );
    run.cff = cff;
    run.fd = &cff->fds[fd];
    run.budget = 100000UL;
    t2_run( &run, start, size );
    if ( run.open ) {
        gl_close();
    }
    return 1;
}
