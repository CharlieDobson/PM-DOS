/*
 * F_TXT.C - plain text.
 *
 * READ.  A file that starts with a byte-order mark is UTF-8 or UTF-16
 * and says so.  One that does not is UTF-8 if it has bytes above 7Fh
 * and every one of them is part of a well-formed sequence - text in a
 * code page almost never is, by chance - and otherwise it is in the
 * machine's own code page (txt_codepage: PMWRITE sets it from the
 * country at start, and it is 437 until then).  CR LF, LF alone and CR
 * alone each end a paragraph; a form feed starts a page; a Ctrl+Z ends
 * the file, as it always has on DOS.
 *
 * The text is shown in a fixed-pitch font, because a text file's
 * columns were lined up in one.
 *
 * WRITE.  Paragraphs with CR LF between, in code page 437, in Windows'
 * 1252, or as UTF-8.  What the code page has not got is written as the
 * nearest thing (CP.C).  Pictures are left out, and a note's text
 * follows the paragraph it was called from, in square brackets.
 */
#include "fmt.h"

int txt_codepage = CP_437;

static const char *const txt_variants[] = {
    "MS-DOS Text",
    "Windows Text",
    "Unicode Text (UTF-8)",
    NULL
};

/* 0 not UTF-8; 1 nothing above 7Fh; 2 UTF-8 with such bytes in it */
static int utf8_kind( const uint8_t *data, uint32_t len )
{
    const uint8_t *cur = data, *end = data + len;
    int high = 0;

    while ( cur < end ) {
        if ( *cur < 0x80 ) {
            cur++;
            continue;
        }
        high = 1;
        if ( utf8_get( &cur, end ) == 0xFFFD ) {
            /* a file cut off in the middle of its last character is
               still UTF-8; anything else is not */
            if ( end - cur > 3 || cur == data ) {
                return 0;
            }
            return 2;
        }
    }
    return high ? 2 : 1;
}

static int txt_detect( const uint8_t *data, uint32_t len, const char *ext )
{
    unsigned int index, look, odd = 0;
    int by_name = str_cmp( ext, "TXT" ) == 0 || str_cmp( ext, "ASC" ) == 0 ||
                  str_cmp( ext, "ME" ) == 0 || str_cmp( ext, "BAT" ) == 0 ||
                  str_cmp( ext, "INI" ) == 0 || str_cmp( ext, "LOG" ) == 0;

    if ( len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF ) {
        return 50;
    }
    if ( len >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF)) ) {
        return 50;
    }
    /* anything at all can be shown as text, so text is never "no" - it
       is only less sure than every format that knows its own files */
    look = len < 4096 ? len : 4096;
    for ( index = 0; index < look; index++ ) {
        if ( data[index] < 0x20 && data[index] != '\t' && data[index] != '\r' &&
             data[index] != '\n' && data[index] != '\f' && data[index] != 0x1A ) {
            odd++;
        }
    }
    if ( odd * 20 > look ) {
        return by_name ? 10 : 2;        /* a twentieth of it is control codes */
    }
    return by_name ? 40 : 20;
}

static void txt_unit( WBUILD *wb, uint32_t code, int *was_cr )
{
    if ( code == '\n' && *was_cr ) {
        *was_cr = 0;
        return;
    }
    *was_cr = code == '\r';
    if ( code == '\r' || code == '\n' ) {
        wb_par( wb );
    } else if ( code == '\f' ) {
        wb_char( wb, WCH_PAGE );
    } else if ( code == '\t' || code >= 0x20 ) {
        wb_char( wb, code == 0xFEFF ? 0 : code );
    }
}

