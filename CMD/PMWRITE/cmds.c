/*
 * CMDS.C - the menus, the keys, and what each command does.
 *
 * The menus are Word's, as far as PMWRITE has the commands: File,
 * Edit, View, Insert, Format, Tools, Window, Help, with Word's own
 * shortcut keys beside them.  A command is a number (PW.H); a menu
 * line, a toolbar button and a key all end in cmd_do with one.
 *
 * WHAT IS GREYED: app_can says, in one place, which commands make
 * sense now - nothing that changes text while a PDF's pages are
 * showing, no Paste with an empty clipboard, no Undo with nothing to
 * undo - and a key for a command that is greyed does nothing.
 */
#include "pw.h"

/* ------------------------------------------------------------------ */
/* the menus                                                           */
/* ------------------------------------------------------------------ */

static const MENUITEM file_items[] = {
    { "&New",               "Ctrl+N",   CMD_NEW,        "Starts a new document" },
    { "&Open...",           "Ctrl+O",   CMD_OPEN,       "Opens a document of any kind PMWRITE reads" },
    { "&Close",             "Ctrl+W",   CMD_CLOSE,      "Closes this document" },
    { NULL, NULL, 0, NULL },
    { "&Save",              "Ctrl+S",   CMD_SAVE,       "Saves this document" },
    { "Save &As...",        "F12",      CMD_SAVEAS,     "Saves this document under a name, in a format you choose" },
    { "&Edit PDF as Text",  "",         CMD_TOTEXT,     "Makes a document you can edit from this PDF's words" },
    { NULL, NULL, 0, NULL },
    { "Summary &Info...",   "",         CMD_INFO,       "The document's title, author and subject" },
    { "Page Set&up...",     "",         CMD_PAGESETUP,  "The paper's size and the margins" },
    { "&Print...",          "Ctrl+P",   CMD_PRINT,      "Prints this document" },
    { NULL, NULL, 0, NULL },
    { "E&xit",              "Alt+F4",   CMD_EXIT,       "Leaves PMWRITE" }
};

static const MENUITEM edit_items[] = {
    { "&Undo",              "Ctrl+Z",   CMD_UNDO,       "Takes back the last change" },
    { "&Redo",              "Ctrl+Y",   CMD_REDO,       "Makes the change again that Undo took back" },
    { NULL, NULL, 0, NULL },
    { "Cu&t",               "Ctrl+X",   CMD_CUT,        "Moves what is selected to the clipboard" },
    { "&Copy",              "Ctrl+C",   CMD_COPY,       "Copies what is selected to the clipboard" },
    { "&Paste",             "Ctrl+V",   CMD_PASTE,      "Puts the clipboard's contents in at the caret" },
    { "Cle&ar",             "Del",      CMD_CLEAR,      "Deletes what is selected" },
    { "Select A&ll",        "Ctrl+A",   CMD_SELALL,     "Selects the whole document" },
    { NULL, NULL, 0, NULL },
    { "&Find...",           "Ctrl+F",   CMD_FIND,       "Looks for text" },
    { "Find &Next",         "Shift+F4", CMD_FINDNEXT,   "Looks for the same text again" },
    { "R&eplace...",        "Ctrl+H",   CMD_REPLACE,    "Looks for text and puts other text in its place" },
    { "&Go To...",          "Ctrl+G",   CMD_GOTO,       "Goes to a page" }
};

static const MENUITEM view_items[] = {
    { "&Toolbar",           "",         CMD_TOOLBAR,    "Shows or hides the toolbar and the formatting line" },
    { "&Ruler",             "",         CMD_RULER,      "Shows or hides the ruler" },
    { "&Status Bar",        "",         CMD_STATUSBAR,  "Shows or hides the status bar" },
    { "&Hidden Text",       "",         CMD_MARKS,      "Shows or hides hidden text and paragraph ends" },
    { NULL, NULL, 0, NULL },
    { "H&eader",            "",         CMD_HEADER,     "Edits the text at the top of every page" },
    { "&Footer",            "",         CMD_FOOTER,     "Edits the text at the bottom of every page" },
    { "Foot&note",          "",         CMD_VIEWNOTE,   "Edits the note that is called from beside the caret" },
    { "&Document",          "",         CMD_BODY,       "Goes back to the document's own text" },
    { NULL, NULL, 0, NULL },
    { "&Zoom...",           "",         CMD_ZOOM,       "Makes the document larger or smaller on the screen" },
    { "Dis&play...",        "",         CMD_DISPLAY,    "Changes the screen: text, or graphics at a resolution" }
};

