/*
 * IDE.H - what the parts of QBASIC's editor share.  The screen, the
 * keyboard, the menus and the dialogs are EDIT's (CMD\EDIT), dressed as
 * QBasic's; the windows know about BASIC.
 *
 *   MAIN.C      the command line, the windows, the commands, running
 *   IDE_SCR.C   the screen in memory, the colours, the keyboard, the mouse
 *   IDE_UI.C    the menu bar, its menus, dialogs and message boxes
 *   IDE_VIEW.C  a window onto a text: drawing, the cursor, editing, and
 *               the check a line gets when the cursor leaves it
 *   IDE_DLGS.C  Open, Save As, Find, Change, Print, Display, New SUB,
 *               SUBs, Help Path, About, the welcome box
 *   IDE_HELP.C  the help window and the help boxes, from QBASIC.HLP
 */
#ifndef IDE_H
#define IDE_H

#include "qb.h"

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
#define K_F5            K_EXT( 0x3F )
#define K_F6            K_EXT( 0x40 )
#define K_F7            K_EXT( 0x41 )
#define K_F8            K_EXT( 0x42 )
#define K_F9            K_EXT( 0x43 )
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
#define K_SHF2          K_EXT( 0x55 )
#define K_SHF5          K_EXT( 0x58 )
#define K_SHF6          K_EXT( 0x59 )
#define K_CTRLF1        K_EXT( 0x5E )
#define K_CTRLF2        K_EXT( 0x5F )
#define K_CTRLF10       K_EXT( 0x67 )
#define K_ALTF1         K_EXT( 0x68 )
#define K_CLEFT         K_EXT( 0x73 )
#define K_CRIGHT        K_EXT( 0x74 )
#define K_CEND          K_EXT( 0x75 )
#define K_CPGDN         K_EXT( 0x76 )
#define K_CHOME         K_EXT( 0x77 )
#define K_ALTMINUS      K_EXT( 0x82 )
#define K_ALTPLUS       K_EXT( 0x83 )
#define K_CPGUP         K_EXT( 0x84 )
#define K_CUP           K_EXT( 0x8D )
#define K_CDOWN         K_EXT( 0x91 )
#define K_CINS          K_EXT( 0x92 )
#define K_CDEL          K_EXT( 0x93 )
#define K_BREAK         0x0003  /* Ctrl+Break, as the kernel hands it over */

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
int   key_alt_held( void );         /* Alt is down by itself now */

/* ------------------------------------------------------------------ */
/* the mouse: the left button, as events on screen cells               */
/* ------------------------------------------------------------------ */
enum { ME_DOWN, ME_UP, ME_DRAG, ME_DOUBLE };

typedef struct {
    int kind;                   /* ME_* */
    int row, col;               /* the cell it happened on */
    int shift;                  /* the shift keys at the time */
} MEVENT;

extern MEVENT mouse;

void  mouse_init( void );
void  mouse_done( void );
void  mouse_hide( int hide );   /* while the program's screen shows */
int   mouse_present( void );
int   mouse_held( void );
void  mouse_where( int *row, int *col );

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    u8 text;            /* a window's text */
    u8 select;          /* selected text */
    u8 frame;           /* a window's frame, and a title not active */
    u8 title;           /* the active window's title */
    u8 current;         /* the statement a stopped program is at */
    u8 bkpt;            /* a breakpoint's line */
    u8 bkpt_current;    /* ...with the current statement on it */
    u8 scroll;          /* scroll bars */
    u8 thumb;           /* the scroll box in them */
    u8 menu;            /* the menu bar, menus and dialogs */
    u8 menu_hot;        /* an access key in them */
    u8 menu_sel;        /* the highlighted item */
    u8 menu_sel_hot;
    u8 menu_off;        /* an item that cannot be chosen now */
    u8 status;          /* the status line */
    u8 status_right;    /* ...its line and column part */
    u8 field_sel;       /* text offered in a field, a list's chosen entry */
    u8 shadow;
    u8 help;            /* the help window's text */
    u8 help_bold;
    u8 help_link;       /* the arrows round a link */
} PALETTE;

