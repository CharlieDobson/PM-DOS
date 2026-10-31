/*
 * CONIO.C - the console, directly (CONIO.H)
 *
 * Keys: 07h (no echo, no Ctrl+C check) and 0Bh.  Output goes to CON,
 * opened once, so it reaches the screen with standard output
 * redirected.  The cursor and the screen's size are the kernel's
 * console calls, 21h/F0h: AL = 0 clears, 1 gets the cursor into DH
 * (row) and DL (column), 2 sets it from them, 3 gives the size in DH
 * and DL.  Those count from 0; CONIO.H counts from 1.
 */
#include "pmlib.h"
#include "fmt.h"
#include <conio.h>
#include <io.h>
#include <string.h>

static int pushed = -1;                 /* ungetch()'s key */
static int con = -1;                    /* the handle on CON */

static int con_handle( void )
{
    if ( con < 0 ) {
        con = open( "CON", O_WRONLY );
        if ( con < 0 ) {
            con = 1;                    /* no CON: standard output will do */
        }
    }
    return con;
}

static int con_write( void *ctx, const char *s, size_t n )
{
    (void)ctx;
    return write( con_handle(), s, (unsigned)n ) < 0 ? -1 : 0;
}

int getch( void )
{
    pm_regs regs = { 0 };
    int c;

    if ( pushed >= 0 ) {
        c = pushed;
        pushed = -1;
        return c;
    }
    __pm_dos( &regs, 0x0700 );
    return (int)(regs.eax & 0xFF);
}

int getche( void )
{
    int c = getch();

    if ( c != 0 ) {
        putch( c );
    }
    return c;
}

int kbhit( void )
{
    pm_regs regs = { 0 };

    if ( pushed >= 0 ) {
        return 1;
    }
    __pm_dos( &regs, 0x0B00 );
    return (regs.eax & 0xFF) != 0;
}

int ungetch( int c )
{
    if ( pushed >= 0 || c < 0 ) {
        return -1;
    }
    pushed = c & 0xFF;
    return pushed;
}

int putch( int c )
{
    char ch = (char)c;

    return con_write( NULL, &ch, 1 ) < 0 ? -1 : (unsigned char)c;
}

int cputs( const char *s )
{
    return con_write( NULL, s, strlen( s ) );
}

int cprintf( const char *fmt, ... )
{
    va_list args;
    int n;

    va_start( args, fmt );
    n = __pm_vformat( con_write, NULL, fmt, args );
    va_end( args );
    return n;
}

void clrscr( void )
{
    pm_regs regs = { 0 };

    __pm_dos( &regs, 0xF000 );
}

void gotoxy( int x, int y )
{
    pm_regs regs = { 0 };

    regs.edx = ((uint32_t)(y - 1) << 8) | (uint32_t)((x - 1) & 0xFF);
    __pm_dos( &regs, 0xF002 );
}

static uint32_t cursor( void )
{
    pm_regs regs = { 0 };

    __pm_dos( &regs, 0xF001 );
    return regs.edx;
}

int wherex( void )
{
    return (int)(cursor() & 0xFF) + 1;
}

int wherey( void )
{
    return (int)((cursor() >> 8) & 0xFF) + 1;
}

static uint32_t size( void )
{
    pm_regs regs = { 0 };

    __pm_dos( &regs, 0xF003 );
    return regs.edx;
}

int screenwidth( void )
{
    return (int)(size() & 0xFF);
}

int screenheight( void )
{
    return (int)((size() >> 8) & 0xFF);
}