static const MENUITEM insert_items[] = {
    { "&Break...",          "",         CMD_BREAK,      "Starts a new page or a new line" },
    { "Page N&umber",       "",         CMD_PAGENUM,    "Puts in the page's number" },
    { "Number of &Pages",   "",         CMD_NUMPAGES,   "Puts in how many pages there are" },
    { "&Date",              "",         CMD_DATE,       "Puts in the date" },
    { "&Time",              "",         CMD_TIME,       "Puts in the time" },
    { "Foot&note",          "",         CMD_FOOTNOTE,   "Puts in a footnote's number, and goes to the note to type it" },
    { "&Endnote",           "",         CMD_ENDNOTE,    "Puts in an endnote's number, and goes to the note to type it" },
    { NULL, NULL, 0, NULL },
    { "Fi&le...",           "",         CMD_INSFILE,    "Puts another document's text in at the caret" },
    { "P&icture...",        "",         CMD_PICTURE,    "Puts a picture file in at the caret" }
};

static const MENUITEM format_items[] = {
    { "&Font...",           "",         CMD_FONT,       "The font, size, style and color of the text" },
    { "&Paragraph...",      "",         CMD_PARAGRAPH,  "Alignment, indents and spacing" },
    { "&Tabs...",           "",         CMD_TABS,       "The paragraph's tab stops" },
    { "&Style...",          "",         CMD_STYLE,      "Gives the paragraph a style" },
    { NULL, NULL, 0, NULL },
    { "&Bullets",           "",         CMD_BULLETS,    "Makes the paragraphs a bulleted list, or stops" },
    { "&Numbering",         "",         CMD_NUMBERING,  "Makes the paragraphs a numbered list, or stops" }
};

static const MENUITEM tools_items[] = {
    { "&Word Count...",     "",         CMD_COUNT,      "Counts the pages, words and characters" }
};

static const MENUITEM table_items[] = {
    { "&Insert Table...",   "",         CMD_TABLE,      "Puts an empty table in at the caret" },
    { NULL, NULL, 0, NULL },
    { "Insert Row &Above",  "",         CMD_ROWABOVE,   "Puts an empty row in above the one the caret is in" },
    { "Insert Row &Below",  "",         CMD_TABLEROW,   "Puts an empty row in below the one the caret is in" },
    { "Insert Column &Left", "",        CMD_COLLEFT,    "Puts an empty column in to the left of the caret" },
    { "Insert Column &Right", "",       CMD_COLRIGHT,   "Puts an empty column in to the right of the caret" },
    { NULL, NULL, 0, NULL },
    { "Delete R&ow",        "",         CMD_ROWDEL,     "Takes out the rows the caret or the selection is in" },
    { "Delete &Column",     "",         CMD_COLDEL,     "Takes out the column the caret is in" },
    { NULL, NULL, 0, NULL },
    { "Column &Width...",   "",         CMD_COLWIDTH,   "Makes the column the caret is in wider or narrower" }
};

static const MENUITEM help_items[] = {
    { "&Keys",              "F1",       CMD_KEYS,       "The keys PMWRITE knows" },
    { NULL, NULL, 0, NULL },
    { "&About PMWRITE",     "",         CMD_ABOUT,      "The version of PMWRITE" }
};

static MENUITEM window_items[MAX_DOCS + 2];
static char     window_text[MAX_DOCS][48];

static MENU menus[] = {
    { "&File",   file_items,   sizeof( file_items ) / sizeof( MENUITEM ) },
    { "&Edit",   edit_items,   sizeof( edit_items ) / sizeof( MENUITEM ) },
    { "&View",   view_items,   sizeof( view_items ) / sizeof( MENUITEM ) },
    { "&Insert", insert_items, sizeof( insert_items ) / sizeof( MENUITEM ) },
    { "F&ormat", format_items, sizeof( format_items ) / sizeof( MENUITEM ) },
    { "&Tools",  tools_items,  sizeof( tools_items ) / sizeof( MENUITEM ) },
    { "T&able",  table_items,  sizeof( table_items ) / sizeof( MENUITEM ) },
    { "&Window", window_items, 0 },
    { "&Help",   help_items,   sizeof( help_items ) / sizeof( MENUITEM ) }
};

