/*
 * SNTP.C - SNTP.EXE, set the clock from a time server
 *
 * A small UDP client (RFC 4330): one packet to a server's port 123, the
 * transmit timestamp out of the 48 bytes that come back, and the date
 * and time set from it (21h/2Bh and 2Dh) unless /S only shows them.
 *
 *   SNTP [server] [/S] [/Z:+-h]
 *     server   a name or address; the default is pool.ntp.org
 *     /S       show the time, do not set the clock
 *     /Z:+2    add this many hours (the server's time is UTC)
 *
 * Built over CMD\NET\SOCKLIB.C, behind CMD\NET\NETSTART.ASM.
 */
#include "socklib.h"

#define NTP_PORT   123
#define NTP_EPOCH  2208988800UL         /* seconds from 1900 to 1970 */

static void set_clock( u32 unix_secs )
{
    REGS regs;
    u32  days, sod, hour, minute, second;
    u32  z, era, doe, yoe, year, doy, mp, day, month;

    days = unix_secs / 86400UL;
    sod  = unix_secs % 86400UL;
    hour = sod / 3600;
    minute = ( sod / 60 ) % 60;
    second = sod % 60;

    /* civil date from the day number (Hinnant), all in 32 bits */
    z = days + 719468UL;
    era = z / 146097UL;
    doe = z - era * 146097UL;
    yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
    year = yoe + era * 400;
    doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
    mp = ( 5 * doy + 2 ) / 153;
    day = doy - ( 153 * mp + 2 ) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    if ( month <= 2 ) year++;

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x2B00;
    regs.ecx = year;
    regs.edx = ( month << 8 ) | day;
    int21( &regs );

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x2D00;
    regs.ecx = ( hour << 8 ) | minute;
    regs.edx = ( second << 8 );
    int21( &regs );

    sl_puts( "clock set to " );
    sl_putu( year ); sl_putc( '-' );
    if ( month < 10 ) sl_putc( '0' ); sl_putu( month ); sl_putc( '-' );
    if ( day < 10 ) sl_putc( '0' ); sl_putu( day ); sl_putc( ' ' );
    if ( hour < 10 ) sl_putc( '0' ); sl_putu( hour ); sl_putc( ':' );
    if ( minute < 10 ) sl_putc( '0' ); sl_putu( minute ); sl_putc( ':' );
    if ( second < 10 ) sl_putc( '0' ); sl_putu( second );
    sl_puts( "\r\n" );
}

static void show_only( u32 unix_secs )
{
    u32 sod = unix_secs % 86400UL;
    sl_puts( "the server's time is " );
    sl_putu( unix_secs );
    sl_puts( " (" );
    sl_putu( sod / 3600 ); sl_putc( ':' );
    sl_putu( ( sod / 60 ) % 60 ); sl_putc( ':' );
    sl_putu( sod % 60 );
    sl_puts( ")\r\n" );
}

void __cdecl dos_entry( void )
{
    char **argv;
    int argc, i, show = 0, handle, got;
    long zone = 0;
    const char *server = "pool.ntp.org";
    u32 ip, ntp_secs, unix_secs;
    u8  packet[48];

    argc = sl_args( &argv );
    for ( i = 0; i < argc; i++ ) {
        if ( argv[i][0] == '/' || argv[i][0] == '-' ) {
            char opt = argv[i][1];
            if ( opt == 'S' || opt == 's' ) show = 1;
            else if ( ( opt == 'Z' || opt == 'z' ) && argv[i][2] == ':' ) {
                const char *p = argv[i] + 3;
                int neg = 0;
                if ( *p == '+' ) p++;
                else if ( *p == '-' ) { neg = 1; p++; }
                zone = (long)sl_atou( p );
                if ( neg ) zone = -zone;
            } else if ( opt == '?' ) {
                sl_puts( "SNTP [server] [/S] [/Z:+-h] - set the clock from a time server.\r\n" );
                sl_puts( "  /S shows the time without setting it; /Z adds hours to the server's UTC.\r\n" );
                sl_flush();
                return;
            }
        } else {
            server = argv[i];
        }
    }

    if ( !sl_up( 800 ) ) { sl_puts( "SNTP: the network is not up.\r\n" ); sl_flush(); return; }

    ip = sl_resolve( server, 500 );
    if ( ip == 0 ) { sl_puts( "SNTP: cannot find " ); sl_puts( server ); sl_puts( "\r\n" ); sl_flush(); return; }

    handle = sl_open( SOCK_DGRAM );
    if ( handle == 0 ) { sl_puts( "SNTP: no socket.\r\n" ); sl_flush(); return; }

    sl_memzero( packet, sizeof( packet ) );
    packet[0] = 0x23;                           /* LI 0, version 4, mode 3 (client) */

    sl_dgram_send( handle, ip, NTP_PORT, packet, sizeof( packet ) );
    got = sl_dgram_recv( handle, NULL, NULL, packet, sizeof( packet ), 500 );
    sl_sclose( handle );

    if ( got < 48 ) { sl_puts( "SNTP: no answer from " ); sl_puts( server ); sl_puts( "\r\n" ); sl_flush(); return; }

    /* the transmit timestamp: seconds since 1900 at offset 40, big-endian */
    ntp_secs = ( (u32)packet[40] << 24 ) | ( (u32)packet[41] << 16 ) |
               ( (u32)packet[42] << 8 ) | packet[43];
    if ( ntp_secs < NTP_EPOCH ) { sl_puts( "SNTP: the server's time is out of range.\r\n" ); sl_flush(); return; }
    unix_secs = ntp_secs - NTP_EPOCH;
    unix_secs = (u32)( (long)unix_secs + zone * 3600L );

    if ( show ) show_only( unix_secs );
    else set_clock( unix_secs );
    sl_flush();
}
