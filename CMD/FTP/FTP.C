/*
 * FTP.C - FTP.EXE, a file-transfer client
 *
 * The interface is the one every ftp client has had since Berkeley: an
 * "ftp>" prompt and the commands open, user, ls/dir, get, put, cd, pwd,
 * ascii, binary, close and bye/quit.  The control connection is port
 * 21; data moves on a second connection the server names in its answer
 * to PASV (passive mode only - a 32-bit client behind the kernel's one
 * address cannot usefully listen for the server to call back).
 *
 *   FTP [host]
 *
 * Built over CMD\NET\SOCKLIB.C, behind CMD\NET\NETSTART.ASM.
 */
#include "socklib.h"

static int  ctrl;               /* the control socket, 0 if not open */
static int  binary = 1;         /* transfer type: 1 image, 0 ASCII */
static char reply[1024];
static char linebuf[1024];
static u8   xfer[4096];

static int get_line( char *buf, int max );

/* ---- the control connection ---- */

/* read one reply (possibly many lines), print it, return the 3-digit
   code; a line "NNN " (space, not '-') ends the reply */
static int ftp_reply( void )
{
    int code = 0, n, first = 1;
    char want[4];

    want[0] = 0;
    for ( ;; ) {
        n = sl_recvline( ctrl, reply, sizeof( reply ), 1500 );
        if ( n < 0 ) { sl_puts( "  (connection closed)\r\n" ); ctrl = 0; return 0; }
        sl_puts( "  " ); sl_puts( reply ); sl_puts( "\r\n" );
        if ( first && n >= 3 && reply[0] >= '0' && reply[0] <= '9' ) {
            code = ( reply[0] - '0' ) * 100 + ( reply[1] - '0' ) * 10 + ( reply[2] - '0' );
            want[0] = reply[0]; want[1] = reply[1]; want[2] = reply[2]; want[3] = 0;
            first = 0;
            if ( reply[3] == ' ' || n == 3 ) break;
        } else if ( !first && n >= 4 &&
                    reply[0] == want[0] && reply[1] == want[1] && reply[2] == want[2] && reply[3] == ' ' ) {
            break;
        } else if ( !first && n == 3 &&
                    reply[0] == want[0] && reply[1] == want[1] && reply[2] == want[2] ) {
            break;
        }
    }
    return code;
}

static int ftp_cmd( const char *verb, const char *arg )
{
    char *w = linebuf;
    const char *s;
    for ( s = verb; *s; ) *w++ = *s++;
    if ( arg && *arg ) { *w++ = ' '; for ( s = arg; *s; ) *w++ = *s++; }
    *w++ = '\r'; *w++ = '\n';
    sl_send( ctrl, linebuf, (u32)( w - linebuf ), 1500 );
    return ftp_reply();
}

/* PASV: open the data connection the server names, 0 on failure */
static int ftp_pasv( void )
{
    char *w = linebuf;
    const char *s;
    int code, nums[6], ni = 0, val = 0, innum = 0;
    char *p;
    u32 ip;
    u16 port;
    int data;

    for ( s = "PASV\r\n"; *s; ) *w++ = *s++;
    sl_send( ctrl, linebuf, (u32)( w - linebuf ), 1500 );
    code = ftp_reply();
    if ( code != 227 ) return 0;

    /* the six numbers are inside the parentheses of the last reply line,
       still in reply[]; start past the '(' so the 227 code is not read */
    for ( p = reply; *p && *p != '('; p++ ) ;
    if ( *p == '(' ) p++;
    for ( ; *p && *p != ')'; p++ ) {
        if ( *p >= '0' && *p <= '9' ) { val = val * 10 + ( *p - '0' ); innum = 1; }
        else if ( innum ) { if ( ni < 6 ) nums[ni++] = val; val = 0; innum = 0; }
    }
    if ( innum && ni < 6 ) nums[ni++] = val;
    if ( ni < 6 ) return 0;
    ip = MAKE_IP( nums[0], nums[1], nums[2], nums[3] );
    port = (u16)( nums[4] * 256 + nums[5] );

    data = sl_open( SOCK_STREAM );
    if ( data == 0 ) return 0;
    if ( !sl_connect( data, ip, port, 1500 ) ) { sl_sclose( data ); return 0; }
    return data;
}

/* ---- the commands ---- */

