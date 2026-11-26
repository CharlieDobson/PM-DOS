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
 *
 * THE PASSWORD LIST (2026-11-11).  Typing the password at every NET USE,
 * to two domains, was the next thing to go:
 *
 *   NET LOGON [name [password | *]] [/YES]
 *   NET LOGOFF [/YES]
 *   NET PASSWORD [/LIST | /DELETE:domain-or-server]
 *   NET USE ... [/PERSISTENT:YES|NO] [/SAVEPW:NO] [/YES | /NO]
 *
 * NET LOGON opens NAME.PWL, beside NET.EXE, with a password of its own
 * and reconnects the drives the list remembers; NET USE with no password
 * then takes the saved one, and offers to save one it had to ask for.
 * The file is encrypted, and only SMBFS.SYS ever sees inside it - this
 * program reads and writes the file and asks for passwords, and every
 * change comes back from the driver already encrypted (DRV\SMBFS\PWL.C,
 * INC\REDIR.H pwl_req).  NET LOGOFF disconnects the network drives and
 * has the driver forget the list.
 */
#include "socklib.h"
#include "redir.h"

static int call_netuse( unsigned int sub, netuse_req *req )
{
    REGS regs;
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = ( sub << 16 ) | 0xF127UL;
    regs.ecx = sizeof( netuse_req );
    regs.edi = (uint32_t)req;
    int21( &regs );
    if ( CF( regs ) ) return (int)( regs.eax & 0xFFFF );
    return 0;
}

static void put_hex32( uint32_t value )
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
static void say_logon( uint32_t srvstatus )
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
    case MNT_NOSAVED:
        sl_puts( "No password is saved for " );
        sl_puts( nu->domain[0] ? nu->domain : nu->server );
        sl_puts( ".  Give the password, or * to type it.\r\n" );
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

/* NOISE FOR THE DRIVER'S RANDOM NUMBERS (the password list's salt and
   nonce).  A 486 has no time-stamp counter, so the time between keys is
   measured the only way there is: a count that runs until the key comes.
   Each count and the clock are folded into the bytes handed over in every
   pwl_req; the driver hashes them into its own pool. */
static uint8_t  noise[64];
static unsigned noise_at;

static void add_noise( uint32_t value )
{
    int i;
    for ( i = 0; i < 4; i++ ) {
        noise[noise_at % sizeof( noise )] ^= (uint8_t)( value >> ( 8 * i ) );
        noise[( noise_at + 31 ) % sizeof( noise )] += (uint8_t)( value >> ( 8 * i ) );
        noise_at++;
    }
}

/* a key, with the count it took to come folded into the noise */
static int noisy_key( void )
{
    unsigned int spin = 0;
    while ( !sl_kbhit() ) spin++;
    add_noise( spin );
    add_noise( sl_now() );
    return sl_getkey();
}

/* the password for "*": from the keyboard without echo, a key at a time
   (ENTER ends it, BACKSPACE takes one back); from a redirected file, its
   first line.  Always NUL-terminated within max. */
