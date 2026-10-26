/*
 * XARC.H - XARC.EXE's front end: XArchive's DOS text screen as a native
 * PM-DOS program.
 *
 *   SCR.C      the screen as cells, the keyboard and the mouse
 *   GFX.C      the same cells in VGA graphics (XARC /G)
 *   UI.C       boxes, buttons, message boxes, fields, check boxes, the
 *              progress and wait boxes, the text viewer, the title and
 *              status lines, and the critical-error question
 *   MENU.C     the menu bar
 *   LIST.C     the list, flat or as a tree
 *   DIRS.C     a directory as rows, and the drives and folders as a tree
 *   FILEBOX.C  the folder and file chooser
 *   ARCVIEW.C  folders over an archive's flat entry list
 *   ARCICON.C  the icons the graphics screen puts beside each row
 *   COMPDLG.C  File > Compress
 *   XARC.C     the program: the window, the menus, every action
 *   CLI.C      XARC L, T, X, A and MEM
 *   MAIN.C     the start: switches, preferences, which of the two
 *
 * It is the DOS front end (DU32\XARCHIVE\DOSBUILD, C++ on the DOSForms
 * toolkit) rewritten in C, screen for screen: the same layout, colours,
 * keys, mouse and words.  The toolkit's classes are structs and functions
 * here, and its coordinates are kept as it had them - the drawing calls
 * (df_*) count rows and columns from 1, the widgets' own rectangles from
 * 0 - so the two can be read side by side.
 */
#ifndef XARC_H
#define XARC_H

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <direct.h>
#include <sys/stat.h>
#include "xsys.h"
#include "arcfile.h"
#include "arccomp.h"
#include "numfmt.h"
#include "arcpref.h"
#include "arccryp.h"

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
#define KEY_BS          8
#define KEY_TAB         9
#define KEY_ENTER       13
#define KEY_ESC         27
#define KEY_SHTAB       KEY_EXT( 0x0F )
#define KEY_F1          KEY_EXT( 0x3B )
#define KEY_F2          KEY_EXT( 0x3C )
#define KEY_F3          KEY_EXT( 0x3D )
#define KEY_F4          KEY_EXT( 0x3E )
#define KEY_F5          KEY_EXT( 0x3F )
#define KEY_F6          KEY_EXT( 0x40 )
#define KEY_F7          KEY_EXT( 0x41 )
#define KEY_F8          KEY_EXT( 0x42 )
#define KEY_HOME        KEY_EXT( 0x47 )
#define KEY_UP          KEY_EXT( 0x48 )
#define KEY_PGUP        KEY_EXT( 0x49 )
#define KEY_LEFT        KEY_EXT( 0x4B )
#define KEY_RIGHT       KEY_EXT( 0x4D )
#define KEY_END         KEY_EXT( 0x4F )
#define KEY_DOWN        KEY_EXT( 0x50 )
#define KEY_PGDN        KEY_EXT( 0x51 )
#define KEY_INS         KEY_EXT( 0x52 )
#define KEY_DEL         KEY_EXT( 0x53 )
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
   instead of the character - an icon, or a slice of a progress bar.
   A text screen never sees bits 16 and up. */
typedef u32 CELL;

#define CELL_CHAR( c )      ((u8)((c) & 0xFF))
#define CELL_ATTR( c )      ((u8)(((c) >> 8) & 0xFF))
#define CELL_EXTRA( c )     ((u8)(((c) >> 16) & 0xFF))
#define CELL_MAKE( ch, at ) ((CELL)(u8)(ch) | ((CELL)(u8)(at) << 8))
#define CELL_WITH( c, ex )  (((c) & 0xFFFFUL) | ((CELL)(u8)(ex) << 16))

#define EXTRA_ICON( id )        ((u8)((id) + 1))
#define EXTRA_IS_ICON( e )      ((e) >= 1 && (e) <= ICON_MAX)
#define EXTRA_BAR               0x40    /* | how many of 8 columns are full */
#define EXTRA_BAR_LEFT          0x10
#define EXTRA_BAR_RIGHT         0x20
#define EXTRA_IS_BAR( e )       (((e) & 0xC0) == EXTRA_BAR)

#define ICON_MAX        24
#define ICON_NONE       (-1)

