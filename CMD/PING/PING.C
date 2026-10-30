/*
 * PING.C - PING.EXE, see whether another computer answers
 *
 * Looks the name up (SOCK_RESOLVE: NetBIOS for a short name, then DNS)
 * and sends ICMP echoes to it, one a second (SOCK_PING).
 *
 *   PING [-n count] [-w timeout] host
 *     -n count     how many echoes; the default is 4
 *     -w timeout   how long to wait for each answer, in milliseconds;
 *                  the default is 1000
 *
 * A round trip is timed by the clock tick, so it is a multiple of 55ms.
 * The exit code is 0 when any echo was answered, 1 otherwise.
 *
 * Built over CMD\NET\SOCKLIB.C, behind CMD\NET\NETSTART.ASM.
 */
#include "socklib.h"

static void finish( int code )
{
    REGS regs;

    sl_flush();
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x4C00 | (u32)( code & 0xFF );
    int21( &regs );
}

static int is_switch( const char *arg, char letter )
{
    return ( arg[0] == '/' || arg[0] == '-' ) && ( arg[1] | 0x20 ) == letter &&
           ( arg[2] == 0 || arg[2] == ':' );
}

/* the number after a switch: "-n 5", "/N:5" */
static int switch_value( char **argv, int argc, int *i, u32 *value )
{
    const char *p = argv[*i] + 2;

    if ( *p == ':' ) p++;
    else if ( *p == 0 ) {
        if ( *i + 1 >= argc ) return 0;
        p = argv[++*i];
    }
    if ( *p < '0' || *p > '9' ) return 0;
    *value = sl_atou( p );
    return 1;
}

void __cdecl dos_entry( void )
{
    char **argv;
    int argc, i;
    const char *host = NULL;
    u32 count = 4, wait = 1000, ip, rtt, seq, sent = 0, got = 0, start;

    argc = sl_args( &argv );
    for ( i = 0; i < argc; i++ ) {
        if ( is_switch( argv[i], 'n' ) ) {
            if ( !switch_value( argv, argc, &i, &count ) || count == 0 ) {
                sl_puts( "-n needs a count of 1 or more.\r\n" );
                finish( 1 );
                return;
            }
        } else if ( is_switch( argv[i], 'w' ) ) {
            if ( !switch_value( argv, argc, &i, &wait ) || wait == 0 ) {
                sl_puts( "-w needs a time in milliseconds.\r\n" );
                finish( 1 );
                return;
            }
        } else if ( ( argv[i][0] == '/' || argv[i][0] == '-' ) && argv[i][1] == '?' ) {
            sl_puts( "Sends echo requests to another computer, to see whether it answers.\r\n"
                     "\r\n"
                     "PING [-n count] [-w timeout] host\r\n"
                     "\r\n"
                     "  -n count    How many requests to send.  Default is 4.\r\n"
                     "  -w timeout  How long to wait for each answer, in milliseconds.\r\n"
                     "              Default is 1000.\r\n"
                     "  host        A computer name or an IP address.\r\n" );
            finish( 0 );
            return;
        } else if ( argv[i][0] == '/' || argv[i][0] == '-' ) {
            sl_puts( "Invalid switch - " );
            sl_puts( argv[i] );
            sl_puts( "\r\n" );
            finish( 1 );
            return;
        } else if ( host == NULL ) {
            host = argv[i];
        } else {
            sl_puts( "Too many parameters - " );
            sl_puts( argv[i] );
            sl_puts( "\r\n" );
            finish( 1 );
            return;
        }
    }
    if ( host == NULL ) {
        sl_puts( "Required parameter missing - host\r\n" );
        finish( 1 );
        return;
    }
    if ( wait > 60000UL ) wait = 60000UL;

    if ( !sl_up( 100 ) ) {
        sl_puts( "The network is not up.  See IPCONFIG.\r\n" );
        finish( 1 );
        return;
    }
    ip = sl_resolve( host, 1000 );
    if ( ip == 0 ) {
        sl_puts( "Cannot find " );
        sl_puts( host );
        sl_puts( ".\r\n" );
        finish( 1 );
        return;
    }

    sl_puts( "\r\nPinging " );
    sl_puts( host );
    sl_puts( " [" );
    sl_putip( ip );
    sl_puts( "]:\r\n\r\n" );
    sl_flush();

    for ( seq = 1; seq <= count; seq++ ) {
        start = sl_now();
        sent++;
        if ( sl_ping( ip, (u16)seq, &rtt, wait / 10 ) ) {
            got++;
            sl_puts( "Reply from " );
            sl_putip( ip );
            if ( rtt == 0 ) sl_puts( ": time<55ms\r\n" );
            else { sl_puts( ": time=" ); sl_putu( rtt ); sl_puts( "ms\r\n" ); }
        } else {
            sl_puts( "Request timed out.\r\n" );
        }
        sl_flush();
        /* one a second: wait out the rest of it */
        if ( seq < count ) while ( sl_since( start ) < 100 ) ;
    }

    sl_puts( "\r\nSent " );
    sl_putu( sent );
    sl_puts( ", received " );
    sl_putu( got );
    sl_puts( ", lost " );
    sl_putu( sent - got );
    sl_puts( ".\r\n" );
    finish( got ? 0 : 1 );
}
