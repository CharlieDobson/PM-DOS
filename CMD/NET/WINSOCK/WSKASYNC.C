/*
 * WSKASYNC.C - WSKASYNC.EXE, a 16-bit Windows program that uses the
 * MESSAGE-DRIVEN Windows Sockets model (WSAAsyncSelect) to fetch a page,
 * doing its socket work only in response to the FD_* notification
 * messages WINSOCK.DLL posts - never polling.  This proves WINSOCK.DLL's
 * asynchronous notifications (Stage C, C3): FD_CONNECT drives the send,
 * FD_READ drives the recv, FD_CLOSE ends it.
 *
 * Linked against the stock winsock import library, launched by WfW from
 * WIN.INI run=.  It fetches http://example.com/ over SLiRP NAT.  The
 * proof is on port E9h: the app traces each FD_* event it is notified
 * of, interleaved with the VxD's socket-op traces (and the recv bytes),
 * so a healthy run shows FD_CONNECT -> send, FD_READ -> recv HTTP/1...,
 * FD_CLOSE -> close - all message-driven.
 *
 * Built: wcl -bt=windows -l=windows -ml wskasync.c winsock.lib
 */
#define NOCOMM
#include <windows.h>
#include <winsock.h>

#define WM_SOCK  (WM_USER + 1)

/* a byte to port E9h (passes through from a PM app, as WSKPROBE showed) */
extern void e9c( int ch );
#pragma aux e9c = "mov dx,0E9h" "out dx,al" parm [ax] modify [dx];
static void e9s( const char *s ) { while ( *s ) e9c( *s++ ); }
static void e9hex( unsigned long v )
{
    int i; char hx[ 9 ];
    for ( i = 7; i >= 0; i-- ) { int d = (int)( v & 0xF ); hx[ i ] = (char)( d < 10 ? '0'+d : 'A'+d-10 ); v >>= 4; }
    hx[ 8 ] = 0; e9s( hx );
}

static SOCKET g_s = INVALID_SOCKET;
static char g_req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";

LRESULT CALLBACK WndProc( HWND hw, UINT msg, WPARAM wp, LPARAM lp )
{
    if ( msg == WM_SOCK ) {
        int ev = WSAGETSELECTEVENT( lp );
        if ( ev == FD_CONNECT ) {
            e9s( "\r\nASYNC: FD_CONNECT err=" ); e9hex( WSAGETSELECTERROR( lp ) );
            send( g_s, g_req, lstrlen( (LPSTR)g_req ), 0 );
        } else if ( ev == FD_READ ) {
            char buf[ 600 ];
            int n = recv( g_s, buf, sizeof( buf ) - 1, 0 );   /* VxD traces the bytes */
            e9s( "\r\nASYNC: FD_READ n=" ); e9hex( (unsigned long)(unsigned)n );
        } else if ( ev == FD_CLOSE ) {
            e9s( "\r\nASYNC: FD_CLOSE" );
            closesocket( g_s );
            PostQuitMessage( 0 );
        }
        return 0;
    }
    return DefWindowProc( hw, msg, wp, lp );
}

int PASCAL WinMain( HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show )
{
    WSADATA wd;
    WNDCLASS wc;
    HWND hw;
    MSG  msg;
    struct sockaddr_in sin;
    struct hostent FAR *he;
    DWORD start;

    (void)hPrev; (void)cmd; (void)show;

    if ( WSAStartup( 0x0101, &wd ) != 0 ) return 0;

    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInst;
    wc.hIcon = 0;
    wc.hCursor = 0;
    wc.hbrBackground = (HBRUSH)( COLOR_WINDOW + 1 );
    wc.lpszMenuName = 0;
    wc.lpszClassName = "WskAsync";
    RegisterClass( &wc );
    hw = CreateWindow( "WskAsync", "WskAsync", WS_OVERLAPPEDWINDOW,
                       0, 0, 120, 80, 0, 0, hInst, 0 );
    if ( !hw ) { WSACleanup(); return 0; }

    /* let DHCP bind (pump messages while waiting) */
    start = GetTickCount();
    while ( GetTickCount() - start < 3000 ) {
        MSG m;
        while ( PeekMessage( &m, NULL, 0, 0, PM_REMOVE ) ) { TranslateMessage( &m ); DispatchMessage( &m ); }
    }

    he = gethostbyname( "example.com" );
    if ( he == 0 ) { WSACleanup(); return 0; }

    g_s = socket( AF_INET, SOCK_STREAM, 0 );
    if ( g_s == INVALID_SOCKET ) { WSACleanup(); return 0; }

    /* ask for the events, THEN connect - FD_CONNECT will tell us when it
       completes, FD_READ when data arrives, FD_CLOSE when the peer ends */
    WSAAsyncSelect( g_s, hw, WM_SOCK, FD_CONNECT | FD_READ | FD_CLOSE );

    sin.sin_family = AF_INET;
    sin.sin_port = htons( 80 );
    sin.sin_addr.s_addr = *(unsigned long FAR *)he->h_addr;
    e9s( "\r\nASYNC: connecting" );
    connect( g_s, (struct sockaddr *)&sin, sizeof( sin ) );  /* async: WOULDBLOCK */

    /* the message loop - all socket work happens in WndProc on the FD_* messages */
    while ( GetMessage( &msg, NULL, 0, 0 ) ) {
        TranslateMessage( &msg );
        DispatchMessage( &msg );
    }

    WSACleanup();
    return 0;
}