int   scr_open( int graphics );         /* 1 = the graphics screen came up */
void  scr_close( void );
int   scr_graphics( int on );           /* switch; 0 and gfx_error() if it would not */
int   df_rows( void );
int   df_cols( void );
int   df_is_graphics( void );
int   df_has_icons( void );

/* drawing: rows and columns count from 1, as the toolkit's did */
void  df_text( int row, int col, const char *text, int fg, int bg );
void  df_fill( int top, int left, int bottom, int right, int bg );
void  df_clear( int fg, int bg );
void  df_clear_row( int row, int fg, int bg );
void  df_shade( int top, int left, int bottom, int right );
void  df_bar( int row, int col, int cells, u32 done, u32 total, int fg, int bg );
void  df_icon( int row, int col, int icon, int bg );
void  df_cells_read( int top, int left, int bottom, int right, CELL *buf );
void  df_cells_write( int top, int left, int bottom, int right, const CELL *buf );
CELL  df_cell_at( int row, int col );
void  df_flush( void );

int   df_icon_register( const char *const *art, int rows );
const char *const *df_icon_art( int icon, int *rows );

/* input.  ev_wait sends the screen first and waits; ev_poll looks and
   comes back - in test mode with nothing, unless XAPOLL says the keys
   are for a running job too. */
int   ev_wait( EVENT *ev );
int   ev_poll( EVENT *ev );
int   ev_mouse_on( void );
int   ev_buttons( void );               /* the buttons down now */
void  ev_mouse_reset( void );           /* after a change of screen */
u32   ev_clock( void );                 /* hundredths, for double clicks */
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

/* message boxes */
enum { DB_OK, DB_OKCANCEL, DB_YESNO };
enum { ANS_OK, ANS_CANCEL, ANS_YES, ANS_NO };

int   msg_box( int fg, int bg, const char *title, const char *text, int buttons );
void  msg_block( const char *title, const char *text );    /* lines kept as a table */
/* buttons named by the caller; the index of the one used, Esc the last */
int   msg_choose( int fg, int bg, const char *title, const char *text,
                  const char *const *labels, int count );

/* one line to type in: room for the longest path the kernel takes (259
   characters), so a folder typed or picked is never cut to another one */
#define TF_TEXT_MAX     260

typedef struct {
    int  row, col, width;               /* from 0 */
    char buffer[TF_TEXT_MAX];
    int  len, cur, scroll, maxlen;
    int  focus, masked;
    int  fg, bg;
} FIELD;

void  field_init( FIELD *fld, int fg, int bg );
void  field_place( FIELD *fld, int row, int col, int width );
void  field_set( FIELD *fld, const char *text );
void  field_max( FIELD *fld, int max );
int   field_hit( const FIELD *fld, int x, int y );
void  field_draw( FIELD *fld );
int   field_event( FIELD *fld, const EVENT *ev );       /* 1 = it was the field's */

enum { IB_CANCEL, IB_OK, IB_SKIP };
#define IB_MASKED   0x01
#define IB_SKIPBTN  0x02

/* ask for a line; the answer's text into "text" (TF_TEXT_MAX) */
int   input_box( int fg, int bg, const char *title, const char *prompt,
                 char *text, int maxlen, int flags );

/* check boxes and a set of choices */
typedef struct {
    int  row, col, end_col;
    char label[48];
    int  checked;
    int  fg, bg;
} CHECK;

void  check_init( CHECK *chk, int fg, int bg, const char *label, int row, int col );
void  check_draw( const CHECK *chk );
int   check_event( CHECK *chk, const EVENT *ev );

#define RG_MAX  8
typedef struct {
    int  row, col;
    char item[RG_MAX][48];
    int  count, chosen, focused;
    int  fg, bg;
} RADIO;

void  radio_init( RADIO *rad, int fg, int bg, int row, int col );
void  radio_add( RADIO *rad, const char *label );
void  radio_draw( const RADIO *rad );
int   radio_event( RADIO *rad, const EVENT *ev );

/* work that can say how far it has got, and work that cannot */
#define PB_WIDTH    62
#define PB_ROOM     (PB_WIDTH - 6)

void  prog_open( const char *title );
int   prog_update( const char *text, u32 done, u32 total );    /* 0 = stop */
void  prog_close( void );
int   prog_is_open( void );
void  wait_open( const char *message );
void  wait_close( void );

