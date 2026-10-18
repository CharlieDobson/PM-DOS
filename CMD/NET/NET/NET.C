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
 *   NET USE K: \\server\share [/USER:name] [password] [/DOMAIN:dom] [/SMB1]
 *   NET USE K: /DELETE
 *   NET USE                 (list the mapped drives)
 *
 * /SMB1 asks for the down-level SMB1 client (NetBIOS over TCP, port 139)
 * instead of the SMB2/3 default, for Win9x/NT/OS-2/Samba-NT1 peers.
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

static void say_err( int code )
{
    switch ( code ) {
    case 85:   sl_puts( "That drive is already in use.\r\n" ); break;
    case 15:   sl_puts( "That is not a valid drive letter.\r\n" ); break;
    case 2:
    case 3:    sl_puts( "The server or share could not be found.\r\n" ); break;
    default:   sl_puts( "The connection could not be made - check the name, "
                        "the user and the password.\r\n" ); break;
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

static netuse_req req;          /* static: a native program's buffer the kernel
                                   checks must lie in its slot, not up its stack */

static void list_drives( void )
{
    int drive, any = 0;
    for ( drive = 0; drive < 16; drive++ ) {
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
    const char  *unc = NULL, *user = NULL, *pass = NULL, *domain = NULL;
    int          drive = -1, del = 0, smb1 = 0;
    int          rc;

    argc = sl_args( &argv );
    if ( argc < 1 || sl_strcasecmp( argv[0], "USE" ) != 0 ) {
        sl_puts( "usage: NET USE K: \\\\server\\share [/USER:name] [password] [/DOMAIN:dom] [/SMB1]\r\n"
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
        } else if ( match_opt( arg, "/DOMAIN:", &val ) || match_opt( arg, "/D:", &val ) ) {
            domain = val;
        } else if ( sl_strcasecmp( arg, "/DELETE" ) == 0 || sl_strcasecmp( arg, "/D" ) == 0 ) {
            del = 1;
        } else if ( sl_strcasecmp( arg, "/SMB1" ) == 0 ) {
            smb1 = 1;
        } else if ( arg[0] != '/' ) {
            pass = arg;                         /* a bare word is the password */
        }
    }

    if ( drive < 0 || drive >= 16 ) {
        sl_puts( "Name the drive letter, like K:.\r\n" );
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
        sl_puts( "Name the share, like \\\\server\\share.\r\n" );
        sl_flush();
        return;
    }
    if ( !parse_unc( unc, req.server, sizeof( req.server ), req.share, sizeof( req.share ) ) ) {
        sl_puts( "The share name should look like \\\\server\\share.\r\n" );
        sl_flush();
        return;
    }
    if ( user )   sl_strcpy( req.user, user, sizeof( req.user ) );
    if ( pass )   sl_strcpy( req.pass, pass, sizeof( req.pass ) );
    if ( domain ) sl_strcpy( req.domain, domain, sizeof( req.domain ) );
    if ( smb1 )   req.flags |= NUF_SMB1;

    rc = call_netuse( NETUSE_MAP, &req );
    if ( rc == 0 ) {
        sl_putc( (char)( 'A' + drive ) );
        sl_puts( ": is now " );
        sl_puts( unc );
        sl_puts( "\r\n" );
    } else {
        say_err( rc );
    }
    sl_flush();
}
