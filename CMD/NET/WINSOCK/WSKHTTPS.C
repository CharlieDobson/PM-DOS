/*
 * WSKHTTPS.C - WSKHTTPS.EXE, transparent HTTPS through WINSOCK.DLL
 *
 * This is WSKHTTP with one change: it connects to port 443 instead of
 * 80.  Nothing else differs - it uses the ordinary Windows Sockets 1.1
 * calls (WSAStartup / gethostbyname / socket / connect / send / recv)
 * and sends and reads cleartext.  WINSOCK.DLL's port map opens a TLS
 * socket for port 443, so the parked PM-DOS stack runs the whole TLS 1.2
 * handshake and record crypto and the program never knows: it speaks
 * HTTPS without a line of TLS code.  This proves Stage C, C4 (the
 * HTTP/TLS proxy and port map) end to end over the bridge.
 *
 * It fetches https://example.com/ over the emulator's SLiRP NAT.  Proof
 * on port E9h: the VxD traces each socket op the DLL makes - and because
 * the DLL sends and receives PLAINTEXT across the bridge (the crypto is
 * kernel-side), the receive trace shows the decrypted "HTTP/1.1 200 OK".
 *
 * Built: wcl -bt=windows -l=windows -ml wskhttps.c winsock.lib
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
    sin.sin_port = htons( 443 );            /* the port map opens this as TLS */
    sin.sin_addr.s_addr = *(unsigned long FAR *)he->h_addr;

    if ( connect( s, (struct sockaddr *)&sin, sizeof( sin ) ) == 0 ) {
        send( s, req, lstrlen( (LPSTR)req ), 0 );
        recv( s, buf, sizeof( buf ) - 1, 0 );    /* the VxD traces the decrypted reply */
    }

    closesocket( s );
    WSACleanup();
    return 0;
}
