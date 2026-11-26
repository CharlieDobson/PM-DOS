/*
 * F_WS.C - WordStar, for CP/M and for DOS up to 7.0.
 *
 * TWO ERAS.  A WordStar file up to release 4 is seven-bit text: the
 * eighth bit of a byte is WordStar's own mark (the last letter of a
 * word that a space follows, a space it put in to justify a line, the
 * carriage return it put in to break one), and is taken off to get the
 * character.  From release 5 the file starts with a "symmetrical
 * sequence" - 1Dh, a length, a kind, ..., the length again, 1Dh - that
 * holds the version, and from then on a byte above 7Fh is a character
 * of code page 437; only the soft return (8Dh), the soft line feed
 * (8Ah) and the soft space (A0h) keep their old meaning.  The header
 * is how the eras are told apart, and the only signature there is.
 *
 * A LINE THAT STARTS WITH A DOT is a command to the printer: the page,
 * the margins, the header and footer.  The ones that have a place in
 * the document are read; the rest are dropped, except the merge-print
 * commands, which stay as text because they are the document's text
 * to the person who wrote them.
 *
 * LENGTHS are columns and lines, taken as tenths and sixths of an inch
 * (a 10-pitch printer at six lines to the inch) unless the file says
 * inches with a double quote, as release 5 and later write them.
 *
 * LOOKS are toggles in the text: ^B bold, ^S underline, ^Y italic and
 * so on, each on at its first and off at its next.
 *
 * WRITING.  "WordStar 4.0" is the old form, with the eighth bits as
 * WordStar sets them and characters outside ASCII in the 1Bh c 1Ch
 * form that release 3.4 began; every later WordStar reads it.  "7.0"
 * has the header sequence, font sequences and footnote sequences.
 * Lines are broken with soft returns at the right margin.  Pictures
 * and tables' rules are not written; a table is its text, a tab
 * between cells.
 */
#include "fmt.h"

static const char *const ws_variants[] = {
    "WordStar 4.0",
    "WordStar 7.0",
    NULL
};

#define WS_COL      144             /* a column: a tenth of an inch */
#define WS_LINE     240             /* a line: a sixth */
#define WS_DEPTH    2

/* the toggles, a bit each */
#define WT_BOLD     0x01
#define WT_DOUBLE   0x02
#define WT_UNDER    0x04
#define WT_ITALIC   0x08
#define WT_STRIKE   0x10
#define WT_SUB      0x20
#define WT_SUPER    0x40

/* the sixteen colors of a color sequence */
static const uint32_t ws_colors[16] = {
    0x000000UL, 0x0000AAUL, 0x00AA00UL, 0x00AAAAUL, 0xAA0000UL, 0xAA00AAUL, 0xAA5500UL, 0xAAAAAAUL,
    0x555555UL, 0x5555FFUL, 0x55FF55UL, 0x55FFFFUL, 0xFF5555UL, 0xFF55FFUL, 0xFFFF55UL, 0xFFFFFFUL
};

typedef struct {
    const uint8_t *data;
    uint32_t len;
    WDOC    *doc;
    WBUILD   wb;
    int      new_era;         /* release 5 or later: it has the header */
    int      depth;           /* in a note */
    uint32_t toggles;         /* WT_* that are on */
    uint8_t  wunder;          /* .ul off: spaces are not underlined */
    uint8_t  skip_next;       /* the next character overprints: dropped */
    uint8_t  line_start;      /* at column 1 after a hard return */
    uint8_t  soft_seen;       /* the paragraph has spaces WordStar added */
    uint8_t  justify;         /* 0 not said, 1 .oj on, 2 .oj off */
    uint8_t  center;          /* .oc on */
    uint8_t  page_next;
    uint8_t  text_seen;       /* the body has text: the page is settled */
    uint8_t  omit_pn;         /* .op */
    uint8_t  hdr_seen, ftr_seen;    /* a bit for each line given */
    int      col;             /* a column's width, twips */
    int      po, lm, rm, pm;  /* page offset; margins from it; .pm or -1 */
    int      page_rm;         /* the right margin the page was settled with */
    int      mt, mb, pl;
} WS;

static void ws_run( WS *ws, uint32_t pos, uint32_t end, int mode );

#define WSM_BODY    0
#define WSM_HDRFTR  1       /* '#' is the page number */
#define WSM_NOTE    2

/* ------------------------------------------------------------------ */
/* is it WordStar                                                      */
/* ------------------------------------------------------------------ */
static int ws_header( const uint8_t *data, uint32_t len )
{
    return len >= 128 && data[0] == 0x1D && LE16( data + 1 ) == 0x7D && data[3] == 0 &&
           LE16( data + 125 ) == 0x7D && data[127] == 0x1D;
}

static int ws_letter( int ch )
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

/* the dot commands that are read, and the merge-print ones that are
   kept as text: two letters each */
static const char ws_dots[] =
    "plmtmbpopwlmrmpmlspacpheh1h2h3h4h5fof1f2f3f4f5pnopojujrrtblhulocpchmfmcwigsrpsbpxxlqcvkrpf";
static const char ws_merge[] = "dfrvavsvdmcsfirpifeleigomaaw";

static int ws_in_list( const char *list, int one, int two )
{
    unsigned int index;

    for ( index = 0; list[index]; index += 2 ) {
        if ( list[index] == one && list[index + 1] == two ) {
            return 1;
        }
    }
    return 0;
}

