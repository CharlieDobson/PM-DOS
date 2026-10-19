/*
 * WSKHTTP.C - WSKHTTP.EXE, a minimal 16-bit Windows program that uses
 * the STANDARD Windows Sockets 1.1 API (WSAStartup / gethostbyname /
 * socket / connect / send / recv / closesocket / WSACleanup) to fetch a
 * page over TCP.  It is linked against the stock winsock import library,
 * so it binds to WINSOCK.DLL by the standard names at run time - exactly
 * as any third-party Win16 winsock program would.  This proves
 * WINSOCK.DLL end to end: the app's ordinary socket calls reach the
 * parked PM-DOS kernel's TCP/IP stack through the PMDOS.386 bridge.
 *
 * It fetches http://example.com/ over the real internet through the
 * emulator's SLiRP NAT (the reliable outbound path that SOCKTEST and
 * WGET already proved), so gethostbyname and recv are exercised against
 * a real server.
 *
 * Headless proof is on port E9h: the VxD traces each socket op the DLL
 * makes, and the receive shows the reply bytes as text ("HTTP/1...").
 * WfW launches this from WIN.INI run= (winbuild -WinRun); it does its
 * work in WinMain and exits.
 *
 * Built: wcl -bt=windows -l=windows -ml wskhttp.c winsock.lib
 */
#define NOCOMM
#include <windows.h>
#include <winsock.h>

static void pump3( DWORD ms )
{
    DWORD start = GetTickCount();
    while ( GetTickCount() - start < ms ) {
        MSG m;
        while ( PeekMessage( &m, NULL, 0, 0, PM_REMOVE ) ) DispatchMessage( &m );
    }
}

int PASCAL WinMain( HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show )
{
    WSADATA wd;
    SOCKET  s;
    struct sockaddr_in sin;
    struct hostent FAR *he;
    char    buf[ 600 ];
    static char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";

    (void)hInst; (void)hPrev; (void)cmd; (void)show;

    if ( WSAStartup( 0x0101, &wd ) != 0 ) return 0;

    pump3( 3000 );                          /* let DHCP bind */

    he = gethostbyname( "example.com" );    /* resolve through the parked stack */
    if ( he == 0 ) { WSACleanup(); return 0; }

    s = socket( AF_INET, SOCK_STREAM, 0 );
    if ( s == INVALID_SOCKET ) { WSACleanup(); return 0; }

    sin.sin_family = AF_INET;
    sin.sin_port = htons( 80 );
    sin.sin_addr.s_addr = *(unsigned long FAR *)he->h_addr;   /* resolved IP */

    if ( connect( s, (struct sockaddr *)&sin, sizeof( sin ) ) == 0 ) {
        send( s, req, lstrlen( (LPSTR)req ), 0 );
        recv( s, buf, sizeof( buf ) - 1, 0 );    /* the VxD traces the reply */
    }

    closesocket( s );
    WSACleanup();
    return 0;
}
