/*====================================================================
 * WINSWEEP.C - WINSWEEP.EXE, a Windows 3.1 session that runs a list of
 *              programs with nobody at the keyboard and keeps a picture
 *              of each one.
 *
 * Put in WIN.INI as  run=C:\TESTS\WINSWEEP.EXE  it reads C:\SWEEP.LST,
 * one program a line:
 *
 *     NAME|command line|seconds|keys
 *
 * (NAME?file is run only while the file does not exist - an installer,
 * once; NAME!file only once it does - what the installer made), makes
 * the program's own directory current (unless the command begins with
 * a *), starts the command the way Program Manager would (WinExec), sends
 * the keys, waits the seconds, writes the screen to C:\SHOTS\NAME.BMP,
 * notes what windows the program has, closes them (WM_CLOSE, and a
 * second picture if it is still there after that), and goes on to the
 * next line.  Everything it sees goes into C:\SWEEP.TXT, one line at a
 * time, each closed before the next.  At the end it pictures the bare
 * desktop and asks Windows to exit.
 *
 * KEYS, space separated, sent after a second's grace: ENTER, ESC, TAB,
 * SPACE, ALT-x (x a letter), Fn (F1..F12), Wn (wait n seconds), SHOT
 * (a picture now, NAME_1.BMP and so on) and "word" (typed).  They are
 * posted to the window that has the focus, so
 * they reach a dialog's modal loop the way the keyboard would; this is
 * enough for an installer's "Continue" and for a "Save changes?  No".
 *
 * The picture is the whole screen as the display driver has it: a DIB
 * at the screen's own depth (4 bits on VGA), written as a BMP.
 *
 * Built with Open Watcom:  wcl -zq -bt=windows -l=windows winsweep.c
 *===================================================================*/
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <direct.h>
#include <dos.h>

static unsigned drives;

#define LOGNAME     "C:\\SWEEP.TXT"
#define LISTNAME    "C:\\SWEEP.LST"
#define SHOTDIR     "C:\\SHOTS\\"
#define MAXLINE     200

static char     line[MAXLINE];
static char     item[MAXLINE];
static HINSTANCE childInst;
static HWND     found[16];
static int      nfound;
static HTASK    wantTask;
static int      shotsTaken;
static const char *curName;         /* the program being run, for SHOT */
static int      curShot;
static char     shotName[40];

/* one line onto the end of the log, and the file closed again */
static void note( const char *text )
{
    OFSTRUCT of;
    HFILE    file;
    char     stamp[16];

    file = OpenFile( LOGNAME, &of, OF_WRITE );
    if ( file == HFILE_ERROR ) {
        file = _lcreat( LOGNAME, 0 );
    } else {
        _llseek( file, 0L, 2 );
    }
    if ( file == HFILE_ERROR ) return;
    /* the time since Windows started, so that two runs can be compared
       line by line: a cache underneath DOS shows up here first */
    wsprintf( stamp, "%lu ", GetTickCount() );
    _lwrite( file, (LPCSTR)stamp, lstrlen( stamp ) );
    _lwrite( file, (LPCSTR)text, lstrlen( text ) );
    _lwrite( file, (LPCSTR)"\r\n", 2 );
    _lclose( file );
}

static BOOL exists( const char *path )
{
    OFSTRUCT of;

    return OpenFile( path, &of, OF_EXIST ) != HFILE_ERROR;
}

/* wait, pumping messages, so Windows keeps running meanwhile */
static void pause( DWORD ms )
{
    MSG   msg;
    DWORD until = GetTickCount() + ms;

    while ( GetTickCount() < until ) {
        while ( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) ) {
            if ( msg.message == WM_QUIT ) return;
            TranslateMessage( &msg );
            DispatchMessage( &msg );
        }
        Yield();
    }
}

