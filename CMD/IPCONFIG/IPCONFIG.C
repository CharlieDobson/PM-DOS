/*
 * IPCONFIG.C - IPCONFIG.EXE, show the TCP/IP settings and manage the lease
 *
 * TCPIP.SYS gets the address itself - from DHCP at INIT, or from /IP= on
 * its DEVICE= line - and keeps the lease on the clock.  This shows what
 * it holds (21h/F1h AL=26h SOCK_IFSTAT, the long form) and asks it to
 * give the lease back or renew it, or to send its name to DNS again
 * (SOCK_DHCP).
 *
 *   IPCONFIG [/ALL | /RELEASE | /RENEW | /REGISTERDNS]
 *
 * The lease's times are worked out here: the stack counts seconds since
 * the lease was given, and the clock (21h/2Ah, 2Ch) says when that was.
 * Dates and times are written the country's way (21h/38h).
 *
 * The exit code is 0, or 1 when TCPIP.SYS is not loaded, a switch is
 * wrong, or /RELEASE or /RENEW could not be done.
 *
 * Built over CMD\NET\SOCKLIB.C, behind CMD\NET\NETSTART.ASM.
 */
#include "socklib.h"

#define RENEW_WAIT  1000                /* hundredths: how long /RENEW waits */
#define REG_WAIT    1500                /* ...and /REGISTERDNS */

static u32  if_len;                     /* how much SOCK_IFSTAT gave: an older
                                           TCPIP.SYS gives no SOCKIF_LEN3 part */

/* the country's way with dates and times (21h/38h) */
static u32  date_fmt;                   /* 0 m-d-y, 1 d-m-y, 2 y-m-d */
static char date_sep = '-';
static char time_sep = ':';
static int  time_24;

static void get_country( void )
{
    REGS regs;
    u8   buf[34];

    sl_memzero( buf, sizeof( buf ) );
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3800;
    regs.edx = (u32)buf;
    int21( &regs );
    if ( CF( regs ) ) return;
    date_fmt = (u32)buf[0] | ( (u32)buf[1] << 8 );
    if ( date_fmt > 2 ) date_fmt = 0;
    if ( buf[11] ) date_sep = (char)buf[11];
    if ( buf[13] ) time_sep = (char)buf[13];
    time_24 = buf[17] & 1;
}

/* the socket call, with the error kept */
static int sock_call( u32 fn, void *buf, u32 len, u32 *answer )
{
    REGS regs;

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = ( fn << 16 ) | 0xF126;
    regs.ecx = len;
    regs.edi = (u32)buf;
    int21( &regs );
    if ( CF( regs ) ) return (int)( regs.eax & 0xFFFF );
    if ( answer ) *answer = regs.eax;
    return 0;
}

static int get_if( SOCK_IF *info )
{
    u32 got = 0;

    sl_memzero( info, SOCKIF_LEN3 );
    if ( sock_call( SOCK_IFSTAT, info, SOCKIF_LEN3, &got ) != 0 ) return 0;
    if_len = got;
    return 1;
}

/* ---------------------------------------------------------------- */
/* dates: days since 1970-01-01, and back                            */
/* ---------------------------------------------------------------- */

static u32 days_from_civil( u32 year, u32 month, u32 day )
{
    u32 era, yoe, doy, doe;

    if ( month <= 2 ) year--;
    era = year / 400;
    yoe = year - era * 400;
    doy = ( 153 * ( month > 2 ? month - 3 : month + 9 ) + 2 ) / 5 + day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097UL + doe - 719468UL;
}

static void civil_from_days( u32 days, u32 *year, u32 *month, u32 *day )
{
    u32 z, era, doe, yoe, doy, mp;

    z = days + 719468UL;
    era = z / 146097UL;
    doe = z - era * 146097UL;
    yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
    *year = yoe + era * 400;
    doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
    mp = ( 5 * doy + 2 ) / 153;
    *day = doy - ( 153 * mp + 2 ) / 5 + 1;
    *month = mp < 10 ? mp + 3 : mp - 9;
    if ( *month <= 2 ) ( *year )++;
}

