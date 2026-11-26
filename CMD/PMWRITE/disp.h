/*
 * DISP.H - the screen, the keyboard and the mouse, as PMWRITE sees
 * them: SCR.C and GFX.C.  UI.H (menus, dialogs, the file box) is built
 * on this and an application uses both.
 *
 * ONE SCREEN OF CELLS, SHOWN TWO WAYS.  The screen is a grid of
 * character cells, scr_rows by scr_cols, and the whole of it is drawn
 * into memory on every turn of the program's loop - menu bar, status
 * bar, whatever dialogs are open - with scr_put and its relatives.
 * scr_flush sends what changed.  Nothing ever has to remember what was
 * under a dialog: the next turn draws the screen without it.
 *
 * On a TEXT screen (gfx_on == 0) the cells are the hardware's.  On a
 * GRAPHICS screen (gfx_on == 1) each cell is 8 pixels by 16 (CELL_W,
 * CELL_H) and is painted from the video ROM's 8x16 font into a picture
 * in memory, one byte a pixel, which is copied to the screen in the
 * form the hardware wants: 640x480 is 80 by 30 cells, 800x600 is 100
 * by 37 (and 8 spare lines), 1024x768 is 128 by 48, 1280x1024 is 160
 * by 64.
 *
 * PANES ARE THE PARTS THE APPLICATION DRAWS IN PIXELS - a toolbar, a
 * ruler, the page.  A pane is a rectangle of cells handed over with
 * pane_place while the screen is being drawn; the cell layer leaves
 * those pixels alone, and when something drawn over a pane in cells (a
 * menu, a dialog) is gone again, scr_flush calls the pane's paint
 * function with the rectangle that was uncovered.  When the pane's OWN
 * contents change, the application says so (pane_damage) and is called
 * the same way.  Painting is done with the gfx_ calls below, which
 * draw into the picture, clipped.  ON A TEXT SCREEN THERE ARE NO
 * PANES: pane_place does nothing and returns 0, and the application
 * fills those cells with characters instead.
 *
 * THE PALETTE IS A CONTRACT.  A pixel is a number:
 *
 *      0 to 15     the sixteen VGA colours (black, blue, green, cyan,
 *                  red, magenta, brown, light grey, dark grey and the
 *                  bright eight, white last)
 *     16 to 231    a colour cube: 16 + 36 * red + 6 * green + blue,
 *                  each 0 to 5 and standing for 0, 51, 102, 153, 204
 *                  and 255
 *    232 to 255    24 greys: level 8 + 10 * n
 *
 * gfx_colors is 256 when all of them can be shown and 16 when only the
 * first sixteen can (a planar mode); gfx_rgb gives the nearest number
 * there is for a colour either way.  In a 16-colour mode a pixel's
 * upper four bits are not shown.
 *
 * COORDINATES.  Cells are (row, col) from 0.  Pixels are (x, y) from
 * the top left of the SCREEN, for every call here - a pane's paint
 * function, gfx_ drawing, the mouse.  A rectangle given as x0, y0, x1,
 * y1 includes both corners; one given as x, y, w, h is a corner and a
 * size.
 *
 * NAMES.  PMWRITE links a PDF engine that owns many short names, so
 * nothing here is called CLIP, PAGE, VIEW, FONT, PAINT or the like,
 * and there is no gfx_put or gfx_fill: a filled rectangle is gfx_rect.
 */
#ifndef DISP_H
#define DISP_H

#include "types.h"
#include "sys.h"        /* CUR_HIDE, CUR_LINE, CUR_BLOCK; SH_SHIFT, SH_CTRL, SH_ALT */

/* ------------------------------------------------------------------ */
/* starting and stopping                                               */
/* ------------------------------------------------------------------ */

/* Call once, after sys_init.  The text screen is taken as it is found
   (its contents are kept, to be put back by disp_done), the machine is
   asked what graphics it has, the mouse is found, and test mode starts
   if PWTEST says so.  The screen is NOT cleared and no mode is set. */
void  disp_init( void );

