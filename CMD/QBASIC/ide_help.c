/*
 * IDE_HELP.C - QBasic's help: QBASIC.HLP read whole, the help window at
 * the top of the screen, and the help boxes that menus, dialogs and
 * error messages open.
 *
 * THE FILE is HELPMAKE's source format, as HELP.EXE reads HELP.HLP (see
 * CMD\HELP\HLP.C, which this reading is): ".context NAME" lines name a
 * topic, ".topic" gives its title, and in a line \b is bold, \a starts a
 * link's text and \v the context it leads to.  TOOLS\MKQBHLP.PL writes
 * QBASIC.HLP from the parts in DISK\QBHELP.
 *
 * THE WINDOW is QBasic's: above the program, its title " HELP: topic ",
 * a row of links to the Contents, the Index and Back, a line, and the
 * topic.  F1 on a keyword opens it and leaves the keys with the program;
 * Shift+F1, Index and Contents put the keys in it.  Tab goes from link
 * to link, Enter follows one, Alt+F1 goes back, Esc shuts the window.
 *
 * QBASIC.HLP is looked for where Options > Help Path says, beside
 * QBASIC.EXE, in the current directory, and along the PATH.
 */
#include "ide.h"

typedef struct {
    const char *name;
    int   topic;
} CONTEXT;

typedef struct {
    const char *title;
    unsigned int first;
    unsigned int count;
} TOPIC;

typedef struct {
    const char *text;
    uint16_t len;
} HLINE;

#define RL_MAX      256
#define RL_LINKS    40

typedef struct {
    int   start, end;
    const char *ctx;
    int   ctx_len;
} HLINK;

typedef struct {
    uint8_t ch[RL_MAX];
    uint8_t attr[RL_MAX];
    int     len;
    HLINK   links[RL_LINKS];
    int     nlinks;
} RLINE;

static TOPIC       *topics;
static int          ntopics;
static CONTEXT     *contexts;
static int          ncontexts;
static HLINE       *hlines;
static unsigned int nhlines;
static char        *file;
static unsigned int file_len;
static int          loaded;         /* 1 read, -1 not there */

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

