/*
 * TEXT.C - a page as text, for the text screen.
 *
 * A PDF page has no lines and no words: it has characters, each put
 * where the typesetter wanted it, in whatever order the program that
 * wrote the file found convenient.  So the page is run with nothing
 * drawn (INTERP.C), every character is noted with where it landed,
 * and the lines are made here: characters on one baseline, give or
 * take, are a line; they are put in order across; a gap between two
 * is a space, and a wide gap is as many spaces as it is wide, so that
 * a table's columns still stand under one another.
 *
 * A column of the screen is one average character of the page wide,
 * so a line comes out about as long as it has characters, and is
 * indented about as far as it was.
 */
#include "gfx.h"

typedef struct {
    int32_t xpos, ypos;         /* 256ths of a point, from the page's top left */
    uint16_t uni;
    uint16_t advance;
    uint16_t size;
    uint16_t spare;
} TCHAR;

#define LINE_MAX    400

static TCHAR       *chars;
static unsigned int char_count, char_cap;

/* A width or a height in 256ths of a point, with the sign dropped: a
   negative font size (mirrored text) is as tall as a positive one, and
   fx_hypot comes back negative past 32,767 points.  More than 250
   points either way is "not known", 64000. */
static uint16_t in_256ths( fx value )
{
    if ( value < -I2FX( 250 ) || value > I2FX( 250 ) ) {
        return 64000U;
    }
    return (uint16_t)( ( value < 0 ? -value : value ) >> 8 );
}

static void note( uint16_t uni, fx xpos, fx ypos, fx advance, fx size )
{
    TCHAR *grown;

    if ( uni == 0 || xpos < -I2FX( 500 ) || ypos < -I2FX( 500 ) || xpos > I2FX( 20000 ) ||
         ypos > I2FX( 20000 ) ) {
        return;
    }
    if ( char_count == char_cap ) {
        grown = (TCHAR *)pool_big( doc.page_pool, (char_cap ? char_cap * 2 : 2048) * sizeof( TCHAR ) );
        if ( chars ) {
            mem_cpy( grown, chars, char_count * sizeof( TCHAR ) );
            pool_unbig( chars );
        }
        chars = grown;
        char_cap = char_cap ? char_cap * 2 : 2048;
    }
    chars[char_count].xpos = xpos >> 8;
    chars[char_count].ypos = ypos >> 8;
    chars[char_count].uni = uni;
    chars[char_count].advance = in_256ths( advance );
    chars[char_count].size = in_256ths( size );
    char_count++;
}

/* by height, or across within "count" that are one line */
static void sort_chars( TCHAR *list, unsigned int count, int across )
{
    TCHAR hold;
    unsigned int gap, at, back;

    for ( gap = 1; gap < count / 3; gap = gap * 3 + 1 ) {
    }
    for ( ; gap; gap /= 3 ) {
        for ( at = gap; at < count; at++ ) {
            hold = list[at];
            for ( back = at; back >= gap &&
                  (across ? list[back - gap].xpos > hold.xpos : list[back - gap].ypos > hold.ypos);
                  back -= gap ) {
                list[back] = list[back - gap];
            }
            list[back] = hold;
        }
    }
}

