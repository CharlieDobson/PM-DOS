/*
 * DBGTEST.C - 21h/F1h AL=29h, the kernel's door for a debugger, tried
 * from the debugger's side.  Built with CSTART.ASM, like NICTEST.
 *
 *   DBGTEST [path]        the whole test, on DBGTGT.EXE (or path)
 *   DBGTEST /HOLD [path]  load it and end without letting it go: the
 *                         kernel's 4Ch must end it first.  A DBGTEST run
 *                         after this one finds nothing held.
 *
 * Each check prints PASS or FAIL and a name; the last line says how
 * many failed.  The program stepped, stopped and faulted is DBGTGT.ASM,
 * whose table of offsets is read out of it with DBF_READ.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;
} REGS;

void __cdecl int21( REGS *regs );

/* INC\PMDOS.INC's DRX_* record */
typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, efl;
    u16 cs, ds, es, ss, fs, gs;
    u32 vec, err, addr, dr6, limit;
} DRX;

#define DBF_LOAD    0
#define DBF_GO      1
#define DBF_READ    2
#define DBF_WRITE   3
#define DBF_KILL    4

#define SEL_CS      0x3B
#define SEL_DS      0x43

static int failures;
static REGS r;

static u32 slen( const char *s )
{
    u32 n = 0;

    while ( s[n] ) n++;
    return n;
}

static void out( const char *s )
{
    r.eax = 0x4000;
    r.ebx = 1;
    r.ecx = slen( s );
    r.edx = (u32)s;
    r.flags = 0;
    int21( &r );
}

static void hex( u32 v )
{
    char buf[9];
    int i;

    for ( i = 7; i >= 0; i-- ) {
        buf[i] = "0123456789ABCDEF"[v & 15];
        v >>= 4;
    }
    buf[8] = 0;
    out( buf );
}

static void check( int ok, const char *name, u32 got )
{
    out( ok ? "PASS " : "FAIL " );
    out( name );
    if ( !ok ) {
        out( " (got " );
        hex( got );
        out( ")" );
        failures++;
    }
    out( "\r\n" );
}

/* one 21h/F1h AL=29h call; returns CF, AX in *ax */
static int dbg( u32 fn, u32 ebx, u32 ecx, u32 edx, u32 esi, u32 edi, u32 *ax )
{
    r.eax = (fn << 16) | 0xF129;
    r.ebx = ebx;
    r.ecx = ecx;
    r.edx = edx;
    r.esi = esi;
    r.edi = edi;
    r.flags = 0;
    int21( &r );
    *ax = r.eax & 0xFFFF;
    return (int)(r.flags & 1);
}

static u32 get_vec( int vec )
{
    r.eax = 0x3500 | vec;
    r.flags = 0;
    int21( &r );
    return r.ebx;
}

static void dummy_handler( void )
{
}