static int ws_detect( const uint8_t *data, uint32_t len, const char *ext )
{
    uint32_t look = len < 4096 ? len : 4096;
    unsigned int index, marks = 0, strong = 0;
    int by_name = str_cmp( ext, "WS" ) == 0 || str_cmp( ext, "WSD" ) == 0;
    int byte, one, two, at_start = 1, sure;

    if ( ws_header( data, len ) ) {
        return 60;
    }
    if ( len >= 4 && data[0] == 0xFF && data[1] == 'W' && data[2] == 'P' && data[3] == 'C' ) {
        return 0;
    }
    for ( index = 0; index < look; index++ ) {
        byte = one = data[index];
        if ( one == 0 ) {
            return 0;                   /* no WordStar text has a NUL */
        }
        if ( one == 0x1A ) {
            break;
        }
        if ( at_start && one == '.' && index + 2 < look ) {
            one = ch_lower( data[index + 1] & 0x7F );
            two = ch_lower( data[index + 2] & 0x7F );
            if ( ws_in_list( ws_dots, one, two ) ) {
                marks++;
            }
        } else if ( one == 0x8D && index + 1 < look && (data[index + 1] & 0x7F) == 0x0A ) {
            strong++;                   /* a soft return */
        } else if ( one >= 0x80 && ws_letter( one & 0x7F ) && index > 0 && ws_letter( data[index - 1] ) &&
                    index + 1 < look && data[index + 1] == ' ' ) {
            strong++;                   /* the marked last letter of a word */
        } else if ( one == 0x02 || one == 0x13 || one == 0x19 ) {
            marks++;
        }
        at_start = byte == 0x0A;
    }
    if ( strong + marks == 0 ) {
        return 0;
    }
    sure = strong >= 3 ? 45 : strong + marks >= 3 ? 30 : 15;
    if ( by_name ) {
        sure += 30;
    }
    return sure > 60 ? 60 : sure;
}

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

/* a length a dot command gives: "8" is columns or lines ("unit" twips
   each), "1.25"" inches.  -1 if there is no number. */
static int ws_measure( const char *arg, int unit )
{
    unsigned int whole = 0, frac = 0, digits = 0, scale = 0;

    while ( *arg == ' ' ) {
        arg++;
    }
    while ( *arg >= '0' && *arg <= '9' ) {
        if ( whole < 10000 ) {
            whole = whole * 10 + (unsigned int)(*arg - '0');
        }
        arg++;
        digits++;
    }
    if ( *arg == '.' ) {
        arg++;
        while ( *arg >= '0' && *arg <= '9' ) {
            if ( scale < 2 ) {
                frac = frac * 10 + (unsigned int)(*arg - '0');
                scale++;
            }
            arg++;
            digits++;
        }
    }
    if ( digits == 0 ) {
        return -1;
    }
    if ( scale == 1 ) {
        frac *= 10;
    }
    if ( *arg == '"' ) {
        if ( whole > 40 ) {
            whole = 40;
        }
        return (int)((whole * 100 + frac) * 1440 / 100);
    }
    if ( whole > 1000 ) {
        whole = 1000;
    }
    return (int)whole * unit;
}

static int ws_on( const char *arg, int otherwise )
{
    while ( *arg == ' ' ) {
        arg++;
    }
    if ( str_nicmp( arg, "on", 2 ) == 0 || *arg == '1' ) {
        return 1;
    }
    if ( str_nicmp( arg, "off", 3 ) == 0 || *arg == '0' ) {
        return 0;
    }
    return otherwise;
}

static void ws_looks( WS *ws )
{
    uint16_t flags = 0;

    if ( ws->toggles & (WT_BOLD | WT_DOUBLE) ) {
        flags |= CF_BOLD;
    }
    if ( ws->toggles & WT_UNDER ) {
        flags |= ws->wunder ? CF_WUNDER : CF_UNDER;
    }
    if ( ws->toggles & WT_ITALIC ) {
        flags |= CF_ITALIC;
    }
    if ( ws->toggles & WT_STRIKE ) {
        flags |= CF_STRIKE;
    }
    if ( ws->toggles & WT_SUB ) {
        flags |= CF_SUB;
    }
    if ( ws->toggles & WT_SUPER ) {
        flags |= CF_SUPER;
    }
    ws->wb.chp.flags = flags;
}

/* the first text settles the page: what the margins are then is the
   page's, and what they become later is the paragraphs' */
static void ws_settle( WS *ws )
{
    PAGESETUP *page = &ws->doc->page;
    int right;

    if ( ws->text_seen || ws->depth ) {
        return;
    }
    ws->text_seen = 1;
    ws->page_rm = ws->rm;
    page->left = ws->po;
    right = page->width - ws->po - ws->rm;
    page->right = right > 0 ? right : 0;
    page->top = ws->mt;
    page->bottom = ws->mb;
    page->height = ws->pl;
}

static void ws_char( WS *ws, uint32_t code )
{
    if ( ws->skip_next ) {
        ws->skip_next = 0;              /* it was struck over the one before */
        return;
    }
    ws_settle( ws );
    wb_char( &ws->wb, code );
}

static void ws_par( WS *ws )
{
    PAP *pap = &ws->wb.pap;
    int value;

    ws_settle( ws );
    value = ws->lm;
    pap->left = value < 0 ? 0 : value > 20000 ? 20000 : value;
    value = ws->page_rm - ws->rm;
    pap->right = value < -14400 ? -14400 : value > 20000 ? 20000 : value;
    value = ws->pm >= 0 ? ws->pm - ws->lm : 0;
    pap->first = value < -20000 ? -20000 : value > 20000 ? 20000 : value;
    pap->align = (uint8_t)(ws->center ? AL_CENTER : ws->justify == 1 ? AL_JUSTIFY :
                           ws->justify == 0 && ws->soft_seen ? AL_JUSTIFY : AL_LEFT);
    pap->flags = 0;
    if ( ws->page_next && ws->depth == 0 ) {
        pap->flags = PF_PAGEBEFORE;
        ws->page_next = 0;
    }
    wb_par( &ws->wb );
    ws->soft_seen = 0;
}

/* a line broken by WordStar: one space between its last word and the
   next line's first */
static void ws_soft_return( WS *ws )
{
    WPARA *para = ws->wb.para;
    uint16_t last;

    if ( para == NULL || para->len == 0 ) {
        return;
    }
    last = para->text[para->len - 1];
    if ( last != ' ' && last != '-' && last != WCH_SHY && last != WCH_TAB ) {
        wb_char( &ws->wb, ' ' );
    }
}

