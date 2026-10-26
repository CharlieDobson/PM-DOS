/*
 * TREEVIEW.H - TREEVIEW.EXE: DiskStat, the Dobson Utilities' drive
 * space and folder-size viewer, as a native PM-DOS program.
 *
 *   SCR.C       the screen as cells, the keyboard and the mouse
 *   GFX.C       the same cells in VGA graphics (TREEVIEW /G), and the
 *               one picture there is, the pie
 *   UI.C        boxes, buttons, message boxes, the title and status
 *               lines, and the critical-error question
 *   MENU.C      the menu bar
 *   SCAN.C      the disk: free space, folder sizes, the tree's rows
 *   USAGEBAR.C  the left pane: the bar, or the pie on /G, and the legend
 *   FOLDTREE.C  the right pane: the folders with their sizes
 *   MAIN.C      the start, the menus and the loop
 *
 * It is the DOS DiskStat (DUSOURCE\DISKSTAT: DOSMAIN, SCAN, USAGEBAR
 * and FOLDTREE .CPP on the DOSForms toolkit) in C, screen for screen.
 * The toolkit's part comes from XARC.EXE, which made the same move
 * first: SCR, GFX, MENU and UI are XARC's with what TreeView does not
 * use taken out, and LIBC.C, LIBIO.C, XSYS.H and INC\ are XARC's C
 * library, copied unchanged.  As in XARC, the drawing calls (df_*)
 * count rows and columns from 1 and a widget's rectangle counts them
 * from 0, the way the toolkit had them.
 */
#ifndef TREEVIEW_H
#define TREEVIEW_H

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "xsys.h"

#define APP_NAME        "TreeView"

/* ------------------------------------------------------------------ */
/* colours: the toolkit's names for the sixteen and the eight          */
/* ------------------------------------------------------------------ */
enum {
    FG_BLACK, FG_BLUE, FG_GREEN, FG_CYAN, FG_RED, FG_MAGENTA, FG_BROWN, FG_WHITE,
    FG_DARK_GRAY, FG_LIGHT_BLUE, FG_LIGHT_GREEN, FG_LIGHT_CYAN, FG_LIGHT_RED,
    FG_LIGHT_MAGENTA, FG_YELLOW, FG_BRIGHT_WHITE
};
enum { BG_BLACK, BG_BLUE, BG_GREEN, BG_CYAN, BG_RED, BG_MAGENTA, BG_BROWN, BG_WHITE };

enum { BORDER_NONE, BORDER_SINGLE, BORDER_DOUBLE };
enum { JUST_LEFT, JUST_CENTER, JUST_RIGHT };

/* ------------------------------------------------------------------ */
/* keys and events                                                     */
/* ------------------------------------------------------------------ */
#define KEY_EXT( scan ) (0x100 | (scan))
#define KEY_ENTER       13
#define KEY_ESC         27
#define KEY_F5          KEY_EXT( 0x3F )
#define KEY_HOME        KEY_EXT( 0x47 )
#define KEY_UP          KEY_EXT( 0x48 )
#define KEY_PGUP        KEY_EXT( 0x49 )
#define KEY_LEFT        KEY_EXT( 0x4B )
#define KEY_RIGHT       KEY_EXT( 0x4D )
#define KEY_END         KEY_EXT( 0x4F )
#define KEY_DOWN        KEY_EXT( 0x50 )
#define KEY_PGDN        KEY_EXT( 0x51 )
#define KEY_ALT         0x200   /* Alt pressed and let go on its own */

#define LEFT_BUTTON     0x01
#define RIGHT_BUTTON    0x02

enum { EV_NONE, EV_KEY, EV_MOUSE };

typedef struct {
    int kind;
    int key;                    /* EV_KEY: ASCII, or 100h + scan code */
    int x, y;                   /* EV_MOUSE: the cell, from 0 */
    int buttons;                /* EV_MOUSE: the buttons down after it */
    int press;                  /* EV_MOUSE: the buttons that went down in it */
} EVENT;

