/*
 * HELP.H - what the parts of HELP share.
 *
 *   MAIN.C   the command line, the commands, the status bar, the loop
 *   HLP.C    the help file: loading it, its topics and contexts, and
 *            a line of it rendered into characters, colours and links
 *   VIEW.C   the help window: drawing, the cursor, the links, the
 *            scroll bar, the mouse
 *   SCR.C    the screen in memory, colours, the keyboard, the mouse
 *   UI.C     the menu bar, menus, dialogs and message boxes
 *   DLGS.C   Find, Print, Printer Setup, About
 *
 * The screen, keyboard, mouse, menu and dialog code is EDIT's (CMD\EDIT),
 * as PMD's is; what is new is the help file and its window.
 */
#ifndef HELP_H
#define HELP_H

#include "types.h"
#include "rt.h"
#include "sys.h"

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
#define K_F3            K_EXT( 0x3D )
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
#define K_CTRLF1        K_EXT( 0x5E )
#define K_CLEFT         K_EXT( 0x73 )
#define K_CRIGHT        K_EXT( 0x74 )
#define K_CEND          K_EXT( 0x75 )
#define K_CPGDN         K_EXT( 0x76 )
#define K_CHOME         K_EXT( 0x77 )
#define K_CPGUP         K_EXT( 0x84 )
#define K_CUP           K_EXT( 0x8D )
#define K_CDOWN         K_EXT( 0x91 )

#define K_ALTTAP        0x2000  /* Alt pressed and let go on its own */
#define K_DUMP          0x2001  /* test mode: write the screen to the log */
#define K_EOF           0x2002  /* test mode: the key file ran out */
#define K_MOUSE         0x2003  /* the mouse: "mouse" says what it did */
#define K_TICK          0x2004  /* key_get_timed: nothing, in the time */

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
    u8 text;            /* the topic's text */
    u8 bold;            /* \b text */
    u8 italic;          /* \i text, and the brackets round a link */
    u8 under;           /* \u text */
    u8 frame;           /* the window's frame */
    u8 title;           /* the window's title */
    u8 scroll;          /* the scroll bar */
    u8 thumb;           /* the scroll box in it */
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
void  scr_box2( int top, int left, int bottom, int right, u8 attr );   /* double lines */
void  scr_shadow( int top, int left, int bottom, int right );
void  scr_cursor( int row, int col, int kind );
void  scr_flush( void );
void  scr_dump( void );             /* test mode */

/* ------------------------------------------------------------------ */
/* the help file                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *name;       /* the context, as written, NUL-terminated */
    int   topic;
} CONTEXT;

typedef struct {
    const char *title;      /* .topic's text, or its first context */
    u32   first;            /* its first line in hlines */
    u32   count;            /* how many lines */
    int   freeze;           /* .freeze: lines kept at the top */
} TOPIC;

typedef struct {
    const char *text;       /* the line as written, escapes and all */
    u16   len;
} HLINE;

extern TOPIC   *topics;
extern int      ntopics;
extern CONTEXT *contexts;
extern int      ncontexts;
extern HLINE   *hlines;
extern u32      nhlines;

#define RL_MAX      256     /* columns a rendered line can have */
#define RL_LINKS    40      /* links on one line */

typedef struct {
    int   start, end;       /* columns [start, end) of the link's text */
    const char *ctx;        /* the context it leads to */
    int   ctx_len;
} HLINK;

typedef struct {
    u8    ch[RL_MAX];
    u8    attr[RL_MAX];
    int   len;
    HLINK links[RL_LINKS];
    int   nlinks;
} RLINE;

int   hlp_load( const char *path );         /* a DOS error, -1 no memory, -2 not a help file */
int   hlp_find( const char *context );      /* a topic, or -1 */
int   hlp_find_n( const char *context, int len );
void  hlp_render( u32 line, RLINE *out );   /* a line of hlines, as it is shown */
void  hlp_plain( u32 line, char *out, int max );    /* ...as text alone, for printing and finding */
const HLINE *hlp_line( int topic, u32 line );   /* a topic's line */

/* ------------------------------------------------------------------ */
/* the help window                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    int topic;              /* -1: nothing shown */
    u32 top;                /* the first scrolled line shown (after the frozen ones) */
    u32 line, col;          /* the cursor */
    u32 want;               /* the column Up and Down try to keep */
} HVIEW;

extern HVIEW view;