static void ws_page( WS *ws )
{
    if ( ws->depth ) {
        return;
    }
    if ( ws->wb.para != NULL ) {
        ws_par( ws );
    }
    ws->page_next = 1;
}

/* text in another story, in the looks the text is in now */
static void ws_sub( WS *ws, STORY *story, uint32_t pos, uint32_t end, int mode )
{
    WBUILD   keep_wb;
    uint32_t keep_toggles = ws->toggles;
    uint8_t  keep_soft = ws->soft_seen, keep_skip = ws->skip_next, keep_start = ws->line_start;

    if ( story == NULL || ws->depth >= WS_DEPTH ) {
        return;
    }
    keep_wb = ws->wb;
    wb_init( &ws->wb, ws->doc, story );
    ws->wb.chp = keep_wb.chp;
    ws->wb.chp.flags = 0;
    ws->toggles = 0;
    ws->soft_seen = ws->skip_next = 0;
    ws->depth++;
    ws_run( ws, pos, end, mode );
    if ( ws->wb.para != NULL ) {
        wb_par( &ws->wb );
    }
    wb_done( &ws->wb );
    ws->depth--;
    ws->wb = keep_wb;
    ws->toggles = keep_toggles;
    ws->soft_seen = keep_soft;
    ws->skip_next = keep_skip;
    ws->line_start = keep_start;
}

/* tab stops from columns: "list" is .tb's "6 11 #21", or a ruler line
   "L----!----#----R" when "ruler" is set */
static void ws_tabs( WS *ws, const char *list, int ruler )
{
    WTAB tabs[MAX_TABS];
    unsigned int index;
    int where;
    int count = 0, kind, inches;

    mem_set( tabs, 0, sizeof( tabs ) );
    if ( ruler ) {
        for ( index = 0; list[index] && index < 255; index++ ) {
            if ( list[index] == 'L' ) {
                ws->lm = (int)index * ws->col;
            } else if ( list[index] == 'R' ) {
                ws->rm = (int)(index + 1) * ws->col;
            } else if ( list[index] == 'P' ) {
                ws->pm = (int)index * ws->col;
            }
        }
        for ( index = 0; list[index] && index < 255; index++ ) {
            if ( (list[index] == '!' || list[index] == '#') && count < MAX_TABS ) {
                where = (int)index * ws->col - ws->lm;
                if ( where > 0 ) {
                    tabs[count].pos = where;
                    tabs[count].kind = (uint8_t)(list[index] == '#' ? TAB_DECIMAL : TAB_LEFT);
                    count++;
                }
            }
        }
    } else {
        while ( *list && count < MAX_TABS ) {
            while ( *list == ' ' || *list == ',' ) {
                list++;
            }
            kind = TAB_LEFT;
            if ( *list == '#' ) {
                kind = TAB_DECIMAL;
                list++;
            }
            where = ws_measure( list, ws->col );
            if ( where < 0 ) {
                break;
            }
            inches = 0;
            for ( index = 0; list[index] && list[index] != ' ' && list[index] != ','; index++ ) {
                if ( list[index] == '"' ) {
                    inches = 1;
                }
            }
            if ( !inches ) {
                where -= ws->col;       /* a column's number: column 1 is the edge */
            }
            where -= ws->lm;
            if ( where > 0 ) {
                tabs[count].pos = where;
                tabs[count].kind = (uint8_t)kind;
                count++;
            }
            list += index;
        }
    }
    if ( !pap_set_tabs( &ws->wb.pap, tabs, count ) ) {
        ws->doc->failed = 1;
    }
}

/* a header or footer line: .he is line 1, .h2 to .h5 the lines under it */
static void ws_hdrftr( WS *ws, int footer, int line, uint32_t pos, uint32_t end )
{
    STORY *story = footer ? &ws->doc->footer : &ws->doc->header;
    uint8_t *seen = footer ? &ws->ftr_seen : &ws->hdr_seen;

    while ( pos < end && (ws->data[pos] & 0x7F) == ' ' && pos + 1 < end && (ws->data[pos + 1] & 0x7F) == ' ' ) {
        pos++;                          /* spaces that only place the text */
    }
    if ( pos < end && (ws->data[pos] & 0x7F) == ' ' ) {
        pos++;
    }
    if ( line < 1 || line > 5 || (*seen & (1 << line)) || ws->depth != 0 ) {
        return;
    }
    *seen |= (uint8_t)(1 << line);
    if ( pos < end ) {
        ws_sub( ws, story, pos, end, WSM_HDRFTR );
    }
}

/* a dot command: its two letters, and the rest of its line in "arg"
   (and at "pos" to "end" in the file, for the ones whose text is text) */