/* The screen as DOS programs expect it and as it was found: text, the
   lines it had, what was on it, the cursor where it was. */
void  disp_done( void );

/* Round sys_exec: the DOS screen (empty) for another program, and this
   program's mode again afterwards.  After scr_resume everything is
   sent again, panes included. */
void  scr_suspend( void );
void  scr_resume( void );

/* ------------------------------------------------------------------ */
/* display modes                                                       */
/*                                                                     */
/* The list is this machine's: the text screens first (25, 43 and 50   */
/* lines of 80 columns - and the screen the program was started on, if */
/* that was none of those), then one entry for each of 640x480,        */
/* 800x600, 1024x768 and 1280x1024 that the video card offers.  For    */
/* each resolution the best way there is of showing it was chosen:     */
/*                                                                     */
/*   DB_LFB8     a VBE linear frame buffer, a byte a pixel             */
/*   DB_LFBHI    a VBE linear frame buffer of 15, 16, 24 or 32 bits a  */
/*               pixel - still 256 colours to the program, turned into */
/*               the screen's pixels as they are copied out            */
/*   DB_PLANAR   the VGA's own 16-colour planes: 640x480 is BIOS mode  */
/*               12h, 800x600 BIOS mode 6Ah where the card has it      */
/*                                                                     */
/* A mode on the list can still fail to set (some BIOSes list what     */
/* they cannot do, and mode 6Ah is only known by trying it): see       */
/* disp_set_mode.                                                      */
/*                                                                     */
/* FOR TESTING, the environment variable PWVIDEO narrows the choice to */
/* one kind: 4 (planar only), 8, 15, 16, 24 or 32 (a linear frame      */
/* buffer of that depth only).                                         */
/* ------------------------------------------------------------------ */
enum { DB_TEXT, DB_PLANAR, DB_LFB8, DB_LFBHI };

typedef struct {
    uint8_t  backend;           /* DB_* */
    uint8_t  bpp;               /* the hardware's bits a pixel: 0 (text), 4, 8, 15, 16, 24, 32 */
    uint16_t width, height;     /* pixels; 0 and 0 for a text screen */
    uint16_t cols, rows;        /* cells */
    uint16_t colors;            /* what gfx_colors will be: 16 or 256 (16 for text) */
    uint16_t vmode;             /* the BIOS's or VBE's number for it - not for the application */
    char     name[40];          /* for a list: "Text, 50 lines", "800 x 600, 256 colors" */
} DISPMODE;

int   disp_mode_count( void );
const DISPMODE *disp_mode_info( int index );    /* NULL when there is no such entry */
int   disp_mode_now( void );                    /* the entry that is on the screen */

/* The entry for a resolution, or -1 when the machine has not got it:
   disp_mode_find( 800, 600 ) is a graphics screen, and
   disp_mode_find( 0, 43 ) the text screen of 43 lines.  This is how a
   saved setting is turned back into an entry - the numbers in the
   list differ from machine to machine. */
int   disp_mode_find( int width, int height );

/* Change the screen NOW: text to graphics, graphics to text, one
   resolution to another.  Returns 1 when the screen is the entry asked
   for.  Returns 0 when it could not be had; the screen is then some
   other mode that could (the one it was before if that still sets,
   else a smaller one, in the end 25 lines of text), disp_mode_now says
   which, and disp_error why.  An entry the video card refused is
   taken off the list (the entries after it move up).
   EITHER WAY the screen is empty afterwards, scr_rows and scr_cols may
   have changed, and every pane is forgotten: lay the screen out again
   and draw everything.  (ui_set_mode in UI.H does this and tells the
   user when it failed.) */
int   disp_set_mode( int index );

enum { DE_NONE,
       DE_NOMEM,                /* no memory for a picture that size */
       DE_REFUSED,              /* the video BIOS would not set the mode */
       DE_NOFONT };             /* the ROM's 8x16 font could not be found */
int   disp_error( void );       /* why the last disp_set_mode returned 0 */

