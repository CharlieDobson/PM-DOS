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
static int       cbTimers;      /* calls of TickProc, a callback timer */

/* a timer with a callback: counted at the call, not through the queue */
VOID CALLBACK __export TickProc( HWND hwnd, UINT message, UINT id, DWORD when )
{
    (void)hwnd; (void)message; (void)id; (void)when;
    cbTimers++;
}

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
    UINT     timer2;
    FARPROC  tick;
    DWORD    qs;
    int      usage;
    long     turns;
    int      timers;
    int      sample;

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

    /* THE WAIT CANNOT RELY ON THE TIMER IT IS MEASURING.  This used
     * to be a GetMessage loop counting WM_TIMER, and on a Windows whose
     * timer never ticks it waited for ever and wrote nothing.  Now it
     * polls, yields, and every so many turns writes down the three
     * clocks it can see - Windows' GetTickCount, the BIOS count at
     * 40:6C, and the WM_TIMERs received - so that "time does not move"
     * is a line in the log rather than an empty file.  The turn count
     * is the clock of last resort. */
    timer = SetTimer( NULL, 1, 1000, NULL );
    tick = MakeProcInstance( (FARPROC)TickProc, inst );
    timer2 = SetTimer( NULL, 0, 1000, (TIMERPROC)tick );
    wsprintf( line, "WINDRV: SetTimer gave %u (queue) and %u (callback)", timer, timer2 );
    note( line );
    turns = 0;
    timers = 0;
    sample = 0;
    for ( ;; ) {
        if ( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) ) {
            if ( msg.message == WM_QUIT ) break;
            if ( msg.message == WM_TIMER && msg.lParam == 0L ) {
                timers++;               /* the queue timer: no proc */
                seconds++;
            } else {
                TranslateMessage( &msg );
                DispatchMessage( &msg );    /* a WM_TIMER with a proc in
                                             * lParam calls the proc here */
            }
        }
        turns++;
        if ( ( turns % 500000L ) == 0 ) {
            usage = (UINT)childInst > 32 ? GetModuleUsage( childInst ) : 0;
            qs = GetQueueStatus( QS_TIMER );
            if ( sample < 12 || ( sample % 10 ) == 0 ) {
                wsprintf( line, "WINDRV: sample %d: ticks %lu, BIOS 40:6C %lu, timers %d, callbacks %d, qs %04X, usage %d",
                          sample, GetTickCount(), *(DWORD FAR *)MAKELP( 0x0040, 0x006C ),
                          timers, cbTimers, (UINT)qs, usage );
                note( line );
            }
            sample++;
            /* ten samples at least, however quickly the child is done */
            if ( ( sample >= 10 && ( usage == 0 || exists( ENDMARK ) ) ) ||
                 seconds >= MAXSECONDS || sample >= 120 ) {
                break;
            }
        }
    }
    KillTimer( NULL, timer );
    KillTimer( NULL, timer2 );
    FreeProcInstance( tick );
    wsprintf( line, "WINDRV: the program is gone after %d s (end mark %s)",
              seconds, exists( ENDMARK ) ? (LPSTR)"there" : (LPSTR)"missing" );
    note( line );
    note( "WINDRV: asking Windows to exit" );
    if ( !ExitWindows( 0, 0 ) ) {
        note( "WINDRV: ExitWindows was refused" );
    }
    return 0;
}