/*--- the screen into a BMP ------------------------------------------*/
static int screenshot( const char *name )
{
    char     path[80];
    HDC      screen, mem;
    HBITMAP  bmp, old;
    int      width, height, bits, planes, depth, colours;
    BITMAPINFO FAR *info;
    HGLOBAL  hInfo, hBits;
    BYTE FAR *pixels;
    DWORD    rowBytes, imageBytes, infoBytes;
    BITMAPFILEHEADER hdr;
    OFSTRUCT of;
    HFILE    file;
    int      ok = 0;

    lstrcpy( path, SHOTDIR );
    lstrcat( path, name );
    lstrcat( path, ".BMP" );

    screen = CreateDC( "DISPLAY", NULL, NULL, NULL );
    if ( screen == NULL ) return 0;
    width  = GetDeviceCaps( screen, HORZRES );
    height = GetDeviceCaps( screen, VERTRES );
    bits   = GetDeviceCaps( screen, BITSPIXEL );
    planes = GetDeviceCaps( screen, PLANES );
    depth  = bits * planes;
    if ( depth <= 1 ) depth = 1;
    else if ( depth <= 4 ) depth = 4;
    else if ( depth <= 8 ) depth = 8;
    else depth = 24;
    colours = depth == 24 ? 0 : ( 1 << depth );

    mem = CreateCompatibleDC( screen );
    bmp = CreateCompatibleBitmap( screen, width, height );
    old = SelectObject( mem, bmp );
    BitBlt( mem, 0, 0, width, height, screen, 0, 0, SRCCOPY );
    SelectObject( mem, old );

    rowBytes   = ( ( (DWORD)width * depth + 31 ) / 32 ) * 4;
    imageBytes = rowBytes * height;
    infoBytes  = sizeof( BITMAPINFOHEADER ) + (DWORD)colours * sizeof( RGBQUAD );
    hInfo = GlobalAlloc( GHND, infoBytes );
    hBits = GlobalAlloc( GHND, imageBytes );
    if ( hInfo != NULL && hBits != NULL ) {
        info   = (BITMAPINFO FAR *)GlobalLock( hInfo );
        pixels = (BYTE FAR *)GlobalLock( hBits );
        info->bmiHeader.biSize = sizeof( BITMAPINFOHEADER );
        info->bmiHeader.biWidth = width;
        info->bmiHeader.biHeight = height;
        info->bmiHeader.biPlanes = 1;
        info->bmiHeader.biBitCount = depth;
        info->bmiHeader.biCompression = BI_RGB;
        info->bmiHeader.biSizeImage = imageBytes;
        info->bmiHeader.biClrUsed = colours;
        if ( GetDIBits( mem, bmp, 0, height, pixels, info, DIB_RGB_COLORS ) ) {
            file = OpenFile( path, &of, OF_CREATE | OF_WRITE );
            if ( file != HFILE_ERROR ) {
                hdr.bfType = 0x4D42;
                hdr.bfSize = sizeof( hdr ) + infoBytes + imageBytes;
                hdr.bfReserved1 = 0;
                hdr.bfReserved2 = 0;
                hdr.bfOffBits = sizeof( hdr ) + infoBytes;
                _lwrite( file, (LPCSTR)&hdr, sizeof( hdr ) );
                _hwrite( file, (LPCSTR)info, infoBytes );
                _hwrite( file, (LPCSTR)pixels, imageBytes );
                _lclose( file );
                ok = 1;
            }
        }
        GlobalUnlock( hBits );
        GlobalUnlock( hInfo );
    }
    if ( hBits != NULL ) GlobalFree( hBits );
    if ( hInfo != NULL ) GlobalFree( hInfo );
    DeleteObject( bmp );
    DeleteDC( mem );
    DeleteDC( screen );
    shotsTaken++;
    return ok;
}

/*--- the program's windows ------------------------------------------*/
BOOL CALLBACK __export countWindows( HWND hwnd, LPARAM lParam )
{
    (void)lParam;
    if ( GetWindowTask( hwnd ) == wantTask && IsWindowVisible( hwnd ) ) {
        if ( nfound < 16 ) found[nfound] = hwnd;
        nfound++;
    }
    return TRUE;
}

static FARPROC countProc;

static void findWindows( HINSTANCE inst )
{
    nfound = 0;
    wantTask = NULL;
    if ( (UINT)inst > 32 ) {
        /* the task that owns the module: any window of its instance */
        HWND w = GetWindow( GetDesktopWindow(), GW_CHILD );
        while ( w != NULL ) {
            if ( GetWindowWord( w, GWW_HINSTANCE ) == (WORD)inst ) {
                wantTask = GetWindowTask( w );
                break;
            }
            w = GetWindow( w, GW_HWNDNEXT );
        }
    }
    if ( wantTask == NULL ) return;
    EnumWindows( (WNDENUMPROC)countProc, 0L );
}