/* ------------------------------------------------------------------ */
/* the screen in cells                                                 */
/* ------------------------------------------------------------------ */
#define CELL_W          8       /* a cell on a graphics screen, in pixels */
#define CELL_H          16

/* an attribute is a text screen's: the text colour in the low four
   bits, the background in the high four - sixteen of each on a
   graphics screen, eight backgrounds on a text one */
#define ATTR( fg, bg )  ((uint8_t)(((bg) << 4) | (fg)))

/* more that a cell can carry on a graphics screen (scr_flag); a text
   screen ignores them */
#define CELLF_TOP       0x04000000UL    /* a line along the cell's top... */
#define CELLF_BOTTOM    0x08000000UL
#define CELLF_LEFT      0x10000000UL
#define CELLF_RIGHT     0x20000000UL
#define CELLF_UNDER     0x40000000UL    /* the character underlined */
/* ...and what colour those lines are.  With neither of these they are
   the cell's text colour; CELLF_RAISE makes the top and left ones white
   and the bottom and right ones dark grey, so that a run of cells
   stands out of the screen like a button, CELLF_SINK the other way round,
   like a well; both together (CELLF_FRAME) make them all black. */
#define CELLF_RAISE     0x80000000UL
#define CELLF_SINK      0x01000000UL
#define CELLF_FRAME     (CELLF_RAISE | CELLF_SINK)

extern int scr_rows, scr_cols;  /* the screen in cells, now */
extern int gfx_on;              /* 1 on a graphics screen */
extern int gfx_width, gfx_height;       /* its pixels; 0 on a text screen */
extern int gfx_colors;          /* 16 or 256 */

/* Everything is clipped: a cell off the screen is not drawn. */
void  scr_ch( int row, int col, int ch, uint8_t attr );
void  scr_putn( int row, int col, const char *text, int len, uint8_t attr );
void  scr_put( int row, int col, const char *text, uint8_t attr );
void  scr_putw( int row, int col, const char *text, int width, uint8_t attr ); /* padded or cut to "width" */
void  scr_fill( int row, int col, int len, int ch, uint8_t attr );
void  scr_recolor( int row, int col, int len, uint8_t attr );  /* the characters stay */
void  scr_flag( int row, int col, int len, uint32_t flags );   /* CELLF_* onto cells already drawn */

/* "&File": the character after the first '&' in hot_attr - and
   underlined, on a graphics screen - with the '&' not shown.  Pass
   hot_attr the same as attr when access keys are not being shown. */
void  scr_hot( int row, int col, const char *text, uint8_t attr, uint8_t hot_attr );
int   scr_hotlen( const char *text );           /* its length without the '&' */
int   scr_hotkey( const char *text );           /* the access key as a capital, or 0 */

/* a single-line box, its inside filled with blanks; on a graphics
   screen the lines are one pixel wide */
void  scr_box( int top, int left, int bottom, int right, uint8_t attr );

/* the shadow a box casts: two columns to its right and a row below
   take "attr" and keep their characters.  A pane's pixels under a
   shadow are covered by plain cells of that colour. */
void  scr_shadow( int top, int left, int bottom, int right, uint8_t attr );

/* The cursor of the cells: CUR_HIDE, CUR_LINE or CUR_BLOCK at a cell.
   It is the hardware's on a text screen and drawn on a graphics one.
   Set it on every turn before scr_flush, as everything else is. */
void  scr_cursor( int row, int col, int kind );

uint32_t scr_cell( int row, int col );  /* what was drawn there: character in bits 0-7,
                                           attribute in bits 8-15; 0 off the screen */
void     scr_flush( void );             /* what changed, to the screen */
void     scr_repaint( void );           /* the next scr_flush sends everything again */

/* the colour (a pixel number, 0 to 15) of the spare lines under the
   last row of cells - 800x600 has eight */
void  scr_desk( int color );

/* ------------------------------------------------------------------ */
/* panes                                                               */
/* ------------------------------------------------------------------ */
#define MAX_PANES       8

