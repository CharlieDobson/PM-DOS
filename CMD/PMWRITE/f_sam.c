/*
 * F_SAM.C - Ami Pro (.SAM), releases 2 and 3.
 *
 * THE FILE IS TEXT.  It starts with "[ver]" and a number, and goes on
 * in sections, each a name in square brackets on a line of its own and
 * then its values, one to a line behind a tab (two tabs inside a
 * section that is part of another).  "[tag]" is a paragraph style,
 * with its font in "[fnt]", its alignment and indents in "[algn]", its
 * spacing in "[spc]", its page breaks in "[brk]" and its bullet in
 * "[spec]".  "[lay]" is the page: "[rght]" has the paper, the margins
 * and the tab stops, and "[hrght]" and "[frght]" each hold a frame
 * with the running header's and footer's text after "[txt]".  After
 * "[edoc]" comes the text, to a line that is ">" and nothing else.
 *
 * IN THE TEXT a paragraph is one or more lines - Ami Pro breaks a long
 * one wherever it has reached some 250 bytes, in the middle of a word
 * if need be, so the lines are joined with nothing between - and an
 * empty line ends it.  "@Name@" at its start is its style; with none
 * it is "Body Text".  Marks are in angle brackets: <+!> and <-!> turn
 * bold on and off (" italic, # underline, $ word underline, % strike
 * through, & superscript, ' subscript, ) double underline), <+B>
 * centers the paragraph (@ left, A right, C justified), <:f...> is a
 * size, a face and a color, and <:f> alone goes back to the style's.
 * "<<" is a "<", <;> a ">", <[> a "[", </x> the character 40h above x
 * and <\x> the character 80h above x.  The character set is Windows'.
 *
 * WHERE THESE FACTS ARE FROM.  Three files Ami Pro wrote, and for the
 * escapes and the attribute letters another program's reader.  What no
 * file at hand showed - tables, footnotes, frames, pictures, rulers in
 * the text - is not read: a mark that is not known is passed over, its
 * paragraph's text kept, and the document says that layout was lost.
 *
 * WRITING.  The same sections in the same order with the same number
 * of values as Ami Pro 3 writes.  A paragraph whose indents, spacing
 * or list are not its style's gets a style of its own ("Name~1"),
 * because a style is the only place Ami Pro keeps those; its names
 * are thirteen characters at most.  Tab stops of single paragraphs,
 * pictures and tables' rules are not written: a table is its text, a
 * paragraph for each cell, and a note's text follows the paragraph it
 * was called from, in square brackets.
 */
#include "fmt.h"

#define SAM_VALS        100     /* values of one section that are kept */
#define SAM_NAME        40      /* a style's name as the file has it */
#define SAM_STYLES      1024    /* styles of a file that are kept */
#define SAM_OUT_STYLES  250     /* styles that are written */
#define SAM_WRAP        250     /* a line of text is broken before this */
#define SAM_NAME_OUT    13      /* a style's name as Ami Pro takes it */
#define SAM_MAX_TWIPS   31680L

static const char *const sam_variants[] = {
    "Ami Pro 3.0",
    NULL
};

/* the letters of <+x> and <-x> */
static const struct {
    uint16_t flag;
    char     code;
} sam_attr[] = {
    { CF_BOLD, '!' }, { CF_ITALIC, '"' }, { CF_UNDER, '#' }, { CF_WUNDER, '$' },
    { CF_STRIKE, '%' }, { CF_SUPER, '&' }, { CF_SUB, '\'' }, { CF_DUNDER, ')' }
};
#define SAM_NATTR   (sizeof( sam_attr ) / sizeof( sam_attr[0] ))

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *text;
    unsigned int len;
} SAMVAL;

typedef struct {
    char name[SAM_NAME];
    CHP  chp;
    PAP  pap;                   /* tabs and row are NULL */
} SAMSTYLE;

typedef struct {
    const uint8_t *data;
    uint32_t     len;
    unsigned int pos;
    WDOC        *doc;
    int          codepage;
    int          nomem;
    SAMSTYLE    *style;
    unsigned int nstyle, style_cap;
    int          body_style;           /* which is "Body Text", or -1 */
    int          committed;
    SAMVAL       val[SAM_VALS];
    WTAB         tabs[MAX_TABS];       /* the page's, when they are not evenly set */
    int          ntabs;
    WTAB         frame_tabs[MAX_TABS]; /* the header's or footer's frame's */
    int          nframe_tabs;
    uint8_t     *buf;                  /* a paragraph's lines, joined */
    unsigned int blen;
} SAM;

static int sam_clamp( int value, int low, int high )
{
    return value < low ? low : value > high ? high : value;
}

static int sam_num( const uint8_t *text, unsigned int len )
{
    unsigned int index = 0, value = 0;
    int minus = 0;

    while ( index < len && (text[index] == ' ' || text[index] == '\t') ) {
        index++;
    }
    if ( index < len && text[index] == '-' ) {
        minus = 1;
        index++;
    }
    while ( index < len && text[index] >= '0' && text[index] <= '9' ) {
        if ( value < 100000000UL ) {
            value = value * 10 + (unsigned int)(text[index] - '0');
        }
        index++;
    }
    return minus ? -(int)value : (int)value;
}

/* the next line without its end -> 0 at the end of the file */
static int sam_line( SAM *sam, const uint8_t **text, unsigned int *len )
{
    unsigned int start = sam->pos, end;

    if ( start >= sam->len ) {
        return 0;
    }
    end = start;
    while ( end < sam->len && sam->data[end] != '\r' && sam->data[end] != '\n' ) {
        end++;
    }
    *text = sam->data + start;
    *len = end - start;
    if ( end < sam->len && sam->data[end] == '\r' ) {
        end++;
    }
    if ( end < sam->len && sam->data[end] == '\n' ) {
        end++;
    }
    sam->pos = end;
    return 1;
}

/* a line that names a section -> the name and its length, or NULL */
static const uint8_t *sam_section( const uint8_t *text, unsigned int len, unsigned int *name_len )
{
    unsigned int start = 0, end;

    while ( start < len && text[start] == '\t' ) {
        start++;
    }
    if ( start >= len || text[start] != '[' ) {
        return NULL;
    }
    start++;
    end = start;
    while ( end < len && text[end] != ']' ) {
        end++;
    }
    if ( end >= len ) {
        return NULL;
    }
    *name_len = end - start;
    return text + start;
}

static int sam_is( const uint8_t *name, unsigned int len, const char *want )
{
    return str_len( want ) == len && mem_cmp( name, want, len ) == 0;
}

/* a section's values, to the line that names the next -> how many; the
   first SAM_VALS are in sam->val, without the tabs before them */