static int read_whole( const char *path )
{
    uint32_t handle, size;
    unsigned int done, got = 0;
    int rc;

    rc = sys_open_read( path, &handle );
    if ( rc ) {
        return rc;
    }
    rc = sys_file_size( handle, &size );
    if ( rc || size > 0x1000000UL ) {
        sys_close( handle );
        return rc ? rc : -1;
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
    file_len = got;
    file[got] = 0;
    return 0;
}

enum { D_NONE, D_CONTEXT, D_TOPIC, D_IGNORE };

static int directive( const char *text, uint16_t len, const char **arg, int *arg_len )
{
    static const char *const names[] = {
        "context", "topic", "freeze", "comment", "list", "category", "next",
        "previous", "popup", "paste", "command", "execute", "length", "raw", "line", NULL
    };
    int index, wlen;

    if ( len < 2 || text[0] != '.' ) {
        return D_NONE;
    }
    for ( wlen = 1; wlen < len && text[wlen] != ' ' && text[wlen] != '\t'; wlen++ ) {
    }
    for ( index = 0; names[index]; index++ ) {
        if ( (int)str_len( names[index] ) == wlen - 1
             && str_nicmp( text + 1, names[index], (unsigned int)(wlen - 1) ) == 0 ) {
            while ( wlen < len && (text[wlen] == ' ' || text[wlen] == '\t') ) {
                wlen++;
            }
            *arg = text + wlen;
            *arg_len = len - wlen;
            while ( *arg_len > 0 && ((*arg)[*arg_len - 1] == ' ' || (*arg)[*arg_len - 1] == '\t'
                                     || (*arg)[*arg_len - 1] == '\r') ) {
                (*arg_len)--;
            }
            return index == 0 ? D_CONTEXT : index == 1 ? D_TOPIC : D_IGNORE;
        }
    }
    return D_NONE;
}

static int walk( int fill )
{
    char *at = file, *end = file + file_len, *line;
    const char *arg;
    uint16_t len;
    int kind, arg_len, in_head = 0;
    unsigned int lines = 0, topic_count = 0, ctx_count = 0;
    TOPIC *topic = NULL;

    while ( at < end ) {
        line = at;
        while ( at < end && *at != '\n' ) {
            at++;
        }
        len = (uint16_t)(at - line);
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
                }
                topic_count++;
            }
            if ( fill ) {
                contexts[ctx_count].name = arg;
                contexts[ctx_count].topic = (int)topic_count - 1;
                ((char *)arg)[arg_len] = 0;
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
            in_head = 2;
            continue;
        }
        if ( kind == D_IGNORE ) {
            continue;
        }
        if ( topic_count == 0 ) {
            continue;
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

static int load_from( const char *dir, unsigned int dlen )
{
    char path[PATH_MAX];

    if ( dlen + 12 >= PATH_MAX ) {
        return 0;
    }
    mem_cpy( path, dir, dlen );
    path[dlen] = 0;
    if ( dlen && path[dlen - 1] != '\\' && path[dlen - 1] != ':' ) {
        str_catn( path, "\\", sizeof( path ) );
    }
    str_catn( path, "QBASIC.HLP", sizeof( path ) );
    if ( read_whole( path ) != 0 ) {
        return 0;
    }
    if ( !walk( 0 ) ) {
        xfree( file );
        file = NULL;
        return 0;
    }
    topics = (TOPIC *)try_alloc( (unsigned int)ntopics * sizeof( TOPIC ) );
    contexts = (CONTEXT *)try_alloc( (unsigned int)ncontexts * sizeof( CONTEXT ) );
    hlines = (HLINE *)try_alloc( (nhlines + 1) * sizeof( HLINE ) );
    if ( topics == NULL || contexts == NULL || hlines == NULL ) {
        xfree( topics );
        xfree( contexts );
        xfree( hlines );
        xfree( file );
        topics = NULL;
        contexts = NULL;
        hlines = NULL;
        file = NULL;
        return 0;
    }
    walk( 1 );
    return 1;
}

/* (again, after Options > Help Path) */
void help_init( void )
{
    const char *path, *end;

    if ( loaded > 0 ) {
        help_close();                           /* its topic and history are the old file's */
        xfree( topics );
        xfree( contexts );
        xfree( hlines );
        xfree( file );
        topics = NULL;
        contexts = NULL;
        hlines = NULL;
        file = NULL;
        ntopics = ncontexts = 0;
    }
    loaded = -1;
    if ( help_path[0] && load_from( help_path, str_len( help_path ) ) ) {
        loaded = 1;
        return;
    }
    {
        /* PM-DOS's own directory, where its shell is (and SETUP puts
           QBASIC.EXE): a native program is not told its own path */
        const char *shell = sys_shell(), *scan, *last = NULL;
        for ( scan = shell; scan != NULL && *scan; scan++ ) {
            if ( *scan == '\\' || *scan == ':' ) {
                last = scan;
            }
        }
        if ( last != NULL && load_from( shell, (unsigned int)(last - shell + 1) ) ) {
            loaded = 1;
            return;
        }
    }
    if ( load_from( ".", 1 ) ) {
        loaded = 1;
        return;
    }
    path = sys_getenv( "PATH" );
    while ( path && *path ) {
        for ( end = path; *end && *end != ';'; end++ ) {
        }
        if ( end > path && load_from( path, (unsigned int)(end - path) ) ) {
            loaded = 1;
            return;
        }
        path = *end ? end + 1 : end;
    }
}

static int find_n( const char *context, int len )
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
             && str_nicmp( contexts[index].name, context, (unsigned int)len ) == 0 ) {
            return contexts[index].topic;
        }
    }
    return -1;
}

static int find( const char *context )
{
    return find_n( context, (int)str_len( context ) );
}

/* ------------------------------------------------------------------ */
/* a line as it is shown                                               */
/* ------------------------------------------------------------------ */

static RLINE rl;