static int ws_dot( WS *ws, int one, int two, const char *arg, uint32_t pos, uint32_t end )
{
    int value;

    if ( ws_in_list( ws_merge, one, two ) ) {
        return 0;                       /* merge print: it stays as text */
    }
    value = ws_measure( arg, (one == 'p' && two == 'l') || one == 'm' || (one == 'l' && two == 's') ? WS_LINE : ws->col );
    if ( one == 'p' && two == 'l' && value >= 1440 ) {
        ws->pl = value;
    } else if ( one == 'm' && two == 't' && value >= 0 ) {
        ws->mt = value;
    } else if ( one == 'm' && two == 'b' && value >= 0 ) {
        ws->mb = value;
    } else if ( one == 'p' && two == 'o' && value >= 0 ) {
        ws->po = value;
    } else if ( one == 'p' && two == 'w' && value >= 1440 && !ws->text_seen ) {
        ws->doc->page.width = value;
    } else if ( one == 'l' && two == 'm' && value >= 0 ) {
        /* a column's number counts from 1; inches are from the edge */
        ws->lm = str_chr( arg, '"' ) != NULL ? value : value >= ws->col ? value - ws->col : 0;
    } else if ( one == 'r' && two == 'm' && value > 0 ) {
        ws->rm = value;
    } else if ( one == 'p' && two == 'm' ) {
        ws->pm = value <= 0 ? -1 : str_chr( arg, '"' ) != NULL ? value : value - ws->col;
    } else if ( one == 'l' && two == 's' && value >= WS_LINE && value <= 9 * WS_LINE ) {
        ws->wb.pap.line = (uint16_t)value;
        ws->wb.pap.line_rule = LS_MULTIPLE;
    } else if ( one == 'l' && two == 'h' && value > 0 ) {
        value = value / ws->col * 30;   /* 48ths of an inch */
        if ( value == 240 ) {
            ws->wb.pap.line_rule = LS_MULTIPLE;
        } else if ( value >= 60 && value <= 2880 ) {
            ws->wb.pap.line = (uint16_t)value;
            ws->wb.pap.line_rule = LS_EXACT;
        }
    } else if ( one == 'c' && two == 'w' && value > 0 ) {
        value = value / ws->col * 12;   /* 120ths of an inch */
        if ( value >= 48 && value <= 720 ) {
            ws->col = value;
        }
    } else if ( one == 'p' && (two == 'a' || two == 'f') ) {
        if ( two == 'a' ) {
            ws_page( ws );
        }
    } else if ( one == 'h' && (two == 'e' || (two >= '1' && two <= '5')) ) {
        ws_hdrftr( ws, 0, two == 'e' ? 1 : two - '0', pos, end );
    } else if ( one == 'f' && (two == 'o' || (two >= '1' && two <= '5')) ) {
        ws_hdrftr( ws, 1, two == 'o' ? 1 : two - '0', pos, end );
    } else if ( one == 'p' && two == 'n' ) {
        if ( value > 0 && value / ws->col <= 32000 ) {
            ws->doc->page.first_page = (uint16_t)(value / ws->col);
        }
        ws->omit_pn = 0;
    } else if ( one == 'o' && two == 'p' ) {
        ws->omit_pn = 1;
    } else if ( one == 'o' && two == 'j' ) {
        ws->justify = (uint8_t)(ws_on( arg, 1 ) ? 1 : 2);
    } else if ( one == 'o' && two == 'c' ) {
        ws->center = (uint8_t)ws_on( arg, 1 );
    } else if ( one == 'u' && two == 'l' ) {
        ws->wunder = (uint8_t)!ws_on( arg, 1 );
        ws_looks( ws );
    } else if ( one == 'r' && two == 'r' ) {
        while ( *arg == ' ' ) {
            arg++;
        }
        if ( *arg == 'L' || *arg == '-' || *arg == '!' || *arg == 'P' ) {
            ws_tabs( ws, arg, 1 );
        }
    } else if ( one == 't' && two == 'b' ) {
        ws_tabs( ws, arg, 0 );
    }
    return 1;                           /* read, or dropped */
}

/* the line at "pos" starts with a dot: a command that is read or
   dropped -> where the next line starts; 0 for one that is text */
static uint32_t ws_dot_line( WS *ws, uint32_t pos, uint32_t end )
{
    char arg[256];
    const uint8_t *data = ws->data;
    uint32_t stop = pos, next;
    unsigned int used = 0;
    uint32_t index;
    int one, two;

    while ( stop < end && data[stop] != 0x0D && data[stop] != 0x0A && data[stop] != 0x1A && data[stop] != 0x8D ) {
        stop++;
    }
    next = stop;
    if ( next < end && (data[next] & 0x7F) == 0x0D ) {
        next++;
    }
    if ( next < end && (data[next] & 0x7F) == 0x0A ) {
        next++;
    }
    if ( stop - pos < 3 ) {
        /* a dot alone, or ".." and the like: a remark */
        return (stop - pos == 1 || data[pos + 1] == '.') ? next : 0;
    }
    one = ch_lower( data[pos + 1] & 0x7F );
    two = ch_lower( data[pos + 2] & 0x7F );
    if ( one == '.' ) {
        return next;                    /* ".. a remark" */
    }
    if ( !ws_letter( one ) || !(ws_letter( two ) || (two >= '0' && two <= '9')) ) {
        return 0;                       /* ".5 percent": text */
    }
    for ( index = pos + 3; index < stop && used < sizeof( arg ) - 1; index++ ) {
        arg[used++] = (char)(data[index] < 0x80 || !ws->new_era ? data[index] & 0x7F : '?');
    }
    arg[used] = 0;
    return ws_dot( ws, one, two, arg, pos + 3, stop ) ? next : 0;
}

/* a font sequence: the height is the size, the typestyle's number and
   bits say what kind of face */
static void ws_font( WS *ws, const uint8_t *body, uint32_t len )
{
    static const char *const face[4] = { "Arial", "Times New Roman", "Script", "Arial" };
    uint32_t height, style, number;
    const char *name;

    if ( len < 6 ) {
        return;
    }
    height = LE16( body + 2 );
    style = LE16( body + 4 );
    number = style & 0x1FF;
    if ( number == 4 || number == 12 || number == 46 ) {
        name = "Arial";
    } else if ( number == 5 || number == 31 ) {
        name = "Times New Roman";
    } else if ( number <= 3 || number == 8 || number == 10 || number == 48 || !(style & 0x8000) ) {
        name = "Courier New";
    } else {
        name = face[(style >> 10) & 3];
    }
    ws->wb.chp.font = (uint16_t)wd_font( ws->doc, name, FAM_ANY, -1 );
    if ( height >= 40 && height <= 14400 ) {
        ws->wb.chp.size = (uint16_t)(height / 10);
    }
}

