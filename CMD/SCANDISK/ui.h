/*
 * UI.H - SCANDISK's screens: SCR.C (the screen in memory, the keyboard,
 * the mouse - EDIT's, by way of PMD), GFX.C (the same screen drawn in a
 * graphics mode - DOSSHELL's), UI.C (buttons, boxes, a window of text)
 * and SCREEN.C (ScanDisk's own screens).  None of it is in the host
 * build.
 */
#ifndef UI_H
#define UI_H

#include "scandisk.h"

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
#define K_F3            K_EXT( 0x3D )
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
void  key_poll_phase( int phase );  /* test mode: which check is running */
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

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/*                                                                     */
/* A cell is a character and its colours, as on a text screen.  On a   */
/* graphics screen a cell can be more - a picture in place of the      */
/* character, the edge of a panel, part of a button - and those bits   */
/* a text screen never sees.                                           */
/* ------------------------------------------------------------------ */
typedef u32 CELL;

#define CF_PIC_SHIFT    16              /* bits 16-21: a PIC_*, and the
                                           character is what it needs told */
#define CF_PIC_MASK     0x003F0000UL
#define CF_TOP          0x00400000UL    /* a line along the cell's top... */
#define CF_BOTTOM       0x00800000UL
#define CF_LEFT         0x01000000UL    /* (a button's first cell) */
#define CF_RIGHT        0x02000000UL    /* (a button's last cell) */
#define CF_RAISED       0x04000000UL    /* ...in light and shade: a panel */
#define CF_UNDER        0x08000000UL    /* the character underlined */
#define CF_PART_SHIFT   28              /* bits 28-29: a BP_* of a button */
#define CF_PART_MASK    0x30000000UL
#define CF_FOCUS        0x40000000UL    /* button: the one Enter presses */
#define CF_DOWN         0x80000000UL    /* button: held down */

/* a button on a graphics screen is its row of text and four scan lines
   of the rows above and below */
enum { BP_NONE, BP_ABOVE, BP_LABEL, BP_BELOW };

enum {
    PIC_NONE,
    PIC_BAR,                    /* a piece of the progress bar: the character
                                   is how many of its eight columns are done */
    PIC_MAP,                    /* two blocks of the surface map, one above
                                   the other: MB_* of the upper in the low
                                   four bits of the character and of the lower
                                   in the high four, MC_* where the colours go */
    PIC_TICK,                   /* a check that is done: two cells */
    PIC_NOW,                    /* the check being made: two cells */
    PIC_ASK,                    /* a question: four cells by two rows */
    PIC_WARN,                   /* a problem */
    PIC_INFO,                   /* something to be read */
    PIC_COUNT
};

/* a block of the surface map */
#define MB_SOME         0x01    /* some of its clusters are in use */
#define MB_ALL          0x02    /* all of them */
#define MB_BAD          0x04    /* one at least is bad */
#define MB_READ         0x08    /* the scan has been over it */

#define MC_UPPER        0x01    /* there is an upper block */
#define MC_LOWER        0x02
#define MC_UPPER_NOW    0x04    /* ...and the scan is reading it */
#define MC_LOWER_NOW    0x08

typedef struct {
    u8 desk;            /* the screen, and ordinary text on it */
    u8 bright;          /* the title, the verdict, the line at the bottom */
    u8 rule;            /* the lines under the title and over the bottom */
    u8 mark;            /* the tick against a check that has finished */
    u8 button;          /* < A button > */
    u8 button_hot;      /* its access key */
    u8 button_focus;    /* the one Enter would press */
    u8 button_down;     /* held down by the mouse */
    u8 button_shadow;   /* the shadow a button on the screen casts */
    u8 dlg;             /* a box */
    u8 dlg_frame;       /* its lines and its title */
    u8 dlg_bright;      /* a heading in it */
    u8 dlg_caption;     /* graphics: the bar its title is in */
    u8 shadow;
    u8 bar_done;        /* the progress bar */
    u8 bar_todo;
    u8 map;             /* the surface map: blocks not yet read */
    u8 map_read;        /* ...and read */
    u8 map_bad;
} PALETTE;

extern PALETTE pal;
extern int scr_rows, scr_cols;
extern int gfx_on;              /* the screen is a graphics one */

void  scr_init( int graphics );
void  scr_done( void );
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
void  scr_dump( void );             /* test mode */
void  scr_shot( void );             /* test mode */
/* graphics only; a text screen is left as it is */
void  scr_flag( int row, int col, int len, u32 flags );
void  scr_pic( int row, int col, int pic, int data, u8 attr );
void  scr_icon( int row, int col, int pic, u8 attr );   /* all its cells */

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
void  gfx_map_colors( int todo, int read, int bad );
int   gfx_snapshot( const char *path ); /* the picture as a .BMP file */

/* ------------------------------------------------------------------ */
/* buttons, boxes, a window of text                                    */
/* ------------------------------------------------------------------ */
#define BTN_MAX 5

typedef struct {
    const char *label;          /* with '&' before the access key */
    int id;                     /* what choosing it answers; never 0 */
} BTN;

/* a row of buttons, where they were last drawn */
typedef struct {
    const BTN *btn;
    int count;
    int focus;                  /* the one Enter presses, or -1: none is lit
                                   and only the letters and the mouse work */
    int armed;                  /* the one the mouse went down on, or -1 */
    int row, col[BTN_MAX], len[BTN_MAX];
    int on_desk;                /* on the screen, not in a box: shadows */
} BTNROW;

void  btn_set( BTNROW *row, const BTN *btn, int count, int focus, int on_desk );
void  btn_draw( BTNROW *row, int at_row, int left, int width );     /* centred in that span */
void  btn_draw_from( BTNROW *row, int at_row, int col );            /* from that column on */
int   btn_width( const BTNROW *row );
int   btn_key( BTNROW *row, int key );  /* an id when one is chosen, else 0 */

/* what is under a box: called before the box is drawn, every time */
void  ui_backdrop( void (*paint)( void ) );
int   ui_box_up( void );            /* a box is on the screen now */

/* a box's frame: its lines and title, a rule across it over the row
   its buttons are in, and its shadow */
void  ui_frame( int top, int left, int bottom, int right, const char *title );

/* a box with a title, a paragraph or more of text and buttons; the id
   of the button chosen.  Ctrl+C answers "ctrlc" when that is not 0.
   "pic" is a PIC_* a graphics screen shows beside the text, or 0. */
int   ui_dialog( const char *title, const TEXT *body, const BTN *btn, int count, int focus,
                 int ctrlc, int pic );
/* the same from one paragraph, wrapped to fit */
int   ui_ask( const char *title, const char *para, const BTN *btn, int count, int focus,
              int ctrlc, int pic );
/* a window of text that scrolls, with buttons under it; a line that
   begins with '\1' is a heading.  Esc answers "esc". */
int   ui_text( const char *title, const TEXT *text, const BTN *btn, int count, int ctrlc,
               int esc );
/* a paragraph in that window */
int   ui_info( const char *title, const char *para, const BTN *btn, int count, int ctrlc,
               int esc );

void  app_abandon( void );          /* test mode: the key file ran out */

#endif