static int txt_load( const uint8_t *data, uint32_t len, WDOC *doc )
{
    WBUILD wb;
    const uint8_t *cur = data, *end = data + len;
    int was_cr = 0, wide = 0, big = 0, utf8 = 0;
    uint32_t code;

    if ( len >= 2 && data[0] == 0xFF && data[1] == 0xFE ) {
        wide = 1;
        cur += 2;
    } else if ( len >= 2 && data[0] == 0xFE && data[1] == 0xFF ) {
        wide = big = 1;
        cur += 2;
    } else if ( len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF ) {
        utf8 = 1;
        cur += 3;
    } else if ( utf8_kind( data, len ) == 2 ) {
        utf8 = 1;
    }
    doc->variant = (wide || utf8) ? 2 : txt_codepage == CP_1252 ? 1 : 0;

    wb_init( &wb, doc, &doc->body );
    wb.chp.font = (uint16_t)wd_font( doc, "Courier New", FAM_MODERN, 1 );
    wb.chp.size = 20;
    doc->style[0].chp = wb.chp;

    if ( wide ) {
        while ( end - cur >= 2 ) {
            code = big ? BE16( cur ) : LE16( cur );
            cur += 2;
            if ( code >= 0xD800 && code <= 0xDBFF && end - cur >= 2 ) {
                uint32_t low = big ? BE16( cur ) : LE16( cur );

                if ( low >= 0xDC00 && low <= 0xDFFF ) {
                    code = 0x10000UL + ((code - 0xD800) << 10) + (low - 0xDC00);
                    cur += 2;
                }
            }
            if ( code == 0x1A ) {
                break;
            }
            txt_unit( &wb, code, &was_cr );
        }
    } else {
        while ( cur < end ) {
            if ( *cur == 0x1A ) {
                break;
            }
            if ( utf8 ) {
                code = utf8_get( &cur, end );
            } else {
                code = cp_to_uni( txt_codepage, *cur++ );
            }
            txt_unit( &wb, code, &was_cr );
        }
    }
    wb_done( &wb );
    return wd_failed( doc ) ? WE_NOMEM : WE_OK;
}

static void txt_story( WDOC *doc, OUT *out, STORY *story, int codepage, int depth );

static void txt_para( WDOC *doc, OUT *out, WPARA *para, int codepage, int depth )
{
    char near_buf[8];
    uint8_t utf[4];
    unsigned int pos = 0, at;
    uint32_t code;
    int obj;

    while ( pos < para->len ) {
        at = pos;
        code = utf16_get( para->text, para->len, &pos );
        if ( code == WCH_LINE ) {
            out_str( out, "\r\n" );
        } else if ( code == WCH_PAGE ) {
            out_byte( out, '\f' );
        } else if ( code == WCH_OBJ || WCH_IS_FIELD( code ) ) {
            continue;
        } else if ( code == WCH_SHY ) {
            continue;                   /* a hyphen that is not showing */
        } else if ( codepage == CP_UTF8 ) {
            out_put( out, utf, (unsigned int)utf8_put( utf, code ) );
        } else {
            out_put( out, near_buf, (unsigned int)cp_put( codepage, code, near_buf ) );
        }
        (void)at;
    }
    out_str( out, "\r\n" );
    /* the notes called from this paragraph, after it */
    if ( depth == 0 ) {
        for ( at = 0; at < para->nanch; at++ ) {
            obj = (int)para->anch[at].obj;
            if ( (unsigned int)obj < doc->nobj && doc->obj[obj].kind == OBJ_NOTE && doc->obj[obj].story != NULL ) {
                out_byte( out, '[' );
                txt_story( doc, out, doc->obj[obj].story, codepage, 1 );
                out_str( out, "]\r\n" );
            }
        }
    }
}

static void txt_story( WDOC *doc, OUT *out, STORY *story, int codepage, int depth )
{
    unsigned int index;

    for ( index = 0; index < story->count; index++ ) {
        txt_para( doc, out, story->para[index], codepage, depth );
    }
}

static int txt_save( WDOC *doc, OUT *out, int variant )
{
    /* MS-DOS text goes out in the code page it is read in */
    int codepage = variant == 2 ? CP_UTF8 : variant == 1 ? CP_1252 : txt_codepage != CP_1252 ? txt_codepage : CP_437;

    if ( codepage == CP_UTF8 ) {
        out_put( out, "\xEF\xBB\xBF", 3 );
    }
    txt_story( doc, out, &doc->body, codepage, 0 );
    return out_error( out ) ? WE_IO : WE_OK;
}

const FORMAT fmt_txt = {
    FMT_TXT, "Text Only", "TXT;ASC", txt_detect, txt_load, txt_save, txt_variants, 0
};
