/*
 * MAIN.C - PMWRITE: a word processor for PM-DOS.
 *
 *   PMWRITE [file ...] [/T | /G[:width]] [/P:n] [/X]
 *
 * THE SCREEN is drawn whole on every turn of the loop (DISP.H): the
 * menu bar, what BARS.C draws round the document, and the document
 * itself, which is VIEW.C's.  app_layout says where each is, from the
 * screen's size and what View has switched on - so taking the toolbar
 * away, or changing from a text screen to a graphics one or between
 * resolutions, is a matter of drawing the next turn differently.  The
 * documents do not notice: an EDITOR lays its text out for whichever
 * kind of screen it finds itself on.
 *
 * WHAT IS REMEMBERED from one run to the next is in PMWRITE.INI,
 * beside PMWRITE.EXE: the screen, the zoom, and which bars show.  One
 * line there is the user's and is only kept: "Fonts=dir;dir" names
 * directories of TrueType files, which are looked in after FONTS beside
 * the program and before Windows' own (TF.H).
 *
 * WHEN MEMORY RUNS OUT in a place that cannot answer "no" - the PDF
 * code and the screen take memory that way - every changed document
 * is written as text to PMWRITEn.SAV beside the program before the
 * program ends.  The editing commands themselves never end it: they
 * answer that there is not enough memory and leave the document whole.
 */
#include "pw.h"
#include "pdfview.h"

PWDOC      docs[MAX_DOCS];
int        ndocs;
int        cur_doc;
SETTINGS   set;
SCREENPLAN plan;

extern int txt_codepage;                /* F_TXT.C: the code page a text file is taken to be in */
extern int sfont_as_acroread;           /* PDF\SFONT.C: 1 keeps a mistake ACROREAD drew PDFs with */
extern const FORMAT fmt_txt;

static int menu_row_set = -1;           /* the row the menus were last told they are on */
static int screen_on;                   /* disp_init has been called */
static char font_dirs[PATH_LEN];        /* PMWRITE.INI's Fonts= line */

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

int mem_reclaim( void )
{
    return pdfview_reclaim() || tf_reclaim() || view_reclaim();
}

void no_memory( void )
{
    msg_box( "There is not enough memory to do that.", MB_OK );
}

void out_of_memory( void )
{
    static int here;
    static const char said[] = "PMWRITE: out of memory. Changed documents were saved as PMWRITEn.SAV.\r\n";
    OUT *out;
    char path[PATH_LEN];
    int index;

    pdfview_out_of_memory();            /* does not come back if the PDF code was what ran out */
    if ( here++ == 0 ) {
        for ( index = 0; index < ndocs; index++ ) {
            if ( docs[index].ed == NULL || !ed_modified( docs[index].ed ) ) {
                continue;
            }
            sfmt( path, sizeof( path ), "%sPMWRITE%u.SAV", sys_program_dir(), (uint32_t)index + 1 );
            out = out_file( path );
            if ( out != NULL ) {
                fmt_txt.save( docs[index].ed->doc, out, 2 );
                out_close( out );
            }
        }
    }
    if ( screen_on ) {
        disp_done();
    }
    sys_stdout( said, sizeof( said ) - 1 );
    sys_exit( 8 );
}

/* ------------------------------------------------------------------ */
/* PMWRITE.INI                                                         */
/* ------------------------------------------------------------------ */

static void ini_path( char *path )
{
    sfmt( path, PATH_LEN, "%sPMWRITE.INI", sys_program_dir() );
}