/* a file, or text in memory, to read */
int   view_file( const char *path, const char *title );    /* -1 = unreadable */
int   view_text( const char *text, const char *title );

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

void  shorten_tail( char *dst, int room, const char *text );

/* ------------------------------------------------------------------ */
/* MENU.C                                                              */
/* ------------------------------------------------------------------ */
typedef void (*MENUACT)( void );

int   menu_add( const char *label, char hotkey );
int   menu_item( int menu, const char *label, char hotkey, MENUACT action );
void  menu_relabel( int menu, int item, const char *label );
void  menu_row( int row );
void  menu_draw( void );
int   menu_event( const EVENT *ev );   /* 1 = the menu took it */

/* ------------------------------------------------------------------ */
/* LIST.C                                                              */
/* ------------------------------------------------------------------ */
typedef void (*ROWFILL)( void *user, int row, char *buf, int buflen );
typedef void (*ROWACT)( void *user, int row );
typedef int  (*ROWINT)( void *user, int row );
typedef void (*ROWNAME)( void *user, int row, char *buf, int buflen );

typedef struct {
    RECT    at;                         /* the frame, from 0 */
    int     fg, bg, sel_fg, sel_bg, mark_fg;
    char    header[128];
    int     count, sel, top;
    u8     *marks;
    int     mark_count;
    void   *user;
    ROWFILL fill;
    ROWACT  on_activate, on_double;
    void  (*on_marks)( void *user );
    ROWINT  can_mark, row_icon;
    int     mark_icon;
    /* a tree: the list owns the name field at the left */
    ROWINT  depth, has_kids, expanded, toggle;
    ROWNAME label;
    int     name_width;
    int     last_click_row;
    u32     last_click_at;
} LIST;

void  list_init( LIST *lst, int fg, int bg );
void  list_free( LIST *lst );
void  list_region( LIST *lst, int row, int col, int end_row, int end_col );
void  list_header( LIST *lst, const char *text );
void  list_data( LIST *lst, int count, ROWFILL fill, void *user );
void  list_count( LIST *lst, int count );       /* the same list, more or fewer */
void  list_tree( LIST *lst, ROWINT depth, ROWINT has_kids, ROWINT expanded,
                 ROWINT toggle, ROWNAME label, int name_width );
void  list_toggle_node( LIST *lst, int row );
void  list_select( LIST *lst, int row );
int   list_marked( const LIST *lst, int row );
void  list_mark( LIST *lst, int row, int on );
void  list_mark_all( LIST *lst );
void  list_unmark_all( LIST *lst );
void  list_invert( LIST *lst );
int   list_get_marked( const LIST *lst, int *out, int max );
void  list_draw( LIST *lst );
int   list_event( LIST *lst, const EVENT *ev );    /* 1 = the list's */

/* ------------------------------------------------------------------ */
/* DIRS.C                                                              */
/* ------------------------------------------------------------------ */
#define DS_PATHLEN      260

enum { DS_PARENT, DS_DRIVE, DS_DIR, DS_FILE };

typedef struct {
    u32   name;                 /* where its name is in the pool */
    int   kind;
    u32   size;
    u16   date, time;
    u8    attr;
} DSENT;

typedef struct {
    DSENT *ents;
    int    count, cap;
    char  *pool;                /* the names, one after another */
    u32    pool_used, pool_cap;
    char   path[DS_PATHLEN];
    int    drives;              /* the drive list, not a directory */
    char   filter[64];
    int    files_only;
    int    cut;                 /* more entries than the list could hold */
} DIRSCAN;

int   drive_exists( int drive );        /* 1 = A: */
void  dirscan_init( DIRSCAN *ds );
void  dirscan_free( DIRSCAN *ds );
int   dirscan_rescan( DIRSCAN *ds );
int   dirscan_path( DIRSCAN *ds, const char *dir );   /* by name, not by moving */
void  dirscan_empty( DIRSCAN *ds );
void  dirscan_show_directory( DIRSCAN *ds );
int   dirscan_items( const DIRSCAN *ds );
int   dirscan_kind( const DIRSCAN *ds, int row );
const char *dirscan_name( const DIRSCAN *ds, int row );
void  dirscan_disp( const DIRSCAN *ds, int row, char *buf, int buflen );
u32   dirscan_size( const DIRSCAN *ds, int row );
int   dirscan_is_dir( const DIRSCAN *ds, int row );
int   dirscan_is_up( const DIRSCAN *ds, int row );
void  dirscan_date( const DIRSCAN *ds, int row, char *buf, int buflen );
void  dirscan_attr( const DIRSCAN *ds, int row, char *buf, int buflen );
void  dirscan_full( const DIRSCAN *ds, int row, char *dst, int size );
int   dirscan_enter( DIRSCAN *ds, int row );
int   dirscan_up( DIRSCAN *ds );

