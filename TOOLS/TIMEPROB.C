/*====================================================================
 * TIMEPROB.C - TIMEPROB.EXE, the clocks a Windows 3.1 program can see,
 *              written down.
 *
 * A Windows program that starts, measures and leaves - no window - so
 * that it can be a line in WINSWEEP's list.  Everything goes into
 * C:\TIMEPROB.TXT, a line at a time, each closed before the next.
 *
 * WHY.  Two programs in the enhanced-mode sweep (Monopoly Deluxe, and
 * Brickbuster) started, read their segments, asked DOS the date and
 * time, and then never made another DOS or BIOS call and never showed
 * a window: a loop waiting for something the machine provides without
 * DOS.  Monopoly does the same under WIN /S and runs on real MS-DOS 5
 * with the same Windows files, so the something is machine state the
 * park leaves different.  This lists the candidates: the tick counts
 * (GetTickCount, GetCurrentTime, the BIOS's at 40:6C, timeGetTime from
 * MMSYSTEM), whether INT 1Ch reaches a protected-mode hook, the 8254's
 * counter and its mode (read-back), the real-time clock's seconds, and
 * the interrupt masks.  Every wait is bounded by a turn count.
 *
 * Built with Open Watcom:  wcl -zq -bt=windows -l=windows timeprob.c
 *===================================================================*/
#include <windows.h>
#include <conio.h>
#include <dos.h>
#include <string.h>

#define LOGNAME "C:\\TIMEPROB.TXT"

static char line[160];

static void note( const char *text )
{
    OFSTRUCT of;
    HFILE    file;

    file = OpenFile( LOGNAME, &of, OF_WRITE );
    if ( file == HFILE_ERROR ) file = _lcreat( LOGNAME, 0 );
    else _llseek( file, 0L, 2 );
    if ( file == HFILE_ERROR ) return;
    _lwrite( file, (LPCSTR)text, lstrlen( text ) );
    _lwrite( file, (LPCSTR)"\r\n", 2 );
    _lclose( file );
}

/*--- INT 1Ch, hooked through DOS the way a program would -----------*/
static volatile unsigned ticks1C, ticks08;
static void ( __interrupt __far *old1C )( void );
static void ( __interrupt __far *old08 )( void );

static void __interrupt __far tick1C( void )
{
    ticks1C++;
    _chain_intr( old1C );
}

static void __interrupt __far tick08( void )
{
    ticks08++;
    _chain_intr( old08 );
}

static unsigned pitRead( void )
{
    unsigned lo, hi;

    outp( 0x43, 0x00 );                /* latch counter 0 */
    lo = inp( 0x40 );
    hi = inp( 0x40 );
    return ( hi << 8 ) | lo;
}

static unsigned pitStatus( void )
{
    outp( 0x43, 0xE2 );                /* read-back: status of counter 0 */
    return inp( 0x40 );
}

static unsigned rtcSeconds( void )
{
    outp( 0x70, 0x00 );
    return inp( 0x71 );
}

static DWORD bdaTicks( void )
{
    return *(DWORD FAR *)MAKELP( 0x0040, 0x006C );
}

typedef DWORD ( FAR PASCAL *TIMEGETTIME )( void );