static void settings_load( void )
{
    static const char *const keys[] = { "Toolbar", "Ruler", "StatusBar", "ScreenWidth", "ScreenHeight", "Zoom",
                                        "Inches", "MatchCase", "WholeWord" };
    int *const values[] = { &set.show_toolbar, &set.show_ruler, &set.show_status, &set.mode_w, &set.mode_h,
                            &set.zoom, &set.inches, &set.find_case, &set.find_word };
    char path[PATH_LEN], number[12];
    uint8_t *data;
    uint32_t len;
    unsigned int pos = 0, start, index, used;
    int ok;

    set.show_toolbar = set.show_ruler = set.show_status = 1;
    set.mode_w = 0;
    set.mode_h = 0;                     /* the text screen as it is found */
    set.zoom = 100;
    set.inches = country.date_order == 0;       /* month first: the country of inches */
    ini_path( path );
    data = file_read( path, &len, 1 );
    if ( data == NULL ) {
        return;
    }
    while ( pos < len ) {
        start = pos;
        while ( pos < len && data[pos] != '\r' && data[pos] != '\n' ) {
            pos++;
        }
        if ( pos - start > 6 && str_nicmp( (const char *)data + start, "Fonts=", 6 ) == 0 ) {
            used = pos - start - 6 >= sizeof( font_dirs ) ? sizeof( font_dirs ) - 1 : pos - start - 6;
            mem_cpy( font_dirs, data + start + 6, used );
            font_dirs[used] = 0;
        }
        for ( index = 0; index < sizeof( keys ) / sizeof( keys[0] ); index++ ) {
            used = str_len( keys[index] );
            if ( pos - start > used && data[start + used] == '=' &&
                 str_nicmp( (const char *)data + start, keys[index], used ) == 0 ) {
                used++;
                mem_set( number, 0, sizeof( number ) );
                mem_cpy( number, data + start + used, pos - start - used > 9 ? 9 : pos - start - used );
                *values[index] = (int)dec_parse( number, &ok );
                if ( !ok ) {
                    *values[index] = 0;
                }
            }
        }
        while ( pos < len && (data[pos] == '\r' || data[pos] == '\n') ) {
            pos++;
        }
    }
    xfree( data );
    if ( set.zoom < 25 || set.zoom > 400 ) {
        set.zoom = 100;
    }
}

void settings_save( void )
{
    OUT *out;
    char path[PATH_LEN];

    ini_path( path );
    out = out_file( path );
    if ( out == NULL ) {
        return;                         /* a disk that cannot be written: nothing is remembered */
    }
    out_fmt( out, "Toolbar=%u\r\nRuler=%u\r\nStatusBar=%u\r\nScreenWidth=%u\r\nScreenHeight=%u\r\nZoom=%u\r\n"
             "Inches=%u\r\nMatchCase=%u\r\nWholeWord=%u\r\n", (uint32_t)set.show_toolbar, (uint32_t)set.show_ruler,
             (uint32_t)set.show_status, (uint32_t)set.mode_w, (uint32_t)set.mode_h, (uint32_t)set.zoom, (uint32_t)set.inches,
             (uint32_t)set.find_case, (uint32_t)set.find_word );
    if ( font_dirs[0] ) {
        out_fmt( out, "Fonts=%s\r\n", font_dirs );
    }
    out_close( out );
}

/* ------------------------------------------------------------------ */
/* the documents that are open                                         */
/* ------------------------------------------------------------------ */

void doc_name( int index )
{
    PWDOC *doc = &docs[index];
    const char *base = doc->path, *scan;

    for ( scan = doc->path; *scan; scan++ ) {
        if ( *scan == '\\' || *scan == ':' || *scan == '/' ) {
            base = scan + 1;
        }
    }
    if ( *base ) {
        str_cpyt( doc->name, base, sizeof( doc->name ) );
    } else if ( doc->ed != NULL && doc->ed->untitled ) {
        sfmt( doc->name, sizeof( doc->name ), "Document%u", (uint32_t)doc->ed->untitled );
    } else {
        str_cpy( doc->name, "Untitled" );
    }
}

int doc_find( const char *path )
{
    int index;

    for ( index = 0; index < ndocs; index++ ) {
        if ( docs[index].path[0] && str_icmp( docs[index].path, path ) == 0 ) {
            return index;
        }
    }
    return -1;
}

