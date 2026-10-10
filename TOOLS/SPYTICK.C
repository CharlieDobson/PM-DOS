/*====================================================================
 * SPYTICK.C - SPYTICK.EXE, where is the processor?  A Windows 3.1
 *             program that hooks the timer interrupt in protected mode
 *             and, every ninth tick, writes the interrupted CS:IP to
 *             port E9h - the Bochs debug port that DOSBox-X logs when
 *             BUILD rme9's trace is on.
 *
 * For a program that starts and never comes back: run this first (it
 * stays, with a hidden window), then the program; the log then says
 * where the program is spinning, about twice a second, as PM selector
 * and offset, with the flags and the low word of the frame's SS:SP.
 * DOSBox-X only - 86Box has no port E9h.
 *
 * Built with Open Watcom:  wcl -zq -bt=windows -l=windows spytick.c
 *===================================================================*/
#include <windows.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>

static void ( __interrupt __far *old08 )( void );
static unsigned nth;

static void putHex( unsigned v )
{
    static const char digits[] = "0123456789ABCDEF";
    outp( 0xE9, digits[( v >> 12 ) & 15] );
    outp( 0xE9, digits[( v >> 8 ) & 15] );
    outp( 0xE9, digits[( v >> 4 ) & 15] );
    outp( 0xE9, digits[v & 15] );
}

static void __interrupt __far spy08( union INTPACK r )
{
    if ( ++nth >= 9 ) {
        nth = 0;
        outp( 0xE9, 'S' ); outp( 0xE9, 'P' ); outp( 0xE9, 'Y' ); outp( 0xE9, ' ' );
        putHex( r.x.cs ); outp( 0xE9, ':' ); putHex( r.x.ip );
        outp( 0xE9, ' ' ); outp( 0xE9, 'f' ); outp( 0xE9, '=' ); putHex( r.x.flags );
        outp( 0xE9, ' ' ); outp( 0xE9, 'a' ); outp( 0xE9, 'x' ); outp( 0xE9, '=' ); putHex( r.x.ax );
        outp( 0xE9, ' ' ); outp( 0xE9, 'b' ); outp( 0xE9, 'x' ); outp( 0xE9, '=' ); putHex( r.x.bx );
        outp( 0xE9, ' ' ); outp( 0xE9, 'c' ); outp( 0xE9, 'x' ); outp( 0xE9, '=' ); putHex( r.x.cx );
        outp( 0xE9, ' ' ); outp( 0xE9, 'd' ); outp( 0xE9, 'x' ); outp( 0xE9, '=' ); putHex( r.x.dx );
        outp( 0xE9, ' ' ); outp( 0xE9, 'd' ); outp( 0xE9, 's' ); outp( 0xE9, '=' ); putHex( r.x.ds );
        {
            DWORD base = GetSelectorBase( r.x.cs );     /* CS's linear address, to name the module */
            outp( 0xE9, ' ' ); outp( 0xE9, '@' ); putHex( (unsigned)( base >> 16 ) ); putHex( (unsigned)base );
        }
        outp( 0xE9, '\n' );
    }
    _chain_intr( old08 );
}

LRESULT CALLBACK __export SpyWndProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    if ( msg == WM_CLOSE ) return 0;    /* it stays: WINSWEEP's close is refused */
    if ( msg == WM_DESTROY ) {
        _dos_setvect( 0x08, old08 );
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProc( hwnd, msg, wParam, lParam );
}

int PASCAL WinMain( HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show )
{
    WNDCLASS wc;
    HWND     hwnd;
    MSG      msg;

    (void)cmdLine; (void)show;
    if ( !prev ) {
        wc.style = 0;
        wc.lpfnWndProc = SpyWndProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = inst;
        wc.hIcon = NULL;
        wc.hCursor = NULL;
        wc.hbrBackground = NULL;
        wc.lpszMenuName = NULL;
        wc.lpszClassName = "SpyTick";
        RegisterClass( &wc );
    }
    hwnd = CreateWindow( "SpyTick", "SpyTick", WS_OVERLAPPED, 0, 0, 10, 10, NULL, NULL, inst, NULL );
    old08 = _dos_getvect( 0x08 );
    _dos_setvect( 0x08, spy08 );
    outp( 0xE9, 'S' ); outp( 0xE9, 'P' ); outp( 0xE9, 'Y' ); outp( 0xE9, ' ' );
    outp( 0xE9, 'o' ); outp( 0xE9, 'n' ); outp( 0xE9, '\n' );
    while ( GetMessage( &msg, NULL, 0, 0 ) ) {
        TranslateMessage( &msg );
        DispatchMessage( &msg );
    }
    (void)hwnd;
    return 0;
}