static int sam_values( SAM *sam )
{
    const uint8_t *text;
    unsigned int len, mark, skip, name_len;
    int count = 0;

    for ( ;; ) {
        mark = sam->pos;
        if ( !sam_line( sam, &text, &len ) ) {
            break;
        }
        if ( sam_section( text, len, &name_len ) != NULL ) {
            sam->pos = mark;
            break;
        }
        skip = 0;
        while ( skip < len && text[skip] == '\t' ) {
            skip++;
        }
        if ( count < SAM_VALS ) {
            sam->val[count].text = text + skip;
            sam->val[count].len = len - skip;
            count++;
        }
    }
    return count;
}

static int sam_val( const SAM *sam, int count, int index )
{
    return index < count ? sam_num( sam->val[index].text, sam->val[index].len ) : 0;
}

/* ASCII of a file, cut to fit and ended */
static void sam_copy( char *dst, unsigned int max, const uint8_t *text, unsigned int len )
{
    unsigned int index;

    for ( index = 0; index + 1 < max && index < len; index++ ) {
        dst[index] = (char)(text[index] >= 0x20 && text[index] < 0x7F ? text[index] : '?');
    }
    dst[index] = 0;
}

static SAMSTYLE *sam_style_new( SAM *sam, const uint8_t *name, unsigned int len )
{
    SAMSTYLE *fresh;
    unsigned int cap;

    if ( sam->nstyle >= SAM_STYLES ) {
        return NULL;
    }
    if ( sam->nstyle >= sam->style_cap ) {
        cap = sam->style_cap + 16;
        fresh = (SAMSTYLE *)try_realloc( sam->style, cap * sizeof( SAMSTYLE ) );
        if ( fresh == NULL ) {
            sam->nomem = 1;
            return NULL;
        }
        sam->style = fresh;
        sam->style_cap = cap;
    }
    fresh = &sam->style[sam->nstyle++];
    mem_set( fresh, 0, sizeof( SAMSTYLE ) );
    sam_copy( fresh->name, SAM_NAME, name, len );
    fresh->chp = sam->doc->style[0].chp;
    pap_init( &fresh->pap );
    return fresh;
}

/* "[fnt]": the face, the size in twips, the color, and the attributes
   with the kind of face in the top two bits */
static void sam_fnt( SAM *sam, SAMSTYLE *style, int count )
{
    char face[FONT_NAME_LEN];
    static const uint8_t family[4] = { FAM_ANY, FAM_SWISS, FAM_MODERN, FAM_ROMAN };
    uint32_t flags = (uint32_t)sam_val( sam, count, 3 ), color = (uint32_t)sam_val( sam, count, 2 );
    int size = sam_val( sam, count, 1 );

    if ( count < 4 ) {
        return;
    }
    sam_copy( face, FONT_NAME_LEN, sam->val[0].text, sam->val[0].len );
    if ( face[0] ) {
        style->chp.font = (uint16_t)wd_font( sam->doc, face, family[(flags >> 14) & 3],
                                             ((flags >> 14) & 3) == 2 ? 1 : -1 );
    }
    if ( size >= 20 ) {
        style->chp.size = (uint16_t)(sam_clamp( size, 20, 16380 ) / 10);
    }
    style->chp.color = color == 0 ? COLOR_AUTO : RGB( color & 255, (color >> 8) & 255, (color >> 16) & 255 );
    style->chp.flags = 0;
    if ( flags & 1 ) {
        style->chp.flags |= CF_BOLD;
    }
    if ( flags & 2 ) {
        style->chp.flags |= CF_ITALIC;
    }
    if ( flags & 4 ) {
        style->chp.flags |= CF_UNDER;
    }
    if ( flags & 8 ) {
        style->chp.flags |= CF_WUNDER;
    }
    if ( flags & 64 ) {
        style->chp.flags |= CF_DUNDER;
    }
}

/* "[algn]": which side, a 1, and the indents of every line, of the
   first line, and of the rest */
static void sam_algn( SAM *sam, SAMSTYLE *style, int count )
{
    int32_t flags = sam_val( sam, count, 0 );
    int every = sam_clamp( sam_val( sam, count, 2 ), 0, SAM_MAX_TWIPS );
    int first = sam_clamp( sam_val( sam, count, 3 ), 0, SAM_MAX_TWIPS );
    int rest = sam_clamp( sam_val( sam, count, 4 ), 0, SAM_MAX_TWIPS );

    style->pap.align = (uint8_t)(flags & 8 ? AL_JUSTIFY : flags & 4 ? AL_CENTER : flags & 2 ? AL_RIGHT : AL_LEFT);
    style->pap.left = every + rest;
    style->pap.first = first - rest;
}

/* "[spc]": single, one and a half, double or a height of its own; the
   height; a 1; the space above; the space below */
static void sam_spc( SAM *sam, SAMSTYLE *style, int count )
{
    int32_t flags = sam_val( sam, count, 0 );

    style->pap.line_rule = LS_MULTIPLE;
    style->pap.line = 240;
    if ( flags & 8 ) {
        style->pap.line_rule = LS_EXACT;
        style->pap.line = (uint16_t)sam_clamp( sam_val( sam, count, 1 ), 20, SAM_MAX_TWIPS );
    } else if ( flags & 4 ) {
        style->pap.line = 480;
    } else if ( flags & 2 ) {
        style->pap.line = 360;
    }
    style->pap.before = (uint16_t)sam_clamp( sam_val( sam, count, 3 ), 0, SAM_MAX_TWIPS );
    style->pap.after = (uint16_t)sam_clamp( sam_val( sam, count, 4 ), 0, SAM_MAX_TWIPS );
}

static void sam_brk( SAM *sam, SAMSTYLE *style, int count )
{
    int32_t flags = sam_val( sam, count, 0 );

    style->pap.flags &= (uint16_t)~(PF_PAGEBEFORE | PF_KEEPNEXT | PF_KEEP);
    if ( flags & 1 ) {
        style->pap.flags |= PF_PAGEBEFORE;
    }
    if ( flags & 64 ) {
        style->pap.flags |= PF_KEEPNEXT;
    }
    if ( count > 0 && !(flags & 4) ) {
        style->pap.flags |= PF_KEEP;
    }
}

/* "[spec]": its third value is what stands before the paragraph, with
   <*:> in it where a number goes */
static void sam_spec( SAM *sam, SAMSTYLE *style, int count )
{
    style->pap.list = LIST_NONE;
    if ( count > 2 && sam->val[2].len > 0 ) {
        style->pap.list = (uint8_t)(mem_find( sam->val[2].text, sam->val[2].len, "<*:>" ) >= 0 ? LIST_NUMBER
                                                                                         : LIST_BULLET);
    }
}