/* the clock now, as seconds since 1970 */
static u32 clock_now( void )
{
    REGS regs;
    u32 year, month, day, hour, minute, second;

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x2A00;
    int21( &regs );
    year = regs.ecx & 0xFFFF;
    month = ( regs.edx >> 8 ) & 0xFF;
    day = regs.edx & 0xFF;

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x2C00;
    int21( &regs );
    hour = ( regs.ecx >> 8 ) & 0xFF;
    minute = regs.ecx & 0xFF;
    second = ( regs.edx >> 8 ) & 0xFF;

    if ( year < 1970 ) year = 1970;
    return days_from_civil( year, month, day ) * 86400UL +
           hour * 3600UL + minute * 60UL + second;
}

static void put2( u32 value )
{
    if ( value < 10 ) sl_putc( '0' );
    sl_putu( value );
}

static void put_when( u32 secs )
{
    u32 year, month, day, sod, hour;

    civil_from_days( secs / 86400UL, &year, &month, &day );
    sod = secs % 86400UL;

    if ( date_fmt == 1 ) {
        put2( day ); sl_putc( date_sep ); put2( month ); sl_putc( date_sep ); sl_putu( year );
    } else if ( date_fmt == 2 ) {
        sl_putu( year ); sl_putc( date_sep ); put2( month ); sl_putc( date_sep ); put2( day );
    } else {
        put2( month ); sl_putc( date_sep ); put2( day ); sl_putc( date_sep ); sl_putu( year );
    }
    sl_putc( ' ' );

    hour = sod / 3600;
    if ( time_24 ) put2( hour );
    else sl_putu( hour % 12 == 0 ? 12 : hour % 12 );
    sl_putc( time_sep ); put2( ( sod / 60 ) % 60 );
    sl_putc( time_sep ); put2( sod % 60 );
    if ( !time_24 ) sl_puts( hour < 12 ? " AM" : " PM" );
}

/* ---------------------------------------------------------------- */
/* the listing                                                       */
/* ---------------------------------------------------------------- */

static void label( const char *name )
{
    u32 len = sl_strlen( name );

    sl_puts( "   " );
    sl_puts( name );
    while ( len < 28 ) {
        sl_putc( ( len & 1 ) ? ' ' : '.' );
        len++;
    }
    sl_puts( ": " );
}

static void line_ip( const char *name, u32 ip )
{
    label( name );
    sl_putip( ip );
    sl_puts( "\r\n" );
}

/* one more address under the last, in the column label() leaves */
static void more_ip( u32 ip )
{
    u32 col;

    if ( ip == 0 ) return;
    for ( col = 0; col < 3 + 28 + 2; col++ ) sl_putc( ' ' );
    sl_putip( ip );
    sl_puts( "\r\n" );
}

/* how this computer's name stands in DNS (SOCK_IF.regstate) */
static void show_reg( const SOCK_IF *info )
{
    switch ( info->regstate ) {
    case DREG_BUSY:     sl_puts( "Being sent" ); break;
    case DREG_DONE:     sl_puts( "Registered" ); break;
    case DREG_REFUSED:
        sl_puts( "Refused by the DNS server (error " );
        sl_putu( info->regrcode );
        sl_putc( ')' );
        break;
    case DREG_NOANSWER: sl_puts( "No DNS server answered" ); break;
    case DREG_NOZONE:   sl_puts( "No zone in DNS for the domain" ); break;
    default:            sl_puts( info->domain[0] ? "Not sent" : "Not sent: no DNS suffix" ); break;
    }
}

