/*
 * HLP.C - the help file: HELP.HLP read whole, its topics found, and a
 * line of it turned into what the screen shows.
 *
 * THE FILE IS HELPMAKE'S SOURCE FORMAT, as it stands - the text that
 * Microsoft's HELPMAKE compiled into MS-DOS's HELP.HLP, read here
 * without the compiling.  Anyone with EDIT can change it:
 *
 *   .context NAME     a name a topic answers to; several lines of these
 *                     in a row name the one topic.  "HELP NAME" and a
 *                     link in another topic reach it by any of them.
 *                     Case does not matter.
 *   .topic TITLE      the topic's title, shown in the window's title
 *   .freeze N         its first N lines stay put while the rest scroll
 *   .comment TEXT     ignored
 *   .list .category .next .previous .popup .paste .command .execute
 *   .length .raw .line                     accepted and ignored
 *
 * Anything else is a line of the topic, up to 255 columns, with these
 * in it:
 *
 *   \b   bold on and off          \i   italic on and off
 *   \u   underlined on and off    \p   plain: all of them off
 *   \a   a link's text starts     \v   ends the link's text and starts
 *                                      the name it leads to; a second
 *                                      \v ends the name
 *   \\   a backslash
 *
 * A link with no \v after it leads to the context its own text names.
 * Every '<', '>', 0x11 and 0x10 is shown in the link colour, which is
 * how the brackets round a link light up without being marked.
 * A line starting with a '.' that is not a directive above is text.
 *
 * The first topic in the file is the one shown when none is asked for
 * unless a topic answers to "Contents"; HELP.H lists the other names
 * the viewer itself looks for (F1, the menus' and dialogs' help).
 */
#include "help.h"

TOPIC   *topics;
int      ntopics;
CONTEXT *contexts;
int      ncontexts;
HLINE   *hlines;
u32      nhlines;

static char *file;              /* the whole file */
static u32   file_len;

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

static int read_whole( const char *path )
{
    u32 handle, size, done, got = 0;
    int rc;

    rc = sys_open_read( path, &handle );
    if ( rc ) {
        return rc;
    }
    rc = sys_file_size( handle, &size );
    if ( rc ) {
        sys_close( handle );
        return rc;
    }
    if ( size > 0x1000000UL ) {         /* 16M is more than any help file, */
        sys_close( handle );            /* and size + 2 must not wrap round */
        return -1;                      /* to a block of nothing */
    }
    file = (char *)try_alloc( size + 2 );
    if ( file == NULL ) {
        sys_close( handle );
        return -1;
    }
    while ( got < size ) {
        rc = sys_read( handle, file + got, size - got, &done );
        if ( rc || done == 0 ) {
            break;
        }
        got += done;
    }
    sys_close( handle );
    if ( rc ) {
        return rc;
    }
    file_len = got;
    file[got] = 0;
    return 0;
}

/* a directive at the start of a line: which, and where its text is */
enum { D_NONE, D_CONTEXT, D_TOPIC, D_FREEZE, D_IGNORE };

static int directive( const char *text, u16 len, const char **arg, int *arg_len )
{
    static const char *const names[] = {
        "context", "topic", "freeze", "comment", "list", "category", "next",
        "previous", "popup", "paste", "command", "execute", "length", "raw", "line", NULL
    };
    static const int kinds[] = {
        D_CONTEXT, D_TOPIC, D_FREEZE, D_IGNORE, D_IGNORE, D_IGNORE, D_IGNORE,
        D_IGNORE, D_IGNORE, D_IGNORE, D_IGNORE, D_IGNORE, D_IGNORE, D_IGNORE, D_IGNORE
    };
    int index, wlen;

    if ( len < 2 || text[0] != '.' ) {
        return D_NONE;
    }
    for ( wlen = 1; wlen < len && text[wlen] != ' ' && text[wlen] != '\t'; wlen++ ) {
    }
    for ( index = 0; names[index]; index++ ) {
        if ( (int)str_len( names[index] ) == wlen - 1
             && str_nicmp( text + 1, names[index], (u32)(wlen - 1) ) == 0 ) {
            while ( wlen < len && (text[wlen] == ' ' || text[wlen] == '\t') ) {
                wlen++;
            }
            *arg = text + wlen;
            *arg_len = len - wlen;
            while ( *arg_len > 0 && ((*arg)[*arg_len - 1] == ' ' || (*arg)[*arg_len - 1] == '\t') ) {
                (*arg_len)--;
            }
            return kinds[index];
        }
    }
    return D_NONE;
}