/* a symmetrical sequence, whole: by its kind */
static void ws_sequence( WS *ws, uint32_t pos, uint32_t count )
{
    const uint8_t *body = ws->data + pos + 4;
    uint32_t len = count - 4, kind = ws->data[pos + 3];
    int obj;

    switch ( kind ) {
    case 1:
        if ( len >= 1 ) {
            ws->wb.chp.color = (body[0] & 0x0F) ? ws_colors[body[0] & 0x0F] : COLOR_AUTO;
        }
        break;
    case 2:
        ws_font( ws, body, len );
        break;
    case 3:
    case 4:
    case 5:
        /* lines, the number, a flag, then the note's text */
        if ( len > 5 && ws->depth == 0 ) {
            obj = wd_note_new( ws->doc, kind == 4 ? NOTE_END : NOTE_FOOT );
            if ( obj >= 0 ) {
                ws_settle( ws );
                wb_object( &ws->wb, obj );
                ws_sub( ws, wd_note_story( ws->doc, obj ), pos + 9, pos + count, WSM_NOTE );
            }
        }
        break;
    case 6:
        ws->doc->lost |= LOST_NOTES;    /* a comment that is not printed */
        break;
    case 9:
        /* a tab: one WordStar made to indent a line is not the text's */
        if ( len >= 5 && body[4] != 0xA0 ) {
            ws_char( ws, WCH_TAB );
        }
        break;
    case 0x10:
        ws->doc->lost |= LOST_IMAGES;
        break;
    default:
        break;
    }
}

static void ws_toggle( WS *ws, uint32_t bit )
{
    ws->toggles ^= bit;
    ws_looks( ws );
}

static void ws_run( WS *ws, uint32_t pos, uint32_t end, int mode )
{
    const uint8_t *data = ws->data;
    uint32_t code, count, next;

    while ( pos < end ) {
        code = data[pos];
        if ( mode == WSM_BODY && ws->line_start && code == '.' ) {
            next = ws_dot_line( ws, pos, end );
            if ( next ) {
                pos = next;
                continue;
            }
        }
        if ( code != 0x0A ) {
            ws->line_start = 0;         /* the line feed after a return is still the line's end */
        }
        pos++;
        if ( code == 0x1D ) {
            /* 1Dh, count, kind ... count, 1Dh: the count is of all but
               the last three bytes */
            if ( IN_RANGE( pos, 2, end ) ) {
                count = LE16( data + pos );
                if ( count >= 4 && IN_RANGE( pos - 1, count + 3, end ) &&
                     LE16( data + pos - 1 + count ) == count && data[pos + 1 + count] == 0x1D ) {
                    ws_sequence( ws, pos - 1, count );
                    pos += count + 2;
                }
            }
            continue;
        }
        if ( code == 0x1B && IN_RANGE( pos, 2, end ) && data[pos + 1] == 0x1C ) {
            ws_char( ws, cp_to_uni( CP_437, data[pos] ) );     /* an extended character */
            pos += 2;
            continue;
        }
        if ( code >= 0x80 ) {
            if ( code == 0x8D ) {
                ws_soft_return( ws );
                continue;
            }
            if ( code == 0xA0 ) {
                ws->soft_seen = 1;
                continue;
            }
            if ( code == 0x8A ) {
                continue;
            }
            if ( ws->new_era ) {
                ws_char( ws, cp_to_uni( CP_437, (int)code ) );
                continue;
            }
            code &= 0x7F;               /* the mark on a word's last letter */
        }
        switch ( code ) {
        case 0x02:
            ws_toggle( ws, WT_BOLD );
            break;
        case 0x04:
            ws_toggle( ws, WT_DOUBLE );
            break;
        case 0x13:
            ws_toggle( ws, WT_UNDER );
            break;
        case 0x19:
            ws_toggle( ws, WT_ITALIC );
            break;
        case 0x18:
            ws_toggle( ws, WT_STRIKE );
            break;
        case 0x16:
            ws_toggle( ws, WT_SUB );
            break;
        case 0x14:
            ws_toggle( ws, WT_SUPER );
            break;
        case 0x08:
            ws->skip_next = 1;
            break;
        case 0x09:
            ws_char( ws, WCH_TAB );
            break;
        case 0x0C:
            ws_page( ws );
            break;
        case 0x0D:
            if ( mode == WSM_HDRFTR ) {
                break;
            }
            ws_par( ws );
            ws->line_start = 1;
            break;
        case 0x0A:
            break;
        case 0x0F:
            ws_char( ws, WCH_NBSP );
            break;
        case 0x1E:
        case 0x1F:
            ws_char( ws, WCH_SHY );
            break;
        case 0x1A:
            if ( mode == WSM_BODY ) {
                return;                 /* the end of the file */
            }
            break;
        case '#':
            ws_char( ws, mode == WSM_HDRFTR ? WCH_PAGENUM : '#' );
            break;
        case '\\':
            /* in a header the next character is itself */
            if ( mode == WSM_HDRFTR && pos < end ) {
                ws_char( ws, data[pos++] & 0x7F );
            } else {
                ws_char( ws, '\\' );
            }
            break;
        default:
            if ( code >= 0x20 && code != 0x7F ) {
                ws_char( ws, code );
            }
            break;
        }
    }
}

static int ws_load( const uint8_t *data, uint32_t len, WDOC *doc )
{
    WS ws;
    WPARA *para;
    uint32_t start = 0;
    uint16_t page_num = WCH_PAGENUM;

    mem_set( &ws, 0, sizeof( ws ) );
    ws.data = data;
    ws.len = len;
    ws.doc = doc;
    if ( ws_header( data, len ) ) {
        ws.new_era = 1;
        start = 128;
    }
    doc->variant = ws.new_era ? 1 : 0;

    /* WordStar's own page: 66 lines, 3 above the text and 8 below, the
       text 8 columns in and 65 wide */
    ws.col = WS_COL;
    ws.pl = 66 * WS_LINE;
    ws.mt = 3 * WS_LINE;
    ws.mb = 8 * WS_LINE;
    ws.po = 8 * WS_COL;
    ws.rm = 65 * WS_COL;
    ws.pm = -1;
    ws.line_start = 1;

    wb_init( &ws.wb, doc, &doc->body );
    ws.wb.chp.font = (uint16_t)wd_font( doc, "Courier New", FAM_MODERN, 1 );
    ws.wb.chp.size = 24;
    doc->style[0].chp = ws.wb.chp;

    ws_run( &ws, start, len, WSM_BODY );
    if ( ws.wb.para != NULL ) {
        ws_par( &ws );
    }
    ws_settle( &ws );
    wb_done( &ws.wb );

    /* with no footer of its own, and not told otherwise, WordStar
       prints the page's number under the text */
    if ( !ws.omit_pn && doc->footer.count == 0 && !ws.ftr_seen ) {
        para = wd_para_add( doc, &doc->footer, NULL );
        if ( para != NULL ) {
            para->pap.align = AL_CENTER;
            wd_text_insert( doc, para, 0, &page_num, 1, wd_chp( doc, &doc->style[0].chp ) );
        }
    }
    return wd_failed( doc ) ? WE_NOMEM : WE_OK;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    WDOC        *doc;
    OUT         *out;
    int          v7;            /* the 7.0 form */
    int          depth;
    uint32_t     toggles;       /* on in the file */
    int          held;          /* a letter not yet written: a space after it marks it */
    unsigned int column;        /* how far along the line */
    unsigned int width;         /* columns a line may have */
    int          lm, rm;        /* the margins last written, twips; -1 not yet */
    int          pm;
    int          align;
    uint32_t     line;
    uint32_t     font_size;     /* 7.0: the font sequence last written */
    int          font;
    unsigned int notes;
} WSS;