static void describe( const char *name )
{
    char title[80];
    char cls[40];
    int  i, n;

    n = nfound < 16 ? nfound : 16;
    wsprintf( line, "%s: %d visible window(s)", (LPSTR)name, nfound );
    note( line );
    for ( i = 0; i < n; i++ ) {
        title[0] = '\0';
        GetWindowText( found[i], title, sizeof( title ) );
        GetClassName( found[i], cls, sizeof( cls ) );
        wsprintf( line, "  %04X %s \"%s\"", (UINT)found[i], (LPSTR)cls, (LPSTR)title );
        note( line );
    }
}

/*--- keys to the window with the focus ------------------------------*/
static void postKey( HWND hwnd, UINT vk, BOOL alt )
{
    UINT  scan = MapVirtualKey( vk, 0 );
    DWORD down = 1L | ( (DWORD)scan << 16 );
    DWORD up   = down | 0xC0000000L;

    if ( alt ) {
        down |= 0x20000000L;
        up   |= 0x20000000L;
        PostMessage( hwnd, WM_SYSKEYDOWN, vk, down );
        PostMessage( hwnd, WM_SYSCHAR, vk, down );
        PostMessage( hwnd, WM_SYSKEYUP, vk, up );
    } else {
        PostMessage( hwnd, WM_KEYDOWN, vk, down );
        if ( vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_TAB || vk == VK_SPACE ) {
            PostMessage( hwnd, WM_CHAR, vk, down );
        } else if ( vk >= 'A' && vk <= 'Z' ) {
            PostMessage( hwnd, WM_CHAR, vk + 32, down );    /* the letter, as typed:
                                                             * a dialog's mnemonic */
        }
        PostMessage( hwnd, WM_KEYUP, vk, up );
    }
}

static void postText( HWND hwnd, const char *text )
{
    while ( *text ) {
        PostMessage( hwnd, WM_CHAR, (UINT)(BYTE)*text, 1L );
        text++;
    }
}

static HWND focusWindow( void )
{
    HWND w = GetFocus();
    if ( w == NULL ) w = GetActiveWindow();
    return w;
}

static void sendKeys( char *keys )
{
    char *tok;
    HWND  w;

    tok = strtok( keys, " " );
    while ( tok != NULL ) {
        if ( tok[0] == 'W' && tok[1] >= '0' && tok[1] <= '9' ) {
            pause( (DWORD)atoi( tok + 1 ) * 1000L );
        } else if ( lstrcmp( tok, "SHOT" ) == 0 ) {
            /* its own buffer: the name is a piece of item, and writing
             * item here renamed the program WIN32S_1_2_3_4_5 */
            wsprintf( shotName, "%s_%d", (LPSTR)curName, ++curShot );
            if ( !screenshot( shotName ) ) {
                wsprintf( line, "  (picture %s failed)", (LPSTR)shotName );
                note( line );
            }
        } else {
            w = focusWindow();
            wsprintf( line, "  keys \"%s\" to %04X", (LPSTR)tok, (UINT)w );
            note( line );
            if ( w == NULL ) {
                /* nothing has the focus: nothing to type at */
            } else if ( lstrcmpi( tok, "ENTER" ) == 0 ) {
                postKey( w, VK_RETURN, FALSE );
            } else if ( lstrcmpi( tok, "OK" ) == 0 ) {
                /* Enter, only to a DIALOG whose default button is not
                 * Cancel, Exit or No.  An installer's copy-progress
                 * dialog defaults to Cancel, and Enter there is what
                 * cut Win32s short - twice; and Enter to no dialog at
                 * all went to Program Manager and opened File Manager. */
                HWND dlg = GetActiveWindow();
                char cls[40], btn[40];
                HWND def;
                cls[0] = '\0'; btn[0] = '\0';
                if ( dlg != NULL ) GetClassName( dlg, cls, sizeof( cls ) );
                if ( dlg == NULL || lstrcmp( cls, "#32770" ) != 0 ) {
                    wsprintf( line, "  (OK held back: active is %04X \"%s\", not a dialog)", (UINT)dlg, (LPSTR)cls );
                    note( line );
                } else {
                    def = GetDlgItem( dlg, LOWORD( SendMessage( dlg, DM_GETDEFID, 0, 0L ) ) );
                    if ( def != NULL ) GetWindowText( def, btn, sizeof( btn ) );
                    if ( strstr( btn, "ancel" ) != NULL || strstr( btn, "xit" ) != NULL || lstrcmpi( btn, "&No" ) == 0 ) {
                        wsprintf( line, "  (OK held back: the default button is \"%s\")", (LPSTR)btn );
                        note( line );
                    } else {
                        wsprintf( line, "  OK -> \"%s\"", (LPSTR)btn );
                        note( line );
                        postKey( dlg, VK_RETURN, FALSE );
                    }
                }
            } else if ( lstrcmpi( tok, "ESC" ) == 0 ) {
                postKey( w, VK_ESCAPE, FALSE );
            } else if ( lstrcmpi( tok, "TAB" ) == 0 ) {
                postKey( w, VK_TAB, FALSE );
            } else if ( lstrcmpi( tok, "SPACE" ) == 0 ) {
                postKey( w, VK_SPACE, FALSE );
            } else if ( ( tok[0] == 'A' || tok[0] == 'a' ) && tok[1] == 'L' && tok[2] == 'T' && tok[3] == '-' ) {
                postKey( w, (UINT)( tok[4] >= 'a' ? tok[4] - 32 : tok[4] ), TRUE );
            } else if ( tok[0] == 'F' && tok[1] >= '1' && tok[1] <= '9' ) {
                postKey( w, VK_F1 + atoi( tok + 1 ) - 1, FALSE );
            } else {
                postText( w, tok );
            }
            pause( 700 );
        }
        tok = strtok( NULL, " " );
    }
}

