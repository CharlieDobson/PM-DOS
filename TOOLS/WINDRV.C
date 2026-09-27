/*====================================================================
 * WINDRV.C - WINDRV.EXE, a Windows 3.1 run with nobody at the keyboard
 *
 * Put in WIN.INI as  run=C:\TESTS\WINDRV.EXE [program]  it starts the
 * program (C:\TESTS\WINDOS.BAT if none is named) the way Program
 * Manager would, waits for it to end, and then asks Windows to exit.
 * Everything it sees goes into C:\WINDRV.TXT, one line at a time,
 * each one closed before the next - a machine that resets halfway
 * through still leaves the lines before it on the disk.
 *
 * WHY IT EXISTS.  Keystrokes into an emulator are only valid on an
 * idle desktop, and the two things this checks are the two a script
 * could not reach: a DOS program (the 16-bit COMMAND.COM, running a
 * batch file) started FROM Windows, and Windows handing the machine
 * back when it exits.  A batch file named to WinExec runs through
 * COMSPEC, which is exactly the MS-DOS Prompt's path.
 *
 * Built with Open Watcom:  wcl -zq -bt=windows -l=windows windrv.c
 *===================================================================*/
#include <windows.h>

#define LOGNAME     "C:\\WINDRV.TXT"
#define ENDMARK     "C:\\WINDOS.END"
#define DEFPROG     "C:\\TESTS\\WINDOS.BAT"
#define MAXSECONDS  180

static HINSTANCE childInst;
static int       seconds;

/* one line onto the end of the log, and the file closed again */
static void note( const char *text )
{
    OFSTRUCT of;
    HFILE    file;

    file = OpenFile( LOGNAME, &of, OF_WRITE );
    if ( file == HFILE_ERROR ) {
        file = _lcreat( LOGNAME, 0 );
    } else {
        _llseek( file, 0L, 2 );
    }
    if ( file == HFILE_ERROR ) return;
    _lwrite( file, (LPCSTR)text, lstrlen( text ) );
    _lwrite( file, (LPCSTR)"\r\n", 2 );
    _lclose( file );
}

static BOOL exists( const char *name )
{
    OFSTRUCT of;

    return OpenFile( name, &of, OF_EXIST ) != HFILE_ERROR;
}

int PASCAL WinMain( HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show )
{
    char     line[160];
    char     prog[128];
    MSG      msg;
    OFSTRUCT of;
    UINT     timer;
    int      usage;

    (void)inst;
    (void)prev;
    (void)show;
    if ( cmdLine != NULL && *cmdLine != '\0' ) {
        lstrcpyn( prog, cmdLine, sizeof( prog ) );
    } else {
        lstrcpy( prog, DEFPROG );
    }
    OpenFile( ENDMARK, &of, OF_DELETE );
    wsprintf( line, "WINDRV: Windows %u.%u is up, flags %04lX",
              LOBYTE( LOWORD( GetVersion() ) ), HIBYTE( LOWORD( GetVersion() ) ),
              GetWinFlags() );
    note( line );
    childInst = (HINSTANCE)WinExec( prog, SW_SHOWNORMAL );
    wsprintf( line, "WINDRV: WinExec(\"%s\") = %u", (LPSTR)prog, (UINT)childInst );
    note( line );

    timer = SetTimer( NULL, 1, 1000, NULL );
    while ( GetMessage( &msg, NULL, 0, 0 ) ) {
        if ( msg.message == WM_TIMER ) {
            seconds++;
            usage = (UINT)childInst > 32 ? GetModuleUsage( childInst ) : 0;
            if ( usage == 0 || exists( ENDMARK ) || seconds >= MAXSECONDS ) {
                break;
            }
            continue;
        }
        TranslateMessage( &msg );
        DispatchMessage( &msg );
    }
    KillTimer( NULL, timer );
    wsprintf( line, "WINDRV: the program is gone after %d s (end mark %s)",
              seconds, exists( ENDMARK ) ? (LPSTR)"there" : (LPSTR)"missing" );
    note( line );
    note( "WINDRV: asking Windows to exit" );
    if ( !ExitWindows( 0, 0 ) ) {
        note( "WINDRV: ExitWindows was refused" );
    }
    return 0;
}
