/*
 * PWDEMO.C - the smallest program that proves PMWRITE's display and
 * user-interface toolkit by itself: SCR.C, GFX.C, UI.C and FBOX.C over
 * SYS_D32.C.  NOT SHIPPED; MKDEMO.CMD builds it, and it is run in an
 * emulator from key files (PWTEST) to see that each part does what
 * DISP.H and UI.H say.  The word processor has a toolbar, a ruler and
 * a page of its own - none of that is drawn here.
 *
 * WHAT IS ON THE SCREEN: a menu bar of three menus (one line of them
 * greyed, one with a check mark), a status line, and between them ONE
 * PANE with a pattern that uses every call there is for drawing in
 * pixels - rectangles, lines, the five frames, text, an icon as it is
 * and greyed, a block of all 256 colours, antialiasing ramps, a patch
 * turned into its opposite, the caret (the arrow keys move it) and the
 * I-beam pointer.  On a text screen the same rows hold characters.
 *
 * WHAT THE MENUS REACH: the file box, both ways; a dialog with one of
 * every kind of control; the display modes this machine offers, any of
 * which is set at once; a message box.  The status line shows the last
 * key as a number, which is how a key file proves that Ctrl+I and Tab
 * arrive as different keys.
 */
#include "rt.h"
#include "ui.h"

enum { CMD_OPEN = 1, CMD_SAVEAS, CMD_EXIT, CMD_STATUS, CMD_CONTROLS, CMD_DISPLAY, CMD_GREY,
       CMD_BUSY, CMD_ABOUT };

#define PANE_TEST       0

static const MENUITEM file_items[] = {
    { "&Open...",    "Ctrl+O", CMD_OPEN,   "Asks for a file with the file box" },
    { "Save &As...", "F12",    CMD_SAVEAS, "Asks for a name to save under" },
    { NULL,          NULL,     0,          NULL },
    { "E&xit",       "Alt+F4", CMD_EXIT,   "Leaves the demonstration" }
};

static const MENUITEM view_items[] = {
    { "&Status Line",  "",       CMD_STATUS,   "Shows the last key and the caret, or not" },
    { "&Controls...",  "Ctrl+D", CMD_CONTROLS, "A dialog with one of every kind of control" },
    { "&Display...",   "",       CMD_DISPLAY,  "The screens this machine offers" },
    { NULL,            NULL,     0,            NULL },
    { "&Never Ready",  "",       CMD_GREY,     "A line that cannot be chosen" },
    { "&Busy Box",     "",       CMD_BUSY,     "The box that says work is going on" }
};

static const MENUITEM help_items[] = {
    { "&About...", "", CMD_ABOUT, "What this program is" }
};

static const MENU menus[] = {
    { "&File", file_items, 4 },
    { "&View", view_items, 6 },
    { "&Help", help_items, 1 }
};

/* a diskette, for the icon calls: the toolkit's own key of letters */
static const char *const disk_icon[16] = {
    "................",
    ".kkkkkkkkkkkkk..",
    ".kbkWWWWWWWkbkk.",
    ".kbkWWWWWWWkbbk.",
    ".kbkWWWWWWWkbbk.",
    ".kbkWWWWWWWkbbk.",
    ".kbkkkkkkkkkbbk.",
    ".kbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbk.",
    ".kbbkkkkkkkkbbk.",
    ".kbbkwwwwkkkbbk.",
    ".kbbkwwwwkkkbbk.",
    ".kbbkwwwwkkkbbk.",
    ".kbbkwwwwkkkbbk.",
    ".kkkkkkkkkkkkkk.",
    "................"
};

static const char *status_hint;
static int  status_on = 1;
static int  last_key;
static int  car_col = 2, car_row = 1;   /* the caret, in cells of the pane */
static int  leaving;
static int  seen_x, seen_y, seen_kind = -1;      /* the mouse's last event */

/* ------------------------------------------------------------------ */
/* what RT.C asks of a program                                         */
/* ------------------------------------------------------------------ */

int mem_reclaim( void )
{
    return 0;
}

void out_of_memory( void )
{
    disp_done();
    sys_stdout( "PWDEMO: out of memory\r\n", 23 );
    sys_exit( 8 );
}

/* ------------------------------------------------------------------ */
/* the pane                                                            */
/* ------------------------------------------------------------------ */

/* Every pixel call, once.  The whole pattern is drawn whatever the
   rectangle asked for: the clip rectangle is set, and cuts it. */