static void do_open( const char *host, u16 port )
{
    u32 ip;
    char user[64], pass[64];

    if ( ctrl ) { sl_puts( "Already connected; close first.\r\n" ); return; }
    ip = sl_resolve( host, 600 );
    if ( ip == 0 ) { sl_puts( "ftp: cannot find " ); sl_puts( host ); sl_puts( "\r\n" ); return; }
    ctrl = sl_open( SOCK_STREAM );
    if ( ctrl == 0 ) { sl_puts( "ftp: no socket\r\n" ); return; }
    if ( !sl_connect( ctrl, ip, port, 1500 ) ) { sl_puts( "ftp: cannot connect\r\n" ); sl_sclose( ctrl ); ctrl = 0; return; }
    sl_puts( "Connected to " ); sl_puts( host ); sl_puts( ".\r\n" );
    ftp_reply();

    /* log in: a name, then a password */
    sl_puts( "User (" ); sl_puts( host ); sl_puts( ":anonymous): " ); sl_flush();
    if ( get_line( user, sizeof( user ) ) < 0 ) user[0] = 0;
    if ( user[0] == 0 ) sl_strcpy( user, "anonymous", sizeof( user ) );

    if ( ftp_cmd( "USER", user ) == 331 ) {
        sl_puts( "Password: " ); sl_flush();
        if ( get_line( pass, sizeof( pass ) ) < 0 ) pass[0] = 0;
        if ( pass[0] == 0 ) sl_strcpy( pass, "pmdos@pm-dos", sizeof( pass ) );
        ftp_cmd( "PASS", pass );
    }
    ftp_cmd( "TYPE", binary ? "I" : "A" );
}

static void do_list( const char *arg )
{
    int data, got;
    if ( !ctrl ) { sl_puts( "Not connected.\r\n" ); return; }
    data = ftp_pasv();
    if ( data == 0 ) { sl_puts( "ftp: cannot open the data connection\r\n" ); return; }
    ftp_cmd( "LIST", arg );
    for ( ;; ) {
        got = sl_recvwait( data, xfer, sizeof( xfer ), 2000 );
        if ( got <= 0 ) break;
        {
            REGS regs;
            sl_memzero( &regs, sizeof( regs ) );
            regs.eax = 0x4000; regs.ebx = 1; regs.ecx = (u32)got; regs.edx = (u32)xfer;
            int21( &regs );
        }
    }
    sl_sclose( data );
    ftp_reply();                        /* the 226 completion */
}

static void do_get( const char *remote, const char *local )
{
    int data, got, handle;
    u32 total = 0;
    REGS regs;

    if ( !ctrl ) { sl_puts( "Not connected.\r\n" ); return; }
    if ( local == NULL || *local == 0 ) local = remote;
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3C00; regs.ecx = 0; regs.edx = (u32)local;
    int21( &regs );
    if ( CF( regs ) ) { sl_puts( "ftp: cannot create " ); sl_puts( local ); sl_puts( "\r\n" ); return; }
    handle = (int)regs.eax;

    data = ftp_pasv();
    if ( data == 0 ) { sl_puts( "ftp: cannot open the data connection\r\n" ); sl_memzero(&regs,sizeof(regs)); regs.eax=0x3E00; regs.ebx=handle; int21(&regs); return; }
    ftp_cmd( "RETR", remote );
    for ( ;; ) {
        got = sl_recvwait( data, xfer, sizeof( xfer ), 2000 );
        if ( got <= 0 ) break;
        total += got;
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x4000; regs.ebx = handle; regs.ecx = (u32)got; regs.edx = (u32)xfer;
        int21( &regs );
    }
    sl_sclose( data );
    sl_memzero( &regs, sizeof( regs ) ); regs.eax = 0x3E00; regs.ebx = handle; int21( &regs );
    ftp_reply();
    sl_puts( "got " ); sl_putu( total ); sl_puts( " bytes\r\n" );
}

static void do_put( const char *local, const char *remote )
{
    int data, got, handle;
    u32 total = 0;
    REGS regs;

    if ( !ctrl ) { sl_puts( "Not connected.\r\n" ); return; }
    if ( remote == NULL || *remote == 0 ) remote = local;
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3D00; regs.edx = (u32)local;        /* open for reading */
    int21( &regs );
    if ( CF( regs ) ) { sl_puts( "ftp: cannot open " ); sl_puts( local ); sl_puts( "\r\n" ); return; }
    handle = (int)regs.eax;

    data = ftp_pasv();
    if ( data == 0 ) { sl_puts( "ftp: cannot open the data connection\r\n" ); sl_memzero(&regs,sizeof(regs)); regs.eax=0x3E00; regs.ebx=handle; int21(&regs); return; }
    ftp_cmd( "STOR", remote );
    for ( ;; ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x3F00; regs.ebx = handle; regs.ecx = sizeof( xfer ); regs.edx = (u32)xfer;
        int21( &regs );
        got = (int)regs.eax;
        if ( got <= 0 ) break;
        total += got;
        sl_send( data, xfer, (u32)got, 3000 );
    }
    sl_sclose( data );
    sl_memzero( &regs, sizeof( regs ) ); regs.eax = 0x3E00; regs.ebx = handle; int21( &regs );
    ftp_reply();
    sl_puts( "sent " ); sl_putu( total ); sl_puts( " bytes\r\n" );
}