/* A pane's paint function: draw what the pane shows inside the
   rectangle x0, y0 to x1, y1 (screen pixels, both corners included).
   The clip rectangle is set to exactly that before the call, so it may
   simply draw everything and let the clipping cut it; but drawing only
   what the rectangle touches is faster.  It is called from inside
   scr_flush and must not call scr_flush, key_get or anything that
   waits. */
typedef void (*PANEPAINT)( int pane, int x0, int y0, int x1, int y1 );

/* once, at the start: pane numbers are 0 to MAX_PANES - 1 and the
   application's to give out */
void  pane_define( int pane, PANEPAINT paint );

/* While drawing the screen, on EVERY turn (it is part of the picture,
   like scr_put): these cells are the pane's.  Cells drawn over them
   afterwards cover it.  A pane that is not placed in a turn is not on
   the screen.  When the rectangle is not the one of the turn before,
   the whole pane is painted again.  Returns 1; on a text screen it
   does nothing and returns 0. */
int   pane_place( int pane, int row, int col, int rows, int cols );

/* The pane's own contents changed inside this rectangle (screen
   pixels): its paint function will be called for it by the next
   scr_flush.  Cells lying over the pane are put back on top. */
void  pane_damage( int pane, int x0, int y0, int x1, int y1 );
void  pane_damage_all( int pane );

/* where the pane was last placed, in screen pixels; 0 if it is not on
   the screen */
int   pane_rect( int pane, int *x, int *y, int *w, int *h );

/* the pane showing at a pixel, or -1: a menu or a dialog over a pane
   hides it there */
int   pane_at( int x, int y );

/* the pointer's shape while it is over the pane: MP_ARROW or MP_IBEAM */
void  pane_pointer( int pane, int shape );

/* ------------------------------------------------------------------ */
/* drawing in pixels                                                   */
/*                                                                     */
/* All of these draw into the picture in memory, cut to the clip       */
/* rectangle, and note what they changed; scr_flush puts it on the     */
/* screen.  On a text screen they do nothing.  A colour is a pixel     */
/* number - see the palette at the top.                                */
/* ------------------------------------------------------------------ */

/* the clip rectangle: both corners included, cut to the screen.  A
   pane's paint function is given one already; code that draws outside
   scr_flush sets its own and ends with gfx_clip_all. */
void  gfx_clip( int x0, int y0, int x1, int y1 );
void  gfx_clip_all( void );
void  gfx_clip_get( int *x0, int *y0, int *x1, int *y1 );

/* the nearest pixel number for a colour written 0x00RRGGBB - of all
   256 on a 256-colour screen, of the first sixteen on a 16-colour one */
int   gfx_rgb( uint32_t rgb );

/* the colour of a pixel number, as 0x00RRGGBB */
uint32_t gfx_color_rgb( int color );

void  gfx_rect( int x, int y, int w, int h, int color );      /* filled */
void  gfx_hline( int x, int y, int len, int color );
void  gfx_vline( int x, int y, int len, int color );
void  gfx_outline( int x, int y, int w, int h, int color );   /* its four edges, one pixel */

/* A frame with depth, drawn just inside the rectangle; the middle is
   not touched.  The colours are the sixteen-colour ones: white for
   light, dark grey for shade, black.
     FR_RAISED   one pixel: light along the top and left, shade along
                 the bottom and right
     FR_SUNKEN   the other way round
     FR_BUTTON   a button standing out: a black outline, light inside
                 it at the top and left, two pixels of shade at the
                 bottom and right
     FR_PRESSED  a button held down: a black outline, shade inside it
                 at the top and left
     FR_FIELD    a well for text: shade and black at the top and left,
                 light grey and white at the bottom and right - two
                 pixels all round */
enum { FR_RAISED, FR_SUNKEN, FR_BUTTON, FR_PRESSED, FR_FIELD };
void  gfx_frame( int x, int y, int w, int h, int style );

/* "len" characters of the ROM's 8x16 font (code page 437, or whatever
   the video card holds) with their top left at x, y.  bg < 0 leaves
   the background as it is. */
