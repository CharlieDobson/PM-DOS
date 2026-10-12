/*
 * UI.H - DEFRAG's screen: SCR.C (the screen in memory, the keyboard,
 * the mouse - EDIT's, by way of PMD and SCANDISK), GFX.C (the same
 * screen drawn in a graphics mode - DOSSHELL's), UI.C (the menu, a
 * box, buttons, a list, a window of text) and SCREEN.C (DEFRAG's own
 * screen and the questions it asks).  None of it is in the host build.
 */
#ifndef UI_H
#define UI_H

#include "defrag.h"

/* ------------------------------------------------------------------ */
/* keys: ASCII as itself, extended keys as 100h + scan code            */
/* ------------------------------------------------------------------ */
#define K_EXT( scan )   (0x100 | (scan))
#define K_SHIFT         0x1000  /* added to a movement key with Shift down */

#define K_CTRLC         0x03
#define K_BS            0x08
#define K_TAB           0x09
#define K_ENTER         0x0D
#define K_ESC           0x1B
#define K_SHTAB         K_EXT( 0x0F )
#define K_F1            K_EXT( 0x3B )
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
#define K_CLEFT         K_EXT( 0x73 )
#define K_CRIGHT        K_EXT( 0x74 )
#define K_CEND          K_EXT( 0x75 )
#define K_CPGDN         K_EXT( 0x76 )
#define K_CHOME         K_EXT( 0x77 )
#define K_CPGUP         K_EXT( 0x84 )

#define K_ALTTAP        0x2000  /* Alt pressed and let go on its own */
#define K_DUMP          0x2001  /* test mode: write the screen to the log */
#define K_EOF           0x2002  /* test mode: the key file ran out */
#define K_MOUSE         0x2003  /* the mouse: "mouse" says what it did */
#define K_TICK          0x2004  /* nothing, in the time */
#define K_SHOT          0x2005  /* test mode: a graphics screen as a .BMP */

int   key_get( void );
int   key_poll( void );             /* K_TICK when there is nothing */
int   key_wait_for( int hundredths );   /* ...or after that long */
int   key_shift( void );
int   key_alt_letter( int key );    /* 'A'..'Z', '0'..'9', or 0 */
void  key_init( void );
int   key_testing( void );
int   key_log_drive( void );        /* test mode: the drive the log is on */

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

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/*                                                                     */
/* A cell is a character and its colours, as on a text screen.  On a   */
/* graphics screen a cell can be more - a picture in place of the      */
/* character, a line along one of its edges - and those bits a text    */
/* screen never sees.                                                  */
/* ------------------------------------------------------------------ */
typedef u32 CELL;

#define CF_PIC_SHIFT    16              /* bits 16-21: a PIC_*, and the
                                           character is what it needs told */
#define CF_PIC_MASK     0x003F0000UL
#define CF_TOP          0x00400000UL    /* a line along the cell's top... */
#define CF_BOTTOM       0x00800000UL
#define CF_LEFT         0x01000000UL
#define CF_RIGHT        0x02000000UL
#define CF_UNDER        0x08000000UL    /* the character underlined */

enum {
    PIC_NONE,
    PIC_BAR,                    /* a piece of the progress bar: the character
                                   is how many of its eight columns are done,
                                   and the colour outside it in the top four
                                   bits */
    PIC_BLOCK,                  /* a block of the map: the character is a BK_* */
    PIC_RADIO,                  /* a round button, two cells: bit 0 its right
                                   half, bit 1 it is the one chosen */
    PIC_CLOSE,                  /* the box in the corner of a title: two
                                   cells, bit 0 the right one */
    PIC_DRIVE,                  /* a drive, three cells: bits 0-1 which, bit 2
                                   a diskette drive */
    PIC_COUNT
};

/* what a block of the map shows */
enum { BK_UNUSED, BK_USED, BK_DONE, BK_FIXED, BK_BAD, BK_READ, BK_WRITE, BK_NONE };

typedef struct {
    u8 desk;            /* the screen, the map, the two boxes under it */
    u8 desk_title;      /* a box's title, a heading */
    u8 done;            /* a block that has been optimized */
    u8 bar;             /* the top line */
    u8 bar_hot;         /* a letter that chooses, there and in the menu */
    u8 bar_sel;         /* the menu's name while it is open, an item lit */
    u8 hint;            /* the bottom line */
    u8 hint_rule;       /* the stroke between what it says and the name */
    u8 dlg;             /* a box */
    u8 dlg_title;       /* its title, the name of a group in it */
    u8 dlg_caption;     /* graphics: the bar its title is in */
    u8 dlg_focus;       /* the round button the keys are on */
    u8 err;             /* a box that says something is wrong */
    u8 err_title;
    u8 btn;             /* a button */
    u8 btn_default;     /* the one Enter presses */
    u8 btn_focus;       /* ...when the keys are on it */
    u8 list_sel;        /* the line of a list that is chosen */
    u8 track;           /* a scroll bar */
    u8 shadow;
} PALETTE;

