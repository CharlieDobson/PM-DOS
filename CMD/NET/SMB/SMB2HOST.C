/* ==========================================================================
 * SMB2HOST.C - prove SMB2.C on the host over Winsock before the VM.
 *   wcl386 -q -bt=nt -fe=smb2host.exe smb2host.c smb2.c smbcrypt.c wsock32.lib
 *   smb2host.exe [ip] [share] [user] [pass] [domain]
 * Defaults: 127.0.0.1 PMSHARE pmdostest PmdosSmb!2026 <computername>
 * ========================================================================== */
#include "smb2.h"
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static SOCKET g_sock;

static int io_send( void *ctx, const unsigned char *buf, int len )
{
    (void)ctx;
    return send( g_sock, (const char *)buf, len, 0 );
}
static int io_recv( void *ctx, unsigned char *buf, int len )
{
    (void)ctx;
    return recv( g_sock, (char *)buf, len, 0 );
}

static int g_count;
static void on_entry( void *user, const char *name, int isdir, unsigned long size )
{
    (void)user;
    printf( "    %-20s %s %lu\n", name, isdir ? "<DIR>" : "     ", size );
    g_count++;
}

int main( int argc, char **argv )
{
    WSADATA wsa;
    struct sockaddr_in sa;
    smb2_client cl;
    const char *ip     = ( argc > 1 ) ? argv[1] : "127.0.0.1";
    const char *share  = ( argc > 2 ) ? argv[2] : "PMSHARE";
    const char *user   = ( argc > 3 ) ? argv[3] : "pmdostest";
    const char *pass   = ( argc > 4 ) ? argv[4] : "PmdosSmb!2026";
    char domain[64];
    int rc;

    if ( argc > 5 ) { strcpy( domain, argv[5] ); }
    else { DWORD n = sizeof( domain ); if ( !GetComputerName( domain, &n ) ) strcpy( domain, "." ); }

    if ( WSAStartup( 0x0101, &wsa ) != 0 ) { printf( "WSAStartup failed\n" ); return 1; }
    g_sock = socket( AF_INET, SOCK_STREAM, 0 );
    if ( g_sock == INVALID_SOCKET ) { printf( "socket failed\n" ); return 1; }
    memset( &sa, 0, sizeof( sa ) );
    sa.sin_family = AF_INET;
    sa.sin_port = htons( 445 );
    sa.sin_addr.s_addr = inet_addr( ip );
    if ( connect( g_sock, (struct sockaddr *)&sa, sizeof( sa ) ) != 0 ) {
        printf( "connect to %s:445 failed (%d)\n", ip, WSAGetLastError() );
        return 1;
    }
    printf( "connected to %s:445; user=%s domain=%s share=%s\n", ip, user, domain, share );

    rc = smb2_connect( &cl, io_send, io_recv, (void *)0, (unsigned long)time( (time_t *)0 ),
                       ip, share, user, pass, domain );
    if ( rc != 0 ) {
        printf( "FAIL at stage '%s', NTSTATUS 0x%08lX\n", smb2_stage( &cl ), smb2_status( &cl ) );
        closesocket( g_sock );
        return 1;
    }
    printf( "negotiated dialect 0x%04X, signing_required=%d\n",
            smb2_dialect( &cl ), cl.signing_required );
    printf( "tree connected; listing \\\\%s\\%s :\n", ip, share );

    if ( smb2_list( &cl, "*", on_entry, (void *)0 ) != 0 ) {
        printf( "list FAIL at stage '%s', NTSTATUS 0x%08lX\n", smb2_stage( &cl ), smb2_status( &cl ) );
        smb2_disconnect( &cl );
        closesocket( g_sock );
        return 1;
    }
    printf( "%d entries listed.\n", g_count );

    /* file I/O: read HELLO.TXT, then write + read back a temp file */
    {
        smb2_fh fh;
        unsigned char data[256];
        int n;

        if ( smb2_open_file( &cl, "HELLO.TXT", 0, 0, &fh ) == 0 ) {
            n = smb2_read( &cl, &fh, 0, 0, data, sizeof( data ) - 1 );
            if ( n >= 0 ) { data[n] = 0; printf( "read HELLO.TXT (%d bytes, size=%lu): %s", n, fh.size_lo, (char *)data ); }
            else printf( "read HELLO.TXT FAILED\n" );
            smb2_close_file( &cl, &fh );
        } else printf( "open HELLO.TXT FAILED: stage %s 0x%08lX\n", smb2_stage( &cl ), smb2_status( &cl ) );

        if ( smb2_open_file( &cl, "PMDOS.TMP", 1, 1, &fh ) == 0 ) {
            const char *wmsg = "written by PM-DOS SMB2\r\n";
            int wn = smb2_write( &cl, &fh, 0, 0, (const unsigned char *)wmsg, (int)strlen( wmsg ) );
            printf( "wrote PMDOS.TMP: %d bytes\n", wn );
            smb2_close_file( &cl, &fh );
            if ( smb2_open_file( &cl, "PMDOS.TMP", 0, 0, &fh ) == 0 ) {
                n = smb2_read( &cl, &fh, 0, 0, data, sizeof( data ) - 1 );
                if ( n >= 0 ) { data[n] = 0; printf( "readback PMDOS.TMP (%d bytes): %s", n, (char *)data ); }
                smb2_close_file( &cl, &fh );
            }
        } else printf( "open PMDOS.TMP (write) FAILED: stage %s 0x%08lX\n", smb2_stage( &cl ), smb2_status( &cl ) );
    }
    printf( "OK\n" );

    smb2_disconnect( &cl );
    closesocket( g_sock );
    WSACleanup();
    return 0;
}