static void show( const SOCK_IF *info, int all )
{
    int i;

    if ( all ) {
        sl_puts( "\r\n" );
        if ( if_len >= SOCKIF_LEN3 ) {
            label( "Host Name" );
            sl_puts( info->hostname );
            sl_puts( "\r\n" );
            label( "DNS Suffix" );
            sl_puts( info->domain );
            sl_puts( "\r\n" );
        }
        label( "Computer Name" );
        sl_puts( info->nbname );
        sl_puts( "\r\n" );
    }
    sl_puts( "\r\nEthernet adapter " );
    sl_puts( info->nic[0] ? info->nic : "(unnamed)" );
    sl_puts( ":\r\n\r\n" );

    if ( all ) {
        label( "Physical Address" );
        for ( i = 0; i < 6; i++ ) {
            if ( i ) sl_putc( '-' );
            sl_putc( "0123456789ABCDEF"[info->mac[i] >> 4] );
            sl_putc( "0123456789ABCDEF"[info->mac[i] & 15] );
        }
        sl_puts( "\r\n" );
        label( "DHCP Enabled" );
        sl_puts( info->dhcp == DHS_STATIC ? "No\r\n" : "Yes\r\n" );
    }

    line_ip( "IP Address", info->up ? info->ip : 0 );
    line_ip( "Subnet Mask", info->up ? info->mask : 0 );
    label( "Default Gateway" );
    if ( info->up && info->gw ) sl_putip( info->gw );
    sl_puts( "\r\n" );

    if ( !all ) return;

    if ( info->dhcp == DHS_BOUND && info->dhcpsrv ) line_ip( "DHCP Server", info->dhcpsrv );
    label( "DNS Servers" );
    if ( info->dns ) sl_putip( info->dns );
    sl_puts( "\r\n" );
    if ( if_len >= SOCKIF_LEN3 ) {
        more_ip( info->dns2 );
        more_ip( info->dns3 );
        label( "Name in DNS" );
        show_reg( info );
        sl_puts( "\r\n" );
    }
    label( "WINS Server" );
    if ( info->wins ) sl_putip( info->wins );
    sl_puts( "\r\n" );
    if ( if_len >= SOCKIF_LEN3 ) {
        label( "HTTPS Certificates" );
        sl_puts( info->cabytes ? "Checked\r\n" : "Not checked: no trust bundle\r\n" );
    }

    if ( info->up && info->dhcp == DHS_BOUND ) {
        u32 now = clock_now();
        u32 got = now - info->leaseage;

        label( "Lease Obtained" );
        put_when( got );
        sl_puts( "\r\n" );
        label( "Lease Expires" );
        if ( info->leasesecs == 0xFFFFFFFFUL || got + info->leasesecs < got ) sl_puts( "Never" );
        else put_when( got + info->leasesecs );
        sl_puts( "\r\n" );
    } else if ( info->dhcp != DHS_STATIC && info->dhstate == DHST_ASKING ) {
        label( "DHCP" );
        sl_puts( "Looking for a DHCP server\r\n" );
    }
}

/* ---------------------------------------------------------------- */
/* /RELEASE and /RENEW                                               */
/* ---------------------------------------------------------------- */

static int lease_op( u32 op, SOCK_IF *info )
{
    int err = sock_call( SOCK_DHCP, &op, sizeof( op ), NULL );

    if ( err == ERR_BADDATA ) {
        sl_puts( "The IP address was set with TCPIP.SYS /IP= and has no lease.\r\n" );
        return 0;
    }
    if ( err ) {
        sl_puts( "TCPIP.SYS could not do this.\r\n" );
        return 0;
    }
    return get_if( info );
}

static int do_release( void )
{
    SOCK_IF info;

    if ( !get_if( &info ) ) return 0;
    if ( info.dhcp != DHS_STATIC && !info.up ) {
        sl_puts( "There is no IP address to release.\r\n" );
        return 0;
    }
    if ( !lease_op( DHCPOP_RELEASE, &info ) ) return 0;
    show( &info, 0 );
    return 1;
}

static int do_renew( void )
{
    SOCK_IF info;
    u32 start;
    u16 acks;
    int had;

    if ( !get_if( &info ) ) return 0;
    had = info.up && info.dhcp == DHS_BOUND;
    acks = info.acks;
    if ( !lease_op( DHCPOP_RENEW, &info ) ) return 0;

    /* wait for the stack to take a fresh ACK (a lease with no end goes
       back to bound without one, so the state alone does not say) */
    start = sl_now();
    for ( ;; ) {
        if ( !get_if( &info ) ) return 0;
        if ( info.up && info.dhstate == DHST_BOUND && info.acks != acks ) break;
        if ( sl_since( start ) > RENEW_WAIT ) {
            sl_puts( had ? "Failed to renew the lease.\r\n" : "Failed to get IP address\r\n" );
            if ( had ) show( &info, 0 );
            return 0;
        }
    }
    show( &info, 0 );
    return 1;
}

/* /REGISTERDNS: TCPIP.SYS sends the name on its clock; this waits to
   say how that went */
