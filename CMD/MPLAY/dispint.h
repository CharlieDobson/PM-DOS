/*
 * DISPINT.H - what SCR.C and GFX.C say to each other.  Not for the
 * application: DISP.H is the whole of what it may use.
 *
 * SCR.C keeps the cells, the panes, the keyboard and the mouse; GFX.C
 * keeps the picture and the hardware of a graphics screen.  The calls
 * here are the ones that cross: a cell to be painted, the cursor, the
 * caret and the pointer to be laid over the picture, a mode to be
 * entered and left.
 */
#ifndef DISPINT_H
#define DISPINT_H

#include "rt.h"
#include "disp.h"

typedef uint32_t CELL;          /* character 0-7, attribute 8-15, CELLF_* above */

#define CELLF_PANE      0x02000000UL    /* the cell is a pane's: its number is the character */

#define MAX_ROWS        64      /* 1280x1024 in 8x16 cells */
#define MAX_COLS        160

/* SYS_D32.C has it and SYS.H has not: how far a mouse wheel turned
   since last asked, 0 for a driver without one */
int   sys_mouse_wheel( void );

/* the graphics modes this machine offers, into "list": how many */
int   gfx_probe( DISPMODE *list, int max );

/* Into a graphics mode: DE_NONE when it is on the screen, the picture
   empty; else why not, and then no picture exists and the screen may
   be in any mode at all - the caller sets another, or calls gfx_leave. */
int   gfx_enter( const DISPMODE *mode );

/* the text screen there was before the first gfx_enter, and the
   picture's memory given back; harmless when no mode was entered */
void  gfx_leave( void );

void  gfx_cell( int row, int col, CELL cell );      /* a cell into the picture */
void  gfx_strip( int color );                       /* the spare lines under the last row */
void  gfx_flush( void );                            /* the picture's changes to the screen */
void  gfx_cursor( int row, int col, int kind, int color );  /* the cells' cursor */
void  gfx_caret( int x, int y, int height, int show );      /* a pane's caret */
void  gfx_pointer( int x, int y, int shape, int show );
int   gfx_snapshot( const char *path );             /* the picture as a .BMP file: 1 = written */

#endif