int   view_rows( void );                    /* text rows in the window */
int   view_cols( void );
void  view_show( int topic );               /* a topic, cursor at its first link */
void  view_draw( void );
void  view_place_cursor( void );
int   view_key( int key );                  /* 1 = the window took it */
void  view_keep_visible( void );
void  view_goto( u32 line, u32 col );       /* the cursor, and scroll to it */
int   view_link_at_cursor( HLINK *link );   /* 1 = the cursor is on a link */
int   view_next_link( int step, int letter );   /* Tab, Shift+Tab, a letter: moves the cursor */
void  view_scroll( int lines );
int   view_lines( void );                   /* lines in the topic shown */
void  view_dump( char *buf, u32 max );      /* test mode: the window's numbers */

/* the parts of the window a mouse can press */
enum { VH_NONE, VH_TEXT, VH_UP, VH_DOWN, VH_PGUP, VH_PGDN, VH_VTHUMB };
int   view_hit( int row, int col );
void  view_scroll_part( int part, int row, int col );
void  view_mouse_to( int row, int col );

/* ------------------------------------------------------------------ */
/* the application                                                     */
/* ------------------------------------------------------------------ */
enum {
    CMD_NONE, CMD_PRINT, CMD_EXIT, CMD_FIND, CMD_REPEAT, CMD_HOWTO, CMD_ABOUT,
    CMD_CONTENTS, CMD_NEXT, CMD_PREV, CMD_BACK
};

/* contexts the viewer itself looks for in the help file */
#define CTX_CONTENTS    "Contents"
#define CTX_HOWTO       "Help on Help"
#define CTX_MENU_FILE   "File Menu"
#define CTX_MENU_SEARCH "Search Menu"
#define CTX_MENU_HELP   "Help Menu"
#define CTX_PRINT_CMD   "Print Command"
#define CTX_EXIT_CMD    "Exit Command"
#define CTX_FIND_CMD    "Find Command"
#define CTX_REPEAT_CMD  "Repeat Last Find Command"
#define CTX_HOWTO_CMD   "Help on Help Command"
#define CTX_ABOUT_CMD   "About Command"
#define CTX_FIND_DLG    "Find Dialog"
#define CTX_PRINT_DLG   "Print Dialog"
#define CTX_SETUP_DLG   "Printer Setup Dialog"

const char *err_text( int rc );     /* a DOS error, as a sentence */
void  app_draw( void );             /* everything under the dialogs */
void  app_status( const char *text );   /* a hint in the status bar */
void  app_command( int cmd );
int   app_can( int cmd );           /* can it be chosen right now? */
void  app_show( int topic );        /* a topic, remembering where we were */
void  app_show_ctx( const char *context );  /* ...by name; nothing when there is none */
void  app_find_next( void );
void  help_main( void );
void  out_of_memory( void );        /* says so, and goes on */
void  app_abandon( void );          /* test mode's end: out */
void  help_show( const char *context );     /* F1 in a dialog or a menu */

extern char print_port[8];

/* ------------------------------------------------------------------ */
/* menus, dialogs, message boxes                                       */
/* ------------------------------------------------------------------ */
#define MB_OK           1
#define MB_YESNO        2
#define MB_YESNOCANCEL  3
#define MB_OKCANCEL     4
#define MB_OKHELP       5
#define ID_OK           1
#define ID_CANCEL       2
#define ID_YES          3
#define ID_NO           4
#define ID_HELP         5

int   menu_run( int key );          /* F10, Alt, Alt+letter -> a command or 0 */
void  menu_draw_bar( int selected, int show_hot );
int   msg_box( const char *text, int buttons, const char *help );
int   msg_box2( const char *line1, const char *line2, int buttons, const char *help );

/* the dialog machinery */
enum { CT_LABEL, CT_EDIT, CT_CHECK, CT_RADIO, CT_LIST, CT_BUTTON, CT_TEXT, CT_RULE, CT_BOX };

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
    int   off;              /* greyed: shown, not reached */
} CTL;

typedef struct DLG {
    const char *title;
    int   rows, cols;
    int   top, left;
    CTL  *ctl;
    int   nctl;
    int   focus;
    const char *help;       /* the context F1 and the Help button show */
    void *data;
    /* changed: a control's value moved; activate: Enter on a list, or a
       button - return an id to close the dialog, 0 to stay */
    void (*changed)( struct DLG *dlg, int ctl );
    int  (*activate)( struct DLG *dlg, int ctl );
} DLG;

int   dlg_run( DLG *dlg );
void  dlg_draw( DLG *dlg );
void  ui_push( DLG *dlg );          /* dialogs drawn over the window */
void  ui_pop( void );
void  ui_redraw( void );            /* the whole screen, dialogs and all */

/* DLGS.C */
int   dlg_find( void );             /* 1 = a search was asked for and done */
int   find_can_repeat( void );
void  find_again( void );
int   dlg_print( void );
void  dlg_about( void );

#endif