static void do_close( void )
{
    if ( !ctrl ) { sl_puts( "Not connected.\r\n" ); return; }
    ftp_cmd( "QUIT", NULL );
    sl_sclose( ctrl );
    ctrl = 0;
}

/* read a line from handle 0 a byte at a time (so a redirected script
   feeds it as well as the keyboard, whose cooked read echoes itself);
   -1 at end of input */
static int get_line( char *buf, int max )
{
    REGS regs;
    int  n = 0;
    u8   ch;

    for ( ;; ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x3F00;
        regs.ebx = 0;
        regs.ecx = 1;
        regs.edx = (u32)&ch;
        int21( &regs );
        if ( CF( regs ) || ( regs.eax & 0xFFFF ) == 0 ) { buf[n] = 0; return n ? n : -1; }
        if ( ch == '\n' ) break;
        if ( ch != '\r' && n < max - 1 ) buf[n++] = (char)ch;
    }
    buf[n] = 0;
    return n;
}

/* split a line into a verb and up to two arguments */
static int split( char *line, char **verb, char **a1, char **a2 )
{
    char *p = line;
    int n = 0;
    *verb = *a1 = *a2 = (char *)"";
    for ( ;; ) {
        char **slot;
        while ( *p == ' ' || *p == '\t' ) p++;
        if ( *p == 0 ) break;
        slot = ( n == 0 ) ? verb : ( n == 1 ) ? a1 : a2;
        *slot = p;
        while ( *p && *p != ' ' && *p != '\t' ) p++;
        if ( *p ) *p++ = 0;
        if ( ++n == 3 ) break;
    }
    return n;
}

void __cdecl dos_entry( void )
{
    char **argv, *verb, *a1, *a2;
    char cmd[1024];
    int argc;

    argc = sl_args( &argv );

    if ( !sl_up( 800 ) ) { sl_puts( "FTP: the network is not up.\r\n" ); sl_flush(); return; }
    sl_puts( "PM-DOS FTP.  Type help for the commands, bye to leave.\r\n" );

    if ( argc >= 1 ) do_open( argv[0], 21 );

    for ( ;; ) {
        sl_puts( "ftp> " ); sl_flush();
        if ( get_line( cmd, sizeof( cmd ) ) < 0 ) { sl_puts( "\r\n" ); break; }
        split( cmd, &verb, &a1, &a2 );
        if ( *verb == 0 ) continue;

        if ( !sl_strcasecmp( verb, "bye" ) || !sl_strcasecmp( verb, "quit" ) || !sl_strcasecmp( verb, "exit" ) ) {
            do_close();
            break;
        } else if ( !sl_strcasecmp( verb, "open" ) ) {
            u16 port = ( *a2 ) ? (u16)sl_atou( a2 ) : 21;
            do_open( a1, port );
        } else if ( !sl_strcasecmp( verb, "close" ) || !sl_strcasecmp( verb, "disconnect" ) ) {
            do_close();
        } else if ( !sl_strcasecmp( verb, "ls" ) || !sl_strcasecmp( verb, "dir" ) ) {
            do_list( a1 );
        } else if ( !sl_strcasecmp( verb, "get" ) || !sl_strcasecmp( verb, "recv" ) ) {
            do_get( a1, a2 );
        } else if ( !sl_strcasecmp( verb, "put" ) || !sl_strcasecmp( verb, "send" ) ) {
            do_put( a1, a2 );
        } else if ( !sl_strcasecmp( verb, "cd" ) ) {
            if ( ctrl ) ftp_cmd( "CWD", a1 ); else sl_puts( "Not connected.\r\n" );
        } else if ( !sl_strcasecmp( verb, "pwd" ) ) {
            if ( ctrl ) ftp_cmd( "PWD", NULL ); else sl_puts( "Not connected.\r\n" );
        } else if ( !sl_strcasecmp( verb, "ascii" ) ) {
            binary = 0; if ( ctrl ) ftp_cmd( "TYPE", "A" ); else sl_puts( "Type set to A.\r\n" );
        } else if ( !sl_strcasecmp( verb, "binary" ) || !sl_strcasecmp( verb, "bin" ) ) {
            binary = 1; if ( ctrl ) ftp_cmd( "TYPE", "I" ); else sl_puts( "Type set to I.\r\n" );
        } else if ( !sl_strcasecmp( verb, "user" ) ) {
            if ( ctrl ) ftp_cmd( "USER", a1 ); else sl_puts( "Not connected.\r\n" );
        } else if ( !sl_strcasecmp( verb, "help" ) || *verb == '?' ) {
            sl_puts( "open close user ls dir get put cd pwd ascii binary bye\r\n" );
        } else {
            sl_puts( "?Invalid command.  Type help.\r\n" );
        }
    }
    sl_puts( "Goodbye.\r\n" );
    sl_flush();
}