int PASCAL WinMain( HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show )
{
    DWORD       t0, t1, c0, b0, m0, m1;
    long        turns;
    unsigned    p0, p1, p2, s0, s1, r0, r1, n0;
    HINSTANCE   mm;
    TIMEGETTIME timeGetTime = NULL;

    (void)inst; (void)prev; (void)cmdLine; (void)show;
    wsprintf( line, "TIMEPROB: Windows %u.%u, flags %04lX",
              LOBYTE( LOWORD( GetVersion() ) ), HIBYTE( LOWORD( GetVersion() ) ), GetWinFlags() );
    note( line );

    /* the clocks at the start */
    t0 = GetTickCount();
    c0 = GetCurrentTime();
    b0 = bdaTicks();
    p0 = pitRead();
    s0 = pitStatus();
    r0 = rtcSeconds();
    wsprintf( line, "start: GetTickCount %lu, GetCurrentTime %lu, 40:6C %lu, PIT %04X status %02X (mode %u), RTC sec %02X, masks %02X/%02X",
              t0, c0, b0, p0, s0, ( s0 >> 1 ) & 7, r0, inp( 0x21 ), inp( 0xA1 ) );
    note( line );

    /* DOS's date and time, as a C runtime takes them: 2Ah and 2Ch */
    {
        struct dosdate_t dd;
        struct dostime_t dt;
        _dos_getdate( &dd );
        _dos_gettime( &dt );
        wsprintf( line, "DOS 2Ah: %u-%02u-%02u weekday %u; 2Ch: %02u:%02u:%02u.%02u",
                  dd.year, dd.month, dd.day, dd.dayofweek, dt.hour, dt.minute, dt.second, dt.hsecond );
        note( line );
    }

    /* MMSYSTEM's timeGetTime, if MMSYSTEM will load */
    mm = LoadLibrary( "MMSYSTEM.DLL" );
    if ( (UINT)mm > 32 ) {
        timeGetTime = (TIMEGETTIME)GetProcAddress( mm, "TIMEGETTIME" );
    }
    m0 = timeGetTime != NULL ? timeGetTime() : 0L;
    wsprintf( line, "MMSYSTEM %s, timeGetTime %lu", (UINT)mm > 32 ? (LPSTR)"loaded" : (LPSTR)"NOT loaded", m0 );
    note( line );

    /* INT 1Ch to a hook of ours, for a while */
    old1C = _dos_getvect( 0x1C );
    _dos_setvect( 0x1C, tick1C );
    old08 = _dos_getvect( 0x08 );
    _dos_setvect( 0x08, tick08 );

    /* a bounded wait: either the tick count moves by 36 (two seconds)
     * or 4 million turns go by */
    n0 = ticks1C;
    p1 = pitRead();
    for ( turns = 0; turns < 40000000L; turns++ ) {
        if ( GetTickCount() - t0 >= 2000L ) break;
    }
    t1 = GetTickCount();
    p2 = pitRead();
    s1 = pitStatus();
    r1 = rtcSeconds();
    m1 = timeGetTime != NULL ? timeGetTime() : 0L;
    _dos_setvect( 0x1C, old1C );
    _dos_setvect( 0x08, old08 );
    wsprintf( line, "after %ld turns: GetTickCount +%lu, GetCurrentTime +%lu, 40:6C +%lu, timeGetTime +%lu, INT 1Ch hook called %u times, INT 08h hook %u times",
              turns, t1 - t0, GetCurrentTime() - c0, bdaTicks() - b0, m1 - m0, ticks1C - n0, ticks08 );
    note( line );
    wsprintf( line, "PIT %04X -> %04X -> %04X, status %02X, RTC sec %02X -> %02X",
              p0, p1, p2, s1, r0, r1 );
    note( line );
    wsprintf( line, "verdict: ticks %s, 40:6C %s, 1Ch %s, PIT %s, RTC %s",
              t1 - t0 >= 1000L ? (LPSTR)"move" : (LPSTR)"STUCK",
              bdaTicks() - b0 >= 18 ? (LPSTR)"moves" : (LPSTR)"STUCK",
              ticks1C - n0 >= 18 ? (LPSTR)"fires" : (LPSTR)"SILENT",
              p0 != p1 || p1 != p2 ? (LPSTR)"counts" : (LPSTR)"FROZEN",
              r0 != r1 ? (LPSTR)"moves" : (LPSTR)"same" );
    note( line );
    if ( (UINT)mm > 32 ) FreeLibrary( mm );
    note( "TIMEPROB: done" );
    return 0;
}