static void test_paint( int pane, int x0, int y0, int x1, int y1 )
{
    static const char sample[] = "The quick brown fox jumps over the lazy dog 0123456789";
    static const char *const frames[5] = { "raised", "sunken", "button", "pressed", "field" };
    uint8_t *block;
    int px, py, pw, ph, index, col, row;

    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    if ( !pane_rect( pane, &px, &py, &pw, &ph ) ) {
        return;
    }
    gfx_rect( px, py, pw, ph, pal.px_desk );
    /* a sheet, with an outline and a shadow of two lines */
    gfx_rect( px + 8, py + 8, pw - 24, ph - 24, pal.px_page );
    gfx_outline( px + 8, py + 8, pw - 24, ph - 24, pal.px_frame );
    gfx_hline( px + 10, py + ph - 16, pw - 24, 0 );
    gfx_vline( px + pw - 16, py + 10, ph - 25, 0 );
    px += 16;
    py += 16;
    gfx_text( px, py, sample, (int)str_len( sample ), pal.px_ink, -1 );
    gfx_text( px, py + 16, sample, 19, pal.px_select_ink, pal.px_select );
    gfx_invert( px + 160, py + 16, 80, 16 );
    /* the five frames, each on a patch of the toolbar's grey */
    for ( index = 0; index < 5; index++ ) {
        gfx_rect( px + index * 72, py + 40, 64, 24, pal.px_face );
        gfx_frame( px + index * 72, py + 40, 64, 24, index );
        gfx_text( px + index * 72 + 4, py + 44, frames[index], (int)str_len( frames[index] ),
                  pal.px_ink, -1 );
    }
    /* an icon as it is, and as a button that cannot be pressed shows it */
    gfx_rect( px, py + 72, 24, 22, pal.px_face );
    gfx_frame( px, py + 72, 24, 22, FR_BUTTON );
    gfx_icon( px + 4, py + 75, disk_icon, pal.px_ink, 0 );
    gfx_rect( px + 32, py + 72, 24, 22, pal.px_face );
    gfx_frame( px + 32, py + 72, 24, 22, FR_BUTTON );
    gfx_icon( px + 36, py + 75, disk_icon, pal.px_ink, 1 );
    /* lines in the sixteen colours */
    for ( index = 0; index < 16; index++ ) {
        gfx_hline( px + 72, py + 72 + index, 100, index );
        gfx_vline( px + 180 + index * 2, py + 72, 22, index );
    }
    /* all 256 pixel numbers, and a ramp of coverage three ways */
    block = (uint8_t *)try_alloc( 256 * 16 );
    if ( block != NULL ) {
        for ( row = 0; row < 16; row++ ) {
            for ( col = 0; col < 256; col++ ) {
                block[row * 256 + col] = (uint8_t)col;
            }
        }
        gfx_blit( px, py + 104, 256, 16, block, 256 );
        gfx_cover( px, py + 128, 256, 16, block, 256, pal.px_ink, pal.px_page );
        gfx_cover( px, py + 148, 256, 16, block, 256, gfx_rgb( 0xC00000UL ), gfx_rgb( 0xFFFF80UL ) );
        gfx_rect( px, py + 168, 256, 16, pal.px_face );
        gfx_cover( px, py + 168, 256, 16, block, 256, pal.px_select, -1 );
        xfree( block );
    }
    /* colours asked for by what they are */
    for ( index = 0; index < 8; index++ ) {
        gfx_rect( px + 272 + index * 20, py + 104, 18, 16,
                  gfx_rgb( ((uint32_t)(index * 36) << 16) | ((uint32_t)(255 - index * 36) << 8) | 0x80UL ) );
    }
}

/* the caret's place on the screen, in pixels */
static void caret_place( int *x, int *y )
{
    int px, py, pw, ph;

    pane_rect( PANE_TEST, &px, &py, &pw, &ph );
    *x = px + 16 + car_col * CELL_W;
    *y = py + 16 + car_row * CELL_H;
}

/* ------------------------------------------------------------------ */
/* the application's side of UI.H                                      */
/* ------------------------------------------------------------------ */