void  gfx_text( int x, int y, const char *text, int len, int fg, int bg );

/* A 16x16 icon: sixteen strings of sixteen characters, a character a
   pixel.  '.' is nothing (what is there shows through), '#' is the
   colour "ink", and a letter is one of the sixteen colours whatever
   "ink" is:
       k black      b blue          g green        c cyan
       r red        m magenta       n brown        w light grey
       d dark grey  B bright blue   G bright green C bright cyan
       R bright red M bright magenta y yellow      W white
   "disabled" draws the shape embossed in grey instead - every pixel
   that is not '.', 'W' or 'w' in dark grey with white below and to the
   right of it - as a button that cannot be pressed shows its picture. */
void  gfx_icon( int x, int y, const char *const *rows, int ink, int disabled );

/* a block of pixel numbers, "stride" bytes from one row of "src" to
   the next */
void  gfx_blit( int x, int y, int w, int h, const uint8_t *src, int stride );

/* A block of COVERAGE - 0 is all background, 255 all foreground, as an
   antialiasing rasterizer makes for text - in colour "fg" over colour
   "bg".  On a 256-colour screen the shades between are mixed from the
   colour cube and the greys; on a 16-colour one a pixel is "fg" when
   its coverage is 128 or more.  bg < 0 mixes with what is in the
   picture already (slower). */
void  gfx_cover( int x, int y, int w, int h, const uint8_t *cov, int stride, int fg, int bg );

/* The coverage from which a pixel is "fg" on a 16-colour screen: 128
   until this is called, 1 to 255.  At 128 a thin upright stroke that
   falls across two pixels, half in each, vanishes; at 96 or so it is
   kept, two pixels wide, and everything is a little bolder.  (The
   environment variable PWCUT sets it too, for trying values.)  It does
   nothing on a 256-colour screen. */
void  gfx_cover_cut( int level );

/* Every pixel of the rectangle turned into its opposite (black and
   white change places, navy and yellow, a cube colour and the one
   across the cube); doing it twice puts everything back.  For a
   selection. */
void  gfx_invert( int x, int y, int w, int h );

/* For a renderer that writes rows itself: the address of pixel 0 of
   row y in the picture (gfx_width bytes; NULL off the screen or on a
   text screen), and afterwards a call to say which pixels were
   written.  THE CLIP RECTANGLE IS NOT APPLIED TO WHAT IS WRITTEN THIS
   WAY: stay inside the rectangle the paint function was given. */
uint8_t *gfx_row( int y );
void     gfx_dirty( int x0, int y0, int x1, int y1 );

/* ------------------------------------------------------------------ */
/* the caret                                                           */
/*                                                                     */
/* On a graphics screen a pane has a caret of its own: a bar two       */
/* pixels wide and "height" high with its top at x, y, which turns the */
/* pixels under it into their opposites, so it shows on any            */
/* background.  It is laid over the picture as the picture is copied   */
/* out and is never in it.  It shows only while every cell it touches  */
/* is a pane's - a dialog over it hides it - and it blinks while       */
/* key_get waits.  Set it on every turn before scr_flush (or call      */
/* caret_off).  On a text screen there is only scr_cursor.             */
/* ------------------------------------------------------------------ */
void  caret_at( int x, int y, int height );
void  caret_off( void );
void  caret_blink( int on );    /* 0: a steady caret, and key_get sleeps in the kernel
                                   instead of looking at the clock.  It starts on. */