static int do_regdns( void )
{
    SOCK_IF info;
    u32 op = DHCPOP_REGDNS, start;

    if ( !get_if( &info ) ) return 0;
    if ( if_len < SOCKIF_LEN3 ) {
        sl_puts( "This TCPIP.SYS cannot send its name to DNS.\r\n" );
        return 0;
    }
    if ( !info.up ) {
        sl_puts( "There is no IP address to send to DNS.\r\n" );
        return 0;
    }
    if ( info.domain[0] == 0 ) {
        sl_puts( "There is no DNS suffix to put the name under.  Set Domain= in TCPIP.INI.\r\n" );
        return 0;
    }
    if ( sock_call( SOCK_DHCP, &op, sizeof( op ), NULL ) != 0 || !get_if( &info ) ) {
        sl_puts( "TCPIP.SYS could not do this.\r\n" );
        return 0;
    }
    if ( info.regstate != DREG_BUSY ) {
        sl_puts( "Sending the name to DNS is turned off: RegisterDNS=No in TCPIP.INI.\r\n" );
        return 0;
    }
    start = sl_now();
    while ( info.regstate == DREG_BUSY && sl_since( start ) < REG_WAIT ) {
        if ( !get_if( &info ) ) return 0;
    }
    switch ( info.regstate ) {
    case DREG_DONE:
        sl_puts( info.hostname );
        sl_putc( '.' );
        sl_puts( info.domain );
        sl_puts( " is now " );
        sl_putip( info.ip );
        sl_puts( " in DNS.\r\n" );
        return 1;
    case DREG_BUSY:
        sl_puts( "The DNS server has not answered yet.\r\n" );
        return 0;
    default:
        show_reg( &info );
        sl_puts( ".\r\n" );
        return 0;
    }
}

/* ---------------------------------------------------------------- */

static void finish( int code )
{
    REGS regs;

    sl_flush();
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x4C00 | (u32)( code & 0xFF );
    int21( &regs );
}

void __cdecl dos_entry( void )
{
    char **argv;
    int argc, i, all = 0, release = 0, renew = 0, regdns = 0;
    SOCK_IF info;

    argc = sl_args( &argv );
    for ( i = 0; i < argc; i++ ) {
        if ( argv[i][0] == '/' || argv[i][0] == '-' ) {
            const char *opt = argv[i] + 1;
            if ( sl_strcasecmp( opt, "ALL" ) == 0 ) all = 1;
            else if ( sl_strcasecmp( opt, "RELEASE" ) == 0 ) release = 1;
            else if ( sl_strcasecmp( opt, "RENEW" ) == 0 ) renew = 1;
            else if ( sl_strcasecmp( opt, "REGISTERDNS" ) == 0 ) regdns = 1;
            else if ( opt[0] == '?' ) {
                sl_puts( "Shows the TCP/IP settings, and gives back or renews the DHCP lease.\r\n"
                         "\r\n"
                         "IPCONFIG [/ALL | /RELEASE | /RENEW | /REGISTERDNS]\r\n"
                         "\r\n"
                         "  /ALL          Shows every setting, with the lease's times.\r\n"
                         "  /RELEASE      Gives the IP address back to the DHCP server.\r\n"
                         "  /RENEW        Renews the lease, or asks for a new IP address.\r\n"
                         "  /REGISTERDNS  Sends this computer's name and IP address to DNS again.\r\n" );
                finish( 0 );
                return;
            } else {
                sl_puts( "Invalid switch - " );
                sl_puts( argv[i] );
                sl_puts( "\r\n" );
                finish( 1 );
                return;
            }
        } else {
            sl_puts( "Too many parameters - " );
            sl_puts( argv[i] );
            sl_puts( "\r\n" );
            finish( 1 );
            return;
        }
    }
    if ( all + release + renew + regdns > 1 ) {
        sl_puts( "Use only one of /ALL, /RELEASE, /RENEW and /REGISTERDNS.\r\n" );
        finish( 1 );
        return;
    }

    if ( !get_if( &info ) ) {
        sl_puts( "TCPIP.SYS is not loaded.\r\n" );
        finish( 1 );
        return;
    }
    get_country();

    if ( release ) { finish( do_release() ? 0 : 1 ); return; }
    if ( renew ) { finish( do_renew() ? 0 : 1 ); return; }
    if ( regdns ) { finish( do_regdns() ? 0 : 1 ); return; }
    show( &info, all );
    finish( 0 );
}