/*--- closing what it opened -----------------------------------------*/
static void closeProgram( const char *name, HINSTANCE inst )
{
    int  i, n, tries;
    HWND w;
    char cls[40];

    /* A DOS box is not asked to close: WINOLDAP answers WM_CLOSE with a
     * system-modal "Application still active", which stops everything.
     * Its batch file ends by itself; this waits for that, up to 40 s. */
    findWindows( inst );
    if ( nfound > 0 ) {
        GetClassName( found[0], cls, sizeof( cls ) );
        if ( lstrcmpi( cls, "tty" ) == 0 ) {
            for ( tries = 0; tries < 20; tries++ ) {
                pause( 2000 );
                findWindows( inst );
                if ( nfound == 0 ) break;
            }
            wsprintf( line, "%s: DOS box %s", (LPSTR)name,
                      nfound == 0 ? (LPSTR)"ended by itself" : (LPSTR)"STILL OPEN after 40 s" );
            note( line );
            if ( nfound > 0 ) {
                wsprintf( item, "%s_2", (LPSTR)name );
                screenshot( item );
            }
            return;
        }
    }

    /* ONE WM_CLOSE, TO ONE WINDOW.  Closing every top-level window at
     * once gave SimEarth a second WM_CLOSE inside the "save the world?"
     * box the first had put up, and nothing moved after that.  So the
     * main window (the one with a caption, else the first) gets one
     * WM_CLOSE, and a box that follows is answered by button id, which
     * is what a message box's dialog procedure actually acts on: No,
     * then Cancel. */
    for ( tries = 0; tries < 2; tries++ ) {
        findWindows( inst );
        if ( nfound == 0 ) break;
        n = nfound < 16 ? nfound : 16;
        w = found[0];
        for ( i = 0; i < n; i++ ) {
            if ( GetParent( found[i] ) == NULL && ( GetWindowLong( found[i], GWL_STYLE ) & WS_CAPTION ) == WS_CAPTION ) {
                w = found[i];
                break;
            }
        }
        wsprintf( line, "  WM_CLOSE to %04X", (UINT)w );
        note( line );
        PostMessage( w, WM_CLOSE, 0, 0L );
        pause( 2500 );
        w = GetActiveWindow();
        if ( w != NULL && GetWindowTask( w ) == wantTask ) {
            GetClassName( w, cls, sizeof( cls ) );
            if ( lstrcmp( cls, "#32770" ) == 0 ) {
                note( "  a dialog: No" );
                PostMessage( w, WM_COMMAND, IDNO, 0L );
                pause( 1500 );
                w = GetActiveWindow();
                if ( w != NULL && GetWindowTask( w ) == wantTask && IsWindow( w ) ) {
                    note( "  still a dialog: Cancel" );
                    PostMessage( w, WM_COMMAND, IDCANCEL, 0L );
                    pause( 1500 );
                }
            }
        }
    }
    findWindows( inst );
    if ( nfound > 0 ) {
        wsprintf( line, "%s: STILL THERE after WM_CLOSE (%d window(s))", (LPSTR)name, nfound );
        note( line );
        wsprintf( item, "%s_2", (LPSTR)name );
        screenshot( item );
    } else {
        wsprintf( line, "%s: closed", (LPSTR)name );
        note( line );
    }
}