extern PALETTE pal;
extern int scr_rows, scr_cols;
extern int gfx_on;              /* the screen is a graphics one */

void  scr_init( int graphics );
void  scr_done( void );
void  scr_ch( int row, int col, int ch, u8 attr );
void  scr_putn( int row, int col, const char *text, int len, u8 attr );
void  scr_put( int row, int col, const char *text, u8 attr );
void  scr_center( int row, int left, int width, const char *text, u8 attr );
void  scr_hot( int row, int col, const char *text, u8 attr, u8 hot_attr );
int   scr_hotlen( const char *text );
int   scr_hotkey( const char *text );
void  scr_fill( int row, int col, int len, int ch, u8 attr );
void  scr_recolor( int row, int col, int len, u8 attr );
void  scr_box( int top, int left, int bottom, int right, u8 attr, int twice );
void  scr_shadow( int top, int left, int bottom, int right );
void  scr_cursor( int row, int col, int kind );
void  scr_flush( void );
void  scr_dump( void );             /* test mode */
void  scr_shot( void );             /* test mode */
/* graphics only; a text screen is left as it is */
void  scr_flag( int row, int col, int len, u32 flags );
void  scr_pic( int row, int col, int pic, int data, u8 attr );

/* ------------------------------------------------------------------ */
/* GFX.C - the screen in a graphics mode                               */
/* ------------------------------------------------------------------ */
enum { GA_NONE, GA_EGA, GA_VGA };

extern int gfx_rows;            /* lines of text on the graphics screen */
extern int gfx_cell_h;          /* scan lines in one */

int   gfx_probe( void );                /* GA_* */
int   gfx_enter( void );                /* 1 = the mode is on */
void  gfx_leave( void );
void  gfx_cell( int row, int col, CELL cell );
void  gfx_flush( void );
void  gfx_pointer( int px, int py, int show );
int   gfx_snapshot( const char *path ); /* the picture as a .BMP file */

/* ------------------------------------------------------------------ */
/* UI.C - what the screen's boxes are made of                          */
/* ------------------------------------------------------------------ */

/* what is under a box: called before the box is drawn, every time */
void  ui_backdrop( void (*paint)( void ) );
void  ui_paint_under( void );
/* the line at the bottom of the screen while a box is up */
void  ui_hint( const char *text );
const char *ui_hint_now( void );

/* a key every box takes the same way: the test mode's own, F1.  1 = it
   was one of those and has been seen to */
int   ui_common_key( int key );
void  ui_help( void );              /* the window of text F1 opens */

/* A box.  "title" NULL: one with no title.  "error": in the colours of
   something wrong. */
void  ui_frame( int top, int left, int bottom, int right, const char *title, int error );
/* 1 = the mouse event is a press on the box in a title's corner */
int   ui_close_hit( int top, int left );

#define BTN_MAX 3

typedef struct {
    const char *label;          /* '&' before a letter that chooses it; the
                                   '&' is not shown, nor the letter marked */
    int id;                     /* what choosing it answers; never 0 */
} BTN;

/* a row, or a column, of buttons, where they were last drawn */
typedef struct {
    const BTN *btn;
    int count;
    int width;                  /* of each */
    int chosen;                 /* the one Enter presses */
    int focus;                  /* 1 = the keys are on the buttons */
    int armed;                  /* the one the mouse went down on, or -1 */
    int row[BTN_MAX], col[BTN_MAX];
    u8  ground;                 /* the colours they stand on */
} BTNROW;

void  btn_set( BTNROW *row, const BTN *btn, int count, int width, int chosen );
void  btn_draw_at( BTNROW *row, int index, int at_row, int col, u8 ground );
/* all of them along one row, centred in "width" cells from "left" */
void  btn_draw( BTNROW *row, int at_row, int left, int width, int gap, u8 ground );
int   btn_key( BTNROW *row, int key );  /* an id when one is chosen, else 0 */

/* a group of round buttons, one of them chosen */
typedef struct {
    const char *const *label;
    int count;
    int chosen;
    int at;                     /* the one the keys are on */
    int row, col;               /* the first one's "( )" */
    int step;                   /* rows from one to the next */
    int gap;                    /* cells from a "( )" to its label */
    int wide;                   /* cells the mouse may press: both */
} RADIO;

void  radio_draw( const RADIO *grp, int focus, u8 attr );
int   radio_key( RADIO *grp, int key );     /* 1 = it took the key */
int   radio_mouse( RADIO *grp );            /* 1 = the mouse chose one */

/* the Optimize menu.  -> a GO_*, or 0 when it was left with nothing
   chosen.  "open": pulled down from the start. */
int   ui_menu( int open );
void  ui_menu_bar( int lit );       /* the top line */
int   ui_menu_hit( void );          /* the mouse event is on the menu's name */

/* a box with lines of text and buttons; the id of the button chosen, and
   "esc" for Esc.  "error": in red. */
int   ui_ask( const char *title, const char *text, const BTN *btn, int count, int width,
              int chosen, int esc, int error );

void  app_abandon( void );          /* test mode: the key file ran out */

#endif