static void wss_flush( WSS *ws )
{
    if ( ws->held ) {
        out_byte( ws->out, ws->held );
        ws->held = 0;
    }
}

static void wss_raw( WSS *ws, int byte )
{
    wss_flush( ws );
    out_byte( ws->out, byte );
}

static void wss_newline( WSS *ws, int soft )
{
    wss_flush( ws );
    out_byte( ws->out, soft ? 0x8D : 0x0D );
    out_byte( ws->out, 0x0A );
    ws->column = 0;
}

/* a character of the text, in the file's form */
static void wss_char( WSS *ws, uint32_t code )
{
    char bytes[8];
    int count, index;

    if ( code == ' ' ) {
        /* the letter a space follows carries WordStar's mark (4.0) */
        if ( ws->held && !ws->v7 ) {
            ws->held |= 0x80;
        }
        wss_raw( ws, ' ' );
    } else if ( code > 0x20 && code < 0x7F ) {
        wss_flush( ws );
        ws->held = (int)code;
    } else {
        count = cp_put( CP_437, code, bytes );
        for ( index = 0; index < count; index++ ) {
            code = (uint8_t)bytes[index];
            if ( code < 0x20 || (code >= 0x80 && (!ws->v7 || code == 0x8D || code == 0x8A || code == 0xA0)) ) {
                wss_raw( ws, 0x1B );
                out_byte( ws->out, (uint8_t)bytes[index] );
                out_byte( ws->out, 0x1C );
            } else {
                wss_raw( ws, (uint8_t)bytes[index] );
            }
        }
    }
    ws->column++;
}

static void wss_toggles( WSS *ws, uint32_t want )
{
    static const uint8_t code_of[7] = { 0x02, 0x04, 0x13, 0x19, 0x18, 0x16, 0x14 };
    unsigned int index;

    for ( index = 0; index < 7; index++ ) {
        if ( (ws->toggles ^ want) & (1UL << index) ) {
            wss_raw( ws, code_of[index] );
        }
    }
    ws->toggles = want;
}

static uint32_t wss_toggles_of( uint16_t flags )
{
    return ((flags & CF_BOLD) ? WT_BOLD : 0) | ((flags & (CF_UNDER | CF_WUNDER | CF_DUNDER)) ? WT_UNDER : 0) |
           ((flags & CF_ITALIC) ? WT_ITALIC : 0) | ((flags & CF_STRIKE) ? WT_STRIKE : 0) |
           ((flags & CF_SUB) ? WT_SUB : 0) | ((flags & CF_SUPER) ? WT_SUPER : 0);
}

static void wss_seq_open( WSS *ws, int kind, uint32_t body_len )
{
    wss_raw( ws, 0x1D );
    out_le16( ws->out, body_len + 4 );
    out_byte( ws->out, kind );
}

static void wss_seq_close( WSS *ws, uint32_t body_len )
{
    out_le16( ws->out, body_len + 4 );
    out_byte( ws->out, 0x1D );
}

/* 7.0: the font and size of a run */
static void wss_font( WSS *ws, const CHP *chp )
{
    const WFONT *font = &ws->doc->font[chp->font < ws->doc->nfont ? chp->font : 0];
    uint32_t style;

    if ( !ws->v7 || (ws->font == (int)chp->font && ws->font_size == chp->size) ) {
        return;
    }
    if ( font->fixed || font->family == FAM_MODERN || str_nicmp( font->name, "Courier", 7 ) == 0 ) {
        style = 0x4200 | 3;
    } else if ( font->family == FAM_SWISS || str_nicmp( font->name, "Arial", 5 ) == 0 ||
                str_nicmp( font->name, "Helv", 4 ) == 0 ) {
        style = 0xC200 | 4;
    } else {
        style = 0xC600 | 5;
    }
    wss_seq_open( ws, 2, 12 );
    out_le16( ws->out, (style & 0x8000) ? (uint32_t)chp->size * 5 : 180 );
    out_le16( ws->out, (uint32_t)chp->size * 10 );
    out_le16( ws->out, style );
    out_fill( ws->out, 0, 6 );
    wss_seq_close( ws, 12 );
    ws->font = (int)chp->font;
    ws->font_size = chp->size;
}

static void wss_dot( WSS *ws, const char *name, uint32_t value )
{
    wss_flush( ws );
    out_fmt( ws->out, ".%s %u\r\n", name, value );
}

static void wss_text( WSS *ws, WPARA *para, int hdrftr );