/* ------------------------------------------------------------------ */
/* keys                                                                */
/*                                                                     */
/* A key is a number: a character as itself (20h to FFh - Shift is     */
/* already in which character it is), or one of the names below, with  */
/* K_SHIFT, K_CTRL and K_ALT added for the keys held with it:          */
/*                                                                     */
/*   K_C( 'S' )                  Ctrl+S        (always the capital)    */
/*   K_C( 'S' ) | K_SHIFT        Ctrl+Shift+S                          */
/*   K_A( 'F' )                  Alt+F         (letters and digits)    */
/*   K_F5 | K_SHIFT              Shift+F5                              */
/*   K_LEFT | K_CTRL | K_SHIFT   Ctrl+Shift+Left                       */
/*   K_TAB | K_SHIFT             Shift+Tab     (K_SHTAB)               */
/*   K_ENTER | K_CTRL            Ctrl+Enter                            */
/*   K_BS | K_CTRL               Ctrl+Backspace                        */
/*   ' ' | K_CTRL                Ctrl+Space                            */
/*                                                                     */
/* Ctrl+I is K_C( 'I' ) and Tab is K_TAB; Ctrl+H and Backspace, Ctrl+M */
/* and Enter, Ctrl+[ and Esc are told apart the same way (by the       */
/* key's scan code).  (key & ~K_MODS) is the key without what was held */
/* with it.                                                            */
/* ------------------------------------------------------------------ */
#define K_SHIFT         0x1000
#define K_CTRL          0x2000
#define K_ALT           0x4000
#define K_MODS          0x7000

#define K_EXT( scan )   (0x100 | (scan))        /* a key with no character */
#define K_C( ch )       (K_CTRL | (ch))
#define K_A( ch )       (K_ALT | (ch))

#define K_BS            0x08
#define K_TAB           0x09
#define K_ENTER         0x0D
#define K_ESC           0x1B
#define K_SHTAB         (K_TAB | K_SHIFT)
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
#define K_F11           K_EXT( 0x85 )
#define K_F12           K_EXT( 0x86 )
#define K_HOME          K_EXT( 0x47 )
#define K_UP            K_EXT( 0x48 )
#define K_PGUP          K_EXT( 0x49 )
#define K_LEFT          K_EXT( 0x4B )
#define K_CENTER        K_EXT( 0x4C )           /* the 5 of the number pad */
#define K_RIGHT         K_EXT( 0x4D )
#define K_END           K_EXT( 0x4F )
#define K_DOWN          K_EXT( 0x50 )
#define K_PGDN          K_EXT( 0x51 )
#define K_INS           K_EXT( 0x52 )
#define K_DEL           K_EXT( 0x53 )

/* what key_get returns that is not a key */
#define K_ALTTAP        0x8000  /* Alt pressed and let go on its own: the menu bar */
#define K_MOUSE         0x8001  /* the mouse: "mouse" says what it did */
#define K_TICK          0x8002  /* key_get_timed: nothing came in the time */
#define K_EOF           0x8003  /* test mode: the key file ran out */
#define K_DUMP          0x8004  /* test mode: write the cells to the log (scr_dump) */
#define K_SHOT          0x8005  /* test mode: write the picture as a .BMP (scr_shot) */

/* Wait for a key or the mouse.  The caret blinks meanwhile. */
int   key_get( void );

/* The same, but K_TICK when "hundredths" of a second pass with
   nothing: for scrolling while a button is held past the window's
   edge.  It waits by looking again and again, which costs the whole
   processor - use it only while something is going on.  In test mode
   there is no time: it returns the key file's next key. */
int   key_get_timed( int hundredths );

int   key_pending( void );          /* 1 = a key is waiting (a long job asks, to be stopped by Esc) */
int   key_shift( void );            /* the shift keys now: SH_SHIFT, SH_CTRL, SH_ALT */
int   key_alt_letter( int key );    /* 'A'..'Z' or '0'..'9' for Alt and that key alone, else 0 */
int   key_testing( void );          /* test mode is on */

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/*                                                                     */
/* key_get returns K_MOUSE and "mouse" says what happened.  x and y    */
/* are screen pixels on a graphics screen; on a text screen they are   */
/* the cell's (col * CELL_W, row * CELL_H), so code that works in      */
/* pixels works on both.  A press comes as ME_DOWN, or as ME_DOUBLE    */
/* when it is the second on one spot within four tenths of a second.   */
/* ------------------------------------------------------------------ */
enum { ME_DOWN,                 /* a button went down: "button" says which */
       ME_UP,                   /* ...came up */
       ME_DRAG,                 /* the pointer moved with a button held: "button" is those held */
       ME_DOUBLE,               /* a second press */
       ME_MOVE,                 /* the pointer moved with none held - only after mouse_moves( 1 ) */
       ME_WHEEL };              /* the wheel turned: "wheel" notches, away from the hand negative */