/* tab stops as "[rght]" and "[frmlay]" list them: a kind and a place */
static int sam_tabs( const SAM *sam, int count, int at, WTAB *tabs )
{
    static const uint8_t kinds[5] = { TAB_LEFT, TAB_LEFT, TAB_CENTER, TAB_RIGHT, TAB_DECIMAL };
    int want = sam_val( sam, count, at ), kind;
    int index, have = 0;

    for ( index = 0; index < want && have < MAX_TABS && at + 2 + index * 2 < count; index++ ) {
        kind = sam_val( sam, count, at + 1 + index * 2 );
        tabs[have].pos = sam_clamp( sam_val( sam, count, at + 2 + index * 2 ), 0, SAM_MAX_TWIPS );
        tabs[have].kind = kinds[kind >= 0 && kind <= 4 ? kind : 0];
        tabs[have].lead = LEAD_NONE;
        tabs[have].spare = 0;
        have++;
    }
    return have;
}

/* "[rght]": height, width, a 1, then the left, bottom, (a 1), top and
   right margins, six more, how many columns and each one's edges, how
   many tab stops and each one */
static void sam_rght( SAM *sam, int count )
{
    PAGESETUP *page = &sam->doc->page;
    int columns;
    int at, index, even = 1;

    if ( count < 8 ) {
        return;
    }
    page->height = sam_val( sam, count, 0 );
    page->width = sam_val( sam, count, 1 );
    page->left = sam_val( sam, count, 3 );
    page->bottom = sam_val( sam, count, 4 );
    page->top = sam_val( sam, count, 6 );
    page->right = sam_val( sam, count, 7 );
    page->landscape = (uint8_t)(page->width > page->height);
    columns = sam_clamp( sam_val( sam, count, 14 ), 0, 16 );
    if ( columns > 1 ) {
        sam->doc->lost |= LOST_LAYOUT;
    }
    at = 15 + (int)columns * 2;
    sam->ntabs = sam_tabs( sam, count, at, sam->tabs );
    /* stops set evenly from the margin are what other programs call
       the default tabs */
    for ( index = 0; index < sam->ntabs; index++ ) {
        if ( sam->tabs[index].kind != TAB_LEFT || sam->tabs[index].pos != sam->tabs[0].pos * (index + 1) ) {
            even = 0;
        }
    }
    if ( sam->ntabs >= 2 && even && sam->tabs[0].pos > 0 ) {
        sam->doc->deftab = sam->tabs[0].pos;
        sam->ntabs = 0;
    }
}

/* the styles, once all of them are read, into the document */
static void sam_commit( SAM *sam )
{
    static const struct {
        const char *name;
        int kind;
    } known[] = {
        { "Body Text", STY_NORMAL }, { "Title", STY_TITLE }, { "Header", STY_HEADER },
        { "Footer", STY_FOOTER }, { "Footnote", STY_FOOTNOTE },
        { "Heading 1", STY_HEADING1 }, { "Heading 2", STY_HEADING2 }, { "Heading 3", STY_HEADING3 },
        { "Heading 4", STY_HEADING4 }, { "Heading 5", STY_HEADING5 }, { "Heading 6", STY_HEADING6 }
    };
    SAMSTYLE *style;
    WSTYLE *into;
    unsigned int index, look;
    int kind, number;

    if ( sam->committed ) {
        return;
    }
    sam->committed = 1;
    sam->body_style = -1;
    for ( index = 0; index < sam->nstyle; index++ ) {
        style = &sam->style[index];
        kind = STY_OTHER;
        for ( look = 0; look < sizeof( known ) / sizeof( known[0] ); look++ ) {
            if ( str_icmp( style->name, known[look].name ) == 0 ) {
                kind = known[look].kind;
            }
        }
        number = wd_style( sam->doc, style->name, kind );
        if ( number == 0 && kind != STY_NORMAL ) {
            continue;                   /* the document has no room for it */
        }
        if ( kind == STY_NORMAL && sam->body_style < 0 ) {
            sam->body_style = (int)index;
        }
        style->pap.style = (uint8_t)number;
        into = &sam->doc->style[number];
        into->chp = style->chp;
        into->pap = style->pap;
    }
}

/* the next paragraph's lines as one piece in sam->buf -> 0 when the
   text has ended */
static int sam_join( SAM *sam )
{
    const uint8_t *text;
    unsigned int len, mark;

    sam->blen = 0;
    for ( ;; ) {
        if ( !sam_line( sam, &text, &len ) ) {
            return 0;
        }
        if ( len == 1 && text[0] == '>' ) {
            return 0;
        }
        if ( len != 0 ) {
            break;
        }
    }
    for ( ;; ) {
        /* the buffer is as long as the file: every line fits */
        mem_cpy( sam->buf + sam->blen, text, len );
        sam->blen += len;
        mark = sam->pos;
        if ( !sam_line( sam, &text, &len ) || len == 0 ) {
            break;
        }
        if ( len == 1 && text[0] == '>' ) {
            sam->pos = mark;
            break;
        }
    }
    return 1;
}

static void sam_byte( SAM *sam, WBUILD *wb, uint32_t byte )
{
    wb_char( wb, byte < 0x80 ? byte : cp_to_uni( sam->codepage, (int)(byte & 0xFF) ) );
}

/* "<:f" size "," a digit and the face "," red "," green "," blue ">",
   any of them left out */
static void sam_font( SAM *sam, WBUILD *wb, const uint8_t *text, unsigned int len, const CHP *base )
{
    char face[FONT_NAME_LEN];
    unsigned int start[5], size[5], index, field = 0;
    uint32_t rgb[3];

    if ( len == 0 ) {
        wb->chp.font = base->font;
        wb->chp.size = base->size;
        wb->chp.color = base->color;
        return;
    }
    start[0] = 0;
    for ( index = 0; index < len; index++ ) {
        if ( text[index] == ',' ) {
            size[field] = index - start[field];
            if ( field == 4 ) {
                break;
            }
            start[++field] = index + 1;
        }
    }
    if ( index >= len ) {
        size[field] = len - start[field];
    }
    if ( size[0] > 0 && sam_num( text, size[0] ) >= 20 ) {
        wb->chp.size = (uint16_t)(sam_clamp( sam_num( text, size[0] ), 20, 16380 ) / 10);
    }
    if ( field >= 1 && size[1] > 0 ) {
        /* what comes before the name is a digit or "!", not part of it */
        if ( text[start[1]] >= 0x21 && text[start[1]] <= 0x3F ) {
            start[1]++;
            size[1]--;
        }
        sam_copy( face, FONT_NAME_LEN, text + start[1], size[1] );
        if ( face[0] ) {
            wb->chp.font = (uint16_t)wd_font( sam->doc, face, FAM_ANY, -1 );
        }
    }
    if ( field >= 4 && size[2] > 0 ) {
        for ( index = 0; index < 3; index++ ) {
            rgb[index] = (uint32_t)sam_num( text + start[2 + index], size[2 + index] ) & 255;
        }
        wb->chp.color = (rgb[0] | rgb[1] | rgb[2]) == 0 ? COLOR_AUTO : RGB( rgb[0], rgb[1], rgb[2] );
    }
}

