/*
 * DUMP.C - a document written out line by line, for the tests.
 *
 * Two files that hold the same document give the same dump, whatever
 * formats they are in, so "read it, write it as X, read that, compare
 * the dumps" is the test of X's reader and writer together.  The text
 * is UTF-8; a character that is not printable ASCII and not a letter
 * is written {U+hhhh} so that the dump can be read on any screen.
 *
 * Only what differs from the plainest thing is said: a paragraph that
 * is left-aligned and single spaced says nothing of either.
 */
#include "fmt.h"

static void dump_chp( WDOC *doc, OUT *out, const CHP *chp )
{
    static const char *const flag_names[] = {
        "b", "i", "u", "strike", "super", "sub", "hidden", "smallcaps",
        "allcaps", "dunder", "wunder", "outline", "shadow"
    };
    int bit;

    out_fmt( out, "[%s %u", doc->font[chp->font < doc->nfont ? chp->font : 0].name,
             (u32)chp->size / 2 );
    if ( chp->size & 1 ) {
        out_str( out, ".5" );
    }
    for ( bit = 0; bit < 13; bit++ ) {
        if ( chp->flags & (1u << bit) ) {
            out_fmt( out, " %s", flag_names[bit] );
        }
    }
    if ( chp->color != COLOR_AUTO ) {
        out_fmt( out, " #%06X", chp->color );
    }
    if ( chp->hilite ) {
        out_fmt( out, " hl%u", (u32)chp->hilite );
    }
    out_str( out, "]" );
}

static void dump_text( OUT *out, const u16 *text, u32 len )
{
    u32 pos = 0, code;
    u8 utf[4];

    while ( pos < len ) {
        code = utf16_get( text, len, &pos );
        if ( code == '{' || code == '\\' ) {
            out_byte( out, '\\' );
            out_byte( out, (int)code );
        } else if ( (code >= 0x20 && code < 0x7F) || (code >= 0xC0 && code < 0x250) ||
                    (code >= 0x370 && code < 0x530) ) {
            out_put( out, utf, (u32)utf8_put( utf, code ) );
        } else if ( code == WCH_TAB ) {
            out_str( out, "{tab}" );
        } else if ( code == WCH_LINE ) {
            out_str( out, "{line}" );
        } else if ( code == WCH_PAGE ) {
            out_str( out, "{page}" );
        } else if ( code == WCH_PAGENUM ) {
            out_str( out, "{PAGE}" );
        } else if ( code == WCH_NUMPAGES ) {
            out_str( out, "{NUMPAGES}" );
        } else if ( code == WCH_DATE ) {
            out_str( out, "{DATE}" );
        } else if ( code == WCH_TIME ) {
            out_str( out, "{TIME}" );
        } else if ( code == WCH_FILENAME ) {
            out_str( out, "{FILENAME}" );
        } else {
            out_fmt( out, "{U+%04X}", code );
        }
    }
}

static void dump_pap( WDOC *doc, OUT *out, const PAP *pap )
{
    static const char *const aligns[] = { "left", "center", "right", "justify" };
    static const char *const tabs[] = { "L", "C", "R", "D" };
    static const char *const lists[] = { "", "bullet", "number", "lower", "upper", "roman", "ROMAN" };
    int index;

    if ( pap->style != 0 && pap->style < doc->nstyle ) {
        out_fmt( out, " style=\"%s\"", doc->style[pap->style].name );
    }
    if ( pap->align != AL_LEFT && pap->align <= AL_JUSTIFY ) {
        out_fmt( out, " %s", aligns[pap->align] );
    }
    if ( pap->left || pap->right || pap->first ) {
        out_fmt( out, " ind=%d,%d,%d", pap->left, pap->right, pap->first );
    }
    if ( pap->before || pap->after ) {
        out_fmt( out, " space=%u,%u", (u32)pap->before, (u32)pap->after );
    }
    if ( pap->line_rule != LS_MULTIPLE || pap->line != 240 ) {
        out_fmt( out, " line=%s%u", pap->line_rule == LS_EXACT ? "exact" :
                 pap->line_rule == LS_ATLEAST ? "atleast" : "x", (u32)pap->line );
    }
    if ( pap->list != LIST_NONE && pap->list <= LIST_ROMAN_UPPER ) {
        out_fmt( out, " list=%s/%u", lists[pap->list], (u32)pap->level );
        if ( pap->list_start ) {
            out_fmt( out, "@%u", (u32)pap->list_start );
        }
    }
    if ( pap->flags & PF_KEEP ) {
        out_str( out, " keep" );
    }
    if ( pap->flags & PF_KEEPNEXT ) {
        out_str( out, " keepnext" );
    }
    if ( pap->flags & PF_PAGEBEFORE ) {
        out_str( out, " pagebefore" );
    }
    if ( pap->flags & (PF_BORDER_TOP | PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT) ) {
        out_fmt( out, " border=%s%s%s%s", pap->flags & PF_BORDER_TOP ? "t" : "",
                 pap->flags & PF_BORDER_BOT ? "b" : "", pap->flags & PF_BORDER_LEFT ? "l" : "",
                 pap->flags & PF_BORDER_RIGHT ? "r" : "" );
    }
    if ( pap->flags & PF_INTABLE ) {
        out_fmt( out, " cell%s%s", pap->flags & PF_CELLEND ? "/end" : "",
                 pap->flags & PF_ROWEND ? "/row" : "" );
        if ( (pap->flags & PF_ROWEND) && pap->row != NULL ) {
            out_fmt( out, " row=%d:", pap->row->left );
            for ( index = 0; index < pap->row->ncells && index < MAX_CELLS; index++ ) {
                out_fmt( out, "%s%d", index ? "," : "", pap->row->edge[index] );
            }
            if ( pap->row->borders ) {
                out_str( out, " ruled" );
            }
        }
    }
    if ( pap->ntabs && pap->tabs != NULL ) {
        out_str( out, " tabs=" );
        for ( index = 0; index < pap->ntabs; index++ ) {
            out_fmt( out, "%s%d%s", index ? "," : "", pap->tabs[index].pos,
                     tabs[pap->tabs[index].kind & 3] );
            if ( pap->tabs[index].lead ) {
                out_fmt( out, "/%u", (u32)pap->tabs[index].lead );
            }
        }
    }
}

