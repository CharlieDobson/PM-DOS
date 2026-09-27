/*
 * EDIT.H - what the parts of EDIT share.
 *
 *   MAIN.C   the command line, the windows, the commands, the loop
 *   DOC.C    a file in memory: lines, loading, saving, editing
 *   VIEW.C   a window onto a document: drawing, the cursor, selection
 *   SCR.C    the screen in memory, colours, the keyboard
 *   UI.C     the menu bar, menus, dialogs and message boxes
 *   DLGS.C   Open, Save As, Find, Replace, Print, Settings, Colors
 *   HELP.C   the help viewer and its text
 */
#ifndef EDIT_H
#define EDIT_H

#include "types.h"
#include "rt.h"
#include "sys.h"

#define MAX_DOCS        9       /* files open at once */
#define MAX_LINE        4096    /* bytes in one line */
#define MAX_COL         4095    /* the rightmost column the cursor reaches */

/* ------------------------------------------------------------------ */
/* keys: ASCII as itself, extended keys as 100h + scan code            */
/* ------------------------------------------------------------------ */
#define K_EXT( scan )   (0x100 | (scan))
#define K_SHIFT         0x1000  /* added to a movement key with Shift down */

#define K_BS            0x08
#define K_TAB           0x09
#define K_ENTER         0x0D
#define K_ESC           0x1B
#define K_SHTAB         K_EXT( 0x0F )
#define K_F1            K_EXT( 0x3B )
#define K_F2            K_EXT( 0x3C )
#define K_F3            K_EXT( 0x3D )
#define K_F4            K_EXT( 0x3E )
#define K_F6            K_EXT( 0x40 )
#define K_F10           K_EXT( 0x44 )
#define K_HOME          K_EXT( 0x47 )
#define K_UP            K_EXT( 0x48 )
#define K_PGUP          K_EXT( 0x49 )
#define K_LEFT          K_EXT( 0x4B )
#define K_RIGHT         K_EXT( 0x4D )
#define K_END           K_EXT( 0x4F )
#define K_DOWN          K_EXT( 0x50 )
#define K_PGDN          K_EXT( 0x51 )
#define K_INS           K_EXT( 0x52 )
#define K_DEL           K_EXT( 0x53 )
#define K_SHF1          K_EXT( 0x54 )
#define K_CTRLF4        K_EXT( 0x61 )
#define K_CTRLF6        K_EXT( 0x63 )
#define K_CTRLF8        K_EXT( 0x65 )
#define K_ALTF4         K_EXT( 0x6B )
#define K_CLEFT         K_EXT( 0x73 )
#define K_CRIGHT        K_EXT( 0x74 )
#define K_CEND          K_EXT( 0x75 )
#define K_CPGDN         K_EXT( 0x76 )
#define K_CHOME         K_EXT( 0x77 )
#define K_CPGUP         K_EXT( 0x84 )
#define K_CUP           K_EXT( 0x8D )
#define K_CDOWN         K_EXT( 0x91 )
#define K_CINS          K_EXT( 0x92 )
#define K_CDEL          K_EXT( 0x93 )

#define K_ALTTAP        0x2000  /* Alt pressed and let go on its own */
#define K_DUMP          0x2001  /* test mode: write the screen to the log */
#define K_EOF           0x2002  /* test mode: the key file ran out */
#define K_MOUSE         0x2003  /* the mouse: "mouse" says what it did */
#define K_TICK          0x2004  /* key_get_timed: nothing, in the time */

#define CTRL( ch )      ((ch) & 0x1F)

int   key_get( void );
int   key_get_timed( int hundredths );  /* K_TICK when nothing came */
int   key_shift( void );
int   key_alt_letter( int key );    /* 'A'..'Z', '0'..'9', or 0 */
void  key_init( void );
int   key_testing( void );

/* ------------------------------------------------------------------ */
/* the mouse: the left button, as events on screen cells               */
/* ------------------------------------------------------------------ */
enum { ME_DOWN, ME_UP, ME_DRAG, ME_DOUBLE };

typedef struct {
    int kind;                   /* ME_* */
    int row, col;               /* the cell it happened on */
    int shift;                  /* the shift keys at the time */
} MEVENT;

extern MEVENT mouse;            /* what the last K_MOUSE was */

void  mouse_init( void );       /* after scr_init: the screen's size */
void  mouse_done( void );
int   mouse_present( void );
int   mouse_held( void );       /* the left button is down now */
void  mouse_where( int *row, int *col );    /* the pointer, now */

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    u8 text;            /* a window's text */
    u8 select;          /* selected text */
    u8 frame;           /* a window's frame and title */
    u8 title;           /* the active window's title */
    u8 scroll;          /* scroll bars */
    u8 thumb;           /* the scroll box in them */
    u8 menu;            /* the menu bar and menus */
    u8 menu_hot;        /* an access key in them */
    u8 menu_sel;        /* the highlighted item */
    u8 menu_sel_hot;
    u8 menu_off;        /* an item that cannot be chosen now */
    u8 status;          /* the status bar */
    u8 status_hot;
    u8 dlg;             /* a dialog box */
    u8 dlg_hot;
    u8 field;           /* an entry field or list in it */
    u8 field_sel;       /* the selected entry of a list */
    u8 button;
    u8 button_focus;
    u8 shadow;
} PALETTE;

