/*
 * VIEW.H - the screen and the viewer: what VIDEO.C, VIEW.C and MAIN.C
 * have between them.
 */
#ifndef VIEW_H
#define VIEW_H

#include "gfx.h"

#define VS_TEXT     0
#define VS_VGA      1           /* 640 by 480 in sixteen greys */
#define VS_VBE      2           /* 640 by 480 in colour, through VBE's frame buffer */

#define VGA_BAND    48          /* lines of the VGA screen written at one go */

typedef struct {
    int kind;                   /* VS_* */
    int width, height;
    int bits;                   /* a pixel */
    int canvas_bpp;             /* bytes a pixel of the canvas that suits it */
} VSCREEN;

extern VSCREEN screen;

int   video_probe( void );
int   video_set( int kind );
void  video_text( void );
void  video_show( const CANVAS *cv, int from_x, int from_y, int top, int bottom, int back );

/* what MAIN.C asks for */
#define WANT_BEST   0           /* colour if VBE 2.0 is there, else greys, else text */
#define WANT_TEXT   1
#define WANT_VGA    2
#define WANT_VBE    3

void  view_run( const char *name, int first_page, int want );
const char *why_text( int why );

#endif