void __cdecl dos_entry( void )
{
    static u8 block[8];             /* 4Bh's: environment 0, tail */
    static u8 tail[] = { 0, 13 };
    static u8 buf[32];
    static u8 cc = 0xCC;
    static DRX x;
    static char path[80];
    u8 *psp;
    u32 ax, realstart, bp1, int3site, nulsite, oksite, mine;
    u32 len, i, hold = 0;
    u8 orig;
    int cf;

    /* the tail: [/HOLD] [path] */
    r.eax = 0x6200;
    r.flags = 0;
    int21( &r );
    psp = (u8 *)r.ebx;
    len = psp[0x80];
    i = 0;
    while ( i < len && psp[0x81 + i] == ' ' ) i++;
    if ( i + 5 <= len && psp[0x81 + i] == '/' && (psp[0x82 + i] | 0x20) == 'h' ) {
        hold = 1;
        i += 5;
        while ( i < len && psp[0x81 + i] == ' ' ) i++;
    }
    len -= i;
    if ( len > 70 ) len = 70;
    {
        u32 k;
        for ( k = 0; k < len && psp[0x81 + i + k] != ' '; k++ ) path[k] = psp[0x81 + i + k];
        path[k] = 0;
    }
    if ( path[0] == 0 ) {
        const char *d = "DBGTGT.EXE";
        for ( i = 0; d[i]; i++ ) path[i] = d[i];
        path[i] = 0;
    }
    *(u32 *)(block + 4) = (u32)tail;

    /* a handler of ours on INT 23h, so that whose is live can be seen */
    r.eax = 0x2523;
    r.edx = (u32)dummy_handler;
    r.flags = 0;
    int21( &r );
    mine = get_vec( 0x23 );

    cf = dbg( DBF_LOAD, (u32)block, 0, (u32)path, (u32)&x, 0, &ax );
    check( !cf && ax == 0, "LOAD", ax );
    if ( cf ) goto done;
    if ( hold ) {
        out( "HOLD: ending with the program held\r\n" );
        goto done;
    }
    check( x.vec == 0xFFFFFFFFUL, "LOAD: not run yet", x.vec );
    check( x.cs == SEL_CS && x.ss == SEL_DS && x.ds == SEL_DS, "LOAD: selectors", x.cs );
    check( x.esp == x.limit - 4, "LOAD: stack at the top", x.esp );

    cf = dbg( DBF_READ, x.eip, 26, 0, 0, (u32)buf, &ax );
    check( !cf && buf[0] == 0xEB, "READ the entry", buf[0] );
    realstart = *(u32 *)(buf + 2);
    bp1       = *(u32 *)(buf + 6);
    int3site  = *(u32 *)(buf + 10);
    nulsite   = *(u32 *)(buf + 14);
    oksite    = *(u32 *)(buf + 18);

    cf = dbg( DBF_READ, 0, 4, 0, 0, (u32)buf, &ax );
    check( cf && ax == 13, "READ the guard page refused", ax );
    cf = dbg( DBF_READ, x.limit - 1, 2, 0, 0, (u32)buf, &ax );
    check( cf && ax == 13, "READ past the slot refused", ax );

    x.efl |= 0x100;
    cf = dbg( DBF_GO, 0, 0, 0, (u32)&x, 0, &ax );
    check( !cf && ax == 1 && x.vec == 1, "STEP stops", x.vec );
    check( x.eip == realstart, "STEP over the jump", x.eip );
    check( (x.dr6 & 0x4000) != 0, "STEP: DR6.BS", x.dr6 );
    cf = dbg( DBF_GO, 0, 0, 0, (u32)&x, 0, &ax );
    check( !cf && ax == 1 && x.eip == realstart + 5, "STEP again", x.eip );
    check( x.eax == 0x12345678UL, "STEP: EAX", x.eax );

    dbg( DBF_READ, bp1, 1, 0, 0, (u32)&orig, &ax );
    cf = dbg( DBF_WRITE, bp1, 1, 0, (u32)&cc, 0, &ax );
    check( !cf, "WRITE a breakpoint", ax );
    x.efl &= ~0x100UL;
    cf = dbg( DBF_GO, 0, 0, 0, (u32)&x, 0, &ax );
    check( !cf && ax == 1 && x.vec == 13 && x.err == 0x1A, "INT 3 stops", x.err );
    check( x.eip == bp1 && x.ebx == 0x12345679UL, "INT 3: where, and EBX", x.eip );
    check( get_vec( 0x23 ) == mine, "our INT 23h is live while it is held", get_vec( 0x23 ) );

    dbg( DBF_WRITE, bp1, 1, 0, (u32)&orig, 0, &ax );
    out( "(the program prints T:) " );
    cf = dbg( DBF_GO, 0, 0, 0, (u32)&x, 0, &ax );
    out( "\r\n" );
    check( !cf && ax == 1 && x.vec == 13 && x.eip == int3site, "its own INT 3", x.eip );
    check( get_vec( 0x23 ) == mine, "its INT 23h is not ours to keep", get_vec( 0x23 ) );

    x.eip++;
    cf = dbg( DBF_GO, 0, 0, 0, (u32)&x, 0, &ax );
    check( !cf && ax == 1 && x.vec == 14 && x.addr == 0, "null pointer read stops", x.vec );
    check( x.eip == nulsite, "page fault: where", x.eip );

    x.cs = SEL_DS;
    cf = dbg( DBF_GO, 0, 0, 0, (u32)&x, 0, &ax );
    check( cf && ax == 13, "GO with a bad CS refused", ax );
    x.cs = SEL_CS;
    x.eip = x.limit;
    cf = dbg( DBF_GO, 0, 0, 0, (u32)&x, 0, &ax );
    check( cf && ax == 13, "GO past the code refused", ax );

    x.eip = oksite;
    cf = dbg( DBF_GO, 0, 0, 0, (u32)&x, 0, &ax );
    check( !cf && ax == 0, "it ends", ax );
    r.eax = 0x4D00;
    r.flags = 0;
    int21( &r );
    check( (r.eax & 0xFF) == 7, "its errorlevel", r.eax );
    check( get_vec( 0x23 ) == mine, "our INT 23h after it ended", get_vec( 0x23 ) );
    cf = dbg( DBF_GO, 0, 0, 0, (u32)&x, 0, &ax );
    check( cf && ax == 5, "GO with nothing held refused", ax );

    cf = dbg( DBF_LOAD, (u32)block, 0, (u32)path, (u32)&x, 0, &ax );
    check( !cf, "LOAD again", ax );
    r.eax = 0x4B00;
    r.ebx = (u32)block;
    r.edx = (u32)path;
    r.flags = 0;
    int21( &r );
    check( (r.flags & 1) && (r.eax & 0xFFFF) == 5, "EXEC while holding refused", r.eax );
    cf = dbg( DBF_LOAD, (u32)block, 0, (u32)path, (u32)&x, 0, &ax );
    check( cf && ax == 5, "a second LOAD refused", ax );
    cf = dbg( DBF_KILL, 0, 0, 0, 0, 0, &ax );
    check( !cf, "KILL", ax );
    cf = dbg( DBF_KILL, 0, 0, 0, 0, 0, &ax );
    check( cf && ax == 5, "KILL with nothing held refused", ax );

    cf = dbg( DBF_LOAD, (u32)block, 0, (u32)"A:\\COMMAND.COM", (u32)&x, 0, &ax );
    check( cf && ax == 11, "LOAD a 16-bit program refused", ax );
    if ( !cf ) dbg( DBF_KILL, 0, 0, 0, 0, 0, &ax );
    cf = dbg( DBF_LOAD, (u32)block, 0, (u32)"A:\\NOSUCH.EXE", (u32)&x, 0, &ax );
    check( cf && ax == 2, "LOAD a missing file", ax );
    if ( !cf ) dbg( DBF_KILL, 0, 0, 0, 0, 0, &ax );

done:
    out( "failures: " );
    hex( (u32)failures );
    out( "\r\n" );
    r.eax = 0x4C00 | (failures ? 1 : 0);
    r.flags = 0;
    int21( &r );
}