extern PALETTE pal;
extern int scr_rows, scr_cols;

void  scr_init( int mono, int lines );
void  scr_done( void );
void  scr_palette( int mono );
void  scr_ch( int row, int col, int ch, u8 attr );
void  scr_putn( int row, int col, const char *text, int len, u8 attr );
void  scr_put( int row, int col, const char *text, u8 attr );
void  scr_hot( int row, int col, const char *text, u8 attr, u8 hot_attr );
int   scr_hotlen( const char *text );
int   scr_hotkey( const char *text );
void  scr_fill( int row, int col, int len, int ch, u8 attr );
void  scr_recolor( int row, int col, int len, u8 attr );
void  scr_box( int top, int left, int bottom, int right, u8 attr );
void  scr_shadow( int top, int left, int bottom, int right );
void  scr_cursor( int row, int col, int kind );
void  scr_flush( void );
void  scr_show_dos( void );         /* the screen EDIT was started on, until a key */
void  scr_dump( void );             /* test mode */

/* ------------------------------------------------------------------ */
/* a document                                                          */
/* ------------------------------------------------------------------ */
/* cap == 0 with text: the line lives in one of the document's pools,
   packed as it was loaded, until an edit needs it to grow */
typedef struct {
    char *text;
    u16   len, cap;
} LINE;

typedef struct {
    LINE *lines;
    u32   count, cap;
    char **pools;           /* the blocks loaded lines are packed into */
    u32   npools, pool_cap;
    char  path[PATH_MAX];   /* full path; "" for a new file */
    char  name[PATH_MAX];   /* what the title bar shows */
    int   untitled;         /* UNTITLEDn's n */
    int   modified;
    int   readonly;         /* /R */
    int   binary;           /* /nnn: the line width, 0 for text */
    int   split;            /* a line longer than MAX_LINE was cut on loading */
    u32   at_line, at_col;  /* where its cursor was, when no window shows it */
    u32   at_top, at_left;
} DOC;

typedef struct {
    u32 line, col;          /* a byte position */
} POS;

extern int tab_size;

DOC  *doc_new( void );
void  doc_free( DOC *doc );
int   doc_load( DOC *doc, const char *path );     /* DOS error, or -1 no memory */
int   doc_save( DOC *doc, const char *path );
void  doc_set_path( DOC *doc, const char *path );
LINE *doc_line( DOC *doc, u32 line );
int   doc_insert( DOC *doc, POS *at, const char *text, u32 len );   /* moves "at" */
int   doc_delete( DOC *doc, POS from, POS to );
char *doc_extract( DOC *doc, POS from, POS to, u32 *len );          /* xfree it */
u32   doc_col_of( DOC *doc, LINE *line, u32 index );         /* byte -> screen column */
u32   doc_index_of( DOC *doc, LINE *line, u32 col );         /* screen column -> byte */
u32   doc_size( DOC *doc );

/* ------------------------------------------------------------------ */
/* a window                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    DOC *doc;
    u32  top;               /* the first line shown */
    u32  left;              /* the first column shown */
    u32  line;              /* the cursor's line */
    u32  col;               /* ...and screen column (may be past the end) */
    u32  want;              /* the column up and down try to keep */
    int  sel;               /* a selection is on */
    u32  sel_line, sel_col; /* its other end */
    int  row, height;       /* on the screen: the title row and how many rows */
} VIEW;

extern int insert_mode;

void  view_draw( VIEW *view, int active );
void  view_place_cursor( VIEW *view );
int   view_key( VIEW *view, int key );             /* 1 = it was an editing key */
void  view_goto( VIEW *view, u32 line, u32 col, int select );
void  view_sel_range( VIEW *view, POS *from, POS *to );   /* 0 if none */
int   view_has_sel( VIEW *view );
void  view_sel_clear( VIEW *view );
void  view_keep_visible( VIEW *view );
int   view_text_rows( VIEW *view );
int   view_text_cols( void );
POS   view_pos( VIEW *view );
void  view_delete_sel( VIEW *view );
int   view_insert_text( VIEW *view, const char *text, u32 len );
void  view_fixup( DOC *doc );       /* after a document changes under other views */

/* the parts of a window a mouse can press */
enum { VH_NONE, VH_TITLE, VH_TEXT, VH_UP, VH_DOWN, VH_PGUP, VH_PGDN, VH_VTHUMB,
       VH_LEFT, VH_RIGHT, VH_PGLEFT, VH_PGRIGHT, VH_HTHUMB };