/*--- the list -------------------------------------------------------*/
/* THE LIST IS READ WHOLE AT THE START.  It used to be read a line at a
 * time, and after the first DOS box the handle read nothing more: the
 * sweep ended after two programs.  Whether that is Windows or the DOS
 * underneath it is what the probe handle below measures, separately,
 * instead of the sweep depending on it. */
#define LISTMAX 8192
static char listBuf[LISTMAX];
static int  listLen, listPos;
static HFILE probe = HFILE_ERROR;

static int loadList( void )
{
    OFSTRUCT of;
    HFILE    file;

    file = OpenFile( LISTNAME, &of, OF_READ );
    if ( file == HFILE_ERROR ) return 0;
    listLen = _lread( file, listBuf, LISTMAX - 1 );
    _lclose( file );
    if ( listLen < 0 ) listLen = 0;
    listBuf[listLen] = '\0';
    listPos = 0;
    return 1;
}

static int nextLine( char *buf, int max )
{
    int n = 0;

    if ( listPos >= listLen ) return 0;
    while ( listPos < listLen && listBuf[listPos] != '\n' ) {
        if ( listBuf[listPos] != '\r' && n < max - 1 ) buf[n++] = listBuf[listPos];
        listPos++;
    }
    if ( listPos < listLen ) listPos++;     /* past the newline */
    buf[n] = '\0';
    return 1;
}

/* a file handle held open across every program: can it still be read
 * afterwards?  C:\PROBE.TXT, made at the start. */
static void probeOpen( void )
{
    probe = _lcreat( "C:\\PROBE.TXT", 0 );
    if ( probe == HFILE_ERROR ) { note( "probe: could not create C:\\PROBE.TXT" ); return; }
    _lwrite( probe, (LPCSTR)"probe", 5 );
}

static void probeCheck( const char *after )
{
    char buf[8];
    int  got;

    if ( probe == HFILE_ERROR ) return;
    if ( _llseek( probe, 0L, 0 ) != 0L ) {
        wsprintf( line, "probe handle after %s: seek FAILED", (LPSTR)after );
        note( line );
        return;
    }
    got = _lread( probe, buf, 5 );
    wsprintf( line, "probe handle after %s: %s (%d read)", (LPSTR)after,
              got == 5 && buf[0] == 'p' ? (LPSTR)"ok" : (LPSTR)"FAILED", got );
    note( line );
}