/* ------------------------------------------------------------------ */
/* SCR.C                                                               */
/* ------------------------------------------------------------------ */
/* A cell: the character in bits 0-7 and its colours in 8-15, as video
   memory has them, and in bits 16-23 what a graphics screen draws
   instead of the character - an icon, or a part of the picture.  A
   text screen never sees bits 16 and up. */
typedef u32 CELL;

#define CELL_CHAR( c )      ((u8)((c) & 0xFF))
#define CELL_ATTR( c )      ((u8)(((c) >> 8) & 0xFF))
#define CELL_EXTRA( c )     ((u8)(((c) >> 16) & 0xFF))
#define CELL_MAKE( ch, at ) ((CELL)(u8)(ch) | ((CELL)(u8)(at) << 8))
#define CELL_WITH( c, ex )  (((c) & 0xFFFFUL) | ((CELL)(u8)(ex) << 16))

#define EXTRA_ICON( id )        ((u8)((id) + 1))
#define EXTRA_IS_ICON( e )      ((e) >= 1 && (e) <= ICON_MAX)
#define EXTRA_PICTURE           0x80    /* the character is the drawing's number */

#define ICON_MAX        24
#define ICON_NONE       (-1)

int   scr_open( int graphics );         /* 1 = the graphics screen came up */
void  scr_close( void );
int   df_rows( void );
int   df_cols( void );
int   df_is_graphics( void );
int   df_has_icons( void );
int   df_has_pictures( void );

/* drawing: rows and columns count from 1, as the toolkit's did */
void  df_text( int row, int col, const char *text, int fg, int bg );
void  df_fill( int top, int left, int bottom, int right, int bg );
void  df_clear( int fg, int bg );
void  df_clear_row( int row, int fg, int bg );
void  df_shade( int top, int left, int bottom, int right );
void  df_icon( int row, int col, int icon, int bg );
void  df_cells_read( int top, int left, int bottom, int right, CELL *buf );
void  df_cells_write( int top, int left, int bottom, int right, const CELL *buf );
CELL  df_cell_at( int row, int col );
void  df_flush( void );

int   df_icon_register( const char *const *art, int rows );
const char *const *df_icon_art( int icon, int *rows );

/* The picture: cells top..bottom, left..right that the graphics screen
   fills with pixels instead of characters.  Opening it paints it all
   in "bg" and hands those cells to it; a span is pixels x0..x1 of
   pixel row y, counted from the picture's top left corner.  0 from the
   open on a text screen, or with no memory for it. */
int   df_picture_open( int top, int left, int bottom, int right, int bg );
void  df_picture_span( int y, int x0, int x1, int color );
int   df_cell_w( void );
int   df_cell_h( void );

/* input.  ev_wait sends the screen first and waits; ev_poll looks and
   comes back - in test mode with nothing, unless TVPOLL is set. */
int   ev_wait( EVENT *ev );
int   ev_poll( EVENT *ev );
int   ev_mouse_on( void );
int   ev_buttons( void );               /* the buttons down now */
void  ev_mouse_cell( int *x, int *y );  /* where the pointer is, as a cell */
u32   ev_clock( void );                 /* hundredths, for the scroll arrows */
int   ev_testing( void );
void  ev_init( void );
void  ev_break_off( void );             /* ^C is a key, not the end */

/* test mode, from the key file: the screen as text into the log */
void  scr_dump( const char *why );

/* ------------------------------------------------------------------ */
/* GFX.C                                                               */
/* ------------------------------------------------------------------ */
#define GFX_ROWS        30
#define GFX_CELL_H      16

int   gfx_start( void );                /* 1 = mode 12h is on */
void  gfx_stop( void );
const char *gfx_error( void );
void  gfx_cell( int row, int col, CELL cell );
void  gfx_flush( void );
void  gfx_pointer( int x, int y, int show );
int   gfx_snapshot( const char *path );
int   gfx_picture_open( int row, int col, int rows, int cols, int bg );   /* from 0 */
void  gfx_picture_span( int y, int x0, int x1, int color );

/* ------------------------------------------------------------------ */
/* UI.C                                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    int row, col, end_row, end_col;     /* from 0, the frame included */
} RECT;