int doc_add( EDITOR *ed, PDFDOCVIEW *pdf, const char *path )
{
    PWDOC *doc;

    if ( ndocs >= MAX_DOCS ) {
        msg_box( "No more documents can be open at once. Close one first.", MB_OK );
        return -1;
    }
    doc = &docs[ndocs];
    mem_set( doc, 0, sizeof( PWDOC ) );
    doc->ed = ed;
    doc->pdf = pdf;
    str_cpyt( doc->path, path, PATH_LEN );
    if ( ed != NULL ) {
        str_cpyt( ed->path, path, PATH_LEN );
    }
    doc_name( ndocs );
    ndocs++;
    cmds_init();
    return ndocs - 1;
}

void doc_show( int index )
{
    if ( index >= 0 && index < ndocs ) {
        cur_doc = index;
        view_reset();
        bars_changed();
    }
}

void doc_close( int index )
{
    int scan;

    if ( index < 0 || index >= ndocs ) {
        return;
    }
    /* both, though it has one of them: the documents after this one are
       about to be renumbered, and view_free_pdf also makes the PDF code
       forget which number's file it has open */
    ed_free( docs[index].ed );
    view_free_pdf( docs[index].pdf );
    for ( scan = index; scan + 1 < ndocs; scan++ ) {
        docs[scan] = docs[scan + 1];
    }
    ndocs--;
    /* the place past the last is empty, and with none open that is the
       place "the document on the screen" looks at */
    mem_set( &docs[ndocs], 0, sizeof( PWDOC ) );
    if ( cur_doc > index || cur_doc >= ndocs ) {
        cur_doc = cur_doc > 0 ? cur_doc - 1 : 0;
    }
    view_reclaim();
    view_reset();
    bars_changed();
    cmds_init();
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

void app_layout( void )
{
    int row = 0, editing = cur_doc < ndocs && ED != NULL;

    plan.title_row = gfx_on ? row++ : -1;
    plan.menu_row = row++;
    plan.tool_row = row;
    plan.tool_rows = gfx_on && set.show_toolbar && scr_rows >= 20 ? 2 : 0;
    row += plan.tool_rows;
    plan.ribbon_row = set.show_toolbar && editing ? row++ : -1;
    plan.ruler_row = set.show_ruler && editing ? row++ : -1;
    plan.status_row = set.show_status ? scr_rows - 1 : -1;
    plan.doc_top = row;
    plan.doc_rows = scr_rows - row - (set.show_status ? 1 : 0);
    if ( plan.doc_rows < 1 ) {
        plan.doc_rows = 1;
    }
    plan.doc_cols = scr_cols - 1;
}

void app_draw( void )
{
    int row;

    app_layout();
    if ( plan.menu_row != menu_row_set ) {
        menu_row_set = plan.menu_row;
        cmds_init();
    }
    menu_draw_bar( -1, gfx_on );
    /* the document first: the ruler and the status bar show what its
       lines came out as, and they are measured when it is drawn */
    if ( ndocs > 0 ) {
        view_draw();
    } else {
        for ( row = 0; row < plan.doc_rows; row++ ) {
            scr_fill( plan.doc_top + row, 0, scr_cols, ' ', pal.desk );
        }
        scr_cursor( 0, 0, CUR_HIDE );
        caret_off();
    }
    bars_draw();
}

void app_status( const char *hint )
{
    app_message( hint );
}

void app_abandon( void )
{
    disp_done();
    sys_exit( 0 );
}

void screen_apply( int mode_index )
{
    const DISPMODE *info;

    ui_set_mode( mode_index );
    /* sixteen colors: a file's thin strokes are kept at gfx_cover's cut */
    tf_screen( gfx_on && gfx_colors != 256 ? 96 : 0 );
    info = disp_mode_info( disp_mode_now() );
    if ( info != NULL ) {
        set.mode_w = info->width;
        set.mode_h = info->width ? info->height : info->rows;
    }
    menu_row_set = -1;
    view_reset();
    bars_changed();
}

void app_quit( void )
{
    int index;

    for ( index = ndocs - 1; index >= 0; index-- ) {
        if ( !file_close_ask( index ) ) {
            return;
        }
    }
    settings_save();
    while ( ndocs > 0 ) {
        doc_close( ndocs - 1 );
    }
    ed_clip_free();
    pdfview_close();
    tf_done();
    disp_done();
    sys_exit( 0 );
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */
/* ------------------------------------------------------------------ */

static const char usage[] =
    "PMWRITE, word processor for PM-DOS.\r\n"
    "\r\n"
    "PMWRITE [file ...] [/T | /G[:width]] [/P:n] [/X]\r\n"
    "\r\n"
    "  file      The documents to open.\r\n"
    "  /T        Starts on the text screen.\r\n"
    "  /G        Starts on a graphics screen 640 pixels wide.\r\n"
    "  /G:width  Starts on a graphics screen 800, 1024 or 1280 pixels wide.\r\n"
    "  /P:n      Starts a PDF at page n.\r\n"
    "  /X        Writes the text of each file to standard output. No screen is shown.\r\n";

static void say2( const char *text, const char *name )
{
    sys_stdout( text, str_len( text ) );
    sys_stdout( name, str_len( name ) );
    sys_stdout( "\r\n", 2 );
}

/* /X: a document's words to standard output, as MS-DOS text -> 0 if
   they were written.  Nothing of the screen is used. */
static int extract_text( const char *path )
{
    WDOC *doc;
    OUT *out;
    uint8_t *data;
    uint32_t len;
    int id, rc;

    data = file_read( path, &len, 1 );
    if ( data == NULL ) {
        say2( "File not found - ", path );
        return 1;
    }
    id = fmt_detect( data, len, path );
    doc = id == FMT_NONE ? NULL : wd_new();
    rc = doc == NULL ? WE_NOMEM : formats[id]->load( data, len, doc );
    xfree( data );
    if ( rc == WE_OK && wd_failed( doc ) ) {
        rc = WE_NOMEM;
    }
    if ( rc != WE_OK ) {
        wd_free( doc );
        say2( rc == WE_NOMEM ? "Not enough memory to read " : rc == WE_PASSWORD ? "Protected with a password - " :
              "Cannot read ", path );
        return 1;
    }
    doc->failed = 0;
    wd_finish( doc );
    out = out_mem();
    rc = out == NULL ? WE_NOMEM : fmt_txt.save( doc, out, 0 );
    if ( rc == WE_OK && !out_error( out ) ) {
        data = out_data( out, &len );
        sys_stdout( (const char *)data, len );
    } else {
        rc = WE_NOMEM;
        say2( "Not enough memory to read ", path );
    }
    out_close( out );
    wd_free( doc );
    return rc != WE_OK;
}

/* the next word of the command line, or a name in quotes: 0 at the end */
static int next_word( const char **line, char *word, unsigned int max )
{
    const char *cur = *line;
    unsigned int used = 0;
    int quoted = 0;

    while ( *cur == ' ' || *cur == '\t' ) {
        cur++;
    }
    if ( *cur == 0 || *cur == '\r' ) {
        return 0;
    }
    while ( *cur && *cur != '\r' && (quoted || (*cur != ' ' && *cur != '\t')) ) {
        if ( *cur == '"' ) {
            quoted = !quoted;
        } else if ( used + 1 < max ) {
            word[used++] = *cur;
        }
        cur++;
    }
    word[used] = 0;
    *line = cur;
    return 1;
}

void pw_main( void )
{
    static const int heights[] = { 480, 600, 768, 1024 };
    static const int widths[] = { 640, 800, 1024, 1280 };
    const char *line;
    char word[PATH_LEN];
    int key, cmd, index, mode, want_w = -1, want_h = 0, ok, extract = 0, failed = 0;
    unsigned int start_page = 0;

    sys_init();
    sys_break_install();
    /* the switches first: /? is answered before the screen is touched */
    line = sys_cmdline();
    while ( next_word( &line, word, sizeof( word ) ) ) {
        if ( word[0] != '/' ) {
            continue;
        }
        if ( word[1] == '?' ) {
            sys_stdout( usage, sizeof( usage ) - 1 );
            sys_exit( 0 );
        } else if ( ch_upper( word[1] ) == 'X' && word[2] == 0 ) {
            extract = 1;
        } else if ( ch_upper( word[1] ) == 'P' && word[2] == ':' ) {
            start_page = dec_parse( word + 3, &ok );
            if ( !ok ) {
                start_page = 0;
            }
        } else if ( ch_upper( word[1] ) == 'T' && word[2] == 0 ) {
            want_w = 0;
        } else if ( ch_upper( word[1] ) == 'G' && (word[2] == 0 || word[2] == ':') ) {
            want_w = word[2] == ':' ? (int)dec_parse( word + 3, &ok ) : 640;
            want_h = 480;
            for ( index = 0; index < 4; index++ ) {
                if ( widths[index] == want_w ) {
                    want_h = heights[index];
                }
            }
            if ( want_h == 480 ) {
                want_w = 640;
            }
        } else {
            sys_stdout( "Invalid switch - ", 17 );
            sys_stdout( word, str_len( word ) );
            sys_stdout( "\r\n", 2 );
            sys_exit( 1 );
        }
    }
    /* ACROREAD drew a PDF's accents, dashes and bullets each as the one
       next to it in its list; PMWRITE draws the right ones */
    sfont_as_acroread = 0;
    if ( extract ) {
        /* the words and nothing else: no screen, no settings */
        if ( country.codepage && cp_known( country.codepage ) ) {
            txt_codepage = country.codepage;
        }
        line = sys_cmdline();
        while ( next_word( &line, word, sizeof( word ) ) ) {
            if ( word[0] != '/' && extract_text( word ) ) {
                failed = 1;
            }
        }
        sys_exit( failed );
    }
    tf_init();
    disp_init();
    screen_on = 1;
    /* on a screen of sixteen colours type is all ink or none: at this
       level a thin stroke that falls across two pixels keeps both */
    gfx_cover_cut( 96 );
    settings_load();
    tf_font_dirs( font_dirs, 1 );
    if ( country.codepage && cp_known( country.codepage ) ) {
        txt_codepage = country.codepage;
    }
    if ( want_w >= 0 ) {
        set.mode_w = want_w;
        set.mode_h = want_h;
    }
    ui_look( LOOK_AUTO );
    view_init();
    bars_init();
    mode = set.mode_w || set.mode_h ? disp_mode_find( set.mode_w, set.mode_h ) : -1;
    if ( mode >= 0 && mode != disp_mode_now() ) {
        screen_apply( mode );
    }
    app_layout();
    cmds_init();
    line = sys_cmdline();
    while ( next_word( &line, word, sizeof( word ) ) ) {
        if ( word[0] != '/' && file_open( word, 0 ) && start_page > 0 && PDFV != NULL && PDFV->pages > 0 ) {
            PDFV->page = (int)(start_page > (unsigned int)PDFV->pages ? (unsigned int)PDFV->pages : start_page) - 1;
        }
    }
    if ( ndocs == 0 ) {
        file_new();
    }
    for ( ;; ) {
        app_draw();
        scr_flush();
        if ( view_after_draw() ) {
            continue;
        }
        key = key_get();
        app_message( NULL );
        if ( key_service( key ) ) {
            continue;
        }
        if ( menu_wants( key ) ) {
            cmd_do( menu_run( key ) );
            continue;
        }
        if ( key == K_MOUSE ) {
            if ( mouse.kind == ME_WHEEL ) {
                view_wheel( mouse.wheel );
            } else if ( ndocs > 0 && mouse.row >= plan.doc_top && mouse.row < plan.doc_top + plan.doc_rows ) {
                view_mouse();
            } else {
                cmd = bars_mouse();
                if ( cmd > 0 ) {
                    cmd_do( cmd );
                }
            }
            continue;
        }
        cmd = cmd_of_key( key );
        if ( cmd ) {
            cmd_do( cmd );
        } else {
            cmd_key( key );
        }
    }
}
