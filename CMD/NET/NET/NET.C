/*
 * NET.C - NET.EXE: map a network share to a drive letter.
 *
 * A native PM-DOS command over the shared net runtime (CMD\NET\SOCKLIB
 * behind CMD\NET\NETSTART).  It does not speak SMB itself: it asks the
 * kernel, on 21h/F1h AL=27h (INC\REDIR.H netuse_req), to map a letter
 * to \\server\share, and the kernel's redirector driver (SMBFS.SYS)
 * does the SMB2/3 conversation.  From then on the letter is an ordinary
 * drive - DIR, TYPE, COPY and the rest work on it.
 *
 *   NET USE K: \\server\share [password | *] [/USER:[domain\]name] [/SMB1]
 *   NET USE K: /DELETE
 *   NET USE                 (list the mapped drives)
 *
 * A password of * asks for the password, read without echo (as FTP.EXE
 * reads one), so it is not left on the screen or in a batch file.
 *
 * /USER: takes the domain in front of the name, as NET USE does on
 * Windows NT; the separate /DOMAIN: switch went 2026-11-01.  /SMB1 asks
 * for the down-level SMB1 client (NetBIOS over TCP, port 139) instead
 * of the SMB2/3 default, for Win9x/NT/OS-2/Samba-NT1 peers.
 *
 * A map that fails names the step (REDIR.H MNT_*) and the server's
 * status: "could not be made" alone told the 5x86 nothing (2026-11-01).
 */
#include "socklib.h"
#include "redir.h"

static int call_netuse( u32 sub, netuse_req *req )
{
    REGS regs;
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = ( sub << 16 ) | 0xF127UL;
    regs.ecx = sizeof( netuse_req );
    regs.edi = (u32)req;
    int21( &regs );
    if ( CF( regs ) ) return (int)( regs.eax & 0xFFFF );
    return 0;
}

static void put_hex32( u32 value )
{
    int shift;
    for ( shift = 28; shift >= 0; shift -= 4 ) sl_putc( "0123456789ABCDEF"[( value >> shift ) & 15] );
}

static void say_err( int code )
{
    switch ( code ) {
    case 85:   sl_puts( "That drive is already in use.\r\n" ); break;
    case 15:   sl_puts( "That is not a valid drive letter.\r\n" ); break;
    case 2:
    case 3:    sl_puts( "The server or share could not be found.\r\n" ); break;
    default:   sl_puts( "The connection could not be made.\r\n" ); break;
    }
}

/* the sign-in, refused: the NTSTATUS says why */
static void say_logon( u32 srvstatus )
{
    switch ( srvstatus ) {
    case 0xC000006DUL:                  /* LOGON_FAILURE */
    case 0xC0000064UL:                  /* NO_SUCH_USER */
    case 0xC000006AUL:                  /* WRONG_PASSWORD */
        sl_puts( "The server did not accept the user name or password.\r\n" ); break;
    case 0xC0000072UL:
        sl_puts( "The account is disabled on the server.\r\n" ); break;
    case 0xC0000071UL:                  /* PASSWORD_EXPIRED */
    case 0xC0000224UL:                  /* PASSWORD_MUST_CHANGE */
        sl_puts( "The password has expired.  Change the password on the server.\r\n" ); break;
    case 0xC000006EUL:                  /* ACCOUNT_RESTRICTION */
        sl_puts( "The server refused the account.  A blank password is often not allowed\r\n"
                 "over the network.\r\n" ); break;
    case 0xC000015BUL:                  /* LOGON_TYPE_NOT_GRANTED */
        sl_puts( "The account is not allowed to sign in over the network.\r\n" ); break;
    case 0:
        sl_puts( "The server stopped answering during the sign-in.\r\n" ); break;
    default:
        sl_puts( "The server refused the sign-in.\r\n" ); break;
    }
}

static void say_mapfail( int code, const netuse_req *nu )
{
    switch ( nu->step ) {
    case MNT_NODRIVER:
        sl_puts( "No network redirector found.\r\n" );
        return;
    case MNT_MEMORY:
        sl_puts( "Insufficient memory to map network drive.\r\n" );
        return;
    case MNT_NONAME:
        sl_puts( "The server " ); sl_puts( nu->server ); sl_puts( " could not be found.\r\n" );
        return;
    case MNT_CONNECT:
        sl_puts( "The server " ); sl_puts( nu->server );
        sl_puts( " did not accept a connection on TCP port 445 or 139.\r\n" );
        return;
    case MNT_DIALECT:
        if ( nu->srvstatus == 0 ) sl_puts( "The server stopped answering while the SMB version was agreed.\r\n" );
        else                      sl_puts( "The server and PM-DOS have no SMB version in common.\r\n" );
        break;
    case MNT_NETBIOS:
        sl_puts( "The server refused the NetBIOS session.  Check the server name.\r\n" );
        break;
    case MNT_LOGON:
        say_logon( nu->srvstatus );
        break;
    case MNT_SHARE:
        if ( nu->srvstatus == 0xC00000CCUL ) {          /* BAD_NETWORK_NAME */
            sl_puts( "The share " ); sl_puts( nu->share ); sl_puts( " was not found on the server.\r\n" );
        } else if ( nu->srvstatus == 0xC0000022UL ) {   /* ACCESS_DENIED */
            sl_puts( "The server refused access to the share.\r\n" );
        } else if ( nu->srvstatus == 0 ) {
            sl_puts( "The server stopped answering while the share was opened.\r\n" );
        } else {
            sl_puts( "The share could not be opened.\r\n" );
        }
        break;
    default:
        say_err( code );
        return;
    }
    if ( nu->srvstatus ) {
        sl_puts( "Server status: " );
        put_hex32( nu->srvstatus );
        sl_puts( "\r\n" );
    }
}