/* a note: 7.0 has a sequence for it, 4.0 its text in brackets */
static void wss_note( WSS *ws, const WOBJ *obj )
{
    OUT *main_out = ws->out, *part;
    uint8_t *bytes;
    uint32_t len = 0;
    unsigned int index;
    uint32_t keep_toggles = ws->toggles, keep_size = ws->font_size;
    int keep_font = ws->font;

    if ( obj->story == NULL || ws->depth != 0 ) {
        return;
    }
    if ( !ws->v7 ) {
        wss_char( ws, '[' );
        ws->depth = 1;
        for ( index = 0; index < obj->story->count; index++ ) {
            if ( index ) {
                wss_char( ws, ' ' );
            }
            wss_text( ws, obj->story->para[index], 0 );
        }
        ws->depth = 0;
        wss_char( ws, ']' );
        return;
    }
    wss_flush( ws );
    part = out_mem();
    if ( part == NULL ) {
        out_fail( main_out, WE_NOMEM );
        return;
    }
    ws->out = part;
    ws->depth = 1;
    ws->toggles = 0;                    /* a note starts plain, and ends so */
    for ( index = 0; index < obj->story->count; index++ ) {
        if ( index ) {
            wss_newline( ws, 0 );
        }
        wss_text( ws, obj->story->para[index], 0 );
    }
    wss_toggles( ws, 0 );
    wss_flush( ws );
    ws->toggles = keep_toggles;
    ws->font = keep_font;
    ws->font_size = keep_size;
    ws->depth = 0;
    ws->out = main_out;
    bytes = out_data( part, &len );
    if ( bytes != NULL && len <= 60000UL && !out_error( part ) ) {
        ws->notes++;
        wss_seq_open( ws, obj->fmt == NOTE_END ? 4 : 3, len + 5 );
        out_le16( main_out, 1 );
        out_le16( main_out, ws->notes & 0x7FFF );
        out_byte( main_out, 0x30 );
        out_put( main_out, bytes, len );
        wss_seq_close( ws, len + 5 );
    }
    out_close( part );
}

/* a paragraph's text, with the toggles for its looks; in the body the
   lines are broken at the right margin with soft returns */
static void wss_text( WSS *ws, WPARA *para, int hdrftr )
{
    WDOC *doc = ws->doc;
    const CHP *chp;
    unsigned int pos = 0, run, stop;
    uint32_t code;
    unsigned int ahead, word;
    int obj;

    while ( pos < para->len ) {
        run = wd_run( para, pos );
        if ( run == 0 ) {
            break;
        }
        stop = pos + run;
        chp = wd_chp_get( doc, para->attr[pos] );
        if ( chp->flags & CF_HIDDEN ) {
            pos = stop;
            continue;
        }
        wss_font( ws, chp );
        wss_toggles( ws, wss_toggles_of( chp->flags ) );
        while ( pos < stop ) {
            code = utf16_get( para->text, stop, &pos );
            switch ( code ) {
            case WCH_TAB:
                wss_raw( ws, 0x09 );
                ws->column++;
                break;
            case WCH_LINE:
                if ( !hdrftr ) {
                    wss_newline( ws, 0 );
                }
                break;
            case WCH_PAGE:
                if ( !hdrftr && ws->depth == 0 ) {
                    wss_newline( ws, 0 );
                    out_str( ws->out, ".pa\r\n" );
                }
                break;
            case WCH_NBSP:
                wss_raw( ws, 0x0F );
                ws->column++;
                break;
            case WCH_SHY:
                wss_raw( ws, 0x1E );
                break;
            case WCH_NBHYPHEN:
                wss_char( ws, '-' );
                break;
            case WCH_PAGENUM:
                wss_raw( ws, '#' );
                ws->column++;
                break;
            case WCH_OBJ:
                obj = wd_obj_at( para, pos - 1 );
                if ( obj >= 0 && (unsigned int)obj < doc->nobj && doc->obj[obj].kind == OBJ_NOTE && !hdrftr ) {
                    wss_note( ws, &doc->obj[obj] );
                }
                break;
            case '#':
            case '\\':
                if ( hdrftr ) {
                    wss_raw( ws, '\\' );
                }
                wss_char( ws, code );
                break;
            case ' ':
                wss_char( ws, ' ' );
                /* the next word, if it would pass the margin, starts a line */
                if ( !hdrftr && ws->depth == 0 ) {
                    word = 0;
                    for ( ahead = pos; ahead < para->len && para->text[ahead] != ' '; ahead++ ) {
                        word++;
                    }
                    if ( word && ws->column + word > ws->width && para->text[pos] != '.' ) {
                        wss_newline( ws, 1 );
                    }
                }
                break;
            default:
                if ( !WCH_IS_FIELD( code ) ) {
                    wss_char( ws, code );
                }
                break;
            }
        }
    }
    wss_flush( ws );
}

static void wss_hdrftr( WSS *ws, STORY *story, const char *first, char rest )
{
    unsigned int index;

    for ( index = 0; index < story->count && index < (unsigned int)(ws->v7 ? 5 : 1); index++ ) {
        if ( index == 0 ) {
            out_fmt( ws->out, ".%s ", first );
        } else {
            out_fmt( ws->out, ".%c%u ", rest, index + 1 );
        }
        wss_text( ws, story->para[index], 1 );
        wss_toggles( ws, 0 );
        wss_newline( ws, 0 );
    }
}

/* the ruler line for a paragraph's tab stops: L, a ! or # at each
   stop, R at the right margin */
static void wss_ruler( WSS *ws, const PAP *pap )
{
    char ruler[256];
    unsigned int left = (unsigned int)(ws->lm > 0 ? ws->lm : 0) / WS_COL, right = (unsigned int)(ws->rm > 0 ? ws->rm : 0) / WS_COL;
    unsigned int index, column;

    if ( right < 2 || right > 250 || left + 1 >= right ) {
        return;
    }
    mem_set( ruler, '-', right );
    ruler[right] = 0;
    ruler[left] = 'L';
    ruler[right - 1] = 'R';
    for ( index = 0; index < pap->ntabs; index++ ) {
        column = (unsigned int)(ws->lm + pap->tabs[index].pos) / WS_COL;
        if ( pap->tabs[index].pos > 0 && column > left && column + 1 < right ) {
            ruler[column] = (char)(pap->tabs[index].kind == TAB_DECIMAL ? '#' : '!');
        }
    }
    wss_flush( ws );
    out_fmt( ws->out, ".rr %s\r\n", ruler );
}