#define MBTN_LEFT       1
#define MBTN_RIGHT      2

typedef struct {
    int kind;                   /* ME_* */
    int button;                 /* MBTN_* */
    int x, y;                   /* where, in pixels */
    int row, col;               /* ...and the cell that is */
    int shift;                  /* the shift keys at the time: SH_* */
    int wheel;                  /* ME_WHEEL */
} MEVENT;

extern MEVENT mouse;            /* what the last K_MOUSE was */

int   mouse_present( void );
int   mouse_buttons( void );            /* the buttons held now: MBTN_* */
void  mouse_where( int *x, int *y );    /* the pointer now, in pixels */
void  mouse_moves( int on );            /* ME_MOVE wanted, or not (it starts off) */

/* The pointer on a graphics screen is drawn by this program.  Its
   shape is the pane's it is over (pane_pointer) and an arrow anywhere
   else; mouse_shape( MP_WAIT ) makes it an hourglass everywhere until
   mouse_shape( MP_ARROW ).  A text screen's pointer is the kernel's
   block. */
enum { MP_ARROW, MP_IBEAM, MP_WAIT };
void  mouse_shape( int shape );

/* ------------------------------------------------------------------ */
/* TEST MODE                                                           */
/*                                                                     */
/* With the environment variable PWTEST naming a log file and standard */
/* input taken from a file, keys come from the file and nothing waits. */
/* The file is the tree's key-file notation (TOOLS\MKEDKEYS.PL), a     */
/* little wider; MKPWKEYS.PL writes it from a script:                  */
/*                                                                     */
/*   a byte            that character, typed (01h-1Ah: Ctrl+A to       */
/*                     Ctrl+Z, but 08h, 09h and 0Dh are Backspace, Tab */
/*                     and Enter unless Ctrl is held by FF 04)         */
/*   00 ss             the key with scan code ss and no character      */
/*   FF nn             the shift keys held from now on: nn is SH_* (0  */
/*                     to 0Fh).  Alt held (08) and then let go (00)    */
/*                     with nothing between is K_ALTTAP                */
/*   FF FF             K_DUMP        FF FE    K_SHOT                   */
/*   FF FC aa ss       a key given whole: character aa, scan code ss   */
/*   FF FB dd          the wheel, dd notches (a signed byte)           */
/*   FE bb rr cc       the mouse on the middle of cell rr, cc with     */
/*                     buttons bb (1 left, 2 right) held               */
/*   FF FD bb xl xh yl yh   the mouse at pixel x, y (two bytes each,   */
/*                     low first) with buttons bb held; on a text      */
/*                     screen the pixel is turned into its cell        */
/*                                                                     */
/* The mouse records move the real driver's pointer and press its      */
/* buttons (INT 33h/04h, 21h/F0h AL=0Bh), so everything after that is  */
/* the path a hand takes.  Two clicks on one spot with no key between  */
/* are a double click.  At the end of the file key_get returns K_EOF   */
/* (leave without saving anything) - or, with PWHOLD in the            */
/* environment, the keyboard takes over.                               */
/*                                                                     */
/* scr_dump writes the cells to the log as text, a line a row (a       */
/* pane's cells as blanks), then a line with the mode and the cursor.  */
/* scr_shot writes the picture as an 8-bit .BMP beside the log, named  */
/* after it with a number (T.LOG gives T01.BMP, T02.BMP...), and says  */
/* so in the log; on a text screen it writes the line and no file.     */
/* key_service in UI.H does both for every loop.                       */
/* ------------------------------------------------------------------ */
void  scr_dump( void );
void  scr_shot( void );
void  scr_log( const char *text );      /* a line of the application's own into the log */

#endif