void app_draw( void )
{
    const DISPMODE *mode = disp_mode_info( disp_mode_now() );
    char text[96];
    int row, x, y, bottom = scr_rows - (status_on ? 1 : 0);

    menu_draw_bar( -1, gfx_on );
    if ( pane_place( PANE_TEST, 1, 0, bottom - 1, scr_cols ) ) {
        caret_place( &x, &y );
        caret_at( x, y, CELL_H );
        scr_cursor( 0, 0, CUR_HIDE );
    } else {
        /* a text screen: the same rows, in characters */
        for ( row = 1; row < bottom; row++ ) {
            scr_fill( row, 0, scr_cols, ' ', pal.text );
        }
        scr_put( 2, 2, "The quick brown fox jumps over the lazy dog 0123456789", pal.text );
        scr_put( 3, 2, "The quick brown fox", pal.text_sel );
        scr_put( 5, 2, "A text screen has no panes: these rows are cells.", pal.text );
        scr_cursor( 2 + car_row, 2 + car_col, CUR_LINE );
    }
    if ( status_on ) {
        row = scr_rows - 1;
        scr_fill( row, 0, scr_cols, ' ', pal.status );
        if ( status_hint != NULL ) {
            scr_putw( row, 1, status_hint, scr_cols - 2, pal.status );
        } else {
            sfmt( text, sizeof( text ), "Key %04X   Caret %d,%d   Mouse %d,%d,%d   %s", last_key,
                  car_row, car_col, seen_x, seen_y, seen_kind, mode ? mode->name : "" );
            scr_putw( row, 1, text, scr_cols - 2, pal.status );
        }
        scr_flag( row, 0, scr_cols, CELLF_TOP | CELLF_RAISE );
    }
}

int app_can( int cmd )
{
    return cmd != CMD_GREY;
}

int app_checked( int cmd )
{
    return cmd == CMD_STATUS && status_on;
}

void app_status( const char *hint )
{
    status_hint = hint;
}

void app_abandon( void )
{
    disp_done();
    sys_exit( 0 );
}

/* ------------------------------------------------------------------ */
/* the dialogs                                                         */
/* ------------------------------------------------------------------ */

static const char *const fruit[] = { "Apple", "Banana", "Cherry", "Damson", "Elderberry", "Fig",
                                     "Grape", "Hazelnut", "Jujube", "Kiwi", "Lemon", "Mango" };
static const char *const sizes[] = { "8", "9", "10", "11", "12", "14", "16", "18", "20", "24",
                                     "28", "36", "48", "72" };

/* one of every kind of control, and what they were left at */
static void demo_controls( void )
{
    static char name[40] = "Letter";
    static int bold = 1, align = 1;
    CTL ctl[14];
    DLG dlg;
    char text[160];

    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_label( &ctl[0], 2, 3, "&Name:" );
    ctl_edit( &ctl[1], 2, 11, 24, name, sizeof( name ) );
    ctl_check( &ctl[2], 4, 3, "&Bold", &bold );
    ctl_frame( &ctl[3], 5, 2, 20, 5, "Alignment" );
    ctl_radio( &ctl[4], 6, 4, "&Left", &align, 1 );
    ctl_radio( &ctl[5], 7, 4, "&Centered", &align, 2 );
    ctl_radio( &ctl[6], 8, 4, "&Right", &align, 3 );
    ctl_list( &ctl[7], 4, 24, 18, 7, "&Fruit", fruit, 12 );
    ctl_label( &ctl[8], 11, 3, "&Size:" );
    ctl_drop( &ctl[9], 11, 11, 8, sizes, 14 );
    ctl[9].sel = 4;
    ctl_text( &ctl[10], 11, 24, 0, "Plain text." );
    ctl_button( &ctl[11], 13, 8, "  OK  ", ID_OK, 1 );
    ctl_button( &ctl[12], 13, 20, "Cancel", ID_CANCEL, 0 );
    ctl_button( &ctl[13], 13, 32, "&Greyed", ID_USER, 0 );
    ctl[13].disabled = 1;
    dlg.title = "Controls";
    dlg.rows = 15;
    dlg.cols = 46;
    dlg.sep = 12;
    dlg.ctl = ctl;
    dlg.nctl = 14;
    dlg.focus = 1;
    if ( dlg_run( &dlg ) == ID_OK ) {
        sfmt( text, sizeof( text ), "Name: %s\nBold: %d   Alignment: %d\nFruit: %s   Size: %s",
              name, bold, align, fruit[ctl[7].sel], sizes[ctl[9].sel] );
        msg_box( text, MB_OK );
    }
}

static const char *mode_item( DLG *dlg, int index )
{
    const DISPMODE *mode = disp_mode_info( index );

    (void)dlg;
    return mode ? mode->name : "";
}

/* the screens there are: OK sets the one chosen, now */
static void demo_display( void )
{
    CTL ctl[4];
    DLG dlg;

    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_list( &ctl[0], 2, 3, 32, 10, "&Display", NULL, disp_mode_count() );
    ctl[0].item = mode_item;
    ctl[0].sel = disp_mode_now();
    ctl_button( &ctl[1], 13, 6, "  OK  ", ID_OK, 1 );
    ctl_button( &ctl[2], 13, 20, "Cancel", ID_CANCEL, 0 );
    dlg.title = "Display";
    dlg.rows = 15;
    dlg.cols = 38;
    dlg.ctl = ctl;
    dlg.nctl = 3;
    if ( dlg_run( &dlg ) == ID_OK && ctl[0].sel != disp_mode_now() ) {
        ui_set_mode( ctl[0].sel );
    }
}