static void wss_para( WSS *ws, WPARA *para )
{
    PAGESETUP *page = &ws->doc->page;
    const PAP *pap = &para->pap;
    int text_width = page->width - page->left - page->right;
    int lm = pap->left > 0 ? pap->left : 0, rm = text_width - pap->right, pm;
    uint32_t line;
    int align = pap->align == AL_CENTER ? AL_CENTER : pap->align == AL_JUSTIFY ? AL_JUSTIFY : AL_LEFT;

    if ( pap->flags & PF_PAGEBEFORE ) {
        out_str( ws->out, ".pa\r\n" );
    }
    lm = lm / WS_COL * WS_COL;
    rm = rm / WS_COL * WS_COL;
    if ( rm < lm + 10 * WS_COL ) {
        rm = lm + 10 * WS_COL;
    }
    if ( lm != ws->lm ) {
        wss_dot( ws, "lm", (uint32_t)lm / WS_COL + 1 );
        ws->lm = lm;
    }
    if ( rm != ws->rm ) {
        wss_dot( ws, "rm", (uint32_t)rm / WS_COL );
        ws->rm = rm;
    }
    pm = pap->first ? lm + pap->first : -1;
    if ( pm < 0 && pap->first ) {
        pm = 0;
    }
    if ( pm != ws->pm ) {
        wss_dot( ws, "pm", pm < 0 ? 0 : (uint32_t)pm / WS_COL + 1 );
        ws->pm = pm;
    }
    line = pap->line_rule == LS_MULTIPLE && pap->line >= WS_LINE ? (pap->line + WS_LINE / 2) / WS_LINE : 1;
    if ( line > 9 ) {
        line = 9;
    }
    if ( line != ws->line ) {
        wss_dot( ws, "ls", line );
        ws->line = line;
    }
    if ( align != ws->align ) {
        if ( ws->align == AL_CENTER ) {
            out_str( ws->out, ".oc off\r\n" );
        }
        if ( align == AL_CENTER ) {
            out_str( ws->out, ".oc on\r\n" );
        } else {
            out_str( ws->out, align == AL_JUSTIFY ? ".oj on\r\n" : ".oj off\r\n" );
        }
        ws->align = align;
    }
    if ( pap->ntabs ) {
        wss_ruler( ws, pap );
    }
    ws->width = (unsigned int)(rm - lm) / WS_COL;
    ws->column = 0;
    /* a dot in column 1 would be a command: a soft space goes before it */
    if ( para->len && para->text[0] == '.' ) {
        out_byte( ws->out, 0xA0 );
    }
    wss_text( ws, para, 0 );
    /* a table is its text: a tab between cells, a line for each row */
    if ( (pap->flags & PF_INTABLE) && !(pap->flags & PF_ROWEND) ) {
        wss_raw( ws, (pap->flags & PF_CELLEND) ? 0x09 : ' ' );
    } else {
        wss_newline( ws, 0 );
    }
}

static int ws_save( WDOC *doc, OUT *out, int variant )
{
    WSS ws;
    PAGESETUP *page = &doc->page;
    WPARA *para;
    unsigned int index;
    int text_width = page->width - page->left - page->right;
    int plain_number = 0;

    mem_set( &ws, 0, sizeof( ws ) );
    ws.doc = doc;
    ws.out = out;
    ws.v7 = variant == 1;
    ws.lm = 0;
    ws.rm = 65 * WS_COL;
    ws.pm = -1;
    ws.align = -1;
    ws.line = 1;
    ws.font = -1;

    if ( ws.v7 ) {
        /* the header sequence: 128 bytes, the version in BCD */
        out_byte( out, 0x1D );
        out_le16( out, 0x7D );
        out_byte( out, 0 );
        out_byte( out, 0x70 );
        out_fill( out, 0, 120 );
        out_le16( out, 0x7D );
        out_byte( out, 0x1D );
    }
    /* the page: lines, lines above and below, columns in, and wide */
    wss_dot( &ws, "pl", (uint32_t)(page->height + WS_LINE / 2) / WS_LINE );
    wss_dot( &ws, "mt", (uint32_t)(page->top > 0 ? page->top : 0) / WS_LINE );
    wss_dot( &ws, "mb", (uint32_t)(page->bottom > 0 ? page->bottom : 0) / WS_LINE );
    wss_dot( &ws, "po", (uint32_t)(page->left > 0 ? page->left : 0) / WS_COL );
    if ( text_width < 10 * WS_COL ) {
        text_width = 10 * WS_COL;
    }
    wss_dot( &ws, "rm", (uint32_t)text_width / WS_COL );
    ws.rm = (int)((uint32_t)text_width / WS_COL) * WS_COL;
    if ( page->first_page > 1 ) {
        wss_dot( &ws, "pn", page->first_page );
    }
    wss_hdrftr( &ws, &doc->header, "he", 'h' );
    /* a footer that is the page's number and nothing else is what
       WordStar prints anyway */
    if ( doc->footer.count == 1 ) {
        para = doc->footer.para[0];
        plain_number = para->len == 1 && para->text[0] == WCH_PAGENUM;
    }
    if ( doc->footer.count == 0 ) {
        out_str( out, ".op\r\n" );
    } else if ( !plain_number ) {
        wss_hdrftr( &ws, &doc->footer, "fo", 'f' );
    }
    for ( index = 0; index < doc->body.count; index++ ) {
        wss_para( &ws, doc->body.para[index] );
    }
    wss_toggles( &ws, 0 );
    wss_flush( &ws );
    out_byte( out, 0x1A );
    return out_error( out ) == WE_NOMEM ? WE_NOMEM : out_error( out ) ? WE_IO : WE_OK;
}

const FORMAT fmt_wordstar = {
    FMT_WORDSTAR, "WordStar", "WS;WSD", ws_detect, ws_load, ws_save, ws_variants, 0
};
