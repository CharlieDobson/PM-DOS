/*
 * TELNET.C - TELNET.EXE, a terminal over the network
 *
 * Connect to a host, send what is typed and show what comes back.  The
 * Telnet option bytes (IAC, RFC 854) are answered - the client refuses
 * every option but lets the server echo and suppress go-ahead, which is
 * what a character-at-a-time session needs - so a remote shell or a
 * banner service is usable.  Control-] ends it.  The incoming bytes are
 * written as they are; this is a terminal, not a full ANSI emulator, so
 * a screen-painting program will look rough.
 *
 *   TELNET host [port]
 *
 * Built over CMD\NET\SOCKLIB.C, behind CMD\NET\NETSTART.ASM.
 */
#include "socklib.h"

#define IAC   255
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#define SB    250
#define SE    240
#define OPT_ECHO 1
#define OPT_SGA  3

static int  sock;
static u8   netbuf[2048];
static u8   outbuf[2048];
static u32  outlen;

static void out_flush( void )
{
    REGS regs;
    if ( outlen ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x4000; regs.ebx = 1; regs.ecx = outlen; regs.edx = (u32)outbuf;
        int21( &regs );
        outlen = 0;
    }
}

static void out_byte( u8 ch )
{
    if ( outlen == sizeof( outbuf ) ) out_flush();
    outbuf[outlen++] = ch;
}

static void send_bytes( const u8 *data, u32 len )
{
    sl_send( sock, data, len, 1500 );
}

static void reply_opt( u8 verb, u8 opt )
{
    u8 three[3];
    three[0] = IAC;
    three[1] = verb;
    three[2] = opt;
    send_bytes( three, 3 );
}

/* the incoming stream, IAC stripped and answered, the rest shown */
static int iac_state;           /* 0 data, 1 saw IAC, 2 saw verb(need opt), 3 in SB, 4 SB+IAC */
static u8  iac_verb;

static void feed( const u8 *data, int len )
{
    int i;
    for ( i = 0; i < len; i++ ) {
        u8 ch = data[i];
        switch ( iac_state ) {
        case 0:
            if ( ch == IAC ) iac_state = 1;
            else out_byte( ch );
            break;
        case 1:
            if ( ch == IAC ) { out_byte( IAC ); iac_state = 0; }         /* escaped 255 */
            else if ( ch == WILL || ch == WONT || ch == DO || ch == DONT ) { iac_verb = ch; iac_state = 2; }
            else if ( ch == SB ) iac_state = 3;
            else iac_state = 0;                                          /* a two-byte command */
            break;
        case 2:
            /* answer the option */
            if ( iac_verb == WILL ) {
                if ( ch == OPT_ECHO || ch == OPT_SGA ) reply_opt( DO, ch );
                else reply_opt( DONT, ch );
            } else if ( iac_verb == DO ) {
                if ( ch == OPT_SGA ) reply_opt( WILL, ch );
                else reply_opt( WONT, ch );
            } else if ( iac_verb == WONT ) {
                reply_opt( DONT, ch );
            } else {                                                     /* DONT */
                reply_opt( WONT, ch );
            }
            iac_state = 0;
            break;
        case 3:
            if ( ch == IAC ) iac_state = 4;
            break;
        case 4:
            if ( ch == SE ) iac_state = 0;                               /* end of subnegotiation */
            else iac_state = 3;
            break;
        }
    }
    out_flush();
}

static void send_key( int key )
{
    u8 seq[3];
    if ( key < 0x100 ) {
        if ( key == '\r' ) { seq[0] = '\r'; seq[1] = '\n'; send_bytes( seq, 2 ); }
        else { seq[0] = (u8)key; send_bytes( seq, 1 ); }
    } else {
        /* an extended key: the arrows as ANSI cursor sequences */
        int scan = key & 0xFF;
        char dir = 0;
        if ( scan == 0x48 ) dir = 'A';
        else if ( scan == 0x50 ) dir = 'B';
        else if ( scan == 0x4D ) dir = 'C';
        else if ( scan == 0x4B ) dir = 'D';
        if ( dir ) { seq[0] = 0x1B; seq[1] = '['; seq[2] = (u8)dir; send_bytes( seq, 3 ); }
    }
}

void __cdecl dos_entry( void )
{
    char **argv;
    int   argc, got, i;
    u16   port = 23;
    u32   ip, idle = 0, last;
    const char *host = NULL;

    argc = sl_args( &argv );
    for ( i = 0; i < argc; i++ ) {
        if ( ( argv[i][0] == '/' || argv[i][0] == '-' ) &&
             ( argv[i][1] == 'T' || argv[i][1] == 't' ) && argv[i][2] == ':' ) {
            idle = sl_atou( argv[i] + 3 ) * 100;        /* seconds -> hundredths */
        } else if ( host == NULL ) {
            host = argv[i];
        } else {
            port = (u16)sl_atou( argv[i] );
        }
    }
    if ( host == NULL ) { sl_puts( "usage: TELNET host [port] [/T:idle-seconds]\r\n" ); sl_flush(); return; }
    argv[0] = (char *)host;             /* the messages below name it */

    if ( !sl_up( 800 ) ) { sl_puts( "TELNET: the network is not up.\r\n" ); sl_flush(); return; }
    ip = sl_resolve( argv[0], 600 );
    if ( ip == 0 ) { sl_puts( "TELNET: cannot find " ); sl_puts( argv[0] ); sl_puts( "\r\n" ); sl_flush(); return; }

    sock = sl_open( SOCK_STREAM );
    if ( sock == 0 ) { sl_puts( "TELNET: no socket\r\n" ); sl_flush(); return; }
    sl_puts( "Trying " ); sl_putip( ip ); sl_puts( "...\r\n" ); sl_flush();
    if ( !sl_connect( sock, ip, port, 2000 ) ) { sl_puts( "TELNET: cannot connect\r\n" ); sl_sclose( sock ); sl_flush(); return; }
    sl_puts( "Connected.  Control-] to quit.\r\n" ); sl_flush();

    last = sl_now();
    for ( ;; ) {
        if ( sl_kbhit() ) {
            int key = sl_getkey();
            if ( key == 0x1D ) break;           /* Control-] */
            send_key( key );
            last = sl_now();
        }
        got = sl_recv( sock, netbuf, sizeof( netbuf ) );
        if ( got > 0 ) { feed( netbuf, got ); last = sl_now(); }
        else if ( sl_closed( sock ) ) { sl_puts( "\r\nConnection closed.\r\n" ); break; }
        else if ( idle && sl_since( last ) > idle ) { sl_puts( "\r\n(idle)\r\n" ); break; }
    }

    sl_sclose( sock );
    sl_flush();
}
