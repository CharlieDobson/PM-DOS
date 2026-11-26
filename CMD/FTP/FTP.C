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
 *   FTP [host] [/USER:name [password | *]]
 *
 * /USER: logs in as that name without asking for it.  The word after it
 * is the password; a * or no word asks for the password, which is read
 * without being shown when it comes from the keyboard.  The user
 * command at the prompt takes the same name and password.
 *
 * Built over CMD\NET\SOCKLIB.C, behind CMD\NET\NETSTART.ASM.
 */
#include "socklib.h"

#define CRED_MAX 64             /* a user name or a password, NUL included */

static int     ctrl;            /* the control socket, 0 if not open */
static int     binary = 1;      /* transfer type: 1 image, 0 ASCII */
static char    reply[1024];
static char    linebuf[1024];
static uint8_t xfer[4096];

/* the /USER: login from the command line, for the first connection */
static char cl_user[CRED_MAX];
static char cl_pass[CRED_MAX];
static int  cl_ask = 1;         /* 1: ask for the password */

static int get_line( char *buf, int max );
static int get_secret( char *buf, int max );

/* ---- the control connection ---- */

/* read one reply (possibly many lines), print it, return the 3-digit
   code; a line "NNN " (space, not '-') ends the reply */
static int ftp_reply( void )
{
    int code = 0, n, first = 1;
    char want[4];
    uint32_t asked;

    if ( !ctrl ) return 0;
    want[0] = 0;
    for ( ;; ) {
        asked = sl_now();
        n = sl_recvline( ctrl, reply, sizeof( reply ), 1500 );
        if ( n < 0 ) { sl_puts( "  (connection closed)\r\n" ); ctrl = 0; return 0; }
        if ( n == 0 && sl_since( asked ) > 1500 ) { sl_puts( "  (no reply)\r\n" ); break; }
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

/* send one command and read its reply; -1 if the line was too long to
   send at all */
static int ftp_cmd( const char *verb, const char *arg )
{
    char *w = linebuf;
    const char *s;
    unsigned int need = sl_strlen( verb ) + ( ( arg && *arg ) ? 1 + sl_strlen( arg ) : 0 ) + 2;
    if ( need > sizeof( linebuf ) ) { sl_puts( "ftp: argument too long\r\n" ); return -1; }
    for ( s = verb; *s; ) *w++ = *s++;
    if ( arg && *arg ) { *w++ = ' '; for ( s = arg; *s; ) *w++ = *s++; }
    *w++ = '\r'; *w++ = '\n';
    sl_send( ctrl, linebuf, (uint32_t)( w - linebuf ), 1500 );
    return ftp_reply();
}

/* PASV: open the data connection the server names, 0 on failure */
static int ftp_pasv( void )
{
    char *w = linebuf;
    const char *s;
    int code, nums[6], ni = 0, val = 0, innum = 0;
    char *p;
    uint32_t ip;
    uint16_t port;
    int data;

    for ( s = "PASV\r\n"; *s; ) *w++ = *s++;
    sl_send( ctrl, linebuf, (uint32_t)( w - linebuf ), 1500 );
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
    port = (uint16_t)( nums[4] * 256 + nums[5] );

    data = sl_open( SOCK_STREAM );
    if ( data == 0 ) return 0;
    if ( !sl_connect( data, ip, port, 1500 ) ) { sl_sclose( data ); return 0; }
    return data;
}

/* ---- the commands ---- */

/* USER, then PASS if the server asks for one.  ask: read the password
   now rather than use pass */
static void login( const char *user, const char *pass, int ask )
{
    char typed[CRED_MAX];

    if ( ftp_cmd( "USER", user ) != 331 ) return;
    if ( ask ) {
        sl_puts( "Password: " ); sl_flush();
        if ( get_secret( typed, sizeof( typed ) ) < 0 ) typed[0] = 0;
        pass = typed;
    }
    if ( pass[0] == 0 && !sl_strcasecmp( user, "anonymous" ) ) pass = "pmdos@pm-dos";
    ftp_cmd( "PASS", pass );
}

static void do_open( const char *host, uint16_t port )
{
    uint32_t ip;
    char user[CRED_MAX];

    if ( ctrl ) { sl_puts( "Already connected; close first.\r\n" ); return; }
    ip = sl_resolve( host, 600 );
    if ( ip == 0 ) { sl_puts( "ftp: cannot find " ); sl_puts( host ); sl_puts( "\r\n" ); return; }
    ctrl = sl_open( SOCK_STREAM );
    if ( ctrl == 0 ) { sl_puts( "ftp: no socket\r\n" ); return; }
    if ( !sl_connect( ctrl, ip, port, 1500 ) ) { sl_puts( "ftp: cannot connect\r\n" ); sl_sclose( ctrl ); ctrl = 0; return; }
    sl_puts( "Connected to " ); sl_puts( host ); sl_puts( ".\r\n" );
    ftp_reply();

    /* log in: the /USER: name once, else a name asked for */
    if ( cl_user[0] ) {
        login( cl_user, cl_pass, cl_ask );
        cl_user[0] = 0;
        sl_memzero( cl_pass, sizeof( cl_pass ) );
    } else {
        sl_puts( "User (" ); sl_puts( host ); sl_puts( ":anonymous): " ); sl_flush();
        if ( get_line( user, sizeof( user ) ) < 0 ) user[0] = 0;
        if ( user[0] == 0 ) sl_strcpy( user, "anonymous", sizeof( user ) );
        login( user, "", 1 );
    }
    ftp_cmd( "TYPE", binary ? "I" : "A" );
}

static void do_list( const char *arg )
{
    int data, got;
    if ( !ctrl ) { sl_puts( "Not connected.\r\n" ); return; }
    data = ftp_pasv();
    if ( data == 0 ) { sl_puts( "ftp: cannot open the data connection\r\n" ); return; }
    if ( ftp_cmd( "LIST", arg ) < 0 ) { sl_sclose( data ); return; }
    for ( ;; ) {
        got = sl_recvwait( data, xfer, sizeof( xfer ), 2000 );
        if ( got <= 0 ) break;
        {
            REGS regs;
            sl_memzero( &regs, sizeof( regs ) );
            regs.eax = 0x4000; regs.ebx = 1; regs.ecx = (uint32_t)got; regs.edx = (uint32_t)xfer;
            int21( &regs );
        }
    }
    sl_sclose( data );
    ftp_reply();                        /* the 226 completion */
}

static void do_get( const char *remote, const char *local )
{
    int data, got, handle;
    unsigned int total = 0;
    REGS regs;

    if ( !ctrl ) { sl_puts( "Not connected.\r\n" ); return; }
    if ( local == NULL || *local == 0 ) local = remote;
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3C00; regs.ecx = 0; regs.edx = (uint32_t)local;
    int21( &regs );
    if ( CF( regs ) ) { sl_puts( "ftp: cannot create " ); sl_puts( local ); sl_puts( "\r\n" ); return; }
    handle = (int)regs.eax;

    data = ftp_pasv();
    if ( data == 0 ) { sl_puts( "ftp: cannot open the data connection\r\n" ); sl_memzero(&regs,sizeof(regs)); regs.eax=0x3E00; regs.ebx=handle; int21(&regs); return; }
    if ( ftp_cmd( "RETR", remote ) < 0 ) {
        sl_sclose( data );
        sl_memzero( &regs, sizeof( regs ) ); regs.eax = 0x3E00; regs.ebx = handle; int21( &regs );
        return;
    }
    for ( ;; ) {
        got = sl_recvwait( data, xfer, sizeof( xfer ), 2000 );
        if ( got <= 0 ) break;
        total += got;
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x4000; regs.ebx = handle; regs.ecx = (uint32_t)got; regs.edx = (uint32_t)xfer;
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
    unsigned int total = 0;
    REGS regs;

    if ( !ctrl ) { sl_puts( "Not connected.\r\n" ); return; }
    if ( remote == NULL || *remote == 0 ) remote = local;
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3D00; regs.edx = (uint32_t)local;   /* open for reading */
    int21( &regs );
    if ( CF( regs ) ) { sl_puts( "ftp: cannot open " ); sl_puts( local ); sl_puts( "\r\n" ); return; }
    handle = (int)regs.eax;

    data = ftp_pasv();
    if ( data == 0 ) { sl_puts( "ftp: cannot open the data connection\r\n" ); sl_memzero(&regs,sizeof(regs)); regs.eax=0x3E00; regs.ebx=handle; int21(&regs); return; }
    if ( ftp_cmd( "STOR", remote ) < 0 ) {
        sl_sclose( data );
        sl_memzero( &regs, sizeof( regs ) ); regs.eax = 0x3E00; regs.ebx = handle; int21( &regs );
        return;
    }
    for ( ;; ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x3F00; regs.ebx = handle; regs.ecx = sizeof( xfer ); regs.edx = (uint32_t)xfer;
        int21( &regs );
        got = (int)regs.eax;
        if ( got <= 0 ) break;
        total += got;
        sl_send( data, xfer, (uint32_t)got, 3000 );
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

/* 1 if handle 0 is the console (a character device that is standard
   input), 0 if it is redirected */
static int stdin_is_console( void )
{
    REGS regs;

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x4400;
    regs.ebx = 0;
    int21( &regs );
    return !CF( regs ) && ( regs.edx & 0x81 ) == 0x81;
}

/* read a line from handle 0.  The keyboard is read a whole line in one
   cooked read, which echoes and edits it: PM-DOS's console hands out a
   fresh key on every read, so a byte at a time would get the CR with no
   LF after it, and BACKSPACE could not take back a byte already given.
   A redirected script is read a byte at a time, so that it is not read
   past the line.  -1 at end of input */
static int get_line( char *buf, int max )
{
    static char conl[258];
    REGS    regs;
    int     n = 0, got, i;
    uint8_t ch;

    if ( stdin_is_console() ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x3F00;
        regs.ebx = 0;
        regs.ecx = sizeof( conl ) - 2;
        regs.edx = (uint32_t)conl;
        int21( &regs );
        got = CF( regs ) ? 0 : (int)( regs.eax & 0xFFFF );
        if ( got > (int)sizeof( conl ) - 2 ) got = (int)sizeof( conl ) - 2;
        for ( i = 0; i < got && conl[i] != '\r' && conl[i] != '\n'; i++ )
            if ( n < max - 1 ) buf[n++] = conl[i];
        buf[n] = 0;
        return n;
    }

    for ( ;; ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x3F00;
        regs.ebx = 0;
        regs.ecx = 1;
        regs.edx = (uint32_t)&ch;
        int21( &regs );
        if ( CF( regs ) || ( regs.eax & 0xFFFF ) == 0 ) { buf[n] = 0; return n ? n : -1; }
        if ( ch == '\n' ) break;
        if ( ch != '\r' && n < max - 1 ) buf[n++] = (char)ch;
    }
    buf[n] = 0;
    return n;
}

/* a password: from the keyboard without echo, a key at a time (ENTER
   ends it, BACKSPACE takes one back); from a redirected file as a line,
   as get_line reads it.  -1 at end of input */
static int get_secret( char *buf, int max )
{
    int  n = 0, key;

    if ( !stdin_is_console() ) return get_line( buf, max );

    for ( ;; ) {
        key = sl_getkey();
        if ( key == '\r' ) break;
        if ( key == 8 ) { if ( n > 0 ) n--; continue; }
        if ( key >= 0x100 || key < ' ' ) continue;      /* arrows and the like */
        if ( n < max - 1 ) buf[n++] = (char)key;
    }
    buf[n] = 0;
    sl_puts( "\r\n" );
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

/* "/USER:name" or "-USER:name", any case: the name, else NULL */
static const char *user_opt( const char *arg )
{
    static const char opt[] = "USER:";
    int i;

    if ( arg[0] != '/' && arg[0] != '-' ) return NULL;
    for ( i = 0; opt[i]; i++ ) {
        char ch = arg[i + 1];
        if ( ch >= 'a' && ch <= 'z' ) ch = (char)( ch - 32 );
        if ( ch != opt[i] ) return NULL;
    }
    return arg + 1 + i;
}

/* a password word: * (or none) means ask */
static int ask_for( const char *word )
{
    return word == NULL || word[0] == 0 || ( word[0] == '*' && word[1] == 0 );
}

void __cdecl dos_entry( void )
{
    char **argv, *verb, *a1, *a2;
    char cmd[1024];
    const char *host = NULL, *name;
    int argc, i;

    argc = sl_args( &argv );
    for ( i = 0; i < argc; i++ ) {
        if ( argv[i][0] == '/' && argv[i][1] == '?' ) {
            sl_puts( "FTP [host] [/USER:name [password | *]] - copy files to and from an FTP server.\r\n" );
            sl_puts( "  A * or no password asks for the password.\r\n" );
            sl_flush();
            return;
        }
        name = user_opt( argv[i] );
        if ( name ) {
            if ( name[0] == 0 ) { sl_puts( "FTP: /USER: needs a name.\r\n" ); sl_flush(); return; }
            sl_strcpy( cl_user, name, sizeof( cl_user ) );
            if ( i + 1 < argc && argv[i + 1][0] != '/' && argv[i + 1][0] != '-' ) {
                i++;
                cl_ask = ask_for( argv[i] );
                if ( !cl_ask ) sl_strcpy( cl_pass, argv[i], sizeof( cl_pass ) );
            }
        } else if ( argv[i][0] == '/' || argv[i][0] == '-' ) {
            sl_puts( "FTP: unknown switch " ); sl_puts( argv[i] ); sl_puts( "\r\n" );
            sl_flush();
            return;
        } else if ( host == NULL ) {
            host = argv[i];
        }
    }

    if ( !sl_up( 800 ) ) { sl_puts( "FTP: the network is not up.\r\n" ); sl_flush(); return; }
    sl_puts( "PM-DOS FTP.  Type help for the commands, bye to leave.\r\n" );

    if ( host ) do_open( host, 21 );

    for ( ;; ) {
        sl_puts( "ftp> " ); sl_flush();
        if ( get_line( cmd, sizeof( cmd ) ) < 0 ) { sl_puts( "\r\n" ); break; }
        split( cmd, &verb, &a1, &a2 );
        if ( *verb == 0 ) continue;

        if ( !sl_strcasecmp( verb, "bye" ) || !sl_strcasecmp( verb, "quit" ) || !sl_strcasecmp( verb, "exit" ) ) {
            do_close();
            break;
        } else if ( !sl_strcasecmp( verb, "open" ) ) {
            uint16_t port = ( *a2 ) ? (uint16_t)sl_atou( a2 ) : 21;
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
            if ( !ctrl ) sl_puts( "Not connected.\r\n" );
            else if ( *a1 == 0 ) sl_puts( "user name [password | *]\r\n" );
            else login( a1, a2, ask_for( a2 ) );
        } else if ( !sl_strcasecmp( verb, "help" ) || *verb == '?' ) {
            sl_puts( "open close user ls dir get put cd pwd ascii binary bye\r\n" );
        } else {
            sl_puts( "?Invalid command.  Type help.\r\n" );
        }
    }
    sl_puts( "Goodbye.\r\n" );
    sl_flush();
}