/* what is between a "<" and its ">" */
static void sam_mark( SAM *sam, WBUILD *wb, const uint8_t *text, unsigned int len, const CHP *base )
{
    unsigned int index;

    if ( len == 0 ) {
        return;
    }
    if ( len == 1 ) {
        if ( text[0] == ';' ) {
            wb_char( wb, '>' );
        } else if ( text[0] == '[' ) {
            wb_char( wb, '[' );
        }
        return;
    }
    if ( len == 2 && text[0] == '\\' ) {
        sam_byte( sam, wb, (uint32_t)text[1] | 0x80 );
        return;
    }
    if ( len == 2 && text[0] == '/' ) {
        sam_byte( sam, wb, ((uint32_t)text[1] + 0x40) & 0xFF );
        return;
    }
    if ( len == 2 && (text[0] == '+' || text[0] == '-') ) {
        for ( index = 0; index < SAM_NATTR; index++ ) {
            if ( (uint8_t)sam_attr[index].code == text[1] ) {
                if ( text[0] == '+' ) {
                    wb->chp.flags |= sam_attr[index].flag;
                } else {
                    wb->chp.flags &= (uint16_t)~sam_attr[index].flag;
                }
                return;
            }
        }
        if ( text[0] == '+' && text[1] >= '@' && text[1] <= 'C' ) {
            static const uint8_t sides[4] = { AL_LEFT, AL_RIGHT, AL_CENTER, AL_JUSTIFY };

            wb->pap.align = sides[text[1] - '@'];
        }
        return;
    }
    if ( text[0] != ':' ) {
        return;
    }
    switch ( text[1] ) {
    case 'f':
        sam_font( sam, wb, text + 2, len - 2, base );
        break;
    case 'p':
        wb_char( wb, WCH_PAGE );
        break;
    case 'P':
        wb_char( wb, WCH_PAGENUM );
        break;
    case '#':                           /* the paragraph's size when it was last set out */
    case 's':
        break;
    default:
        sam->doc->lost |= LOST_LAYOUT;
        break;
    }
}

/* the paragraph in sam->buf */
static void sam_para( SAM *sam, WBUILD *wb, const WTAB *tabs, int ntabs )
{
    const uint8_t *buf = sam->buf;
    const SAMSTYLE *style = sam->body_style >= 0 ? &sam->style[sam->body_style] : NULL;
    CHP base;
    unsigned int len = sam->blen, pos = 0, end, depth, index;

    if ( len >= 2 && buf[0] == '@' && buf[1] != '@' ) {
        end = 1;
        while ( end < len && buf[end] != '@' ) {
            end++;
        }
        if ( end < len ) {
            for ( index = 0; index < sam->nstyle; index++ ) {
                if ( str_len( sam->style[index].name ) == end - 1 &&
                     str_nicmp( sam->style[index].name, (const char *)buf + 1, end - 1 ) == 0 ) {
                    style = &sam->style[index];
                    break;
                }
            }
            pos = end + 1;
        }
    } else if ( len >= 2 && buf[0] == '@' ) {
        pos = 1;                        /* "@@": a paragraph that starts with one */
    }
    pap_free( &wb->pap );
    if ( style != NULL ) {
        wb->pap = style->pap;
        base = style->chp;
    } else {
        wb->pap = sam->doc->style[0].pap;
        base = sam->doc->style[0].chp;
    }
    wb->pap.tabs = NULL;
    wb->pap.row = NULL;
    wb->pap.ntabs = 0;
    if ( ntabs > 0 && !pap_set_tabs( &wb->pap, tabs, ntabs ) ) {
        sam->nomem = 1;
    }
    wb->chp = base;
    while ( pos < len ) {
        if ( buf[pos] != '<' ) {
            if ( buf[pos] == '\t' ) {
                wb_char( wb, WCH_TAB );
            } else if ( buf[pos] >= 0x20 ) {
                sam_byte( sam, wb, buf[pos] );
            }
            pos++;
            continue;
        }
        if ( pos + 1 < len && buf[pos + 1] == '<' ) {
            wb_char( wb, '<' );
            pos += 2;
            continue;
        }
        /* to the ">" that closes it: a mark can have marks inside */
        end = pos + 1;
        depth = 1;
        while ( end < len ) {
            if ( buf[end] == '<' ) {
                if ( end + 1 < len && buf[end + 1] == '<' ) {
                    end += 2;
                    continue;
                }
                depth++;
            } else if ( buf[end] == '>' && --depth == 0 ) {
                break;
            }
            end++;
        }
        if ( end >= len ) {
            wb_char( wb, '<' );         /* never closed: it was text */
            pos++;
            continue;
        }
        sam_mark( sam, wb, buf + pos + 1, end - pos - 1, &base );
        pos = end + 1;
    }
    wb_par( wb );
}

/* text, to the line that is ">": into a story, or nowhere */
static void sam_text( SAM *sam, STORY *story, const WTAB *tabs, int ntabs )
{
    WBUILD wb;

    if ( story == NULL ) {
        while ( sam_join( sam ) ) {
        }
        return;
    }
    wb_init( &wb, sam->doc, story );
    while ( sam_join( sam ) ) {
        sam_para( sam, &wb, tabs, ntabs );
    }
    wb_done( &wb );
}

static int sam_detect( const uint8_t *data, uint32_t len, const char *ext )
{
    unsigned int pos = 0;

    (void)ext;
    if ( len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF ) {
        pos = 3;
    }
    while ( pos < len && (data[pos] == '\r' || data[pos] == '\n' || data[pos] == ' ' || data[pos] == '\t') ) {
        pos++;
    }
    if ( !IN_RANGE( pos, 5, len ) || mem_cmp( data + pos, "[ver]", 5 ) != 0 ) {
        return 0;
    }
    if ( mem_find( data + pos, len - pos, "[sty]" ) >= 0 || mem_find( data + pos, len - pos, "[edoc]" ) >= 0 ) {
        return 100;
    }
    return 0;
}