static void demo_file( int save )
{
    static const FBTYPE types[] = {
        { "Programs (*.EXE;*.COM)", "*.EXE;*.COM" },
        { "Text files (*.TXT)",     "*.TXT" },
        { "All files (*.*)",        "*.*" }
    };
    static int type = 0;
    char path[PATH_LEN], text[PATH_LEN + 40];

    path[0] = 0;
    if ( save ? fbox_save( NULL, path, types, 3, &type ) : fbox_open( NULL, path, types, 3, &type ) ) {
        sfmt( text, sizeof( text ), "%s\nKind of file: %d", path, type );
        msg_box( text, MB_OK );
    } else {
        msg_box( "No file was chosen.", MB_OK );
    }
}

static void command( int cmd )
{
    switch ( cmd ) {
    case CMD_OPEN:
        demo_file( 0 );
        break;
    case CMD_SAVEAS:
        demo_file( 1 );
        break;
    case CMD_EXIT:
        leaving = 1;
        break;
    case CMD_STATUS:
        status_on = !status_on;
        break;
    case CMD_CONTROLS:
        demo_controls();
        break;
    case CMD_DISPLAY:
        demo_display();
        break;
    case CMD_BUSY:
        mouse_shape( MP_WAIT );
        busy_show( "Working on nothing at all.", "Any key ends it." );
        key_service( key_get() );
        mouse_shape( MP_ARROW );
        break;
    case CMD_ABOUT:
        msg_text( "About", "PWDEMO\nA test of PMWRITE's screen, keys, mouse,\nmenus, dialogs and file box.",
                  MB_OK );
        break;
    }
}

/* the mouse outside the menus: a press in the pane puts the caret
   there, and the right button brings a menu to the pointer */
static void mouse_event( void )
{
    int px, py, pw, ph;

    seen_x = mouse.x;
    seen_y = mouse.y;
    seen_kind = mouse.kind;
    if ( mouse.kind != ME_DOWN && mouse.kind != ME_DOUBLE ) {
        return;
    }
    if ( mouse.button == MBTN_RIGHT ) {
        command( menu_popup( view_items, 6, mouse.row, mouse.col ) );
        return;
    }
    if ( gfx_on ) {
        if ( pane_at( mouse.x, mouse.y ) != PANE_TEST ) {
            return;
        }
        pane_rect( PANE_TEST, &px, &py, &pw, &ph );
        car_col = (mouse.x - px - 16) / CELL_W;
        car_row = (mouse.y - py - 16) / CELL_H;
    } else {
        car_col = mouse.col - 2;
        car_row = mouse.row - 2;
    }
    if ( car_col < 0 ) {
        car_col = 0;
    }
    if ( car_row < 0 ) {
        car_row = 0;
    }
}

void pw_main( void )
{
    int key;

    sys_init();
    sys_break_install();
    disp_init();
    ui_look( LOOK_AUTO );
    ui_name = "PWDEMO";
    menu_set( menus, 3, 0 );
    pane_define( PANE_TEST, test_paint );
    pane_pointer( PANE_TEST, MP_IBEAM );
    mouse_moves( 1 );
    while ( !leaving ) {
        app_draw();
        scr_flush();
        key = key_get();
        if ( key_service( key ) ) {
            continue;
        }
        if ( menu_wants( key ) ) {
            command( menu_run( key ) );
            continue;
        }
        if ( key == K_MOUSE ) {
            mouse_event();
            continue;
        }
        last_key = key;
        switch ( key ) {
        case K_LEFT:
            if ( car_col > 0 ) {
                car_col--;
            }
            break;
        case K_RIGHT:
            if ( car_col < 60 ) {
                car_col++;
            }
            break;
        case K_UP:
            if ( car_row > 0 ) {
                car_row--;
            }
            break;
        case K_DOWN:
            if ( car_row < 12 ) {
                car_row++;
            }
            break;
        case K_C( 'O' ):
            command( CMD_OPEN );
            break;
        case K_F12:
            command( CMD_SAVEAS );
            break;
        case K_C( 'D' ):
            command( CMD_CONTROLS );
            break;
        case K_F4 | K_ALT:
            leaving = 1;
            break;
        }
    }
    disp_done();
    sys_exit( 0 );
}
