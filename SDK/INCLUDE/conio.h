/*
 * CONIO.H - the console, directly
 *
 * These read the keyboard and write the screen whatever standard input
 * and output have been redirected to.  getch() gives an extended key
 * (F1, the arrows) as two calls: 0 first, then the scan code.
 *
 * Rows and columns for gotoxy(), wherex() and wherey() start at 1, as
 * in Borland's library.
 */
#ifndef _CONIO_H
#define _CONIO_H

#include <_pmdefs.h>

_PM_BEGIN_C

int     getch( void );                  /* a key, no echo */
int     getche( void );                 /* a key, echoed */
int     kbhit( void );                  /* is a key waiting? */
int     ungetch( int c );
int     putch( int c );
int     cputs( const char *s );
int     cprintf( const char *fmt, ... ) _PM_PRINTF( 1, 2 );

void    clrscr( void );
void    gotoxy( int x, int y );
int     wherex( void );
int     wherey( void );

/* the screen's size, in characters */
int     screenwidth( void );
int     screenheight( void );

_PM_END_C

#endif