TEXTPAGE *text_extract( int index )
{
    static char line[LINE_MAX + 8];
    PAGE page;
    VIEW view;
    TEXTPAGE *tp;
    TCHAR *ch;
    char **lines, cells[4];
    unsigned int start, end, at, sum = 0, seen = 0, line_cap, count = 0;
    int32_t cell, left = 0x7FFFFFFFL, top, tall, gap, prev_end, prev_y = -1, prev_tall = 0, target;
    int col, cells_count, index2, blank;

    if ( !pdf_page( index, &page ) ) {
        return NULL;
    }
    chars = NULL;
    char_count = char_cap = 0;
    view_setup( &view, &page, FX_ONE, 0 );
    text_sink = note;
    page_text( &page, &view );
    text_sink = NULL;

    tp = (TEXTPAGE *)pool_alloc( doc.page_pool, sizeof( TEXTPAGE ) );
    if ( char_count == 0 ) {
        return tp;
    }
    /* how wide a character of this page is, and where its text starts */
    for ( at = 0; at < char_count; at++ ) {
        if ( chars[at].uni > 0x20 && chars[at].advance > 64 && chars[at].advance < 64000U ) {
            sum += chars[at].advance;
            seen++;
        }
        if ( chars[at].uni > 0x20 && chars[at].xpos < left ) {
            left = chars[at].xpos;
        }
    }
    cell = seen ? (int32_t)(sum / seen) : 6 * 256;
    if ( cell < 3 * 256 ) {
        cell = 3 * 256;
    }
    if ( cell > 14 * 256 ) {
        cell = 14 * 256;
    }
    if ( left < 0 || left == 0x7FFFFFFFL ) {
        left = 0;
    }

    sort_chars( chars, char_count, 0 );
    line_cap = 64;
    lines = (char **)pool_big( doc.page_pool, line_cap * sizeof( char * ) );
    for ( start = 0; start < char_count; start = end ) {
        /* a line: everything whose baseline is within a third of the
           type's height of the first's */
        top = chars[start].ypos;
        tall = chars[start].size;
        for ( end = start + 1; end < char_count; end++ ) {
            if ( chars[end].size > tall && chars[end].size < 64000U ) {
                tall = chars[end].size;
            }
            if ( chars[end].ypos - top > (tall < 4 * 256 ? 256 : tall / 3) ) {
                break;
            }
        }
        sort_chars( chars + start, end - start, 1 );

        col = 0;
        prev_end = 0;
        for ( at = start; at < end; at++ ) {
            ch = &chars[at];
            cells_count = uni_to_cp437( ch->uni, cells );
            if ( cells_count == 0 ) {
                continue;
            }
            target = (ch->xpos - left) / cell;
            gap = ch->xpos - prev_end;
            if ( col == 0 ) {
                if ( cells[0] == ' ' ) {
                    continue;                       /* a line does not start with one */
                }
                while ( col < target && col < LINE_MAX - 8 ) {
                    line[col++] = ' ';
                }
            } else if ( gap > cell + cell / 2 ) {
                /* a wide gap: to where it would stand, and two at least */
                if ( line[col - 1] != ' ' && col < LINE_MAX - 8 ) {
                    line[col++] = ' ';
                }
                if ( cells[0] != ' ' && col < LINE_MAX - 8 ) {
                    line[col++] = ' ';
                }
                while ( col < target && col < LINE_MAX - 8 ) {
                    line[col++] = ' ';
                }
            } else if ( gap > (int32_t)ch->size / 6 && gap > cell / 5 && line[col - 1] != ' ' &&
                        cells[0] != ' ' && col < LINE_MAX - 8 ) {
                line[col++] = ' ';
            }
            if ( cells[0] == ' ' && line[col - 1] == ' ' ) {
                prev_end = ch->xpos + ch->advance;
                continue;
            }
            for ( index2 = 0; index2 < cells_count && col < LINE_MAX - 8; index2++ ) {
                line[col++] = cells[index2];
            }
            prev_end = ch->xpos + (ch->advance < 64000U ? ch->advance : 0);
        }
        while ( col > 0 && line[col - 1] == ' ' ) {
            col--;
        }
        if ( col == 0 ) {
            continue;
        }
        line[col] = 0;
        /* room between this line and the last: one empty line says so */
        blank = prev_y >= 0 && top - prev_y > (prev_tall > tall ? prev_tall : tall) * 9 / 5;
        if ( count + 2 > line_cap ) {
            char **grown = (char **)pool_big( doc.page_pool, line_cap * 2 * sizeof( char * ) );

            mem_cpy( grown, lines, count * sizeof( char * ) );
            pool_unbig( lines );
            lines = grown;
            line_cap *= 2;
        }
        if ( blank ) {
            lines[count++] = "";
        }
        lines[count++] = pool_str( doc.page_pool, line );
        prev_y = top;
        prev_tall = tall;
    }
    pool_unbig( chars );
    chars = NULL;
    tp->count = (int)count;
    tp->lines = lines;
    return tp;
}