#define DT_MAX_NODES    1024
#define DT_MAX_DEPTH    34

typedef struct {
    char  name[256];
    u8    depth;
    u8    flags;
} DTNODE;

#define DT_OPEN     0x01
#define DT_KIDS     0x02
#define DT_DRIVE    0x04

typedef struct {
    DTNODE *nodes;
    int     count;
} DIRTREE;

void  dirtree_init( DIRTREE *dt );
void  dirtree_free( DIRTREE *dt );
int   dirtree_build( DIRTREE *dt );
void  dirtree_path( const DIRTREE *dt, int row, char *dst, int size );
int   dirtree_open( DIRTREE *dt, int row );
int   dirtree_close( DIRTREE *dt, int row );
int   dirtree_reveal( DIRTREE *dt, const char *path );

/* a directory search a disk error cannot lie to (see DIRS.C) */
typedef struct {
    int  lfn;
    u32  handle;
    u8   rec[320];
    char name[260];
    u8   attr;
    u32  size;
    u16  date, time;
} FIND;

int   find_first( const char *pattern, int attrs, FIND *fd );   /* 0 = found */
int   find_next( FIND *fd );
void  find_close( FIND *fd );

/* ------------------------------------------------------------------ */
/* FILEBOX.C                                                           */
/* ------------------------------------------------------------------ */
enum { FB_OPEN, FB_SAVE, FB_DIR };

/* "start" is the folder the tree opens on (NULL: the current one) and
   "name" the file's name suggested for FB_SAVE; what was chosen goes
   to "out".  1 = something was chosen. */
int   file_box( const char *title, const char *prompt, int mode, const char *filter,
                const char *start, const char *name, char *out, int size );

/* ------------------------------------------------------------------ */
/* ARCVIEW.C                                                           */
/* ------------------------------------------------------------------ */
void  view_attach( ArcFile *arc );
void  view_folder_mode( int on );
int   view_is_folder_mode( void );
int   view_rebuild( void );
int   view_count( void );
const char *view_cwd( void );
int   view_is_up( int row );
int   view_is_dir( int row );
int   view_index( int row );
void  view_row_name( int row, char *dst, int size );
int   view_enter( int row );
int   view_up( void );
void  view_set_cwd( const char *path );
int   view_expand_one( int row, int *out, int max );
int   view_expand( const int *rows, int nrows, int *out, int max );
int   view_max_expanded( void );

/* ------------------------------------------------------------------ */
/* ARCICON.C                                                           */
/* ------------------------------------------------------------------ */
void  icons_register( void );
int   icon_up( void );
int   icon_drive( void );
int   icon_folder( void );
int   icon_file( void );
int   icon_mark( void );
int   icon_for_name( const char *name );

/* ------------------------------------------------------------------ */
/* COMPDLG.C                                                           */
/* ------------------------------------------------------------------ */
#define CD_PATH_MAX     260

typedef struct {
    char archive[CD_PATH_MAX];
    int  fmt;                   /* ARC_CF_ZIP, ARC_CF_7Z */
    int  keep_paths;
    int  clear_arc;
    char exclude[TF_TEXT_MAX];
} COMPRESULT;

int   compress_dialog( const char *what, COMPRESULT *res );

/* ------------------------------------------------------------------ */
/* XARC.C, CLI.C                                                       */
/* ------------------------------------------------------------------ */
int   tui_run( const char *open_first, int graphics );
int   cli_is_help( const char *arg );
int   cli_is_mem( const char *arg );
int   cli_is_cmd( const char *arg );
void  cli_usage( void );
int   cli_memory( void );
int   cli_run( int argc, char **argv );
int   cli_video_switch( const char *arg, int *graphics );
void  mem_warn_text( char *buf, int size );
const char *test_passed_text( ArcFile *arc );
void  fmt_total( u64 val, char *buf );          /* grouped */
const char *base_name( const char *path );

#endif