extern PALETTE pal;
extern int scr_rows, scr_cols;

void  scr_init( int lines );
void  scr_done( void );
void  scr_palette( void );
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
void  scr_cursor_get( int *row, int *col, int *kind );
void  scr_flush( void );
void  scr_invalidate( void );       /* the program's screen was showing */
void  scr_dump( void );             /* test mode */
void  scr_dump_output( void );      /* test mode: the program's text screen */
u16   scr_cell( int row, int col );

/* ------------------------------------------------------------------ */
/* windows                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    int   text;             /* which of ptext.texts; -1 the Immediate window */
    DOC  *doc;
    u32   top;              /* the first line shown */
    u32   left;             /* the first column shown */
    u32   line;             /* the cursor's line */
    u32   col;              /* ...and screen column (may be past the end) */
    u32   want;             /* the column Up and Down try to keep */
    int   sel;              /* a selection is on */
    u32   sel_line, sel_col;
    int   row, height;      /* on the screen: the title row and how many rows */
    int   bars;             /* scroll bars drawn (the active window has them) */
    int   dirty;            /* the cursor's line has been typed on */
    int   err_line;         /* a line left with an error in it: not asked again */
} VIEW;

extern int insert_mode;
extern int syntax_check;        /* Options > Syntax Checking */
extern int scroll_bars;         /* Options > Display: the scroll bars shown */

void  view_draw( VIEW *view, int active );
void  view_place_cursor( VIEW *view );
int   view_key( VIEW *view, int key );       /* 1 = it was an editing key */
void  view_goto( VIEW *view, u32 line, u32 col, int select );
void  view_sel_range( VIEW *view, POS *from, POS *to );
int   view_has_sel( VIEW *view );
void  view_keep_visible( VIEW *view );
int   view_text_rows( VIEW *view );
POS   view_pos( VIEW *view );
void  view_delete_sel( VIEW *view );
int   view_insert_text( VIEW *view, const char *text, u32 len );
void  view_fixup( DOC *doc );
int   view_leave_line( VIEW *view );         /* the line checked: 0 = it has an error */
void  view_show_text( VIEW *view, int text );
void  view_word_at( VIEW *view, char *out, u32 max );
void  view_learn_program( void );   /* a program loaded: its names, its lines in form */

enum { VH_NONE, VH_TITLE, VH_MAXBOX, VH_TEXT, VH_UP, VH_DOWN, VH_PGUP, VH_PGDN, VH_VTHUMB,
       VH_LEFT, VH_RIGHT, VH_PGLEFT, VH_PGRIGHT, VH_HTHUMB };
int   view_hit( VIEW *view, int row, int col );
void  view_mouse_to( VIEW *view, int row, int col, int select );
void  view_select_word( VIEW *view );
void  view_scroll_part( VIEW *view, int part, int row, int col );

/* the lines a stopped program marks: the text and line of the current
   statement and its columns, from MAIN.C */
int   app_current( int text, u32 line, int *col, int *len );

/* ------------------------------------------------------------------ */
/* the application                                                     */
/* ------------------------------------------------------------------ */
enum {
    CMD_NONE, CMD_NEW, CMD_OPEN, CMD_SAVE, CMD_SAVEAS, CMD_PRINT, CMD_EXIT,
    CMD_CUT, CMD_COPY, CMD_PASTE, CMD_CLEAR, CMD_NEWSUB, CMD_NEWFUNC,
    CMD_SUBS, CMD_SPLIT, CMD_OUTPUT,
    CMD_FIND, CMD_REPEAT, CMD_CHANGE,
    CMD_START, CMD_RESTART, CMD_CONTINUE,
    CMD_STEP, CMD_PSTEP, CMD_TRACE, CMD_BREAKPOINT, CMD_CLEARBP, CMD_SETNEXT,
    CMD_DISPLAY, CMD_HELPPATH, CMD_SYNTAX,
    CMD_INDEX, CMD_CONTENTS, CMD_TOPIC, CMD_USINGHELP, CMD_ABOUT
};