/* The file is walked twice: once to count, once to fill.  A topic
   starts at a .context that follows anything but another .context;
   the first text line after the directives closes the topic's head. */
static int walk( int fill )
{
    char *at = file, *end = file + file_len, *line;
    const char *arg;
    u16 len;
    int kind, arg_len, in_head = 0, ok;
    u32 lines = 0, topic_count = 0, ctx_count = 0;
    TOPIC *topic = NULL;

    while ( at < end ) {
        line = at;
        while ( at < end && *at != '\n' ) {
            at++;
        }
        len = (u16)(at - line);
        if ( at < end ) {
            at++;
        }
        if ( len && line[len - 1] == '\r' ) {
            len--;
        }
        if ( len > 1000 ) {
            len = 1000;
        }
        kind = directive( line, len, &arg, &arg_len );
        if ( kind == D_CONTEXT ) {
            if ( !in_head ) {
                in_head = 1;
                if ( fill ) {
                    topic = &topics[topic_count];
                    topic->title = NULL;
                    topic->first = lines;
                    topic->count = 0;
                    topic->freeze = 0;
                }
                topic_count++;
            }
            if ( fill ) {
                contexts[ctx_count].name = arg;
                contexts[ctx_count].topic = (int)topic_count - 1;
                ((char *)arg)[arg_len] = 0;     /* the line's end is ours to write on */
                if ( topic->title == NULL ) {
                    topic->title = arg;
                }
            }
            ctx_count++;
            continue;
        }
        if ( kind == D_TOPIC ) {
            if ( fill && topic ) {
                topic->title = arg;
                ((char *)arg)[arg_len] = 0;
            }
            in_head = 2;                        /* a .context after this starts a new topic */
            continue;
        }
        if ( kind == D_FREEZE ) {
            if ( fill && topic ) {
                /* the number alone: dec_parse wants nothing after the digits */
                char num[8];
                int ndigits = arg_len < 7 ? arg_len : 7;
                mem_cpy( num, arg, (u32)ndigits );
                num[ndigits] = 0;
                topic->freeze = (int)dec_parse( num, &ok );
                if ( !ok || topic->freeze > 20 ) {
                    topic->freeze = 0;
                }
            }
            continue;
        }
        if ( kind == D_IGNORE ) {
            continue;
        }
        /* a line of text */
        if ( topic_count == 0 ) {
            continue;                           /* before the first topic: a comment */
        }
        in_head = 0;
        if ( fill ) {
            hlines[lines].text = line;
            hlines[lines].len = len;
            topic->count++;
        }
        lines++;
    }
    if ( !fill ) {
        ntopics = (int)topic_count;
        ncontexts = (int)ctx_count;
        nhlines = lines;
    }
    return ntopics > 0;
}

int hlp_load( const char *path )
{
    int rc = read_whole( path );

    if ( rc ) {
        return rc;
    }
    if ( !walk( 0 ) ) {
        return -2;
    }
    topics = (TOPIC *)try_alloc( (u32)ntopics * sizeof( TOPIC ) );
    contexts = (CONTEXT *)try_alloc( (u32)ncontexts * sizeof( CONTEXT ) );
    hlines = (HLINE *)try_alloc( (nhlines + 1) * sizeof( HLINE ) );
    if ( topics == NULL || contexts == NULL || hlines == NULL ) {
        return -1;
    }
    walk( 1 );
    return 0;
}

/* ------------------------------------------------------------------ */
/* contexts                                                            */
/* ------------------------------------------------------------------ */