#define MENU_WINDOW     7

/* the Window menu is the documents that are open: made again whenever
   one opens, closes or is renamed */
void cmds_init( void )
{
    int index, count = 0;

    window_items[count].text = "&Next Window";
    window_items[count].keys = "Ctrl+F6";
    window_items[count].cmd = CMD_NEXTWIN;
    window_items[count].hint = "Shows the next open document";
    count++;
    if ( ndocs ) {
        window_items[count].text = NULL;
        window_items[count].keys = NULL;
        window_items[count].cmd = 0;
        window_items[count].hint = NULL;
        count++;
    }
    for ( index = 0; index < ndocs; index++ ) {
        sfmt( window_text[index], sizeof( window_text[index] ), "&%u %s", (u32)index + 1, docs[index].name );
        window_items[count].text = window_text[index];
        window_items[count].keys = "";
        window_items[count].cmd = CMD_WINDOW + index;
        window_items[count].hint = "Shows this document";
        count++;
    }
    menus[MENU_WINDOW].count = count;
    menu_set( menus, sizeof( menus ) / sizeof( menus[0] ), plan.menu_row );
}

int app_can( int cmd )
{
    EDITOR *ed = cur_doc >= 0 && cur_doc < ndocs ? ED : NULL;
    int pdf = cur_doc >= 0 && cur_doc < ndocs && PDFV != NULL;

    switch ( cmd ) {
    case CMD_NEW:
    case CMD_OPEN:
    case CMD_EXIT:
    case CMD_TOOLBAR:
    case CMD_RULER:
    case CMD_STATUSBAR:
    case CMD_DISPLAY:
    case CMD_KEYS:
    case CMD_ABOUT:
        return 1;
    case CMD_CLOSE:
        return ndocs > 0;
    case CMD_NEXTWIN:
        return ndocs > 1;
    case CMD_TOTEXT:
    case CMD_PAGEPREV:
    case CMD_PAGENEXT:
        return pdf;
    case CMD_GOTO:
        return ed != NULL || pdf;
    case CMD_ZOOM:
        return gfx_on && (ed != NULL || pdf);
    }
    if ( cmd >= CMD_WINDOW && cmd < CMD_WINDOW + MAX_DOCS ) {
        return cmd - CMD_WINDOW < ndocs;
    }
    if ( ed == NULL ) {
        return 0;                       /* everything else wants a document to edit */
    }
    switch ( cmd ) {
    case CMD_UNDO:
        return we_can_undo( ed->doc );
    case CMD_REDO:
        return we_can_redo( ed->doc );
    case CMD_CUT:
    case CMD_COPY:
    case CMD_CLEAR:
        return ed_has_sel( ed );
    case CMD_PASTE:
        return ed_can_paste();
    case CMD_TABLE:
        return !ed_in_table( ed );      /* not a table inside a table */
    case CMD_TABLEROW:
    case CMD_ROWABOVE:
    case CMD_ROWDEL:
    case CMD_COLLEFT:
    case CMD_COLRIGHT:
    case CMD_COLDEL:
    case CMD_COLWIDTH:
        return ed_in_table( ed );
    case CMD_FOOTNOTE:
    case CMD_ENDNOTE:
        return ed->story == &ed->doc->body;
    case CMD_VIEWNOTE:
        return ed_note_near( ed ) != NULL;
    case CMD_BODY:
        return ed->story != &ed->doc->body;
    case CMD_HEADER:
        return ed->story != &ed->doc->header;
    case CMD_FOOTER:
        return ed->story != &ed->doc->footer;
    }
    return 1;
}