/* \\server\share -> server and share */
static int parse_unc( const char *unc, char *server, int slen_server, char *share, int slen_share )
{
    int i;
    if ( unc[0] != '\\' || unc[1] != '\\' ) return 0;
    unc += 2;
    for ( i = 0; i + 1 < slen_server && unc[i] && unc[i] != '\\'; i++ ) server[i] = unc[i];
    server[i] = 0;
    if ( unc[i] != '\\' ) return 0;
    unc += i + 1;
    for ( i = 0; i + 1 < slen_share && unc[i] && unc[i] != '\\'; i++ ) share[i] = unc[i];
    share[i] = 0;
    return server[0] && share[0];
}

static int match_opt( const char *arg, const char *opt, const char **val )
{
    int i;
    for ( i = 0; opt[i]; i++ ) {
        char a = arg[i], b = opt[i];
        if ( a >= 'a' && a <= 'z' ) a = (char)( a - 32 );
        if ( a != b ) return 0;
    }
    *val = arg + i;
    return 1;
}

static int stdin_is_console( void )
{
    REGS regs;

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x4400;
    regs.ebx = 0;
    int21( &regs );
    return !CF( regs ) && ( regs.edx & 0x81 ) == 0x81;
}

/* the password for "*": from the keyboard without echo, a key at a time
   (ENTER ends it, BACKSPACE takes one back); from a redirected file, its
   first line.  Always NUL-terminated within max. */
static void get_secret( char *buf, int max )
{
    REGS regs;
    int  n = 0, key;
    u8   ch;

    if ( stdin_is_console() ) {
        for ( ;; ) {
            key = sl_getkey();
            if ( key == '\r' ) break;
            if ( key == 8 ) { if ( n > 0 ) n--; continue; }
            if ( key >= 0x100 || key < ' ' ) continue;  /* arrows and the like */
            if ( n < max - 1 ) buf[n++] = (char)key;
        }
        buf[n] = 0;
        sl_puts( "\r\n" );
        return;
    }
    for ( ;; ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x3F00;
        regs.ebx = 0;
        regs.ecx = 1;
        regs.edx = (u32)&ch;
        int21( &regs );
        if ( CF( regs ) || ( regs.eax & 0xFFFF ) == 0 ) break;
        if ( ch == '\n' ) break;
        if ( ch != '\r' && n < max - 1 ) buf[n++] = (char)ch;
    }
    buf[n] = 0;
    sl_puts( "\r\n" );
}

static netuse_req req;          /* static: a native program's buffer the kernel
                                   checks must lie in its slot, not up its stack */

/* How many letters there are: LASTDRIVE= in CONFIG.SYS, P: without the
   line.  0Eh answers the count; selecting the current drive changes
   nothing. */
static int letter_count( void )
{
    REGS regs;
    int  count;
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x1900UL;
    int21( &regs );
    regs.edx = regs.eax & 0xFF;
    regs.eax = 0x0E00UL;
    int21( &regs );
    count = (int)( regs.eax & 0xFF );
    if ( count < 1 || count > 26 ) count = 26;
    return count;
}

static void list_drives( void )
{
    int drive, any = 0, count = letter_count();
    for ( drive = 0; drive < count; drive++ ) {
        sl_memzero( &req, sizeof( req ) );
        req.drive = (u32)drive;
        if ( call_netuse( NETUSE_QUERY, &req ) != 0 ) continue;
        if ( req.flags & NUF_INUSE ) {
            sl_putc( (char)( 'A' + drive ) );
            sl_puts( ": is a network drive.\r\n" );
            any = 1;
        }
    }
    if ( !any ) sl_puts( "No network drives are mapped.\r\n" );
}

