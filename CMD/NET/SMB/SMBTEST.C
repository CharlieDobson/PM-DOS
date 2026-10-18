/*
 * SMBTEST.C - SMBTEST.EXE: connect to an SMB2/3 server and list a share,
 * to prove the SMB2 engine (SMB2.C) on PM-DOS the way SOCKTEST proved the
 * stack.  Built over CMD\NET\SOCKLIB.C behind CMD\NET\NETSTART.ASM.
 *
 *   SMBTEST server share user pass [domain]
 *     e.g.  SMBTEST 10.0.2.2 PMSHARE pmdostest PmdosSmb!2026 KELDOR
 */
#include "socklib.h"
#include "smb2.h"

static int smb_send( void *ctx, const unsigned char *buf, int len )
{
    return sl_send( (int)ctx, buf, (u32)len, 2000 );
}
static int smb_recv( void *ctx, unsigned char *buf, int len )
{
    return sl_recvwait( (int)ctx, buf, (u32)len, 2000 );
}

static smb2_client g_cl;

static void put_hex16( u32 value )
{
    static const char hex[] = "0123456789ABCDEF";
    sl_putc( hex[( value >> 12 ) & 0xF] );
    sl_putc( hex[( value >> 8 ) & 0xF] );
    sl_putc( hex[( value >> 4 ) & 0xF] );
    sl_putc( hex[value & 0xF] );
}
static void put_hex32( u32 value )
{
    put_hex16( value >> 16 );
    put_hex16( value & 0xFFFF );
}

static void on_entry( void *user, const char *name, int isdir, unsigned long size )
{
    (void)user;
    sl_puts( "  " );
    sl_puts( name );
    if ( isdir ) {
        sl_puts( "  <DIR>\r\n" );
    } else {
        sl_puts( "  " );
        sl_putu( size );
        sl_puts( "\r\n" );
    }
}

void __cdecl dos_entry( void )
{
    char       **argv;
    int          argc;
    const char  *server = NULL, *share = NULL, *user = NULL, *pass = NULL, *domain = "";
    u32          ip;
    int          handle, rc;

    argc = sl_args( &argv );
    if ( argc >= 1 ) server = argv[0];
    if ( argc >= 2 ) share  = argv[1];
    if ( argc >= 3 ) user   = argv[2];
    if ( argc >= 4 ) pass   = argv[3];
    if ( argc >= 5 ) domain = argv[4];
    if ( server == NULL || share == NULL || user == NULL || pass == NULL ) {
        sl_puts( "usage: SMBTEST server share user pass [domain]\r\n" );
        sl_flush();
        return;
    }

    if ( !sl_up( 800 ) ) { sl_puts( "SMBTEST: the network is not up.\r\n" ); sl_flush(); return; }
    ip = sl_resolve( server, 600 );
    if ( ip == 0 ) { sl_puts( "SMBTEST: cannot find " ); sl_puts( server ); sl_puts( "\r\n" ); sl_flush(); return; }

    handle = sl_open( SOCK_STREAM );
    if ( handle == 0 ) { sl_puts( "SMBTEST: no socket.\r\n" ); sl_flush(); return; }
    if ( !sl_connect( handle, ip, 445, 3000 ) ) {
        sl_puts( "SMBTEST: cannot connect to " ); sl_puts( server ); sl_puts( " port 445.\r\n" );
        sl_sclose( handle ); sl_flush(); return;
    }

    rc = smb2_connect( &g_cl, smb_send, smb_recv, (void *)handle, sl_now() ^ 0x9E3779B9UL,
                       server, share, user, pass, domain );
    if ( rc != 0 ) {
        sl_puts( "SMBTEST: failed at " ); sl_puts( smb2_stage( &g_cl ) );
        sl_puts( ", NTSTATUS 0x" ); put_hex32( smb2_status( &g_cl ) ); sl_puts( "\r\n" );
        sl_sclose( handle ); sl_flush(); return;
    }

    sl_puts( "connected: dialect 0x" ); put_hex16( (u32)smb2_dialect( &g_cl ) );
    sl_puts( ", signing " ); sl_puts( g_cl.signing_mode == 3 ? "AES-CMAC" : "HMAC-SHA256" );
    sl_puts( "\r\nlisting \\\\" ); sl_puts( server ); sl_puts( "\\" ); sl_puts( share ); sl_puts( ":\r\n" );

    if ( smb2_list( &g_cl, "*", on_entry, NULL ) != 0 ) {
        sl_puts( "SMBTEST: list failed at " ); sl_puts( smb2_stage( &g_cl ) );
        sl_puts( ", NTSTATUS 0x" ); put_hex32( smb2_status( &g_cl ) ); sl_puts( "\r\n" );
    } else {
        sl_puts( "OK\r\n" );
    }

    smb2_disconnect( &g_cl );
    sl_sclose( handle );
    sl_flush();
}