static void dump_story( WDOC *doc, OUT *out, STORY *story, const char *name, int depth )
{
    WPARA *para;
    WOBJ *obj;
    u32 index, off, run;
    int which;

    out_fmt( out, "%s: %u paragraph%s\r\n", name, story->count, story->count == 1 ? "" : "s" );
    for ( index = 0; index < story->count; index++ ) {
        para = story->para[index];
        out_str( out, "P" );
        dump_pap( doc, out, &para->pap );
        out_str( out, "\r\n" );
        for ( off = 0; off < para->len; off += run ) {
            run = wd_run( para, off );
            out_str( out, "  " );
            dump_chp( doc, out, wd_chp_get( doc, para->attr[off] ) );
            if ( para->text[off] == WCH_OBJ ) {
                which = wd_obj_at( para, off );
                if ( which < 0 || (u32)which >= doc->nobj ) {
                    out_str( out, " {object?}\r\n" );
                    continue;
                }
                obj = &doc->obj[which];
                if ( obj->kind == OBJ_IMAGE ) {
                    out_fmt( out, " {image fmt=%u bytes=%u size=%dx%d}\r\n", (u32)obj->fmt, obj->len,
                             obj->width, obj->height );
                } else {
                    out_fmt( out, " {%s}\r\n", obj->fmt == NOTE_END ? "endnote" : "footnote" );
                    if ( depth < 2 && obj->story != NULL ) {
                        dump_story( doc, out, obj->story, "  note", depth + 1 );
                    }
                }
                continue;
            }
            out_str( out, " " );
            dump_text( out, para->text + off, run );
            out_str( out, "\r\n" );
        }
    }
}

void wd_dump( WDOC *doc, OUT *out )
{
    u32 index;

    out_fmt( out, "page %dx%d margins %d,%d,%d,%d header %d footer %d%s first %u deftab %d\r\n",
             doc->page.width, doc->page.height, doc->page.left, doc->page.right, doc->page.top,
             doc->page.bottom, doc->page.header, doc->page.footer,
             doc->page.landscape ? " landscape" : "", (u32)doc->page.first_page, doc->deftab );
    if ( doc->title[0] ) {
        out_fmt( out, "title: %s\r\n", doc->title );
    }
    if ( doc->author[0] ) {
        out_fmt( out, "author: %s\r\n", doc->author );
    }
    if ( doc->subject[0] ) {
        out_fmt( out, "subject: %s\r\n", doc->subject );
    }
    if ( doc->keywords[0] ) {
        out_fmt( out, "keywords: %s\r\n", doc->keywords );
    }
    if ( doc->lost ) {
        out_fmt( out, "lost: %04X\r\n", doc->lost );
    }
    for ( index = 1; index < doc->nstyle; index++ ) {
        out_fmt( out, "style \"%s\" kind %u ", doc->style[index].name, (u32)doc->style[index].kind );
        dump_chp( doc, out, &doc->style[index].chp );
        dump_pap( doc, out, &doc->style[index].pap );
        out_str( out, "\r\n" );
    }
    dump_story( doc, out, &doc->body, "body", 0 );
    if ( doc->header.count ) {
        dump_story( doc, out, &doc->header, "header", 0 );
    }
    if ( doc->footer.count ) {
        dump_story( doc, out, &doc->footer, "footer", 0 );
    }
}