int hlp_find_n( const char *context, int len )
{
    int index;

    while ( len > 0 && (context[0] == ' ' || context[0] == '\t') ) {
        context++;
        len--;
    }
    while ( len > 0 && (context[len - 1] == ' ' || context[len - 1] == '\t') ) {
        len--;
    }
    if ( len == 0 ) {
        return -1;
    }
    for ( index = 0; index < ncontexts; index++ ) {
        if ( (int)str_len( contexts[index].name ) == len
             && str_nicmp( contexts[index].name, context, (u32)len ) == 0 ) {
            return contexts[index].topic;
        }
    }
    return -1;
}

int hlp_find( const char *context )
{
    return hlp_find_n( context, (int)str_len( context ) );
}

const HLINE *hlp_line( int topic, u32 line )
{
    static const HLINE none = { "", 0 };

    if ( topic < 0 || topic >= ntopics || line >= topics[topic].count ) {
        return &none;
    }
    return &hlines[topics[topic].first + line];
}

/* ------------------------------------------------------------------ */
/* rendering                                                           */
/* ------------------------------------------------------------------ */

static int is_bracket( int ch )
{
    return ch == '<' || ch == '>' || ch == 0x11 || ch == 0x10;
}

void hlp_render( u32 line, RLINE *out )
{
    const HLINE *src = &hlines[line];
    const char *text = src->text, *end = src->text + src->len;
    int bold = 0, italic = 0, under = 0, anchor = 0, index;
    HLINK *link = NULL;
    u8 attr;
    int ch;

    out->len = 0;
    out->nlinks = 0;
    while ( text < end ) {
        ch = (u8)*text++;
        if ( ch == '\\' && text < end ) {
            ch = (u8)*text++;
            switch ( ch_lower( ch ) ) {
            case 'b': bold = !bold;     continue;
            case 'i': italic = !italic; continue;
            case 'u': under = !under;   continue;
            case 'p': bold = italic = under = 0; continue;
            case 'a':
                if ( out->nlinks < RL_LINKS ) {
                    link = &out->links[out->nlinks++];
                    link->start = out->len;
                    link->end = out->len;
                    link->ctx = NULL;
                    link->ctx_len = 0;
                    anchor = 1;
                }
                continue;
            case 'v':
                if ( anchor && link ) {
                    /* the link's text ends; the context runs to the next \v */
                    link->end = out->len;
                    link->ctx = text;
                    while ( text < end && !(text[0] == '\\' && text + 1 < end && ch_lower( text[1] ) == 'v') ) {
                        text++;
                    }
                    link->ctx_len = (int)(text - link->ctx);
                    if ( text < end ) {
                        text += 2;
                    }
                    anchor = 0;
                }
                continue;
            case '\\':
                ch = '\\';
                break;
            default:
                break;                          /* not an escape: shown as it is */
            }
        }
        if ( ch == '\t' ) {
            ch = ' ';
        }
        if ( out->len < RL_MAX ) {
            attr = anchor ? pal.text : bold ? pal.bold : italic ? pal.italic
                   : under ? pal.under : pal.text;
            out->ch[out->len] = (u8)ch;
            out->attr[out->len] = attr;
            out->len++;
            if ( anchor && link ) {
                link->end = out->len;
            }
        }
    }
    /* every bracket is shown in the link colour, as the original shows
       them, whether or not a link is inside - bold ones stay bold */
    for ( index = 0; index < out->len; index++ ) {
        if ( is_bracket( out->ch[index] ) && out->attr[index] == pal.text ) {
            out->attr[index] = pal.italic;
        }
    }
}

/* the line as text alone, trailing blanks dropped */
void hlp_plain( u32 line, char *out, int max )
{
    static RLINE rl;
    int len;

    hlp_render( line, &rl );
    len = rl.len < max - 1 ? rl.len : max - 1;
    mem_cpy( out, rl.ch, (u32)len );
    while ( len > 0 && out[len - 1] == ' ' ) {
        len--;
    }
    out[len] = 0;
}