int   view_hit( VIEW *view, int row, int col );
void  view_mouse_to( VIEW *view, int row, int col, int select );
void  view_select_word( VIEW *view );
void  view_scroll_part( VIEW *view, int part, int row, int col );

/* ------------------------------------------------------------------ */
/* the application                                                     */
/* ------------------------------------------------------------------ */
enum {
    CMD_NONE, CMD_NEW, CMD_OPEN, CMD_SAVE, CMD_SAVEAS, CMD_CLOSE, CMD_PRINT,
    CMD_EXIT, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_CLEAR, CMD_FIND, CMD_REPEAT,
    CMD_REPLACE, CMD_SPLIT, CMD_SIZE, CMD_CLOSEWIN, CMD_OUTPUT, CMD_SETTINGS,
    CMD_COLORS, CMD_HELPCMDS, CMD_HELPKEYS, CMD_ABOUT,
    CMD_DOC1               /* ...to CMD_DOC1 + 8: bring that file forward */
};

extern DOC  *docs[MAX_DOCS];
extern int   ndocs;
extern VIEW  views[2];
extern int   nviews;
extern int   active;
extern char  clip_text_empty;
extern int   short_names;       /* /S */
extern int   split_at;          /* the top window's height when split */
extern char  print_port[8];

const char *err_text( int rc );     /* a DOS error, as a sentence */
void  app_layout( void );

void  app_draw( void );             /* everything under the dialogs */
void  app_status( const char *text );   /* a hint in the status bar */
void  app_command( int cmd );
int   app_can( int cmd );           /* can it be chosen right now? */
DOC  *app_doc( void );
VIEW *app_view( void );
int   app_save( DOC *doc, int ask_name );   /* 1 = saved */
int   app_open_file( const char *path );    /* 1 = opened */
int   clip_has( void );
void  clip_set( char *text, u32 len );      /* takes the block over */
void  edit_main( void );
void  out_of_memory( void );        /* says so, and goes on */
void  app_abandon( void );          /* test mode's end: out, saving nothing */
void  app_show_doc( VIEW *view, DOC *doc );

/* ------------------------------------------------------------------ */
/* menus, dialogs, message boxes                                       */
/* ------------------------------------------------------------------ */
#define MB_OK           1
#define MB_YESNO        2
#define MB_YESNOCANCEL  3
#define MB_OKCANCEL     4
#define ID_OK           1
#define ID_CANCEL       2
#define ID_YES          3
#define ID_NO           4
#define ID_HELP         5

int   menu_run( int key );          /* F10, Alt, Alt+letter -> a command or 0 */
void  menu_draw_bar( int selected, int show_hot );
int   msg_box( const char *text, int buttons );
int   msg_box2( const char *line1, const char *line2, int buttons );

/* the dialog machinery */
enum { CT_LABEL, CT_EDIT, CT_CHECK, CT_RADIO, CT_LIST, CT_BUTTON, CT_TEXT };

struct DLG;
typedef struct CTL {
    int   type;
    int   row, col, width, height;
    const char *text;       /* with '&' before the access key */
    int   id;               /* a button's answer, or a control's name */
    char *buf;              /* CT_EDIT */
    int   max, caret, scroll, fresh;
    int  *value;            /* CT_CHECK and CT_RADIO: *value, CT_RADIO: == id */
    int   count;            /* CT_LIST */
    const char *(*item)( struct DLG *dlg, int index );
    int   sel, top;
    int   is_default;       /* CT_BUTTON: Enter presses it */
} CTL;

typedef struct DLG {
    const char *title;
    int   rows, cols;
    int   top, left;
    CTL  *ctl;
    int   nctl;
    int   focus;
    int   help;             /* the help topic F1 and the Help button show */
    void *data;
    /* changed: a control's value moved; activate: Enter on a list, or a
       button - return an id to close the dialog, 0 to stay */
    void (*changed)( struct DLG *dlg, int ctl );
    int  (*activate)( struct DLG *dlg, int ctl );
} DLG;

int   dlg_run( DLG *dlg );
void  dlg_draw( DLG *dlg );
void  ui_push( DLG *dlg );          /* dialogs drawn over the windows */
void  ui_pop( void );
void  ui_redraw( void );            /* the whole screen, dialogs and all */

/* DLGS.C */
int   dlg_open( char *path );
int   dlg_saveas( DOC *doc, char *path );
int   dlg_find( int replace );
void  find_again( void );
int   find_can_repeat( void );
int   dlg_print( void );
void  dlg_settings( void );
void  dlg_colors( void );
void  dlg_about( void );
void  dlg_size_window( void );

/* HELP.C */
enum { HELP_INDEX, HELP_KEYS, HELP_MENUS, HELP_SELECT, HELP_DIALOGS, HELP_CMDLINE,
       HELP_OPEN, HELP_SAVE, HELP_FIND, HELP_REPLACE, HELP_PRINT, HELP_SETTINGS,
       HELP_COLORS, HELP_MOUSE, HELP_TOPICS };
void  help_show( int topic );

#endif