static void runOne( char *spec )
{
    char *name, *cmd, *secs, *keys;
    int   seconds;
    UINT  before, after;
    DWORD t0;

    name = strtok( spec, "|" );
    cmd  = strtok( NULL, "|" );
    secs = strtok( NULL, "|" );
    keys = strtok( NULL, "|" );
    if ( name == NULL || cmd == NULL ) return;

    /* NAME?file runs only while the file is missing (an installer, once);
     * NAME!file only once it is there (what the installer made) */
    {
        char *cond = strchr( name, '?' );
        int   skip = 0;
        if ( cond != NULL ) {
            *cond++ = '\0';
            skip = exists( cond );
        } else if ( ( cond = strchr( name, '!' ) ) != NULL ) {
            *cond++ = '\0';
            skip = !exists( cond );
        }
        if ( skip ) {
            wsprintf( line, "--- %s: skipped (%s)", (LPSTR)name, (LPSTR)cond );
            note( line );
            return;
        }
    }
    seconds = secs != NULL ? atoi( secs ) : 10;
    if ( seconds < 0 ) seconds = 0;     /* 0: wait for the program to END, an installer */
    curName = name;
    curShot = 0;

    before = GetFreeSystemResources( GFSR_SYSTEMRESOURCES );
    wsprintf( line, "--- %s: WinExec(\"%s\"), %d s, resources %u%%, free %lu KB",
              (LPSTR)name, (LPSTR)cmd, seconds, before, GetFreeSpace( 0 ) / 1024L );
    note( line );
    /* The program's own directory is made current first, as Program
     * Manager's "Working Directory" would: SimEarth and The Incredible
     * Machine look for their data files there and quit without a word
     * when they are not found.  The current directory is one per
     * machine in Windows 3.1, so this is simply DOS's. */
    if ( *cmd == '*' ) {
        cmd++;                          /* *command: leave the directory alone */
    } else {
        char dir[128];
        char *end;
        lstrcpy( dir, cmd );
        end = strchr( dir, ' ' );
        if ( end != NULL ) *end = '\0';
        end = strrchr( dir, '\\' );
        if ( end != NULL && end > dir + 2 ) {
            *end = '\0';
            if ( dir[1] == ':' ) _dos_setdrive( ( dir[0] & 0xDF ) - 'A' + 1, &drives );
            if ( chdir( dir ) != 0 ) {
                wsprintf( line, "  (chdir %s failed)", (LPSTR)dir );
                note( line );
            }
        }
    }
    t0 = GetTickCount();
    childInst = (HINSTANCE)WinExec( cmd, SW_SHOWNORMAL );
    if ( (UINT)childInst <= 32 ) {
        wsprintf( line, "%s: WinExec FAILED, code %u", (LPSTR)name, (UINT)childInst );
        note( line );
        screenshot( name );
        return;
    }
    pause( 1500 );
    if ( keys != NULL && *keys ) sendKeys( keys );
    if ( seconds == 0 ) {
        /* An installer: it is not closed, it is waited for - up to ten
         * minutes, a picture every thirty seconds, until the module is
         * gone.  (If it restarts Windows, this program starts again
         * and the NAME?file lines skip what is done.) */
        int half;
        for ( half = 0; half < 20; half++ ) {
            pause( 30000L );
            if ( GetModuleUsage( childInst ) == 0 ) break;
            wsprintf( shotName, "%s_%d", (LPSTR)curName, ++curShot );
            if ( !screenshot( shotName ) ) {
                wsprintf( line, "  (picture %s failed)", (LPSTR)shotName );
                note( line );
            }
        }
        wsprintf( line, "%s: %s after %lu ms", (LPSTR)name,
                  GetModuleUsage( childInst ) == 0 ? (LPSTR)"ended by itself" : (LPSTR)"STILL RUNNING",
                  GetTickCount() - t0 );
        note( line );
        return;
    }
    pause( (DWORD)seconds * 1000L );
    findWindows( childInst );
    describe( name );
    wsprintf( line, "%s: usage %d after %lu ms", (LPSTR)name,
              GetModuleUsage( childInst ), GetTickCount() - t0 );
    note( line );
    if ( !screenshot( name ) ) note( "  (screenshot failed)" );
    closeProgram( name, childInst );
    after = GetFreeSystemResources( GFSR_SYSTEMRESOURCES );
    wsprintf( line, "%s: done, resources %u%% -> %u%%, free %lu KB",
              (LPSTR)name, before, after, GetFreeSpace( 0 ) / 1024L );
    note( line );
}

int PASCAL WinMain( HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show )
{
    int      n = 0;

    (void)prev; (void)cmdLine; (void)show;
    /* A program the loader cannot start (a missing DLL, most often)
     * would otherwise be a message box inside WinExec, which does not
     * return until somebody presses Close; with this it is an error
     * code in the log instead. */
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX );
    countProc = MakeProcInstance( (FARPROC)countWindows, inst );
    wsprintf( line, "WINSWEEP: Windows %u.%u is up, flags %04lX, %dx%d",
              LOBYTE( LOWORD( GetVersion() ) ), HIBYTE( LOWORD( GetVersion() ) ),
              GetWinFlags(), GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
    note( line );
    pause( 3000 );                      /* the desktop settles */
    screenshot( "DESKTOP0" );

    probeOpen();
    if ( !loadList() ) {
        note( "WINSWEEP: no C:\\SWEEP.LST" );
    } else {
        while ( nextLine( item, sizeof( item ) ) ) {
            if ( item[0] == '\0' || item[0] == '#' ) continue;
            n++;
            runOne( item );             /* leaves the NAME in item */
            probeCheck( item );
            pause( 1500 );
        }
    }
    if ( probe != HFILE_ERROR ) _lclose( probe );
    screenshot( "DESKTOP1" );
    wsprintf( line, "WINSWEEP: %d program(s) done, %d picture(s); asking Windows to exit", n, shotsTaken );
    note( line );
    if ( !ExitWindows( 0, 0 ) ) {
        note( "WINSWEEP: ExitWindows was refused" );
    }
    FreeProcInstance( countProc );
    return 0;
}