void __cdecl dos_entry( void )
{
    char       **argv;
    int          argc, i;
    const char  *verb, *val;
    const char  *unc = NULL, *user = NULL, *pass = NULL, *who;
    int          drive = -1, del = 0, smb1 = 0;
    int          rc;

    argc = sl_args( &argv );
    if ( argc < 1 || sl_strcasecmp( argv[0], "USE" ) != 0 ) {
        sl_puts( "usage: NET USE K: \\\\server\\share [password | *] [/USER:[domain\\]name] [/SMB1]\r\n"
                 "       NET USE K: /DELETE\r\n"
                 "       NET USE\r\n" );
        sl_flush();
        return;
    }
    verb = argv[0];
    (void)verb;

    if ( argc == 1 ) { list_drives(); sl_flush(); return; }

    for ( i = 1; i < argc; i++ ) {
        const char *arg = argv[i];
        if ( arg[0] && arg[1] == ':' && arg[2] == 0 ) {
            char letter = arg[0];
            if ( letter >= 'a' && letter <= 'z' ) letter = (char)( letter - 32 );
            drive = letter - 'A';
        } else if ( arg[0] == '\\' && arg[1] == '\\' ) {
            unc = arg;
        } else if ( match_opt( arg, "/USER:", &val ) || match_opt( arg, "/U:", &val ) ) {
            user = val;
        } else if ( sl_strcasecmp( arg, "/DELETE" ) == 0 || sl_strcasecmp( arg, "/D" ) == 0 ) {
            del = 1;
        } else if ( sl_strcasecmp( arg, "/SMB1" ) == 0 ) {
            smb1 = 1;
        } else if ( arg[0] == '/' ) {
            sl_puts( "Invalid switch - " );
            sl_puts( arg );
            sl_puts( "\r\n" );
            sl_flush();
            return;
        } else {
            pass = arg;                         /* a bare word is the password */
        }
    }

    /* A letter past LASTDRIVE= was given the same message as no letter
       at all, and NET USE S: on the 5x86 said "Name the drive letter"
       when one had been named (2026-11-01).  LASTDRIVE= decides how
       many letters there are since the same day; P: without the line. */
    if ( drive < 0 || drive >= 26 ) {
        sl_puts( "Specify the drive letter, like K:.\r\n" );
        sl_flush();
        return;
    }
    if ( drive >= letter_count() ) {
        char last = (char)( 'A' + letter_count() - 1 );
        sl_putc( (char)( 'A' + drive ) );
        sl_puts( ": is past LASTDRIVE=" );
        sl_putc( last );
        sl_puts( " in CONFIG.SYS.  Use a letter up to " );
        sl_putc( last );
        sl_puts( ":, or raise LASTDRIVE= and restart.\r\n" );
        sl_flush();
        return;
    }

    sl_memzero( &req, sizeof( req ) );
    req.drive = (u32)drive;

    if ( del ) {
        rc = call_netuse( NETUSE_UNMAP, &req );
        if ( rc == 0 ) { sl_putc( (char)( 'A' + drive ) ); sl_puts( ": disconnected.\r\n" ); }
        else say_err( rc );
        sl_flush();
        return;
    }

    if ( unc == NULL ) {
        sl_puts( "Specify the share, like \\\\server\\share.\r\n" );
        sl_flush();
        return;
    }
    if ( !parse_unc( unc, req.server, sizeof( req.server ), req.share, sizeof( req.share ) ) ) {
        sl_puts( "The share name should look like \\\\server\\share.\r\n" );
        sl_flush();
        return;
    }
    who = user;
    if ( user ) {
        /* /USER:domain\name - the domain is whatever comes before the
           first backslash */
        u32 split;
        for ( split = 0; user[split] && user[split] != '\\'; split++ ) ;
        if ( user[split] == '\\' ) {
            if ( split >= sizeof( req.domain ) || user[split + 1] == 0 ) {
                sl_puts( "The user name should look like domain\\name or name.\r\n" );
                sl_flush();
                return;
            }
            sl_strcpy( req.domain, user, split + 1 );
            user += split + 1;
        }
        sl_strcpy( req.user, user, sizeof( req.user ) );
    }
    if ( pass && pass[0] == '*' && pass[1] == 0 ) {
        sl_puts( "Password for " );
        if ( who ) { sl_puts( who ); sl_puts( " on " ); }
        sl_puts( unc );
        sl_puts( ": " );
        sl_flush();
        get_secret( req.pass, sizeof( req.pass ) );
    } else if ( pass ) {
        sl_strcpy( req.pass, pass, sizeof( req.pass ) );
    }
    if ( smb1 )   req.flags |= NUF_SMB1;

    rc = call_netuse( NETUSE_MAP, &req );
    sl_memzero( req.pass, sizeof( req.pass ) );     /* the kernel has its copy */
    if ( rc == 0 ) {
        sl_putc( (char)( 'A' + drive ) );
        sl_puts( ": is now " );
        sl_puts( unc );
        sl_puts( "\r\n" );
    } else {
        say_mapfail( rc, &req );
    }
    sl_flush();
}