static int sam_load( const uint8_t *data, uint32_t len, WDOC *doc )
{
    SAM *sam;
    SAMSTYLE *style = NULL;
    const uint8_t *text, *name;
    unsigned int tlen, nlen;
    int count, where, lays = 0, part = 0, code, rc;

    if ( sam_detect( data, len, "" ) == 0 ) {
        return WE_FORMAT;
    }
    sam = (SAM *)try_alloc( sizeof( SAM ) );
    if ( sam == NULL ) {
        return WE_NOMEM;
    }
    mem_set( sam, 0, sizeof( SAM ) );
    sam->buf = (uint8_t *)try_alloc( len + 1 );
    if ( sam->buf == NULL ) {
        xfree( sam );
        return WE_NOMEM;
    }
    sam->data = data;
    sam->len = len;
    sam->doc = doc;
    sam->codepage = CP_1252;
    sam->body_style = -1;
    while ( sam_line( sam, &text, &tlen ) ) {
        name = sam_section( text, tlen, &nlen );
        if ( name == NULL ) {
            continue;
        }
        if ( sam_is( name, nlen, "edoc" ) ) {
            sam_commit( sam );
            sam_text( sam, &doc->body, sam->tabs, sam->ntabs );
            break;
        }
        if ( sam_is( name, nlen, "charset" ) ) {
            /* "ANSI (Windows, IBM CP 1252)" */
            count = sam_values( sam );
            if ( count >= 2 ) {
                where = mem_find( sam->val[1].text, sam->val[1].len, "CP " );
                if ( where >= 0 ) {
                    code = (int)sam_num( sam->val[1].text + where + 3, sam->val[1].len - (unsigned int)where - 3 );
                    if ( code != CP_ASCII && cp_known( code ) ) {
                        sam->codepage = code;
                    }
                }
            }
        } else if ( sam_is( name, nlen, "tag" ) ) {
            count = sam_values( sam );
            style = count > 0 && sam->val[0].len > 0 ? sam_style_new( sam, sam->val[0].text, sam->val[0].len )
                                                     : NULL;
        } else if ( sam_is( name, nlen, "fnt" ) ) {
            count = sam_values( sam );
            if ( style != NULL ) {
                sam_fnt( sam, style, count );
            }
        } else if ( sam_is( name, nlen, "algn" ) ) {
            count = sam_values( sam );
            if ( style != NULL ) {
                sam_algn( sam, style, count );
            }
        } else if ( sam_is( name, nlen, "spc" ) ) {
            count = sam_values( sam );
            if ( style != NULL ) {
                sam_spc( sam, style, count );
            }
        } else if ( sam_is( name, nlen, "brk" ) ) {
            count = sam_values( sam );
            if ( style != NULL ) {
                sam_brk( sam, style, count );
            }
        } else if ( sam_is( name, nlen, "spec" ) ) {
            count = sam_values( sam );
            if ( style != NULL ) {
                sam_spec( sam, style, count );
            }
        } else if ( sam_is( name, nlen, "lay" ) ) {
            sam_commit( sam );
            style = NULL;
            lays++;
            part = 0;
        } else if ( sam_is( name, nlen, "rght" ) ) {
            count = sam_values( sam );
            if ( lays == 1 ) {
                sam_rght( sam, count );
            }
        } else if ( sam_is( name, nlen, "hrght" ) ) {
            part = 1;
        } else if ( sam_is( name, nlen, "frght" ) ) {
            part = 2;
        } else if ( sam_is( name, nlen, "frmlay" ) ) {
            /* as "[rght]"; the header is as far down as its frame's top
               margin, the footer as far up as its frame's bottom one */
            count = sam_values( sam );
            sam->nframe_tabs = 0;
            if ( lays == 1 && part != 0 && count >= 8 ) {
                if ( part == 1 ) {
                    doc->page.header = sam_val( sam, count, 6 );
                } else {
                    doc->page.footer = sam_val( sam, count, 4 );
                }
                sam->nframe_tabs = sam_tabs( sam, count, 15 + (int)sam_clamp( sam_val( sam, count, 14 ), 0, 16 ) * 2,
                                             sam->frame_tabs );
            }
        } else if ( sam_is( name, nlen, "txt" ) ) {
            if ( lays == 1 && part == 1 && doc->header.count == 0 ) {
                sam_text( sam, &doc->header, sam->frame_tabs, sam->nframe_tabs );
            } else if ( lays == 1 && part == 2 && doc->footer.count == 0 ) {
                sam_text( sam, &doc->footer, sam->frame_tabs, sam->nframe_tabs );
            } else {
                sam_text( sam, NULL, NULL, 0 );
            }
            part = 0;
        } else if ( sam_is( name, nlen, "elay" ) ) {
            part = 0;
        }
    }
    rc = sam->nomem || wd_failed( doc ) ? WE_NOMEM : WE_OK;
    xfree( sam->style );
    xfree( sam->buf );
    xfree( sam );
    return rc;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

/* a style as it is written: one of the document's, or one made for
   paragraphs that are not as their style says */
typedef struct {
    uint8_t  base;              /* the document's style it is, or came from */
    uint8_t  list, line_rule, used;
    uint16_t flags, before, after, line;
    int      left, first;
    char     name[SAM_NAME_OUT + 1];
} SAMOUT;

typedef struct {
    WDOC        *doc;
    OUT         *out;
    SAMOUT      *sty;
    unsigned int nsty;
    unsigned int made;          /* how many were made for paragraphs */
    unsigned int col;           /* bytes on the line being written */
} SAMW;

/* what of a PAP a style of Ami Pro's can hold, as it will be read back */
static void samw_key( const PAP *pap, int base, SAMOUT *key )
{
    mem_set( key, 0, sizeof( SAMOUT ) );
    key->base = (uint8_t)base;
    key->list = (uint8_t)(pap->list == LIST_NONE ? LIST_NONE : pap->list == LIST_BULLET ? LIST_BULLET : LIST_NUMBER);
    key->flags = (uint16_t)(pap->flags & (PF_PAGEBEFORE | PF_KEEPNEXT | PF_KEEP));
    key->before = (uint16_t)sam_clamp( pap->before, 0, SAM_MAX_TWIPS );
    key->after = (uint16_t)sam_clamp( pap->after, 0, SAM_MAX_TWIPS );
    if ( pap->line_rule == LS_MULTIPLE ) {
        key->line_rule = LS_MULTIPLE;
        key->line = (uint16_t)(pap->line < 300 ? 240 : pap->line < 420 ? 360 : 480);
    } else {
        key->line_rule = LS_EXACT;
        key->line = (uint16_t)sam_clamp( pap->line, 20, SAM_MAX_TWIPS );
    }
    key->left = sam_clamp( pap->left, 0, SAM_MAX_TWIPS );
    key->first = sam_clamp( pap->first, -key->left, SAM_MAX_TWIPS );
}

static int samw_same( const SAMOUT *lhs, const SAMOUT *rhs )
{
    return lhs->base == rhs->base && lhs->list == rhs->list && lhs->line_rule == rhs->line_rule &&
           lhs->flags == rhs->flags && lhs->before == rhs->before && lhs->after == rhs->after &&
           lhs->line == rhs->line && lhs->left == rhs->left && lhs->first == rhs->first;
}

/* the style a paragraph is written with, made if there is none yet */
static unsigned int samw_style( SAMW *sw, const PAP *pap )
{
    SAMOUT key, *fresh;
    char cut[SAM_NAME_OUT + 1];
    unsigned int index, base = pap->style < sw->doc->nstyle ? pap->style : 0;

    samw_key( pap, (int)base, &key );
    for ( index = 0; index < sw->nsty; index++ ) {
        if ( samw_same( &sw->sty[index], &key ) ) {
            sw->sty[index].used = 1;
            return index;
        }
    }
    if ( sw->nsty >= SAM_OUT_STYLES ) {
        sw->sty[base].used = 1;
        return base;
    }
    fresh = &sw->sty[sw->nsty];
    *fresh = key;
    fresh->used = 1;
    sw->made++;
    str_cpyt( cut, sw->sty[base].name, sw->made < 10 ? SAM_NAME_OUT - 1 : sw->made < 100 ? SAM_NAME_OUT - 2
                                                                                       : SAM_NAME_OUT - 3 );
    sfmt( fresh->name, sizeof( fresh->name ), "%s~%u", cut, sw->made );
    return sw->nsty++;
}

/* bytes of text that must stay on one line */
static void samw_tok( SAMW *sw, const char *text, unsigned int len )
{
    if ( sw->col > 0 && sw->col + len > SAM_WRAP ) {
        out_str( sw->out, "\r\n" );
        sw->col = 0;
    }
    out_put( sw->out, text, len );
    sw->col += len;
}

static void samw_str( SAMW *sw, const char *text )
{
    samw_tok( sw, text, str_len( text ) );
}

static void samw_byte( SAMW *sw, uint32_t byte )
{
    char esc[4];

    if ( byte == '<' ) {
        samw_str( sw, "<<" );
    } else if ( byte == '>' ) {
        samw_str( sw, "<;>" );
    } else if ( byte == '[' ) {
        samw_str( sw, "<[>" );
    } else if ( byte >= 0x80 && (byte & 0x3F) != 0x3F ) {
        esc[0] = '<';
        esc[1] = (char)(byte < 0xC0 ? '/' : '\\');
        esc[2] = (char)(byte < 0xC0 ? byte - 0x40 : byte & 0x7F);
        esc[3] = '>';
        samw_tok( sw, esc, 4 );
    } else if ( byte >= 0x20 ) {
        esc[0] = (char)byte;
        samw_tok( sw, esc, 1 );
    }
}

static void samw_char( SAMW *sw, uint32_t code )
{
    char bytes[8];
    int count, index;

    if ( code == WCH_TAB ) {
        samw_str( sw, "\t" );
    } else if ( code == WCH_PAGE ) {
        samw_str( sw, "<:p<* >>" );
    } else if ( code == WCH_PAGENUM ) {
        samw_str( sw, "<:P10,0,>" );
    } else if ( code == WCH_LINE ) {
        samw_str( sw, " " );
    } else if ( code == WCH_NBHYPHEN ) {
        samw_str( sw, "-" );
    } else if ( code == WCH_OBJ || code == WCH_SHY || WCH_IS_FIELD( code ) || code < 0x20 ) {
        return;
    } else {
        count = cp_put( CP_1252, code, bytes );
        for ( index = 0; index < count; index++ ) {
            samw_byte( sw, (uint8_t)bytes[index] );
        }
    }
}

static void samw_story( SAMW *sw, STORY *story, int depth );

/* a paragraph: "lead" and "trail" are the brackets round a note */
static void samw_para( SAMW *sw, WPARA *para, int depth, int lead, int trail )
{
    static const char sides[4] = { '@', 'B', 'A', 'C' };    /* AL_LEFT, CENTER, RIGHT, JUSTIFY */
    char mark[FONT_NAME_LEN + 48], face[FONT_NAME_LEN];
    const SAMOUT *sty = &sw->sty[samw_style( sw, &para->pap )];
    const CHP *base = &sw->doc->style[sty->base].chp, *chp;
    CHP now;
    unsigned int pos = 0, run, end;
    uint32_t code, index, color;
    uint16_t flags;
    int obj;

    sw->col = 0;
    sfmt( mark, sizeof( mark ), "@%s@", sty->name );
    samw_str( sw, mark );
    if ( para->pap.align != sw->doc->style[sty->base].pap.align && para->pap.align < 4 ) {
        sfmt( mark, sizeof( mark ), "<+%c>", sides[para->pap.align] );
        samw_str( sw, mark );
    }
    if ( lead ) {
        samw_char( sw, '[' );
    }
    now = *base;
    while ( (run = wd_run( para, pos )) != 0 ) {
        chp = wd_chp_get( sw->doc, para->attr[pos] );
        color = chp->color == 0 ? COLOR_AUTO : chp->color;
        if ( chp->font != now.font || chp->size != now.size || color != now.color ) {
            if ( chp->font == base->font && chp->size == base->size && color == base->color ) {
                samw_str( sw, "<:f>" );
            } else {
                str_cpyt( face, sw->doc->font[chp->font < sw->doc->nfont ? chp->font : 0].name, FONT_NAME_LEN );
                for ( index = 0; face[index]; index++ ) {
                    if ( face[index] == ',' || face[index] == '<' || face[index] == '>' ) {
                        face[index] = ' ';
                    }
                }
                sfmt( mark, sizeof( mark ), "<:f%u,2%s,%u,%u,%u>", (uint32_t)chp->size * 10, face,
                      color == COLOR_AUTO ? 0 : (color >> 16) & 255, color == COLOR_AUTO ? 0 : (color >> 8) & 255,
                      color == COLOR_AUTO ? 0 : color & 255 );
                samw_str( sw, mark );
            }
            now.font = chp->font;
            now.size = chp->size;
            now.color = color;
        }
        flags = chp->flags;
        for ( index = 0; index < SAM_NATTR; index++ ) {
            if ( (flags ^ now.flags) & sam_attr[index].flag ) {
                sfmt( mark, sizeof( mark ), "<%c%c>", flags & sam_attr[index].flag ? '+' : '-',
                      sam_attr[index].code );
                samw_str( sw, mark );
            }
        }
        now.flags = flags;
        end = pos + run;
        while ( pos < end ) {
            code = utf16_get( para->text, end, &pos );
            samw_char( sw, code );
        }
    }
    /* nothing is left on for the next paragraph */
    for ( index = 0; index < SAM_NATTR; index++ ) {
        if ( (now.flags ^ base->flags) & sam_attr[index].flag ) {
            sfmt( mark, sizeof( mark ), "<%c%c>", base->flags & sam_attr[index].flag ? '+' : '-',
                  sam_attr[index].code );
            samw_str( sw, mark );
        }
    }
    if ( trail ) {
        samw_str( sw, "]" );
    }
    out_str( sw->out, "\r\n\r\n" );
    sw->col = 0;
    if ( depth == 0 ) {
        for ( index = 0; index < para->nanch; index++ ) {
            obj = (int)para->anch[index].obj;
            if ( (unsigned int)obj < sw->doc->nobj && sw->doc->obj[obj].kind == OBJ_NOTE &&
                 sw->doc->obj[obj].story != NULL ) {
                samw_story( sw, sw->doc->obj[obj].story, 1 );
            }
        }
    }
}

static void samw_story( SAMW *sw, STORY *story, int depth )
{
    unsigned int index;

    for ( index = 0; index < story->count; index++ ) {
        samw_para( sw, story->para[index], depth, depth == 1 && index == 0,
                   depth == 1 && index + 1 == story->count );
    }
}

/* the styles every paragraph of a story needs, before any is written */
static void samw_collect( SAMW *sw, STORY *story, int depth )
{
    WPARA *para;
    unsigned int index, anchor;
    int obj;

    for ( index = 0; index < story->count; index++ ) {
        para = story->para[index];
        samw_style( sw, &para->pap );
        for ( anchor = 0; depth == 0 && anchor < para->nanch; anchor++ ) {
            obj = (int)para->anch[anchor].obj;
            if ( (unsigned int)obj < sw->doc->nobj && sw->doc->obj[obj].kind == OBJ_NOTE &&
                 sw->doc->obj[obj].story != NULL ) {
                samw_collect( sw, sw->doc->obj[obj].story, 1 );
            }
        }
    }
}

/* values, one to a line, each behind "depth" tabs: numbers with spaces
   between */
static void samw_vals( OUT *out, int depth, const char *values )
{
    const char *cur = values;

    while ( *cur ) {
        out_fill( out, '\t', (unsigned int)depth );
        while ( *cur && *cur != ' ' ) {
            out_byte( out, *cur++ );
        }
        out_str( out, "\r\n" );
        if ( *cur == ' ' ) {
            cur++;
        }
    }
}

static void samw_tag( SAMW *sw, const SAMOUT *sty, unsigned int key )
{
    OUT *out = sw->out;
    const CHP *chp = &sw->doc->style[sty->base].chp;
    const PAP *pap = &sw->doc->style[sty->base].pap;
    const WFONT *font = &sw->doc->font[chp->font < sw->doc->nfont ? chp->font : 0];
    uint32_t flags, color = chp->color == COLOR_AUTO ? 0 : chp->color;
    int hang = sty->first < 0 ? -sty->first : 0;

    flags = font->fixed || font->family == FAM_MODERN ? 0x8000UL : font->family == FAM_SWISS ? 0x4000UL : 0xC000UL;
    if ( chp->flags & CF_BOLD ) {
        flags |= 1;
    }
    if ( chp->flags & CF_ITALIC ) {
        flags |= 2;
    }
    if ( chp->flags & CF_UNDER ) {
        flags |= 4;
    }
    if ( chp->flags & CF_WUNDER ) {
        flags |= 8;
    }
    if ( chp->flags & CF_DUNDER ) {
        flags |= 64;
    }
    out_fmt( out, "[tag]\r\n\t%s\r\n\t%u\r\n", sty->name, key );
    out_fmt( out, "\t[fnt]\r\n\t\t%s\r\n\t\t%u\r\n\t\t%u\r\n\t\t%u\r\n", font->name, (uint32_t)chp->size * 10,
             ((color & 255) << 16) | (color & 0xFF00) | ((color >> 16) & 255), flags );
    out_fmt( out, "\t[algn]\r\n\t\t%u\r\n\t\t1\r\n\t\t%d\r\n\t\t%d\r\n\t\t%d\r\n",
             pap->align == AL_JUSTIFY ? 8 : pap->align == AL_CENTER ? 4 : pap->align == AL_RIGHT ? 2 : 1,
             sty->left - hang, sty->first < 0 ? 0 : sty->first, hang );
    out_fmt( out, "\t[spc]\r\n\t\t%u\r\n\t\t%u\r\n\t\t1\r\n\t\t%u\r\n\t\t%u\r\n\t\t1\r\n\t\t100\r\n",
             32 + (sty->line_rule == LS_EXACT ? 8 : sty->line == 480 ? 4 : sty->line == 360 ? 2 : 1),
             sty->line_rule == LS_EXACT ? (uint32_t)sty->line : (uint32_t)chp->size * 10 + (uint32_t)chp->size * 10 / 8,
             (uint32_t)sty->before, (uint32_t)sty->after );
    out_fmt( out, "\t[brk]\r\n\t\t%u\r\n", (sty->flags & PF_KEEP ? 16 : 4) + (sty->flags & PF_KEEPNEXT ? 64 : 0) +
                                          (sty->flags & PF_PAGEBEFORE ? 1 : 0) );
    out_str( out, "\t[line]\r\n" );
    samw_vals( out, 2, "8 0 1 0 1 1 1 10 10 1" );
    out_str( out, "\t[spec]\r\n" );
    samw_vals( out, 2, "0 0" );
    if ( sty->list == LIST_NONE ) {
        out_str( out, "\t\t\r\n" );
        samw_vals( out, 2, "0 1 1 0 0 0 0" );
    } else if ( sty->list == LIST_BULLET ) {
        samw_vals( out, 2, "<*0> 360 1 1 0 0 0 0" );
    } else {
        samw_vals( out, 2, "<*:>. 360 1 1 0 16 0 0" );
    }
    out_str( out, "\t[nfmt]\r\n" );
    samw_vals( out, 2, sty->list == LIST_NONE ? "280 1 2 . , $" : "272 1 2 . , $" );
    out_fmt( out, "\t%s\r\n\t0\r\n\t0\r\n", sty->name );
}

/* the paragraph whose tab stops every paragraph of a story has, or NULL
   when they differ or there are none: a frame's stops are all of its
   text's */
static const WPARA *samw_tabbed( const STORY *story )
{
    const WPARA *first;
    unsigned int index;

    if ( story->count == 0 || story->para[0]->pap.ntabs == 0 || story->para[0]->pap.tabs == NULL ) {
        return NULL;
    }
    first = story->para[0];
    for ( index = 1; index < story->count; index++ ) {
        if ( story->para[index]->pap.ntabs != first->pap.ntabs || story->para[index]->pap.tabs == NULL ||
             mem_cmp( story->para[index]->pap.tabs, first->pap.tabs, first->pap.ntabs * sizeof( WTAB ) ) != 0 ) {
            return NULL;
        }
    }
    return first;
}

/* how many tab stops, and each one's kind and place */
static void samw_tabs( OUT *out, const WPARA *para )
{
    unsigned int index;

    out_fmt( out, "\t\t%u\r\n", (uint32_t)para->pap.ntabs );
    for ( index = 0; index < para->pap.ntabs; index++ ) {
        out_fmt( out, "\t\t%u\r\n\t\t%d\r\n", (uint32_t)(para->pap.tabs[index].kind & 3) + 1,
                 sam_clamp( para->pap.tabs[index].pos, 0, SAM_MAX_TWIPS ) );
    }
}

/* a header's or footer's frame and its text.  The numbers that are not
   lengths are the ones Ami Pro 3 wrote for a frame of its own making. */
static void samw_frame( SAMW *sw, int footer, STORY *story )
{
    OUT *out = sw->out;
    const PAGESETUP *page = &sw->doc->page;
    int top = footer ? page->height - page->bottom : 0;
    int bottom = footer ? page->height : page->top;
    int width = page->width - page->left - page->right;
    const WPARA *tabbed = samw_tabbed( story );

    out_fmt( out, "\t[%s]\r\n\t[lyfrm]\r\n\t\t1\r\n\t\t%u\r\n\t\t0\r\n\t\t%d\r\n\t\t%d\r\n\t\t%d\r\n",
             footer ? "frght" : "hrght", footer ? 13248 : 11200, top, page->width, bottom );
    samw_vals( out, 2, "0 1 3" );
    out_fmt( out, "\t\t1 0 0 0 0 0 0\r\n\t\t0\r\n\t\t0\r\n\t\t%u\r\n", footer ? 2 : 1 );
    out_fmt( out, "\t[frmlay]\r\n\t\t%d\r\n\t\t%d\r\n\t\t1\r\n\t\t%d\r\n\t\t%d\r\n\t\t1\r\n\t\t%d\r\n\t\t%d\r\n",
             bottom, page->width, page->left, footer ? page->footer : 0,
             footer ? top + 72 : page->header, page->right );
    samw_vals( out, 2, "0 1 0 1 1 0 1" );
    out_fmt( out, "\t\t%d\r\n\t\t%d\r\n", page->left, page->width - page->right );
    if ( tabbed != NULL ) {
        samw_tabs( out, tabbed );
    } else {
        out_fmt( out, "\t\t2\r\n\t\t2\r\n\t\t%d\r\n\t\t3\r\n\t\t%d\r\n", width / 2, width );
    }
    out_str( out, "\t[txt]\r\n" );
    samw_story( sw, story, 2 );         /* no notes from a header */
    out_str( out, ">\r\n" );
}

static int sam_save( WDOC *doc, OUT *out, int variant )
{
    SAMW sw;
    const PAGESETUP *page = &doc->page;
    char cut[STYLE_NAME_LEN];
    unsigned int index, look, where, key = 2;
    int step;

    (void)variant;
    mem_set( &sw, 0, sizeof( sw ) );
    sw.doc = doc;
    sw.out = out;
    sw.sty = (SAMOUT *)try_alloc( SAM_OUT_STYLES * sizeof( SAMOUT ) );
    if ( sw.sty == NULL ) {
        return WE_NOMEM;
    }
    /* the document's styles first, each under a name Ami Pro can keep */
    for ( index = 0; index < doc->nstyle && index < MAX_STYLES; index++ ) {
        samw_key( &doc->style[index].pap, (int)index, &sw.sty[index] );
        str_cpyt( cut, index == 0 ? "Body Text" : doc->style[index].name, SAM_NAME_OUT + 1 );
        for ( look = 0; cut[look]; look++ ) {
            if ( cut[look] == '@' || cut[look] == '<' || cut[look] == '>' || cut[look] == '[' ||
                 cut[look] == ']' || cut[look] < 0x20 || cut[look] > 0x7E ) {
                cut[look] = '_';
            }
        }
        str_cpy( sw.sty[index].name, cut[0] ? cut : "_" );
        sw.sty[index].used = 1;
    }
    sw.nsty = index;
    samw_collect( &sw, &doc->header, 1 );
    samw_collect( &sw, &doc->footer, 1 );
    samw_collect( &sw, &doc->body, 0 );

    out_str( out, "[ver]\r\n\t4\r\n[sty]\r\n\t\r\n[files]\r\n[charset]\r\n\t82\r\n"
                  "\tANSI (Windows, IBM CP 1252)\r\n[revisions]\r\n\t0\r\n[prn]\r\n\t\r\n[lang]\r\n\t1\r\n[desc]\r\n" );
    samw_vals( out, 1, "     0 1 0 0 0 0 0 0 0       0" );
    out_str( out, "[fopts]\r\n" );
    samw_vals( out, 1, "0 1 0 0" );
    out_str( out, "[lnopts]\r\n\t2\r\n\tBody Text\r\n\t1\r\n[docopts]\r\n\t5\r\n\t2\r\n" );
    for ( index = 0; index < sw.nsty; index++ ) {
        if ( sw.sty[index].used ) {
            samw_tag( &sw, &sw.sty[index], key <= 12 ? key : 0 );
            key++;
        }
    }
    out_fmt( out, "[lay]\r\n\tStandard\r\n\t516\r\n\t[rght]\r\n\t\t%d\r\n\t\t%d\r\n\t\t1\r\n\t\t%d\r\n\t\t%d\r\n"
                  "\t\t1\r\n\t\t%d\r\n\t\t%d\r\n", page->height, page->width, page->left, page->bottom,
             page->top, page->right );
    samw_vals( out, 2, "0 1 0 1 0 2 1" );
    out_fmt( out, "\t\t%d\r\n\t\t%d\r\n", page->left, page->width - page->right );
    if ( samw_tabbed( &doc->body ) != NULL ) {
        samw_tabs( out, samw_tabbed( &doc->body ) );
    } else {
        out_str( out, "\t\t12\r\n" );
        step = sam_clamp( doc->deftab, 72, 2400 );
        for ( index = 1; index <= 12; index++ ) {
            out_fmt( out, "\t\t1\r\n\t\t%d\r\n", step * (int)index );
        }
    }
    samw_frame( &sw, 0, &doc->header );
    samw_frame( &sw, 1, &doc->footer );
    out_str( out, "[elay]\r\n[edoc]\r\n" );
    samw_story( &sw, &doc->body, 0 );
    out_str( out, ">\r\n\r\n" );
    /* the pictures would follow; with none, the number is where this
       line itself starts */
    where = out_tell( out );
    out_fmt( out, "[Embedded]\r\n%08u\r\n", where );
    xfree( sw.sty );
    return out_error( out ) ? (out_error( out ) == WE_NOMEM ? WE_NOMEM : WE_IO) : WE_OK;
}

const FORMAT fmt_amipro = {
    FMT_AMIPRO, "Ami Pro", "SAM", sam_detect, sam_load, sam_save, sam_variants, 0
};