int app_checked( int cmd )
{
    switch ( cmd ) {
    case CMD_TOOLBAR:
        return set.show_toolbar;
    case CMD_RULER:
        return set.show_ruler;
    case CMD_STATUSBAR:
        return set.show_status;
    case CMD_MARKS:
        return set.show_marks;
    case CMD_BULLETS:
        return ED != NULL && ed_cur_pap( ED )->list == LIST_BULLET;
    case CMD_NUMBERING:
        return ED != NULL && ed_cur_pap( ED )->list >= LIST_NUMBER;
    }
    if ( cmd >= CMD_WINDOW && cmd < CMD_WINDOW + MAX_DOCS ) {
        return cmd - CMD_WINDOW == cur_doc;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* keys                                                                */
/* ------------------------------------------------------------------ */

int cmd_of_key( int key )
{
    static const struct {
        int key, cmd;
    } keys[] = {
        { K_C( 'N' ), CMD_NEW },            { K_C( 'O' ), CMD_OPEN },
        { K_C( 'W' ), CMD_CLOSE },          { K_F4 | K_CTRL, CMD_CLOSE },
        { K_C( 'S' ), CMD_SAVE },           { K_F12, CMD_SAVEAS },
        { K_C( 'P' ), CMD_PRINT },          { K_F4 | K_ALT, CMD_EXIT },
        { K_C( 'Z' ), CMD_UNDO },           { K_BS | K_ALT, CMD_UNDO },
        { K_C( 'Y' ), CMD_REDO },           { K_C( 'X' ), CMD_CUT },
        { K_DEL | K_SHIFT, CMD_CUT },       { K_C( 'C' ), CMD_COPY },
        { K_INS | K_CTRL, CMD_COPY },       { K_C( 'V' ), CMD_PASTE },
        { K_INS | K_SHIFT, CMD_PASTE },     { K_C( 'A' ), CMD_SELALL },
        { K_C( 'F' ), CMD_FIND },           { K_F4 | K_SHIFT, CMD_FINDNEXT },
        { K_F3, CMD_FINDNEXT },             { K_C( 'H' ), CMD_REPLACE },
        { K_C( 'G' ), CMD_GOTO },           { K_F5, CMD_GOTO },
        { K_C( 'B' ), CMD_BOLD },           { K_C( 'I' ), CMD_ITALIC },
        { K_C( 'U' ), CMD_UNDERLINE },      { K_C( 'L' ), CMD_LEFT },
        { K_C( 'E' ), CMD_CENTER },         { K_C( 'R' ), CMD_RIGHT },
        { K_C( 'J' ), CMD_JUSTIFY },        { K_C( 'M' ), CMD_INDENT },
        { K_C( 'M' ) | K_SHIFT, CMD_OUTDENT }, { K_C( '1' ), CMD_SPACE1 },
        { K_C( '5' ), CMD_SPACE15 },        { K_C( '2' ), CMD_SPACE2 },
        { '=' | K_CTRL, CMD_SUB },          { '=' | K_CTRL | K_SHIFT, CMD_SUPER },
        { '+' | K_CTRL, CMD_SUPER },        { '+' | K_CTRL | K_SHIFT, CMD_SUPER },
        { K_C( 'S' ) | K_SHIFT, CMD_PICKSTYLE }, { K_C( 'F' ) | K_SHIFT, CMD_PICKFONT },
        { K_C( 'P' ) | K_SHIFT, CMD_PICKSIZE }, { K_F6 | K_CTRL, CMD_NEXTWIN },
        { K_F1, CMD_KEYS }
    };
    u32 index;

    for ( index = 0; index < sizeof( keys ) / sizeof( keys[0] ); index++ ) {
        if ( keys[index].key == key ) {
            return keys[index].cmd;
        }
    }
    return 0;
}

void cmd_key( int key )
{
    EDITOR *ed = ED;
    int plain = key & ~K_MODS, select = (key & K_SHIFT) != 0, ctrl = (key & K_CTRL) != 0;

    if ( cur_doc < 0 || cur_doc >= ndocs ) {
        return;
    }
    if ( ed == NULL ) {
        view_pdf_key( key );
        return;
    }
    if ( key & K_ALT ) {
        return;
    }
    switch ( plain ) {
    case K_LEFT:
        ed_move( ed, ctrl ? MV_WORD_LEFT : MV_LEFT, select );
        return;
    case K_RIGHT:
        ed_move( ed, ctrl ? MV_WORD_RIGHT : MV_RIGHT, select );
        return;
    case K_UP:
        ed_move( ed, ctrl ? MV_PARA_UP : MV_UP, select );
        return;
    case K_DOWN:
        ed_move( ed, ctrl ? MV_PARA_DOWN : MV_DOWN, select );
        return;
    case K_HOME:
        ed_move( ed, ctrl ? MV_TOP : MV_HOME, select );
        return;
    case K_END:
        ed_move( ed, ctrl ? MV_BOTTOM : MV_END, select );
        return;
    case K_PGUP:
        ed_move( ed, MV_PAGE_UP, select );
        return;
    case K_PGDN:
        ed_move( ed, MV_PAGE_DOWN, select );
        return;
    case K_INS:
        if ( !select && !ctrl ) {
            ed->overtype = !ed->overtype;
        }
        return;
    case K_DEL:
        if ( ctrl ) {
            ed_delete_word( ed, 0 );
        } else {
            ed_delete( ed );
        }
        return;
    case K_BS:
        if ( ctrl ) {
            ed_delete_word( ed, 1 );
        } else {
            ed_backspace( ed );
        }
        return;
    case K_ENTER:
        if ( ctrl ) {
            ed_break( ed, WCH_PAGE );
        } else if ( select ) {
            ed_break( ed, WCH_LINE );
        } else {
            ed_enter( ed );
        }
        return;
    case K_TAB:
        /* in a table it goes from cell to cell, as in Word; elsewhere it
           is a tab */
        if ( !ctrl && !ed_cell_step( ed, select ) && !select ) {
            ed_type( ed, WCH_TAB );
        }
        return;
    }
    if ( !ctrl && key >= 0x20 && key <= 0xFF ) {
        /* a character, in the keyboard's code page */
        ed_type( ed, cp_to_uni( country.codepage ? country.codepage : CP_437, key ) );
    }
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void set_align( int align )
{
    PAP pap;

    pap_init( &pap );
    pap.align = (u8)align;
    ed_set_pap( ED, PM_ALIGN, &pap, 0 );
}

static void set_spacing( int line )
{
    PAP pap;

    pap_init( &pap );
    pap.line = (u16)line;
    pap.line_rule = LS_MULTIPLE;
    ed_set_pap( ED, PM_LINE, &pap, 0 );
}

/* a list on, or off again: with it goes the indent its mark hangs in */
static void set_list( int list )
{
    PAP pap;

    pap_init( &pap );
    if ( ed_cur_pap( ED )->list != LIST_NONE && (ed_cur_pap( ED )->list == LIST_BULLET) == (list == LIST_BULLET) ) {
        ed_set_pap( ED, PM_LIST | PM_LEFT | PM_FIRST, &pap, 0 );
        return;
    }
    pap.list = (u8)list;
    pap.left = 360;
    pap.first = -360;
    ed_set_pap( ED, PM_LIST | PM_LEFT | PM_FIRST, &pap, 0 );
}

/* the header or the footer, made if the document had none */
static void edit_story( STORY *story )
{
    EDITOR *ed = ED;
    PAP pap;

    if ( story->count == 0 ) {
        pap_init( &pap );
        pap.align = AL_CENTER;
        if ( wd_para_add( ed->doc, story, &pap ) == NULL ) {
            ed->doc->failed = 0;
            no_memory();
            return;
        }
        ed->doc->serial++;
    }
    ed_set_story( ed, story );
    view_reset();
}

static void pick_style( void )
{
    static const char *const basics[] = { "Heading 1", "Heading 2", "Heading 3", "Title" };
    static const int kinds[] = { STY_HEADING1, STY_HEADING2, STY_HEADING3, STY_TITLE };
    const char *names[MAX_STYLES];
    EDITOR *ed = ED;
    u32 index;
    int chosen;

    /* the styles every document may use are there to be chosen */
    for ( index = 0; index < 4; index++ ) {
        wd_style( ed->doc, basics[index], kinds[index] );
    }
    for ( index = 0; index < ed->doc->nstyle; index++ ) {
        names[index] = ed->doc->style[index].name;
    }
    chosen = dlg_pick( "Style", names, (int)ed->doc->nstyle, ed_cur_pap( ed )->style );
    if ( chosen >= 0 ) {
        ed_set_style( ed, chosen );
    }
}

static void pick_font( void )
{
    static const char *const always[] = { "Times New Roman", "Arial", "Courier New", "Symbol" };
    const char *names[64];
    EDITOR *ed = ED;
    CHP chp;
    u32 index;
    int chosen, count;

    for ( index = 0; index < 4; index++ ) {
        wd_font( ed->doc, always[index], FAM_ANY, -1 );
    }
    /* and the families of the TrueType files that were found (TF.H),
       while the list has room */
    for ( index = 0; ed->doc->nfont < 64 && tf_family( index ) != NULL; index++ ) {
        wd_font( ed->doc, tf_family( index ), FAM_ANY, -1 );
    }
    count = ed->doc->nfont > 64 ? 64 : (int)ed->doc->nfont;
    for ( index = 0; index < (u32)count; index++ ) {
        names[index] = ed->doc->font[index].name;
    }
    ed_cur_chp( ed, &chp );
    chosen = dlg_pick( "Font", names, count, chp.font < (u32)count ? chp.font : 0 );
    if ( chosen >= 0 ) {
        chp.font = (u16)chosen;
        ed_set_chp( ed, CM_FONT, &chp, 0 );
    }
}

static void pick_size( void )
{
    static const char *const sizes[] = { "8", "9", "10", "11", "12", "14", "16", "18", "20", "22", "24", "26",
                                         "28", "36", "48", "72" };
    static const u8 points[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };
    EDITOR *ed = ED;
    CHP chp;
    int chosen, start = 4, index;

    ed_cur_chp( ed, &chp );
    for ( index = 0; index < 16; index++ ) {
        if ( points[index] * 2 == chp.size ) {
            start = index;
        }
    }
    chosen = dlg_pick( "Size", sizes, 16, start );
    if ( chosen >= 0 ) {
        chp.size = (u16)(points[chosen] * 2);
        ed_set_chp( ed, CM_SIZE, &chp, 0 );
    }
}

void cmd_do( int cmd )
{
    EDITOR *ed;
    STORY *note;

    if ( cmd == 0 || !app_can( cmd ) ) {
        return;
    }
    ed = cur_doc >= 0 && cur_doc < ndocs ? ED : NULL;
    if ( cmd >= CMD_WINDOW && cmd < CMD_WINDOW + MAX_DOCS ) {
        doc_show( cmd - CMD_WINDOW );
        return;
    }
    switch ( cmd ) {
    case CMD_NEW:       file_new();                         break;
    case CMD_OPEN:      file_open_ask();                    break;
    case CMD_CLOSE:
        if ( file_close_ask( cur_doc ) ) {
            doc_close( cur_doc );
        }
        break;
    case CMD_SAVE:      file_save( cur_doc, 0 );            break;
    case CMD_SAVEAS:    file_save( cur_doc, 1 );            break;
    case CMD_TOTEXT:    file_to_text();                     break;
    case CMD_INFO:      dlg_info();                         break;
    case CMD_PAGESETUP: dlg_page_setup();                   break;
    case CMD_PRINT:     file_print();                       break;
    case CMD_EXIT:      app_quit();                         break;
    case CMD_UNDO:      ed_undo( ed );  view_reset();       break;
    case CMD_REDO:      ed_redo( ed );  view_reset();       break;
    case CMD_CUT:       ed_cut( ed );                       break;
    case CMD_COPY:      ed_copy( ed );                      break;
    case CMD_PASTE:     ed_paste( ed );                     break;
    case CMD_CLEAR:     ed_delete( ed );                    break;
    case CMD_SELALL:    ed_select_all( ed );                break;
    case CMD_FIND:      dlg_find( 0 );                      break;
    case CMD_FINDNEXT:  find_again();                       break;
    case CMD_REPLACE:   dlg_find( 1 );                      break;
    case CMD_GOTO:      dlg_goto();                         break;
    case CMD_TOOLBAR:   set.show_toolbar = !set.show_toolbar;   break;
    case CMD_RULER:     set.show_ruler = !set.show_ruler;       break;
    case CMD_STATUSBAR: set.show_status = !set.show_status;     break;
    case CMD_MARKS:     set.show_marks = !set.show_marks;       break;
    case CMD_ZOOM:      dlg_zoom();                         break;
    case CMD_DISPLAY:   dlg_display();                      break;
    case CMD_HEADER:    edit_story( &ed->doc->header );     break;
    case CMD_FOOTER:    edit_story( &ed->doc->footer );     break;
    case CMD_BODY:      edit_story( &ed->doc->body );       break;
    case CMD_BREAK:     dlg_break();                        break;
    case CMD_PAGENUM:   ed_type( ed, WCH_PAGENUM );         break;
    case CMD_NUMPAGES:  ed_type( ed, WCH_NUMPAGES );        break;
    case CMD_DATE:      ed_type( ed, WCH_DATE );            break;
    case CMD_TIME:      ed_type( ed, WCH_TIME );            break;
    case CMD_INSFILE:   file_insert();                      break;
    case CMD_PICTURE:   file_picture();                     break;
    case CMD_FONT:      dlg_font();                         break;
    case CMD_PARAGRAPH: dlg_paragraph();                    break;
    case CMD_TABS:      dlg_tabs();                         break;
    case CMD_STYLE:
    case CMD_PICKSTYLE: pick_style();                       break;
    case CMD_PICKFONT:  pick_font();                        break;
    case CMD_PICKSIZE:  pick_size();                        break;
    case CMD_BULLETS:   set_list( LIST_BULLET );            break;
    case CMD_NUMBERING: set_list( LIST_NUMBER );            break;
    case CMD_BOLD:      ed_toggle( ed, CF_BOLD );           break;
    case CMD_ITALIC:    ed_toggle( ed, CF_ITALIC );         break;
    case CMD_UNDERLINE: ed_toggle( ed, CF_UNDER );          break;
    case CMD_SUPER:     ed_toggle( ed, CF_SUPER );          break;
    case CMD_SUB:       ed_toggle( ed, CF_SUB );            break;
    case CMD_LEFT:      set_align( AL_LEFT );               break;
    case CMD_CENTER:    set_align( AL_CENTER );             break;
    case CMD_RIGHT:     set_align( AL_RIGHT );              break;
    case CMD_JUSTIFY:   set_align( AL_JUSTIFY );            break;
    case CMD_INDENT:    ed_indent( ed, 1 );                 break;
    case CMD_OUTDENT:   ed_indent( ed, 0 );                 break;
    case CMD_SPACE1:    set_spacing( 240 );                 break;
    case CMD_SPACE15:   set_spacing( 360 );                 break;
    case CMD_SPACE2:    set_spacing( 480 );                 break;
    case CMD_COUNT:     dlg_count();                        break;
    case CMD_TABLE:     dlg_table();                        break;
    case CMD_TABLEROW:  ed_insert_row( ed );                break;
    case CMD_ROWABOVE:  ed_insert_row_above( ed );          break;
    case CMD_ROWDEL:    ed_delete_rows( ed );               break;
    case CMD_COLLEFT:
    case CMD_COLRIGHT:
        if ( !ed_insert_col( ed, cmd == CMD_COLRIGHT ) ) {
            msg_box( "A row of this table has no room for another column.", MB_OK );
        }
        break;
    case CMD_COLDEL:    ed_delete_col( ed );                break;
    case CMD_COLWIDTH:  dlg_col_width();                    break;
    case CMD_FOOTNOTE:
    case CMD_ENDNOTE:
        note = ed_insert_note( ed, cmd == CMD_FOOTNOTE ? NOTE_FOOT : NOTE_END );
        if ( note != NULL ) {
            edit_story( note );
        }
        break;
    case CMD_VIEWNOTE:
        note = ed_note_near( ed );
        if ( note != NULL ) {
            edit_story( note );
        }
        break;
    case CMD_NEXTWIN:   doc_show( (cur_doc + 1) % ndocs );  break;
    case CMD_PAGEPREV:  view_pdf_key( K_PGUP );             break;
    case CMD_PAGENEXT:  view_pdf_key( K_PGDN );             break;
    case CMD_KEYS:      dlg_keys();                         break;
    case CMD_ABOUT:     dlg_about();                        break;
    }
    bars_changed();
}