extern VIEW  views[3];          /* [0], [1] the editor's, [2] Immediate */
extern int   nviews;            /* editor windows: 1 or 2 */
extern int   active;            /* 0, 1 or 2 */
extern char  clip_text_empty;

const char *err_text( int rc );     /* a DOS error, as a sentence */
void  app_layout( void );
void  app_draw( void );             /* everything under the dialogs */
void  app_status( const char *text );   /* a hint on the status line */
void  app_status_wide( const char *text );  /* ...the whole line, no counter */
void  app_command( int cmd );
int   app_can( int cmd );           /* can it be chosen right now? */
int   app_checked( int cmd );       /* a bullet beside it (Trace, Syntax) */
VIEW *app_view( void );
int   clip_has( void );
void  clip_set( char *text, u32 len );
const char *clip_get( u32 *len );
void  app_abandon( void );          /* test mode's end */
void  app_edited( void );           /* the program changed while stopped */
const char *app_title( int text, char *buf, u32 max );
int   app_running( void );          /* a program is stopped part way */
void  app_set_bar_hot( int hot );   /* the menu bar's letters, Alt held */

/* ------------------------------------------------------------------ */
/* menus, dialogs, message boxes                                       */
/* ------------------------------------------------------------------ */
#define MB_OK           1
#define MB_YESNOCANCEL  3
#define MB_OKCANCEL     4
#define ID_OK           1
#define ID_CANCEL       2
#define ID_YES          3
#define ID_NO           4
#define ID_HELP         5

int   menu_run( int key );          /* F10, Alt, Alt+letter -> a command or 0 */
void  menu_draw_bar( int selected, int show_hot, int lit );
int   msg_box( const char *text, int buttons, int help );
int   msg_box2( const char *line1, const char *line2, int buttons, int help );

enum { CT_LABEL, CT_EDIT, CT_CHECK, CT_RADIO, CT_LIST, CT_BUTTON, CT_TEXT, CT_FRAME, CT_SAMPLE };

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
    int   columns;          /* CT_LIST: entries side by side (Open's files) */
    u8    attr;             /* CT_SAMPLE: its colours */
                            /* CT_EDIT: height 1 is a field with no box round it */
} CTL;

typedef struct DLG {
    const char *title;
    int   rows, cols;
    int   top, left;
    CTL  *ctl;
    int   nctl;
    int   focus;
    const char *help;       /* the help topic F1 and the Help button show */
    void *data;
    int   sep;              /* the row of the line above the buttons, 0 none */
    void (*changed)( struct DLG *dlg, int ctl );
    int  (*activate)( struct DLG *dlg, int ctl );
    int   stay;             /* set by activate: Enter was used up, the dialog stays */
} DLG;

int   dlg_run( DLG *dlg );
void  dlg_draw( DLG *dlg );
void  ui_push( DLG *dlg );
void  ui_pop( void );
void  ui_redraw( void );

/* IDE_DLGS.C */
int   dlg_open( char *path );
int   dlg_saveas( char *path );
int   dlg_find( int change );
void  find_again( void );
int   find_can_repeat( void );
void  dlg_print( void );
void  dlg_display( void );
void  dlg_helppath( void );
int   dlg_newproc( int kind, char *name );
int   dlg_subs( void );             /* -> the text chosen, or -1 */
void  dlg_about( void );
int   dlg_welcome( void );          /* 1 = Enter, for the Survival Guide */
extern char help_path[PATH_MAX];

/* IDE_HELP.C */
void  help_init( void );
void  help_topic( const char *name );       /* the help window, on a topic */
int   help_has( const char *name );   /* a topic of that name? */
void  help_box( const char *name );         /* a help box over a dialog */
int   help_shown( void );                   /* the help window is open */
void  help_close( void );
int   help_rows( void );                    /* rows it takes, title and all */
void  help_draw( int active );
int   help_key( int key );                  /* 1 = it was the window's */
void  help_mouse( void );
void  help_keyword( VIEW *view );           /* F1 on the word at the cursor */
int   help_has_keyword( VIEW *view );

#endif