/* a box that puts back what it covered */
typedef struct {
    RECT  at;
    int   fg, bg;
    int   border;                       /* BORDER_* */
    char  title[35];
    int   just;                         /* where the title goes: JUST_* */
    int   shadow;
    CELL *save;
    u32   save_cells;
    int   saved;
    RECT  saved_at;
} POPUP;

void  popup_init( POPUP *pop, int fg, int bg, const char *title );
void  popup_place( POPUP *pop, int row, int col, int end_row, int end_col );
void  popup_draw( POPUP *pop );
void  popup_restore( POPUP *pop );
void  popup_free( POPUP *pop );

void  draw_box( int top, int left, int bottom, int right, int style, int fg, int bg );

/* "[ Label ]" on one row */
typedef struct {
    RECT at;
    char label[24];
    int  fg, bg;
} BUTTON;

void  button_init( BUTTON *btn, int fg, int bg, const char *label );
void  button_row( BUTTON *btn, int row, int col );     /* from 0 */
void  button_draw( const BUTTON *btn );
int   button_hit( const BUTTON *btn, int x, int y );

/* message boxes: sized to the text, or ("at", from 0) where they are put */
int   msg_ok( int fg, int bg, const char *title, const char *text, const RECT *at );
/* buttons named by the caller; the index of the one used, Esc the last */
int   msg_choose( int fg, int bg, const char *title, const char *text,
                  const char *const *labels, int count );

/* the title and status lines */
void  title_set( const char *text );
void  title_draw( void );
void  status_set( const char *text );
void  status_draw( void );

/* a drive that was not there: "No disk in drive A:", Retry or Cancel */
void  crit_install( void );
void  crit_clear( void );
int   crit_hit( void );
int   crit_ask( void );                 /* 1 = try again */
void  crit_text( char *buf, int size );

/* ------------------------------------------------------------------ */
/* MENU.C                                                              */
/* ------------------------------------------------------------------ */
typedef void (*MENUACT)( int arg );

int   menu_add( const char *label, char hotkey );
int   menu_item( int menu, const char *label, char hotkey, MENUACT action, int arg );
void  menu_row( int row );
void  menu_draw( void );
int   menu_event( const EVENT *ev );   /* 1 = the menu took it */

/* ------------------------------------------------------------------ */
/* SCAN.C - the disk model, as DISKMODL.HPP had it                     */
/* ------------------------------------------------------------------ */
#define NODE_HAS_CHILDREN   0x01
#define NODE_EXPANDED       0x02

typedef struct {
    char *name;                 /* the folder's name, long if it has one */
    u64   total_size;           /* bytes in the files of its whole subtree */
    u8    depth;                /* nesting level (0 = a child of the root) */
    u8    flags;                /* NODE_HAS_CHILDREN | NODE_EXPANDED */
} NODE;

extern int   current_drive;     /* 1 = A:, 2 = B:, 3 = C:, ... */
extern u64   total_disk_space;
extern u64   free_disk_space;
extern u64   used_disk_space;
extern NODE *tree_nodes;
extern int   tree_count;
extern int   selected_node;     /* -1 = none, else the cursor's row */
extern int   data_valid;

int   detect_drives( int *drives, int max );    /* C: and up */
int   drive_ready( int drive );                 /* 0 = Cancel */
int   scan_drive_ask( void );                   /* 0 = figures short */
void  expand_node_ask( int idx );
void  collapse_node( int idx );
void  clear_scan( void );

/* "1,234.5 MB": a size as the panes show it, digits grouped by the
   country; and "12.3%" of the disk */
void  format_size( u64 bytes, char *buf );
void  format_pct( u64 part, u64 whole, char *buf );
u32   per_whole( u64 part, u64 whole, u32 scale );  /* part * scale / whole */
void  shorten_name( char *dst, int room, const char *name );

/* ------------------------------------------------------------------ */
/* USAGEBAR.C, FOLDTREE.C                                              */
/* ------------------------------------------------------------------ */
void  usage_region( int row, int col, int end_row, int end_col );
void  usage_draw( void );

void  tree_region( int row, int col, int end_row, int end_col );
void  tree_after_scan( void );
void  tree_draw( void );
int   tree_event( const EVENT *ev );   /* 1 = the tree's */

#endif