static void get_secret( char *buf, int max )
{
    REGS    regs;
    int     n = 0, key;
    uint8_t ch;

    if ( stdin_is_console() ) {
        for ( ;; ) {
            key = noisy_key();
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
        regs.edx = (uint32_t)&ch;
        int21( &regs );
        if ( CF( regs ) || ( regs.eax & 0xFFFF ) == 0 ) break;
        add_noise( sl_now() );
        if ( ch == '\n' ) break;
        if ( ch != '\r' && n < max - 1 ) buf[n++] = (char)ch;
    }
    buf[n] = 0;
    sl_puts( "\r\n" );
}

/* a line typed with echo (a user name); from a redirected file, its first
   line.  NUL-terminated within max. */
static void get_line( char *buf, int max )
{
    REGS    regs;
    int     n = 0, key;
    uint8_t ch;

    if ( stdin_is_console() ) {
        for ( ;; ) {
            key = noisy_key();
            if ( key == '\r' ) break;
            if ( key == 8 ) {
                if ( n > 0 ) { n--; sl_puts( "\b \b" ); sl_flush(); }
                continue;
            }
            if ( key >= 0x100 || key < ' ' ) continue;
            if ( n < max - 1 ) { buf[n++] = (char)key; sl_putc( (char)key ); sl_flush(); }
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
        regs.edx = (uint32_t)&ch;
        int21( &regs );
        if ( CF( regs ) || ( regs.eax & 0xFFFF ) == 0 ) break;
        if ( ch == '\n' ) break;
        if ( ch != '\r' && n < max - 1 ) buf[n++] = (char)ch;
    }
    buf[n] = 0;
    sl_puts( "\r\n" );
}

/* Y or N, from the keyboard.  With no keyboard to ask - a redirected
   input - the answer is the one given, as a batch file means it. */
static int ask_yes( const char *question, int otherwise )
{
    int key;
    sl_puts( question );
    sl_puts( " (Y/N)? " );
    sl_flush();
    if ( !stdin_is_console() ) {
        sl_puts( otherwise ? "Y\r\n" : "N\r\n" );
        return otherwise;
    }
    for ( ;; ) {
        key = noisy_key();
        if ( key == 'y' || key == 'Y' ) { sl_puts( "Y\r\n" ); return 1; }
        if ( key == 'n' || key == 'N' || key == 27 ) { sl_puts( "N\r\n" ); return 0; }
    }
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
        req.drive = (uint32_t)drive;
        if ( call_netuse( NETUSE_QUERY, &req ) != 0 ) continue;
        if ( req.flags & NUF_INUSE ) {
            sl_putc( (char)( 'A' + drive ) );
            sl_puts( ": is a network drive.\r\n" );
            any = 1;
        }
    }
    if ( !any ) sl_puts( "No network drives are mapped.\r\n" );
}

/* ================================================================ */
/* the password list                                                 */
/* ================================================================ */

static pwl_req pq;              /* static, for req's reason */
static char    list_file[144];  /* NAME.PWL, beside NET.EXE */
static char    owner[32];       /* whose list is unlocked, or "" */

/* every field but the file */
static void pwl_reset( void )
{
    sl_memzero( &pq, (uint32_t)( (uint8_t *)pq.data - (uint8_t *)&pq ) );
}

/* a PWL_* call; pq.status says what the driver made of it.  1 when there
   is no driver to ask */
static int call_pwl( unsigned int op )
{
    REGS regs;
    pq.op = op;
    add_noise( sl_now() );
    sl_memcpy( pq.seed, noise, sizeof( pq.seed ) );
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = ( (uint32_t)NETUSE_PWL << 16 ) | 0xF127UL;
    regs.ecx = sizeof( pwl_req );
    regs.edi = (uint32_t)&pq;
    int21( &regs );
    /* the driver clears these; with no driver, nothing else would */
    sl_memzero( pq.pass, sizeof( pq.pass ) );
    sl_memzero( pq.newpass, sizeof( pq.newpass ) );
    sl_memzero( pq.secret, sizeof( pq.secret ) );
    return CF( regs ) ? 1 : 0;
}

/* is a list unlocked?  1 yes (owner says whose), 0 no, -1 no driver */
static int pwl_open( void )
{
    pwl_reset();
    owner[0] = 0;
    if ( call_pwl( PWL_STATUS ) ) return -1;
    if ( !pq.flags ) return 0;
    sl_strcpy( owner, pq.owner, sizeof( owner ) );
    return 1;
}

/* where NET.EXE was started from - the path DOS keeps behind the
   environment - with its backslash; C:\ if there is none */
static void own_dir( char *out, int max )
{
    REGS        regs;
    const char *env, *path;
    int         i, cut = -1;

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x6200;
    int21( &regs );
    env = *(const char **)( (uint8_t *)regs.ebx + 0x2C );
    out[0] = 0;
    if ( env != NULL ) {
        while ( *env ) { while ( *env ) env++; env++; }
        env++;                                  /* past the empty one */
        if ( env[0] == 1 && env[1] == 0 ) {     /* one string: the path */
            path = env + 2;
            for ( i = 0; i < max - 1 && path[i]; i++ ) {
                out[i] = path[i];
                if ( path[i] == '\\' || path[i] == ':' ) cut = i;
            }
            out[cut + 1] = 0;
        }
    }
    if ( out[0] == 0 ) sl_strcpy( out, "C:\\", (uint32_t)max );
}

/* NAME.PWL beside NET.EXE, the name made of the user's letters and digits,
   eight at most.  0 if the name has none. */
static int list_path( const char *name )
{
    char     file[13];
    int      i, len = 0;
    unsigned at;

    for ( i = 0; name[i] && len < 8; i++ ) {
        char ch = name[i];
        if ( ch >= 'a' && ch <= 'z' ) ch = (char)( ch - 32 );
        if ( ( ch >= 'A' && ch <= 'Z' ) || ( ch >= '0' && ch <= '9' ) || ch == '_' || ch == '-' )
            file[len++] = ch;
    }
    if ( len == 0 ) return 0;
    sl_strcpy( file + len, ".PWL", 5 );
    own_dir( list_file, (int)sizeof( list_file ) - 13 );
    at = sl_strlen( list_file );
    sl_strcpy( list_file + at, file, sizeof( list_file ) - at );
    return 1;
}

/* the file into pq.data; 0, or the DOS error (2: there is none) */
static int file_load( void )
{
    REGS     regs;
    uint32_t handle;

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3D00;
    regs.edx = (uint32_t)list_file;
    int21( &regs );
    if ( CF( regs ) ) return (int)( regs.eax & 0xFFFF );
    handle = regs.eax & 0xFFFF;
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3F00;
    regs.ebx = handle;
    regs.ecx = sizeof( pq.data );
    regs.edx = (uint32_t)pq.data;
    int21( &regs );
    pq.len = CF( regs ) ? 0 : ( regs.eax & 0xFFFF );
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3E00;
    regs.ebx = handle;
    int21( &regs );
    return 0;
}

/* pq.data written as the file.  Written whole to NAME.$$$ first and only
   then renamed over the old one, so a failure part way leaves the old
   list as it was.  1 done. */
static int file_save( void )
{
    REGS     regs;
    char     temp[sizeof( list_file )];
    uint32_t handle;
    unsigned len = sl_strlen( list_file );
    int      ok;

    sl_strcpy( temp, list_file, sizeof( temp ) );
    sl_strcpy( temp + len - 3, "$$$", 4 );      /* NAME.PWL -> NAME.$$$ */
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3C00;
    regs.edx = (uint32_t)temp;
    int21( &regs );
    if ( CF( regs ) ) return 0;
    handle = regs.eax & 0xFFFF;
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x4000;
    regs.ebx = handle;
    regs.ecx = pq.len;
    regs.edx = (uint32_t)pq.data;
    int21( &regs );
    ok = !CF( regs ) && ( regs.eax & 0xFFFF ) == pq.len;
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3E00;
    regs.ebx = handle;
    int21( &regs );
    if ( CF( regs ) ) ok = 0;
    if ( ok ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x4100;
        regs.edx = (uint32_t)list_file;
        int21( &regs );                         /* none the first time */
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x5600;
        regs.edx = (uint32_t)temp;
        regs.edi = (uint32_t)list_file;
        int21( &regs );
        if ( !CF( regs ) ) return 1;
    }
    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x4100;
    regs.edx = (uint32_t)temp;
    int21( &regs );
    return 0;
}

/* after a change the driver made: the new file onto the disk */
static int save_list( void )
{
    if ( file_save() ) return 1;
    sl_puts( "The password list could not be written to " );
    sl_puts( list_file );
    sl_puts( ".\r\n" );
    return 0;
}

static void say_pwl( unsigned int status )
{
    switch ( status ) {
    case PWLE_LOCKED:
        sl_puts( "No one is logged on.  Use NET LOGON first.\r\n" ); break;
    case PWLE_BADPASS:
        sl_puts( "The password is not right, or the password list has been changed.\r\n" ); break;
    case PWLE_BADFILE:
        sl_puts( list_file ); sl_puts( " is not a password list this version of PM-DOS can read.\r\n" ); break;
    case PWLE_FULL:
        sl_puts( "The password list is full.  Remove a saved password with NET PASSWORD /DELETE:.\r\n" ); break;
    case PWLE_MEMORY:
        sl_puts( "There is not enough memory for the password list.\r\n" ); break;
    case PWLE_OWNER:
        sl_puts( list_file ); sl_puts( " belongs to another user.\r\n" ); break;
    default:
        sl_puts( "The password list could not be used.\r\n" ); break;
    }
}

static void put_unc( const char *server, const char *share )
{
    sl_puts( "\\\\" );
    sl_puts( server );
    sl_putc( '\\' );
    sl_puts( share );
}

/* the drives the list remembers, connected again with its passwords */
static void reconnect( void )
{
    unsigned int index;
    int rc;

    for ( index = 0; ; index++ ) {
        pwl_reset();
        pq.index = index;
        if ( call_pwl( PWL_GETMAP ) || pq.status != PWLE_OK ) break;
        sl_memzero( &req, sizeof( req ) );
        req.drive = pq.drive;
        req.flags = NUF_SAVED | ( pq.flags & NUF_SMB1 );
        sl_strcpy( req.server, pq.server, sizeof( req.server ) );
        sl_strcpy( req.share, pq.share, sizeof( req.share ) );
        sl_strcpy( req.user, pq.user, sizeof( req.user ) );
        sl_strcpy( req.domain, pq.domain, sizeof( req.domain ) );
        sl_putc( (char)( 'A' + pq.drive ) );
        if ( (int)pq.drive >= letter_count() ) {
            sl_puts( ": is past LASTDRIVE= in CONFIG.SYS, so " );
            put_unc( req.server, req.share );
            sl_puts( " was not connected.\r\n" );
            continue;
        }
        rc = call_netuse( NETUSE_MAP, &req );
        if ( rc == 0 ) {
            sl_puts( ": is now " );
            put_unc( req.server, req.share );
            sl_puts( "\r\n" );
            continue;
        }
        sl_puts( ": " );
        put_unc( req.server, req.share );
        sl_puts( " was not connected.\r\n" );
        say_mapfail( rc, &req );
    }
}

/* NET LOGON [name [password | *]] [/YES] */
static void do_logon( int argc, char **argv )
{
    const char *name = NULL, *pass = NULL;
    static char typed[32], first[64], again[64];
    int         i, yes = 0, rc;

    for ( i = 1; i < argc; i++ ) {
        if ( sl_strcasecmp( argv[i], "/YES" ) == 0 || sl_strcasecmp( argv[i], "/Y" ) == 0 ) yes = 1;
        else if ( argv[i][0] == '/' ) {
            sl_puts( "Invalid switch - " ); sl_puts( argv[i] ); sl_puts( "\r\n" );
            return;
        } else if ( name == NULL ) name = argv[i];
        else pass = argv[i];
    }

    rc = pwl_open();
    if ( rc < 0 ) { sl_puts( "No network redirector found.\r\n" ); return; }
    if ( rc > 0 ) {
        sl_puts( owner );
        if ( name == NULL || sl_strcasecmp( name, owner ) == 0 ) sl_puts( " is already logged on.\r\n" );
        else sl_puts( " is logged on.  Use NET LOGOFF first.\r\n" );
        return;
    }
    if ( name == NULL ) {
        sl_puts( "User name: " );
        sl_flush();
        get_line( typed, sizeof( typed ) );
        if ( typed[0] == 0 ) return;
        name = typed;
    }
    if ( sl_strlen( name ) >= sizeof( pq.owner ) ) {
        sl_puts( "The user name is too long: 31 characters at most.\r\n" );
        return;
    }
    if ( !list_path( name ) ) {
        sl_puts( "A user name needs at least one letter or digit.\r\n" );
        return;
    }

    pwl_reset();
    rc = file_load();
    if ( rc == 2 || rc == 3 ) {
        /* a new list */
        sl_puts( "There is no password list for " ); sl_puts( name ); sl_puts( ".\r\n" );
        if ( !yes && !ask_yes( "Create one", 0 ) ) return;
        if ( pass && !( pass[0] == '*' && pass[1] == 0 ) ) {
            sl_strcpy( first, pass, sizeof( first ) );
        } else {
            sl_puts( "Type a password for the list: " );
            sl_flush();
            get_secret( first, sizeof( first ) );
            sl_puts( "Type the password again: " );
            sl_flush();
            get_secret( again, sizeof( again ) );
            if ( sl_strcmp( first, again ) != 0 ) {
                sl_memzero( first, sizeof( first ) );
                sl_memzero( again, sizeof( again ) );
                sl_puts( "The two passwords are not the same.  The list was not made.\r\n" );
                return;
            }
        }
        pwl_reset();
        sl_strcpy( pq.owner, name, sizeof( pq.owner ) );
        sl_strcpy( pq.pass, first, sizeof( pq.pass ) );
        sl_memzero( first, sizeof( first ) );
        sl_memzero( again, sizeof( again ) );
        if ( call_pwl( PWL_CREATE ) ) { sl_puts( "No network redirector found.\r\n" ); return; }
        if ( pq.status != PWLE_OK ) { say_pwl( pq.status ); return; }
        if ( !save_list() ) {
            pwl_reset();
            call_pwl( PWL_LOCK );
            return;
        }
        sl_puts( "A password list for " ); sl_puts( name ); sl_puts( " is made in " );
        sl_puts( list_file ); sl_puts( ".\r\n" );
        sl_puts( name ); sl_puts( " is logged on.\r\n" );
        return;
    }
    if ( rc != 0 ) {
        sl_puts( list_file ); sl_puts( " could not be read.\r\n" );
        return;
    }

    if ( pass && !( pass[0] == '*' && pass[1] == 0 ) ) {
        sl_strcpy( pq.pass, pass, sizeof( pq.pass ) );
    } else {
        sl_puts( "Password for " ); sl_puts( name ); sl_puts( "'s password list: " );
        sl_flush();
        get_secret( pq.pass, sizeof( pq.pass ) );
    }
    sl_strcpy( pq.owner, name, sizeof( pq.owner ) );
    if ( call_pwl( PWL_UNLOCK ) ) { sl_puts( "No network redirector found.\r\n" ); return; }
    if ( pq.status != PWLE_OK ) { say_pwl( pq.status ); return; }
    sl_puts( name ); sl_puts( " is logged on.\r\n" );
    reconnect();
}

/* NET LOGOFF [/YES] */
static void do_logoff( int argc, char **argv )
{
    int drive, count = letter_count(), yes = 0, i, any = 0;

    for ( i = 1; i < argc; i++ ) {
        if ( sl_strcasecmp( argv[i], "/YES" ) == 0 || sl_strcasecmp( argv[i], "/Y" ) == 0 ) yes = 1;
        else { sl_puts( "Invalid switch - " ); sl_puts( argv[i] ); sl_puts( "\r\n" ); return; }
    }
    i = pwl_open();
    if ( i < 0 ) { sl_puts( "No network redirector found.\r\n" ); return; }
    if ( i == 0 ) { sl_puts( "No one is logged on.\r\n" ); return; }

    /* the connections go too: they were made with the list's passwords,
       and logging off is the way to leave the machine without them */
    for ( drive = 0; drive < count; drive++ ) {
        sl_memzero( &req, sizeof( req ) );
        req.drive = (uint32_t)drive;
        if ( call_netuse( NETUSE_QUERY, &req ) != 0 || !( req.flags & NUF_INUSE ) ) continue;
        if ( !any ) sl_puts( "These network drives will be disconnected:" );
        sl_putc( ' ' );
        sl_putc( (char)( 'A' + drive ) );
        sl_putc( ':' );
        any = 1;
    }
    if ( any ) {
        sl_puts( "\r\n" );
        if ( !yes && !ask_yes( "Continue", 1 ) ) return;
        for ( drive = 0; drive < count; drive++ ) {
            sl_memzero( &req, sizeof( req ) );
            req.drive = (uint32_t)drive;
            if ( call_netuse( NETUSE_QUERY, &req ) != 0 || !( req.flags & NUF_INUSE ) ) continue;
            if ( call_netuse( NETUSE_UNMAP, &req ) == 5 ) {
                sl_putc( (char)( 'A' + drive ) );
                sl_puts( ": stays connected: an image file on the drive is mounted.\r\n" );
            }
        }
    }
    pwl_reset();
    call_pwl( PWL_LOCK );
    sl_puts( owner ); sl_puts( " is logged off.\r\n" );
}

/* NET PASSWORD [name] | /LIST | /DELETE:domain-or-server */
static void do_password( int argc, char **argv )
{
    const char *name = NULL, *forget = NULL, *val;
    static char typed[32], first[64], again[64];
    int         i, list = 0, open;
    unsigned    index;

    for ( i = 1; i < argc; i++ ) {
        if ( sl_strcasecmp( argv[i], "/LIST" ) == 0 || sl_strcasecmp( argv[i], "/L" ) == 0 ) list = 1;
        else if ( match_opt( argv[i], "/DELETE:", &val ) || match_opt( argv[i], "/D:", &val ) ) forget = val;
        else if ( argv[i][0] == '/' ) {
            sl_puts( "Invalid switch - " ); sl_puts( argv[i] ); sl_puts( "\r\n" );
            return;
        } else name = argv[i];
    }
    open = pwl_open();
    if ( open < 0 ) { sl_puts( "No network redirector found.\r\n" ); return; }

    if ( list ) {
        if ( !open ) { say_pwl( PWLE_LOCKED ); return; }
        sl_puts( "Passwords saved in " ); sl_puts( owner ); sl_puts( "'s list:\r\n" );
        for ( index = 0; ; index++ ) {
            pwl_reset();
            pq.index = index;
            if ( call_pwl( PWL_GETCRED ) || pq.status != PWLE_OK ) break;
            sl_puts( "  " ); sl_puts( pq.target );
            if ( pq.user[0] ) { sl_puts( ", as " ); sl_puts( pq.user ); }
            sl_puts( "\r\n" );
        }
        if ( index == 0 ) sl_puts( "  none\r\n" );
        sl_puts( "Drives NET LOGON connects:\r\n" );
        for ( index = 0; ; index++ ) {
            pwl_reset();
            pq.index = index;
            if ( call_pwl( PWL_GETMAP ) || pq.status != PWLE_OK ) break;
            sl_puts( "  " ); sl_putc( (char)( 'A' + pq.drive ) ); sl_puts( ": " );
            put_unc( pq.server, pq.share );
            if ( pq.user[0] ) {
                sl_puts( ", as " );
                if ( pq.domain[0] ) { sl_puts( pq.domain ); sl_putc( '\\' ); }
                sl_puts( pq.user );
            }
            sl_puts( "\r\n" );
        }
        if ( index == 0 ) sl_puts( "  none\r\n" );
        return;
    }

    if ( forget ) {
        if ( !open ) { say_pwl( PWLE_LOCKED ); return; }
        list_path( owner );
        pwl_reset();
        sl_strcpy( pq.target, forget, sizeof( pq.target ) );
        if ( call_pwl( PWL_DELCRED ) ) return;
        if ( pq.status == PWLE_NOTFOUND ) {
            sl_puts( "No password is saved for " ); sl_puts( forget ); sl_puts( ".\r\n" );
            return;
        }
        if ( pq.status != PWLE_OK ) { say_pwl( pq.status ); return; }
        if ( save_list() ) { sl_puts( "The password for " ); sl_puts( forget ); sl_puts( " is no longer saved.\r\n" ); }
        return;
    }

    /* a new password for the list */
    if ( open ) name = owner;
    else if ( name == NULL ) {
        sl_puts( "User name: " );
        sl_flush();
        get_line( typed, sizeof( typed ) );
        if ( typed[0] == 0 ) return;
        name = typed;
    }
    if ( !list_path( name ) ) {
        sl_puts( "A user name needs at least one letter or digit.\r\n" );
        return;
    }
    pwl_reset();
    if ( file_load() != 0 ) {
        sl_puts( "There is no password list for " ); sl_puts( name ); sl_puts( ".\r\n" );
        return;
    }
    sl_puts( "Old password: " );
    sl_flush();
    get_secret( pq.pass, sizeof( pq.pass ) );
    sl_puts( "New password: " );
    sl_flush();
    get_secret( first, sizeof( first ) );
    sl_puts( "Type the new password again: " );
    sl_flush();
    get_secret( again, sizeof( again ) );
    if ( sl_strcmp( first, again ) != 0 ) {
        sl_memzero( pq.pass, sizeof( pq.pass ) );
        sl_memzero( first, sizeof( first ) );
        sl_memzero( again, sizeof( again ) );
        sl_puts( "The two new passwords are not the same.  Nothing was changed.\r\n" );
        return;
    }
    sl_strcpy( pq.owner, name, sizeof( pq.owner ) );
    sl_strcpy( pq.newpass, first, sizeof( pq.newpass ) );
    sl_memzero( first, sizeof( first ) );
    sl_memzero( again, sizeof( again ) );
    if ( call_pwl( PWL_CHANGE ) ) return;
    if ( pq.status != PWLE_OK ) { say_pwl( pq.status ); return; }
    if ( save_list() ) sl_puts( "The password list's password is changed.\r\n" );
    if ( !open ) {                              /* the change opened the list */
        pwl_reset();
        call_pwl( PWL_LOCK );
    }
}

static void usage( void )
{
    sl_puts( "usage: NET USE K: \\\\server\\share [password | *] [/USER:[domain\\]name] [/SMB1]\r\n"
             "               [/PERSISTENT:YES|NO] [/SAVEPW:NO] [/YES | /NO]\r\n"
             "       NET USE K: /DELETE\r\n"
             "       NET USE\r\n"
             "       NET LOGON [name [password | *]] [/YES]\r\n"
             "       NET LOGOFF [/YES]\r\n"
             "       NET PASSWORD [name | /LIST | /DELETE:domain-or-server]\r\n" );
}

/* NET USE */
static void do_use( int argc, char **argv )
{
    int          i;
    const char  *val;
    const char  *unc = NULL, *user = NULL, *pass = NULL, *who;
    int          drive = -1, del = 0, smb1 = 0;
    int          persist = 0;           /* 1 /PERSISTENT:YES, -1 :NO, 0 neither */
    int          savepw = 1;            /* 0 /SAVEPW:NO */
    int          answer = 0;            /* 1 /YES, -1 /NO, 0 ask */
    int          logged, typed = 0, rc;
    static char  secret[64];            /* the password typed or given, kept
                                           until it has been offered for the list */

    if ( argc == 1 ) { list_drives(); return; }

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
        } else if ( match_opt( arg, "/PERSISTENT:", &val ) || match_opt( arg, "/P:", &val ) ) {
            if ( sl_strcasecmp( val, "YES" ) == 0 || sl_strcasecmp( val, "Y" ) == 0 ) persist = 1;
            else if ( sl_strcasecmp( val, "NO" ) == 0 || sl_strcasecmp( val, "N" ) == 0 ) persist = -1;
            else { sl_puts( "Use /PERSISTENT:YES or /PERSISTENT:NO.\r\n" ); return; }
        } else if ( sl_strcasecmp( arg, "/SAVEPW:NO" ) == 0 ) {
            savepw = 0;
        } else if ( sl_strcasecmp( arg, "/YES" ) == 0 || sl_strcasecmp( arg, "/Y" ) == 0 ) {
            answer = 1;
        } else if ( sl_strcasecmp( arg, "/NO" ) == 0 || sl_strcasecmp( arg, "/N" ) == 0 ) {
            answer = -1;
        } else if ( arg[0] == '/' ) {
            sl_puts( "Invalid switch - " );
            sl_puts( arg );
            sl_puts( "\r\n" );
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
        return;
    }

    logged = ( pwl_open() > 0 );
    if ( logged ) list_path( owner );

    sl_memzero( &req, sizeof( req ) );
    req.drive = (uint32_t)drive;

    if ( del ) {
        rc = call_netuse( NETUSE_UNMAP, &req );
        if ( rc == 0 ) { sl_putc( (char)( 'A' + drive ) ); sl_puts( ": disconnected.\r\n" ); }
        else if ( rc == 5 ) {
            /* the kernel keeps a share while an image is mounted from it */
            sl_puts( "An image file on " ); sl_putc( (char)( 'A' + drive ) );
            sl_puts( ": is mounted.  Unmount the image with MOUNT /U first.\r\n" );
            return;
        }
        else say_err( rc );
        /* and NET LOGON no longer makes it, as NT forgets a persistent
           connection that is deleted */
        if ( logged ) {
            pwl_reset();
            pq.drive = (uint32_t)drive;
            if ( !call_pwl( PWL_DELMAP ) && pq.status == PWLE_OK && save_list() ) {
                sl_putc( (char)( 'A' + drive ) );
                sl_puts( ": will no longer be connected by NET LOGON.\r\n" );
            }
        }
        return;
    }

    if ( unc == NULL ) {
        sl_puts( "Specify the share, like \\\\server\\share.\r\n" );
        return;
    }
    if ( !parse_unc( unc, req.server, sizeof( req.server ), req.share, sizeof( req.share ) ) ) {
        sl_puts( "The share name should look like \\\\server\\share.\r\n" );
        return;
    }
    who = user;
    if ( user ) {
        /* /USER:domain\name - the domain is whatever comes before the
           first backslash */
        unsigned int split;
        for ( split = 0; user[split] && user[split] != '\\'; split++ ) ;
        if ( user[split] == '\\' ) {
            if ( split >= sizeof( req.domain ) || user[split + 1] == 0 ) {
                sl_puts( "The user name should look like domain\\name or name.\r\n" );
                return;
            }
            sl_strcpy( req.domain, user, split + 1 );
            user += split + 1;
        }
        sl_strcpy( req.user, user, sizeof( req.user ) );
    }
    if ( smb1 ) req.flags |= NUF_SMB1;

    /* NO PASSWORD, AND A LIST IS OPEN: the saved one.  If the list has
       none for this domain or server, it is asked for - and offered to
       the list once it has worked. */
    rc = -1;
    if ( pass == NULL && logged ) {
        req.flags |= NUF_SAVED;
        rc = call_netuse( NETUSE_MAP, &req );
        req.flags &= ~(uint32_t)NUF_SAVED;
        if ( rc != 0 && req.step == MNT_NOSAVED ) {
            if ( !stdin_is_console() ) {
                say_mapfail( rc, &req );
                return;
            }
            pass = "*";
            rc = -1;
        } else if ( rc != 0 ) {
            say_mapfail( rc, &req );
            return;
        }
    }
    if ( rc != 0 ) {
        if ( pass && pass[0] == '*' && pass[1] == 0 ) {
            sl_puts( "Password for " );
            if ( who ) { sl_puts( who ); sl_puts( " on " ); }
            sl_puts( unc );
            sl_puts( ": " );
            sl_flush();
            get_secret( req.pass, sizeof( req.pass ) );
            typed = 1;
        } else if ( pass ) {
            sl_strcpy( req.pass, pass, sizeof( req.pass ) );
            typed = 1;
        }
        sl_strcpy( secret, req.pass, sizeof( secret ) );
        req.step = 0;
        req.srvstatus = 0;
        rc = call_netuse( NETUSE_MAP, &req );
        sl_memzero( req.pass, sizeof( req.pass ) );     /* the kernel has its copy */
    }
    if ( rc != 0 ) {
        sl_memzero( secret, sizeof( secret ) );
        say_mapfail( rc, &req );
        return;
    }
    sl_putc( (char)( 'A' + drive ) );
    sl_puts( ": is now " );
    sl_puts( unc );
    sl_puts( "\r\n" );

    /* a password that had to be given, offered to the open list */
    if ( typed && logged && savepw && secret[0] && answer >= 0 ) {
        if ( answer > 0 || ask_yes( "Save this password in your password list", 0 ) ) {
            pwl_reset();
            sl_strcpy( pq.target, req.domain[0] ? req.domain : req.server, sizeof( pq.target ) );
            sl_strcpy( pq.user, req.user, sizeof( pq.user ) );
            sl_strcpy( pq.secret, secret, sizeof( pq.secret ) );
            if ( !call_pwl( PWL_ADDCRED ) ) {
                if ( pq.status != PWLE_OK ) say_pwl( pq.status );
                else if ( save_list() ) sl_puts( "The password is saved.\r\n" );
            }
        }
    }
    sl_memzero( secret, sizeof( secret ) );

    /* remembered for NET LOGON, or forgotten - never the password */
    if ( persist > 0 ) {
        if ( !logged ) {
            sl_puts( "Not remembered: no one is logged on.  Use NET LOGON first.\r\n" );
            return;
        }
        pwl_reset();
        pq.drive = (uint32_t)drive;
        pq.flags = req.flags & NUF_SMB1;
        sl_strcpy( pq.server, req.server, sizeof( pq.server ) );
        sl_strcpy( pq.share, req.share, sizeof( pq.share ) );
        sl_strcpy( pq.user, req.user, sizeof( pq.user ) );
        sl_strcpy( pq.domain, req.domain, sizeof( pq.domain ) );
        if ( call_pwl( PWL_ADDMAP ) ) return;
        if ( pq.status != PWLE_OK ) { say_pwl( pq.status ); return; }
        if ( save_list() ) {
            sl_putc( (char)( 'A' + drive ) );
            sl_puts( ": will be connected again by NET LOGON.\r\n" );
        }
    } else if ( persist < 0 && logged ) {
        pwl_reset();
        pq.drive = (uint32_t)drive;
        if ( !call_pwl( PWL_DELMAP ) && pq.status == PWLE_OK && save_list() ) {
            sl_putc( (char)( 'A' + drive ) );
            sl_puts( ": will no longer be connected by NET LOGON.\r\n" );
        }
    }
}

void __cdecl dos_entry( void )
{
    char **argv;
    int    argc;

    argc = sl_args( &argv );
    if ( argc >= 1 && sl_strcasecmp( argv[0], "USE" ) == 0 ) do_use( argc, argv );
    else if ( argc >= 1 && sl_strcasecmp( argv[0], "LOGON" ) == 0 ) do_logon( argc, argv );
    else if ( argc >= 1 && sl_strcasecmp( argv[0], "LOGOFF" ) == 0 ) do_logoff( argc, argv );
    else if ( argc >= 1 && sl_strcasecmp( argv[0], "PASSWORD" ) == 0 ) do_password( argc, argv );
    else usage();
    sl_flush();
}