static int is_bracket( int ch )
{
    return ch == 0x11 || ch == 0x10;
}

static void render_line( unsigned int line, RLINE *out, uint8_t text_attr )
{
    const HLINE *src = &hlines[line];
    const char *text = src->text, *end = src->text + src->len;
    int bold = 0, anchor = 0, index, ch;
    HLINK *link = NULL;

    out->len = 0;
    out->nlinks = 0;
    while ( text < end ) {
        ch = (uint8_t)*text++;
        if ( ch == '\\' && text < end ) {
            ch = (uint8_t)*text++;
            switch ( ch_lower( ch ) ) {
            case 'b': bold = !bold;  continue;
            case 'i': case 'u':      continue;
            case 'p': bold = 0;      continue;
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
                break;
            }
        }
        if ( ch == '\t' ) {
            ch = ' ';
        }
        if ( out->len < RL_MAX ) {
            out->ch[out->len] = (uint8_t)ch;
            out->attr[out->len] = bold ? pal.help_bold : text_attr;
            out->len++;
            if ( anchor && link ) {
                link->end = out->len;
            }
        }
    }
    for ( index = 0; index < out->len; index++ ) {
        if ( is_bracket( out->ch[index] ) ) {
            out->attr[index] = (uint8_t)((text_attr & 0xF0) | (pal.help_link & 0x0F));
        }
    }
    /* a link with no \v leads where its own text says */
    for ( index = 0; index < out->nlinks; index++ ) {
        if ( out->links[index].ctx == NULL ) {
            out->links[index].ctx = (const char *)out->ch + out->links[index].start;
            out->links[index].ctx_len = out->links[index].end - out->links[index].start;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the help window                                                     */
/* ------------------------------------------------------------------ */

#define HISTORY     20

static int          shown_topic = -1;   /* -1: the window is shut */
static unsigned int h_top, h_line, h_col, h_want;
static int          history[HISTORY], nhistory;
static const char nav_line[] = "  \x11\\aContents\\vcontents\\v\x10  \x11\\aIndex\\vindex\\v\x10  \x11\\aBack\\vback\\v\x10";

int help_shown( void )
{
    return shown_topic >= 0;
}

static int topic_lines( void )
{
    return shown_topic < 0 ? 0 : (int)topics[shown_topic].count;
}

/* the window's rows: the title, the links, a line, and the topic, at
   most 20, fewer for a short topic */
int help_rows( void )
{
    int rows = 3 + topic_lines();

    if ( shown_topic < 0 ) {
        return 0;
    }
    if ( rows > 20 ) {
        rows = 20;
    }
    if ( rows < 8 ) {
        rows = 8;
    }
    if ( rows > scr_rows - 5 ) {
        rows = scr_rows - 5;
    }
    return rows;
}

static int text_rows( void )
{
    return help_rows() - 3;
}

static void keep_visible( void )
{
    int rows = text_rows();
    unsigned int count = (unsigned int)topic_lines();

    if ( h_line < h_top ) {
        h_top = h_line;
    }
    if ( h_line >= h_top + (unsigned int)rows ) {
        h_top = h_line - (unsigned int)rows + 1;
    }
    if ( count > (unsigned int)rows && h_top > count - (unsigned int)rows ) {
        h_top = count - (unsigned int)rows;
    }
}

/* line -1 is the row of links at the top */
static void render( int line )
{
    static HLINE nav;
    HLINE *save;

    if ( line < 0 ) {
        nav.text = nav_line;
        nav.len = (uint16_t)str_len( nav_line );
        save = hlines;
        hlines = &nav;
        render_line( 0, &rl, pal.help );
        hlines = save;
        return;
    }
    if ( shown_topic < 0 || (unsigned int)line >= topics[shown_topic].count ) {
        rl.len = 0;
        rl.nlinks = 0;
        return;
    }
    render_line( topics[shown_topic].first + (unsigned int)line, &rl, pal.help );
}

static int next_link( int step )
{
    unsigned int count = (unsigned int)topic_lines(), line = h_line, tries;
    int index, found = -1;

    if ( count == 0 ) {
        return 0;
    }
    render( (int)line );
    if ( step > 0 ) {
        for ( index = 0; index < rl.nlinks; index++ ) {
            if ( rl.links[index].start > (int)h_col ) {
                found = index;
                break;
            }
        }
    } else {
        for ( index = rl.nlinks - 1; index >= 0; index-- ) {
            if ( rl.links[index].start < (int)h_col ) {
                found = index;
                break;
            }
        }
    }
    for ( tries = 0; found < 0 && tries < count; tries++ ) {
        line = step > 0 ? (line + 1) % count : (line == 0 ? count - 1 : line - 1);
        render( (int)line );
        if ( rl.nlinks ) {
            found = step > 0 ? 0 : rl.nlinks - 1;
        }
    }
    if ( found < 0 ) {
        return 0;
    }
    h_line = line;
    h_col = (unsigned int)rl.links[found].start;
    h_want = h_col;
    keep_visible();
    return 1;
}

static void show( int topic, int remember )
{
    if ( remember && shown_topic >= 0 && shown_topic != topic ) {
        if ( nhistory == HISTORY ) {
            mem_cpy( history, history + 1, (HISTORY - 1) * sizeof( int ) );
            nhistory--;
        }
        history[nhistory++] = shown_topic;
    }
    shown_topic = topic;
    h_top = 0;
    h_line = 0;
    h_col = 0;
    h_want = 0;
    app_layout();
}

static int topic_or_missing( const char *name )
{
    int topic;

    if ( loaded == 0 ) {
        help_init();
    }
    if ( loaded < 0 ) {
        msg_box2( "QBASIC.HLP cannot be found.", "Options > Help Path can say where it is.", MB_OK, 0 );
        return -1;
    }
    topic = find( name );
    if ( topic < 0 ) {
        topic = find( "notfound" );
    }
    if ( topic < 0 ) {
        msg_box( "There is no help on that.", MB_OK, 0 );
    }
    return topic;
}

/* is there a topic of that name?  (QBASIC.HLP is read first if need be) */
int help_has( const char *name )
{
    if ( loaded == 0 ) {
        help_init();
    }
    return loaded > 0 && find( name ) >= 0;
}

void help_topic( const char *name )
{
    int topic = topic_or_missing( name );

    if ( topic >= 0 ) {
        show( topic, 1 );
    }
}

void help_close( void )
{
    shown_topic = -1;
    nhistory = 0;
    app_layout();
}

void help_draw( int active )
{
    char title[90];
    int rows = help_rows(), row, len, col, index;
    uint8_t frame = pal.help;

    if ( shown_topic < 0 ) {
        return;
    }
    /* the title */
    scr_fill( 1, 0, scr_cols, 0xC4, frame );
    scr_ch( 1, 0, 0xDA, frame );
    scr_ch( 1, scr_cols - 1, 0xBF, frame );
    str_cpy( title, " HELP: " );
    str_catn( title, topics[shown_topic].title ? topics[shown_topic].title : "", sizeof( title ) - 2 );
    str_catn( title, " ", sizeof( title ) );
    len = (int)str_len( title );
    if ( len > scr_cols - 4 ) {
        len = scr_cols - 4;
    }
    scr_putn( 1, (scr_cols - len) / 2, title, len, active ? pal.menu : frame );
    /* the links to Contents, Index and Back, and a line under them */
    scr_ch( 2, 0, 0xB3, frame );
    scr_fill( 2, 1, scr_cols - 2, ' ', frame );
    render( -1 );
    for ( col = 0; col < rl.len && col < scr_cols - 3; col++ ) {
        scr_ch( 2, 1 + col, rl.ch[col], rl.attr[col] );
    }
    scr_ch( 2, scr_cols - 1, 0xB3, frame );
    scr_ch( 3, 0, 0xC3, frame );
    scr_fill( 3, 1, scr_cols - 2, 0xC4, frame );
    scr_ch( 3, scr_cols - 1, 0xB4, frame );
    for ( row = 0; row < rows - 3; row++ ) {
        int at = 4 + row;
        scr_ch( at, 0, 0xB3, frame );
        scr_fill( at, 1, scr_cols - 2, ' ', frame );
        render( (int)(h_top + (unsigned int)row) );
        for ( col = 0; col < rl.len && col < scr_cols - 2; col++ ) {
            scr_ch( at, 1 + col, rl.ch[col], rl.attr[col] );
        }
        scr_ch( at, scr_cols - 1, 0xB3, frame );
    }
    if ( active && rows - 3 >= 3 ) {
        /* the scroll bar, from the links' row down */
        int first = 2, last = rows, track = last - first - 1, thumb;
        unsigned int count = (unsigned int)topic_lines();
        scr_ch( first, scr_cols - 1, 0x18, pal.scroll );
        for ( index = first + 1; index < last; index++ ) {
            scr_ch( index, scr_cols - 1, 0xB0, pal.scroll );
        }
        scr_ch( last, scr_cols - 1, 0x19, pal.scroll );
        thumb = count > 1 ? (int)(h_line * (unsigned int)(track - 1) / (count - 1)) : 0;
        scr_ch( first + 1 + thumb, scr_cols - 1, ' ', pal.thumb );
    }
    if ( active ) {
        scr_cursor( 4 + (int)(h_line - h_top), 1 + (int)h_col, CUR_LINE );
    }
}

/* the link under the cursor followed; "back" goes back */
static void follow( const char *ctx, int len )
{
    char name[80];
    int topic;

    if ( len <= 0 ) {
        return;
    }
    if ( len >= (int)sizeof( name ) ) {
        len = sizeof( name ) - 1;
    }
    mem_cpy( name, ctx, (unsigned int)len );
    name[len] = 0;
    if ( str_icmp( name, "back" ) == 0 ) {
        if ( nhistory > 0 ) {
            show( history[--nhistory], 0 );
        }
        return;
    }
    topic = topic_or_missing( name );
    if ( topic >= 0 ) {
        show( topic, 1 );
    }
}

static int link_under( unsigned int line, unsigned int col, HLINK *out )
{
    int index;

    render( (int)line );
    for ( index = 0; index < rl.nlinks; index++ ) {
        if ( (int)col >= rl.links[index].start - 1 && (int)col <= rl.links[index].end ) {
            *out = rl.links[index];
            return 1;
        }
    }
    return 0;
}

/* a letter in the Index: to that letter's list (the line with its box) */
static int jump_letter( int letter )
{
    unsigned int line, count = (unsigned int)topic_lines(), first = topics[shown_topic].first;
    const HLINE *src;
    unsigned int at;

    letter = ch_upper( letter );
    for ( line = 0; line < count; line++ ) {
        src = &hlines[first + line];
        for ( at = 0; at + 3 < src->len; at++ ) {
            if ( (uint8_t)src->text[at] == 0xB9 && src->text[at + 1] == ' '
                 && ch_upper( src->text[at + 2] ) == letter && src->text[at + 3] == ' ' ) {
                h_top = line;
                h_line = line + 2 < count ? line + 2 : line;
                h_col = h_want = 2;
                return 1;
            }
        }
    }
    return 0;
}

int help_key( int key )
{
    unsigned int count = (unsigned int)topic_lines();
    int rows = text_rows();
    HLINK link;

    if ( shown_topic < 0 ) {
        return 0;
    }
    switch ( key ) {
    case K_ESC:
        help_close();
        return 1;
    case K_ALTF1:
        follow( "back", 4 );
        return 1;
    case K_CTRLF1:
        show( (shown_topic + 1) % ntopics, 1 );
        return 1;
    case K_ENTER:
        if ( link_under( h_line, h_col, &link ) ) {
            follow( link.ctx, link.ctx_len );
        }
        return 1;
    case K_TAB:
        next_link( 1 );
        return 1;
    case K_SHTAB:
        next_link( -1 );
        return 1;
    case K_UP:
        if ( h_line > 0 ) {
            h_line--;
        }
        h_col = h_want;
        break;
    case K_DOWN:
        if ( h_line + 1 < count ) {
            h_line++;
        }
        h_col = h_want;
        break;
    case K_LEFT:
        if ( h_col > 0 ) {
            h_col--;
        }
        h_want = h_col;
        break;
    case K_RIGHT:
        if ( h_col < (unsigned int)scr_cols - 3 ) {
            h_col++;
        }
        h_want = h_col;
        break;
    case K_HOME:
        h_col = h_want = 0;
        break;
    case K_END:
        render( (int)h_line );
        h_col = h_want = rl.len ? (unsigned int)rl.len - 1 : 0;
        break;
    case K_PGUP:
        h_line = h_line > (unsigned int)(rows - 1) ? h_line - (unsigned int)(rows - 1) : 0;
        h_top = h_top > (unsigned int)(rows - 1) ? h_top - (unsigned int)(rows - 1) : 0;
        break;
    case K_PGDN:
        h_line += (unsigned int)(rows - 1);
        if ( h_line >= count ) {
            h_line = count ? count - 1 : 0;
        }
        h_top += (unsigned int)(rows - 1);
        break;
    case K_CHOME:
        h_line = h_col = h_want = 0;
        h_top = 0;
        break;
    case K_CEND:
        h_line = count ? count - 1 : 0;
        break;
    default:
        if ( ((key >= 'A' && key <= 'Z') || (key >= 'a' && key <= 'z')) && jump_letter( key ) ) {
            break;
        }
        return 0;
    }
    keep_visible();
    return 1;
}

void help_mouse( void )
{
    HLINK link;
    int row = mouse.row, col = mouse.col;

    if ( shown_topic < 0 ) {
        return;
    }
    if ( row == 2 ) {
        render( -1 );
        {
            int index;
            for ( index = 0; index < rl.nlinks; index++ ) {
                if ( col - 1 >= rl.links[index].start - 1 && col - 1 <= rl.links[index].end ) {
                    follow( rl.links[index].ctx, rl.links[index].ctx_len );
                    return;
                }
            }
        }
        return;
    }
    if ( row >= 4 && row < help_rows() + 1 && col >= 1 && col < scr_cols - 1 ) {
        h_line = h_top + (unsigned int)(row - 4);
        if ( h_line >= (unsigned int)topic_lines() ) {
            h_line = topic_lines() ? (unsigned int)topic_lines() - 1 : 0;
        }
        h_col = h_want = (unsigned int)(col - 1);
        if ( mouse.kind == ME_DOUBLE && link_under( h_line, h_col, &link ) ) {
            follow( link.ctx, link.ctx_len );
        }
    } else if ( col == scr_cols - 1 ) {
        if ( row == 2 ) {
            help_key( K_UP );
        } else if ( row == help_rows() ) {
            help_key( K_DOWN );
        } else if ( row > 2 && row < help_rows() ) {
            help_key( row < 4 + text_rows() / 2 ? K_PGUP : K_PGDN );
        }
    }
}

/* ------------------------------------------------------------------ */
/* keywords                                                            */
/* ------------------------------------------------------------------ */

int help_has_keyword( VIEW *view )
{
    char word[40];

    view_word_at( view, word, sizeof( word ) );
    return word[0] != 0;
}

void help_keyword( VIEW *view )
{
    char word[40];
    unsigned int len;

    view_word_at( view, word, sizeof( word ) );
    if ( word[0] == 0 ) {
        help_topic( "contents" );
        return;
    }
    len = str_len( word );
    while ( len > 1 && (word[len - 1] == '.' ) ) {
        word[--len] = 0;
    }
    if ( loaded == 0 ) {
        help_init();
    }
    if ( loaded > 0 && find( word ) < 0 && kw_lookup( word, len ) < 0 ) {
        /* a name of the program's own: help says what kind it is */
        help_topic( "names" );
        return;
    }
    help_topic( word );
}

/* ------------------------------------------------------------------ */
/* help boxes                                                          */
/* ------------------------------------------------------------------ */

/* a topic in a box of its own, over a menu or a dialog: its lines in an
   inner box that scrolls, and OK under them */
void help_box( const char *name )
{
    int topic = topic_or_missing( name ), rows, top, count, key, row, col, inner;
    unsigned int first = 0;
    char title[80];

    if ( topic < 0 ) {
        return;
    }
    count = (int)topics[topic].count;
    inner = count < 14 ? (count > 1 ? count : 1) : 14;
    rows = inner + 6;
    top = (scr_rows - rows) / 2;
    str_cpy( title, " HELP: " );
    str_catn( title, topics[topic].title ? topics[topic].title : "", sizeof( title ) - 2 );
    str_catn( title, " ", sizeof( title ) );
    for ( ;; ) {
        ui_redraw();
        scr_box( top, 6, top + rows - 1, 73, pal.menu );
        scr_shadow( top, 6, top + rows - 1, 73 );
        scr_putn( top, (80 - (int)str_len( title )) / 2, title, (int)str_len( title ), pal.menu );
        scr_box( top + 1, 8, top + 2 + inner, 71, pal.menu );
        for ( row = 0; row < inner; row++ ) {
            if ( first + (unsigned int)row < (unsigned int)count ) {
                render_line( topics[topic].first + first + (unsigned int)row, &rl, pal.menu );
                for ( col = 0; col < rl.len && col < 62; col++ ) {
                    scr_ch( top + 2 + row, 9 + col, rl.ch[col],
                            rl.attr[col] == pal.help_bold ? pal.menu_hot : rl.attr[col] );
                }
            }
        }
        if ( count > inner ) {
            int thumb = (int)(first * (unsigned int)(inner - 3) / (unsigned int)(count - inner));
            scr_ch( top + 2, 71, 0x18, pal.scroll );
            for ( row = 1; row < inner - 1; row++ ) {
                scr_ch( top + 2 + row, 71, 0xB0, pal.scroll );
            }
            scr_ch( top + 1 + inner, 71, 0x19, pal.scroll );
            scr_ch( top + 3 + thumb, 71, ' ', pal.thumb );
        }
        scr_ch( top + rows - 3, 6, 0xC3, pal.menu );
        scr_fill( top + rows - 3, 7, 66, 0xC4, pal.menu );
        scr_ch( top + rows - 3, 73, 0xB4, pal.menu );
        scr_ch( top + rows - 2, 36, '<', pal.menu_hot );
        scr_put( top + rows - 2, 37, "  OK  ", pal.menu );
        scr_ch( top + rows - 2, 43, '>', pal.menu_hot );
        scr_cursor( top + rows - 2, 39, CUR_LINE );
        app_status_wide( " F1=Help   Enter=Execute   Esc=Cancel   Tab=Next Field   Arrow=Next Item" );
        scr_flush();
        key = key_get();
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key == K_DUMP ) {
            scr_dump();
            continue;
        }
        if ( key == K_ENTER || key == K_ESC || key == ' ' ) {
            break;
        }
        if ( key == K_MOUSE ) {
            if ( mouse.kind == ME_UP && mouse.row == top + rows - 2 && mouse.col >= 36 && mouse.col <= 43 ) {
                break;
            }
            if ( mouse.kind == ME_DOWN && mouse.col == 71 ) {
                key = mouse.row <= top + 2 ? K_UP : K_DOWN;
            } else {
                continue;
            }
        }
        if ( (key == K_DOWN || key == K_PGDN) && first + (unsigned int)inner < (unsigned int)count ) {
            first += key == K_DOWN ? 1 : (unsigned int)inner;
            if ( first + (unsigned int)inner > (unsigned int)count ) {
                first = (unsigned int)(count - inner);
            }
        } else if ( key == K_UP && first > 0 ) {
            first--;
        } else if ( key == K_PGUP ) {
            first = first > (unsigned int)inner ? first - (unsigned int)inner : 0;
        }
    }
    app_status( NULL );
}
