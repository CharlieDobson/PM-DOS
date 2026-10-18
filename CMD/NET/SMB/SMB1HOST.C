/* ==========================================================================
 * SMB1HOST.C - prove SMB1.C on the host over Winsock before the VM.
 *   wcl386 -q -bt=nt -fe=smb1host.exe smb1host.c smb1.c smb2.c smbcrypt.c wsock32.lib
 *   smb1host.exe [ip] [port] [share] [user] [pass] [domain]
 * Defaults: 127.0.0.1 8445 PMSHARE pmdostest PmdosSmb!2026 WORKGROUP
 * ========================================================================== */
#include "smb1.h"
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
static void on_entry( void *user, const smb2_dirent *e )
{
    (void)user;
    printf( "    %-24s attr=%02lX size=%lu\n", e->name, e->attr & 0xFF, e->size_lo );
    g_count++;
}

int main( int argc, char **argv )
{
    WSADATA wsa;
    struct sockaddr_in sa;
    smb1_client cl;
    const char *ip     = ( argc > 1 ) ? argv[1] : "127.0.0.1";
    int         port   = ( argc > 2 ) ? atoi( argv[2] ) : 8445;
    const char *share  = ( argc > 3 ) ? argv[3] : "PMSHARE";
    const char *user   = ( argc > 4 ) ? argv[4] : "pmdostest";
    const char *pass   = ( argc > 5 ) ? argv[5] : "PmdosSmb!2026";
    const char *domain = ( argc > 6 ) ? argv[6] : "WORKGROUP";
    int rc;

    if ( WSAStartup( 0x0101, &wsa ) != 0 ) { printf( "WSAStartup failed\n" ); return 1; }
    g_sock = socket( AF_INET, SOCK_STREAM, 0 );
    memset( &sa, 0, sizeof( sa ) );
    sa.sin_family = AF_INET;
    sa.sin_port = htons( (unsigned short)port );
    sa.sin_addr.s_addr = inet_addr( ip );
    if ( connect( g_sock, (struct sockaddr *)&sa, sizeof( sa ) ) != 0 ) {
        printf( "connect to %s:%d failed (%d)\n", ip, port, WSAGetLastError() );
        return 1;
    }
    printf( "connected to %s:%d; user=%s domain=%s share=%s\n", ip, port, user, domain, share );

    rc = smb1_connect( &cl, io_send, io_recv, (void *)0, (unsigned long)time( (time_t *)0 ),
                       ip, share, user, pass, domain );
    if ( rc != 0 ) {
        printf( "FAIL at stage '%s', NTSTATUS 0x%08lX\n", smb1_stage( &cl ), smb1_status( &cl ) );
        closesocket( g_sock );
        return 1;
    }
    printf( "connected: TID=%u UID=%u caps=0x%08lX\n", cl.tree_id, cl.uid, cl.capabilities );

    printf( "listing \\\\%s\\%s :\n", ip, share );
    g_count = 0;
    if ( smb1_list_dir( &cl, "", "*", on_entry, (void *)0 ) != 0 ) {
        printf( "list FAIL at '%s' 0x%08lX\n", smb1_stage( &cl ), smb1_status( &cl ) );
    } else {
        printf( "%d entries.\n", g_count );
    }

    /* read HELLO.TXT */
    {
        smb2_fh fh;
        unsigned char data[512];
        int n;
        if ( smb1_open_disp( &cl, "HELLO.TXT", 0, 1, &fh ) == 0 ) {
            n = smb1_read( &cl, &fh, 0, 0, data, sizeof( data ) - 1 );
            if ( n >= 0 ) { data[n] = 0; printf( "read HELLO.TXT (%d bytes, size=%lu): %s", n, fh.size_lo, (char *)data ); }
            smb1_close_file( &cl, &fh );
        } else printf( "open HELLO.TXT FAILED: '%s' 0x%08lX\n", smb1_stage( &cl ), smb1_status( &cl ) );

        /* write + read back a temp file */
        if ( smb1_open_disp( &cl, "PMSMB1.TMP", 1, 5, &fh ) == 0 ) {
            const char *msg = "written by PM-DOS SMB1\r\n";
            int wn = smb1_write( &cl, &fh, 0, 0, (const unsigned char *)msg, (int)strlen( msg ) );
            printf( "wrote PMSMB1.TMP: %d bytes\n", wn );
            smb1_close_file( &cl, &fh );
            if ( smb1_open_disp( &cl, "PMSMB1.TMP", 0, 1, &fh ) == 0 ) {
                n = smb1_read( &cl, &fh, 0, 0, data, sizeof( data ) - 1 );
                if ( n >= 0 ) { data[n] = 0; printf( "readback (%d bytes): %s", n, (char *)data ); }
                smb1_close_file( &cl, &fh );
            }
            if ( smb1_delete( &cl, "PMSMB1.TMP" ) == 0 ) printf( "deleted PMSMB1.TMP\n" );
            else printf( "delete FAILED 0x%08lX\n", smb1_status( &cl ) );
        } else printf( "create PMSMB1.TMP FAILED: '%s' 0x%08lX\n", smb1_stage( &cl ), smb1_status( &cl ) );

        /* mkdir / rmdir */
        if ( smb1_mkdir( &cl, "PMSMB1D" ) == 0 ) {
            printf( "mkdir PMSMB1D ok\n" );
            if ( smb1_rmdir( &cl, "PMSMB1D" ) == 0 ) printf( "rmdir PMSMB1D ok\n" );
            else printf( "rmdir FAILED 0x%08lX\n", smb1_status( &cl ) );
        } else printf( "mkdir FAILED 0x%08lX\n", smb1_status( &cl ) );
    }

    printf( "OK\n" );
    smb1_disconnect( &cl );
    closesocket( g_sock );
    WSACleanup();
    return 0;
}
