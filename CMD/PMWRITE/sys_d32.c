/*
 * SYS_D32.C - PMWRITE's system calls, PM-DOS native.
 *
 * PMWRITE.EXE is a D32 image run at ring 3, as EDIT.EXE and DOSSHELL.EXE
 * are, and most of this file is DOSSHELL's: INT 21h is the real-mode
 * interface widened to 32 bits, the screen and the keyboard are
 * 21h/F0h, the mouse is INT 33h, and files go by their long names
 * (71xxh) until the kernel answers 7100h.
 *
 * WHAT IS HERE BESIDES FILES:
 *
 *   4B00h         another program, in a slot of its own; this one waits
 *   F1h/03h       where the command interpreter is, to hand a line to
 *   F1h/06h       the volume table: what kind of drive a letter is
 *   7303h, 36h    how full a disk is
 *   38h, 6601h    how the country writes a number, a date and a time,
 *                 and which code page the screen's characters are in
 *
 * AND A GRAPHICS SCREEN.  A native program cannot set a video mode or
 * reach video memory.  21h/F1h AL=1Fh does the first (VBF_SET: a BIOS
 * mode, or a VBE one with its linear frame buffer) and answers with a
 * selector for the second, which VID_PUT in DOSINT.ASM writes through.
 * The planar modes also want the sequencer's map mask, and the ports
 * are reached with plain OUT instructions: they fault at ring 3 and the
 * kernel carries them out.  The ROM's font is read out of the first
 * megabyte with 21h/F1h AL=20h - see GFX.C for where it is found.
 *
 * A DRIVE THAT WILL NOT ANSWER IS FAILED, from the first call on: the
 * INT 24h handler in DOSINT.ASM is installed by sys_init, so that a
 * diskette drive with no diskette in the drive is an error code and
 * never the kernel's own question over a full-screen program.
 * sys_crit_seen says that it happened.
 */
#include "rt.h"
#include "sys.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void __cdecl int33( REGS *regs );
void __cdecl break_handler( void );
void __cdecl crit_handler( void );
int  __cdecl crit_take( void );
void pw_main( void );

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)
#define F1( al, fn )    (0xF100UL | (al) | ((u32)(fn) << 16))

COUNTRY country = { 0, '-', ':', ',', '.', 0, 437 };

static u8   *psp;
static char  tail[300];
static int   lfn_ok = 1;
static u8    find_rec[320];

void __cdecl dos_entry( void )
{
    pw_main();
}

static void call21( REGS *regs, u32 eax )
{
    regs->eax = eax;
    regs->flags = 0;
    int21( regs );
}

static void clear( REGS *regs )
{
    mem_set( regs, 0, sizeof( *regs ) );
}

static int lfn_missing( REGS *regs )
{
    if ( CF( *regs ) && AX( *regs ) == 0x7100 ) {
        lfn_ok = 0;
        return 1;
    }
    return 0;
}

static int result( REGS *regs )
{
    return CF( *regs ) ? (int)AX( *regs ) : 0;
}

/* as much of "src" as "max" bytes hold, with the NUL */
static void str_fit( char *dst, const char *src, u32 max )
{
    u32 len = str_len( src );

    if ( max == 0 ) {
        return;
    }
    if ( len > max - 1 ) {
        len = max - 1;
    }
    mem_cpy( dst, src, len );
    dst[len] = 0;
}

/* what follows the last \ or : of a path */
static char *name_part( char *path )
{
    char *name = path;

    for ( ; *path; path++ ) {
        if ( *path == '\\' || *path == '/' || *path == ':' ) {
            name = path + 1;
        }
    }
    return name;
}

/* ------------------------------------------------------------------ */
/* the program                                                         */
/* ------------------------------------------------------------------ */

int sys_init( void )
{
    REGS regs;
    const char *full;
    u32 len;
    int quoted;
    char probe[PATH_LEN];

    clear( &regs );
    call21( &regs, 0x6200 );
    psp = (u8 *)regs.ebx;
    len = psp[0x80];
    if ( len > 126 ) {
        len = 126;
    }
    mem_cpy( tail, psp + 0x81, len );
    tail[len] = 0;
    /* a command line too long for the PSP is whole in CMDLINE */
    if ( psp[0x80] == 0x7F && (full = sys_getenv( "CMDLINE" )) != NULL ) {
        quoted = 0;
        while ( *full == ' ' || *full == '\t' ) {
            full++;
        }
        while ( *full && (quoted || (*full != ' ' && *full != '\t' && *full != '/')) ) {
            if ( *full == '"' ) {
                quoted = !quoted;
            }
            full++;
        }
        str_fit( tail, full, sizeof( tail ) );
    }
    /* a drive that will not answer is failed, and whoever asked says so */
    clear( &regs );
    regs.edx = (u32)crit_handler;
    call21( &regs, 0x2524 );

    sys_get_cwd( sys_get_drive(), probe );     /* are there long names? */

    /* 38h: how this country writes a number, a date and a time */
    mem_set( probe, 0, 40 );
    clear( &regs );
    regs.edx = (u32)probe;
    call21( &regs, 0x3800 );
    if ( !CF( regs ) ) {
        country.date_order = probe[0] <= 2 ? probe[0] : 0;
        if ( probe[7] ) {
            country.thousands = probe[7];
            fmt_group_set( probe[7] );
        }
        if ( probe[9] ) {
            country.decimal = probe[9];
        }
        if ( probe[0x0B] ) {
            country.date_sep = probe[0x0B];
        }
        if ( probe[0x0D] ) {
            country.time_sep = probe[0x0D];
        }
        country.clock24 = probe[0x11] & 1;
    }
    /* 6601h: the code page the screen's characters are in */
    clear( &regs );
    call21( &regs, 0x6601 );
    if ( !CF( regs ) && (regs.ebx & 0xFFFF) != 0 ) {
        country.codepage = (int)(regs.ebx & 0xFFFF);
    }
    return 0;
}

const char *sys_cmdline( void )
{
    return tail;
}

const char *sys_getenv( const char *name )
{
    const char *env;
    u32 len = str_len( name );

    env = (const char *)*(u32 *)(psp + 0x2C);
    if ( env == NULL ) {
        return NULL;
    }
    while ( *env ) {
        if ( str_nicmp( env, name, len ) == 0 && env[len] == '=' ) {
            return env + len + 1;
        }
        env += str_len( env ) + 1;
    }
    return NULL;
}

/* Behind the environment's strings the kernel puts what DOS 3 and
   later put there: a word that is 1, and the program's own path, whole
   (DOS\INT21.INC, exec_setenv).  A block too full for the path has a 0
   there, and then the program is looked for the way the shell found
   it - here, then along PATH - as DOSSHELL looks for itself. */
const char *sys_program_dir( void )
{
    static char found[PATH_LEN];
    static const char exe[] = "PMWRITE.EXE";
    char trial[PATH_LEN];
    const char *env = (const char *)*(u32 *)(psp + 0x2C);
    const char *path = sys_getenv( "PATH" );
    u16 attr;
    u32 len, end;

    if ( found[0] ) {
        return found;
    }
    if ( env != NULL ) {
        while ( *env ) {
            env += str_len( env ) + 1;
        }
        env++;                                  /* the empty string that ends them */
        /* believed only if it reads like a whole path: "C:\..." or "\\..." */
        if ( *(const u16 *)env == 1 && str_len( env + 2 ) < PATH_LEN
             && ((env[2] && env[3] == ':') || (env[2] == '\\' && env[3] == '\\')) ) {
            str_cpy( found, env + 2 );
        }
    }
    if ( found[0] == 0 && sys_get_attr( exe, &attr ) == 0 ) {
        sys_truename( exe, found, 0 );
    }
    while ( found[0] == 0 && path && *path ) {
        for ( len = 0; path[len] && path[len] != ';'; len++ ) {
        }
        if ( len && len < PATH_LEN - 16 ) {
            mem_cpy( trial, path, len );
            end = len;                          /* "len" stays the entry's, to step past it */
            if ( trial[end - 1] != '\\' && trial[end - 1] != ':' ) {
                trial[end++] = '\\';
            }
            trial[end] = 0;
            str_catn( trial, exe, sizeof( trial ) );
            if ( sys_get_attr( trial, &attr ) == 0 ) {
                sys_truename( trial, found, 0 );
            }
        }
        path += len;
        while ( *path && *path != ';' ) {       /* a directory too long to try */
            path++;
        }
        if ( *path == ';' ) {
            path++;
        }
    }
    if ( found[0] == 0 ) {                      /* nowhere: beside the work, then */
        sys_truename( exe, found, 0 );
    }
    *name_part( found ) = 0;
    return found;
}

void sys_exit( int code )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x4C00 | ((u32)code & 0xFF) );
}

void *sys_mem_alloc( u32 size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = size;                        /* bytes, on PM-DOS */
    call21( &regs, 0x4800 );
    return CF( regs ) ? NULL : (void *)regs.eax;
}

/* the largest block 48h could give now: it refuses the impossible and
   says in EBX what would have fitted, as DOS does */
u32 sys_mem_largest( void )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 0x7FFFFFF0UL;
    call21( &regs, 0x4800 );
    return CF( regs ) ? regs.ebx : 0;
}

/* ^C is not a way out of a program that has changed the screen mode
   and holds a document nobody has saved */
void sys_break_install( void )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)break_handler;
    call21( &regs, 0x2523 );
    clear( &regs );
    regs.edx = (u32)crit_handler;
    call21( &regs, 0x2524 );
}

void sys_stdout( const char *buf, u32 len )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 1;
    regs.ecx = len;
    regs.edx = (u32)buf;
    call21( &regs, 0x4000 );
}

/* 4400h: a device, and the console's input or output */
static int is_console( u32 handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x4400 );
    return !CF( regs ) && (regs.edx & 0x80) && (regs.edx & 0x03);
}

int sys_stdout_is_console( void )
{
    return is_console( 1 );
}

int sys_stdin_is_console( void )
{
    return is_console( 0 );
}

int sys_stdin_byte( void )
{
    REGS regs;
    u8 ch;

    clear( &regs );
    regs.ebx = 0;
    regs.ecx = 1;
    regs.edx = (u32)&ch;
    call21( &regs, 0x3F00 );
    if ( CF( regs ) || regs.eax == 0 ) {
        return -1;
    }
    return ch;
}

int sys_crit_seen( void )
{
    return crit_take();
}

/* ------------------------------------------------------------------ */
/* the text screen                                                     */
/* ------------------------------------------------------------------ */

static int char_height = 16;

void sys_cls( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF000 );
}

void sys_screen_size( int *rows, int *cols )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF003 );
    *rows = (int)((regs.edx >> 8) & 0xFF);
    *cols = (int)(regs.edx & 0xFF);
    if ( *rows < 5 ) {
        *rows = 25;
    }
    if ( *cols < 40 ) {
        *cols = 80;
    }
    char_height = (int)(regs.ecx & 0xFF);
    if ( char_height < 4 || char_height > 32 ) {
        char_height = 16;
    }
}

void sys_put_cells( u32 first, const u16 *cells, u32 count )
{
    REGS regs;

    clear( &regs );
    regs.ebx = first;
    regs.ecx = count;
    regs.esi = (u32)cells;
    call21( &regs, 0xF004 );
}

void sys_get_cells( u32 first, u16 *cells, u32 count )
{
    REGS regs;

    clear( &regs );
    regs.ebx = first;
    regs.ecx = count;
    regs.edi = (u32)cells;
    call21( &regs, 0xF005 );
}

void sys_cursor( int row, int col )
{
    REGS regs;

    clear( &regs );
    regs.edx = (((u32)row & 0xFF) << 8) | ((u32)col & 0xFF);
    call21( &regs, 0xF002 );
}

void sys_get_cursor( int *row, int *col )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF001 );
    *row = (int)((regs.edx >> 8) & 0xFF);
    *col = (int)(regs.edx & 0xFF);
}

/* The shapes in the controller's scan lines for the font in use, as a
   VGA BIOS would set them. */
void sys_cursor_shape( int kind )
{
    REGS regs;
    int first, last;

    clear( &regs );
    if ( kind == CUR_HIDE ) {
        regs.ecx = 0x2000;
    } else {
        if ( kind == CUR_BLOCK ) {
            first = 0;
            last = char_height - 1;
        } else if ( char_height >= 14 ) {
            first = char_height - 3;
            last = char_height - 2;
        } else {
            first = char_height - 2;
            last = char_height - 1;
        }
        regs.ecx = ((u32)first << 8) | (u32)last;
    }
    call21( &regs, 0xF006 );
}

/* A text screen 80 columns wide with 25 lines, or 43, or 50.  F0h/0Ch
   is MODE's call: mode 3 with the scan lines chosen first (BH: 2 is
   350, 3 is 400) and the 8x8 font put in after it (CL = 1) - 350 lines
   of 8 are 43 rows and 400 are 50.  An EGA has 350 lines whatever it
   is asked for, so 43 is all it gives.  A kernel from before that call
   has F0h/0Ah, which knows 25 lines and "more". */
int sys_set_lines( int lines )
{
    REGS regs;
    int rows, cols;

    clear( &regs );
    regs.ebx = 0x0003 | (lines > 25 && lines < 50 ? 0x0200UL : 0x0300UL);
    regs.ecx = lines > 25 ? 1 : 0;
    call21( &regs, 0xF00C );
    if ( CF( regs ) ) {
        clear( &regs );
        regs.ebx = (u32)lines;
        call21( &regs, 0xF00A );
    }
    sys_screen_size( &rows, &cols );
    return rows;
}

/* ------------------------------------------------------------------ */
/* the keyboard and the clock                                          */
/* ------------------------------------------------------------------ */

/* DH: bit 0 not to wait, bit 1 for the mouse to end the wait as well
   (AL = 2 then: INT 33h says what happened) */
int sys_key_wait( int shift_seen, int *shift_now, int flags )
{
    REGS regs;

    clear( &regs );
    regs.edx = ((u32)(flags & 3) << 8) | (u32)(shift_seen & 0xFF);
    call21( &regs, 0xF008 );
    *shift_now = (int)(regs.ebx & 0xFFFF);
    return (int)(regs.eax & 0xFF);
}

int sys_key_read( int *shift_now )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF007 );
    *shift_now = (int)(regs.ebx & 0xFFFF);
    return (int)(regs.eax & 0xFFFF);
}

int sys_shift( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF009 );
    return (int)(regs.ebx & 0xFFFF);
}

/* 2Ch, as hundredths of a second since midnight */
u32 sys_hundredths( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x2C00 );
    return ((((regs.ecx >> 8) & 0xFF) * 60 + (regs.ecx & 0xFF)) * 60
            + ((regs.edx >> 8) & 0xFF)) * 100 + (regs.edx & 0xFF);
}

/*
 * A CLOCK FINER THAN 2Ch's, which is good to a tick - 55 milliseconds.
 * ACROREAD's (ACROREAD\SYS_D32.C says how it came about).
 *
 * The timer chip's counter 0 can be read from here, a port at a time,
 * with no BIOS in it.  The counter runs from 65536 down to nothing 18.2
 * times a second - or twice that often, counting by twos, in mode 3,
 * which is how most BIOSes leave it; the chip says which.  This adds up
 * how far the counter ran since the last asking, so it keeps time for
 * as long as it is asked more often than the counter goes round, and
 * runs slow - never fast - when it is not.
 */
u32 sys_clock_ms( void )
{
    static int started, by_twos;
    static u32 last, halves, millis;
    u32 now;

    if ( !started ) {
        port_out( 0x43, 0xE2 );             /* read back counter 0's status: */
        by_twos = ((port_in( 0x40 ) >> 1) & 3) == 3;        /* bits 3-1, its mode */
    }
    port_out( 0x43, 0x00 );                 /* hold counter 0 still to be read */
    now = port_in( 0x40 ) & 0xFF;
    now |= (port_in( 0x40 ) & 0xFF) << 8;
    if ( started ) {
        /* in half ticks of the chip's 1,193,182 a second */
        halves += by_twos ? ((last - now) & 0xFFFF) : ((last - now) & 0xFFFF) * 2;
        millis += halves / 2386;
        halves %= 2386;
    }
    started = 1;
    last = now;
    return millis;
}

/* 2Ah and 2Ch */
void sys_now( DATETIME *now )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x2A00 );
    now->year = (int)(regs.ecx & 0xFFFF);
    now->month = (int)((regs.edx >> 8) & 0xFF);
    now->day = (int)(regs.edx & 0xFF);
    clear( &regs );
    call21( &regs, 0x2C00 );
    now->hour = (int)((regs.ecx >> 8) & 0xFF);
    now->minute = (int)(regs.ecx & 0xFF);
    now->second = (int)((regs.edx >> 8) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

static void call33( REGS *regs, u32 eax )
{
    regs->eax = eax;
    int33( regs );
}

int sys_mouse_reset( void )
{
    REGS regs;

    clear( &regs );
    call33( &regs, 0x0000 );
    return (regs.eax & 0xFFFF) == 0xFFFF ? (int)(regs.ebx & 0xFFFF) : 0;
}

void sys_mouse_show( int show )
{
    REGS regs;

    clear( &regs );
    call33( &regs, show ? 0x0001 : 0x0002 );
}

static int wheel_known, wheel_there, wheel_turned;

void sys_mouse_state( int *x, int *y, int *buttons )
{
    REGS regs;

    clear( &regs );
    call33( &regs, 0x0003 );
    *buttons = (int)(regs.ebx & 7);
    *x = (int)(regs.ecx & 0xFFFF);
    *y = (int)(regs.edx & 0xFFFF);
    if ( wheel_there ) {
        wheel_turned += (int)(s8)((regs.ebx >> 8) & 0xFF);  /* BH: since the last 03h */
    }
}

/*
 * A WHEEL, where the driver has one.  NOT IN SYS.H - SCR.C declares it
 * for itself (DISPINT.H) - because the kernel's own driver has no
 * wheel and nothing may count on one.  The call is the one CuteMouse
 * added and others took up: 11h answers AX = 574Dh ("WM") with bit 0
 * of CX set when there is a wheel, and from then on 03h's BH is how
 * far it turned since the last 03h, signed, away from the hand
 * negative.  Returns that, added up since it was last asked; always 0
 * on a driver that does not know the call.
 */
int sys_mouse_wheel( void )
{
    REGS regs;
    int turned;

    if ( !wheel_known ) {
        wheel_known = 1;
        clear( &regs );
        call33( &regs, 0x0011 );
        wheel_there = (regs.eax & 0xFFFF) == 0x574D && (regs.ecx & 1);
    }
    turned = wheel_turned;
    wheel_turned = 0;
    return turned;
}

/* 05h and 06h: the presses (or releases) of a button since last asked,
   and where the last one happened */
int sys_mouse_count( int release, int button, int *x, int *y )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)button;
    call33( &regs, release ? 0x0006 : 0x0005 );
    *x = (int)(regs.ecx & 0xFFFF);
    *y = (int)(regs.edx & 0xFFFF);
    return (int)(regs.ebx & 0xFFFF);
}

/* 26h: the largest position the driver will give on this screen */
void sys_mouse_extent( int *xmax, int *ymax )
{
    REGS regs;

    clear( &regs );
    call33( &regs, 0x0026 );
    *xmax = (int)(regs.ecx & 0xFFFF);
    *ymax = (int)(regs.edx & 0xFFFF);
}

/* 07h and 08h: the driver knows the BIOS's own graphics modes and
   nothing of a VBE one, so the screen's size is told to it */
void sys_mouse_range( int xmax, int ymax )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)xmax;
    call33( &regs, 0x0007 );
    clear( &regs );
    regs.edx = (u32)ymax;
    call33( &regs, 0x0008 );
}

void sys_mouse_move( int x, int y )
{
    REGS regs;

    clear( &regs );
    regs.ecx = (u32)x;
    regs.edx = (u32)y;
    call33( &regs, 0x0004 );
}

/* 21h/F0h AL=0Bh: the buttons as if the mouse had changed them where it
   stands - how the test key files press and let go */
void sys_mouse_inject( int buttons )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)(buttons & 7);
    call21( &regs, 0xF00B );
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

int sys_lfn( void )
{
    return lfn_ok;
}

static int open_ext( const char *path, u32 mode, u32 action, u32 *handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = mode;
    regs.ecx = 0;
    regs.edx = action;
    regs.esi = (u32)path;
    call21( &regs, 0x716C );
    if ( lfn_missing( &regs ) ) {
        return -1;
    }
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *handle = AX( regs );
    return 0;
}

static int open_classic( const char *path, u32 func, u32 *handle )
{
    REGS regs;

    clear( &regs );
    regs.ecx = 0;
    regs.edx = (u32)path;
    call21( &regs, func );
    *handle = AX( regs );
    return result( &regs );
}

int sys_open_read( const char *path, u32 *handle )
{
    int rc;

    if ( lfn_ok ) {
        rc = open_ext( path, 0x0000, 0x0001, handle );
        if ( rc >= 0 ) {
            return rc;
        }
    }
    return open_classic( path, 0x3D00, handle );
}

int sys_open_write( const char *path, u32 *handle )
{
    return open_classic( path, 0x3D01, handle );    /* PRN, LPT1, COM1... */
}

int sys_create( const char *path, u32 *handle )
{
    int rc;

    if ( lfn_ok ) {
        rc = open_ext( path, 0x0002, 0x0012, handle );
        if ( rc >= 0 ) {
            return rc;
        }
    }
    return open_classic( path, 0x3C00, handle );
}

int sys_read( u32 handle, void *buf, u32 len, u32 *done )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    call21( &regs, 0x3F00 );
    *done = CF( regs ) ? 0 : regs.eax;
    return result( &regs );
}

int sys_write( u32 handle, const void *buf, u32 len, u32 *done )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    call21( &regs, 0x4000 );
    if ( CF( regs ) ) {
        *done = 0;
        return (int)AX( regs );
    }
    *done = regs.eax;
    return regs.eax < len ? ERR_DISKFULL : 0;
}

int sys_close( u32 handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x3E00 );
    return result( &regs );
}

/* 42h, the native form: the offset in ECX and the new position in EAX,
   32 bits each - NOT DOS's CX:DX and DX:AX. */
int sys_file_size( u32 handle, u32 *size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = 0;
    call21( &regs, 0x4202 );                /* to the end: the size */
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *size = regs.eax;
    return sys_seek( handle, 0 );
}

int sys_seek( u32 handle, u32 pos )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = pos;
    call21( &regs, 0x4200 );
    return result( &regs );
}

int sys_get_ftime( u32 handle, u16 *time, u16 *date )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x5700 );
    *time = (u16)regs.ecx;
    *date = (u16)regs.edx;
    return result( &regs );
}

int sys_set_ftime( u32 handle, u16 time, u16 date )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = time;
    regs.edx = date;
    call21( &regs, 0x5701 );
    return result( &regs );
}

int sys_get_attr( const char *path, u16 *attr )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = 0;
        regs.edx = (u32)path;
        call21( &regs, 0x7143 );
        if ( !lfn_missing( &regs ) ) {
            *attr = (u16)regs.ecx;
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, 0x4300 );
    *attr = (u16)regs.ecx;
    return result( &regs );
}

int sys_set_attr( const char *path, u16 attr )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = 1;
        regs.ecx = attr;
        regs.edx = (u32)path;
        call21( &regs, 0x7143 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.ecx = attr;
    regs.edx = (u32)path;
    call21( &regs, 0x4301 );
    return result( &regs );
}

/* one path in EDX, by its long name and then by the classic call */
static int path_call( const char *path, u32 lfn_func, u32 func )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (u32)path;
        call21( &regs, lfn_func );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, func );
    return result( &regs );
}

int sys_delete( const char *path )
{
    return path_call( path, 0x7141, 0x4100 );   /* ESI = 0: no wildcards */
}

int sys_mkdir( const char *path )
{
    return path_call( path, 0x7139, 0x3900 );
}

int sys_rmdir( const char *path )
{
    return path_call( path, 0x713A, 0x3A00 );
}

int sys_chdir( const char *path )
{
    return path_call( path, 0x713B, 0x3B00 );
}

int sys_rename( const char *from, const char *to )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (u32)from;
        regs.edi = (u32)to;
        call21( &regs, 0x7156 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)from;
    regs.edi = (u32)to;
    call21( &regs, 0x5600 );
    return result( &regs );
}

/* ------------------------------------------------------------------ */
/* searching                                                           */
/* ------------------------------------------------------------------ */

static u8 dta_required;

static void rec_from_lfn( FINDREC *rec )
{
    rec->attr = find_rec[0];
    rec->time = *(u16 *)(find_rec + 0x14);      /* last written, DOS's form */
    rec->date = *(u16 *)(find_rec + 0x16);
    rec->size = *(u32 *)(find_rec + 0x20);
    str_fit( rec->name, (const char *)find_rec + 0x2C, sizeof( rec->name ) );
    str_fit( rec->alias, (const char *)find_rec + 0x130, sizeof( rec->alias ) );
    if ( rec->alias[0] == 0 ) {
        str_fit( rec->alias, rec->name, sizeof( rec->alias ) );
    }
}

static void rec_from_dta( FINDREC *rec )
{
    rec->attr = find_rec[0x15];
    rec->time = *(u16 *)(find_rec + 0x16);
    rec->date = *(u16 *)(find_rec + 0x18);
    rec->size = *(u32 *)(find_rec + 0x1A);
    str_fit( rec->name, (const char *)find_rec + 0x1E, 13 );
    str_fit( rec->alias, rec->name, sizeof( rec->alias ) );
}

static int dta_skip( void )
{
    return (find_rec[0x15] & dta_required) != dta_required;
}

/* "attrs": the attributes a name may have in the low byte, and the
   ones it must have in the high byte.  SI = 1 asks for DOS's dates. */
int sys_find_first( const char *pattern, u16 attrs, FINDREC *rec, u32 *handle )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ecx = attrs;
        regs.edx = (u32)pattern;
        regs.edi = (u32)find_rec;
        regs.esi = 1;
        call21( &regs, 0x714E );
        if ( !lfn_missing( &regs ) ) {
            if ( CF( regs ) ) {
                return (int)AX( regs );
            }
            *handle = AX( regs );
            rec_from_lfn( rec );
            return 0;
        }
    }
    clear( &regs );
    regs.edx = (u32)find_rec;
    call21( &regs, 0x1A00 );
    dta_required = (u8)(attrs >> 8);
    clear( &regs );
    regs.ecx = attrs & 0xFF;
    regs.edx = (u32)pattern;
    call21( &regs, 0x4E00 );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *handle = 0;
    if ( dta_skip() ) {
        return sys_find_next( 0, rec );
    }
    rec_from_dta( rec );
    return 0;
}

int sys_find_next( u32 handle, FINDREC *rec )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = handle;
        regs.edi = (u32)find_rec;
        regs.esi = 1;
        call21( &regs, 0x714F );
        if ( CF( regs ) ) {
            return (int)AX( regs );
        }
        rec_from_lfn( rec );
        return 0;
    }
    do {
        clear( &regs );
        call21( &regs, 0x4F00 );
        if ( CF( regs ) ) {
            return (int)AX( regs );
        }
    } while ( dta_skip() );
    rec_from_dta( rec );
    return 0;
}

void sys_find_close( u32 handle )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = handle;
        call21( &regs, 0x71A1 );
    }
}

/* ------------------------------------------------------------------ */
/* drives and directories                                              */
/* ------------------------------------------------------------------ */

int sys_get_drive( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x1900 );
    return (int)(regs.eax & 0xFF);
}

void sys_set_drive( int drv )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)drv;
    call21( &regs, 0x0E00 );
}

/* 4409h: is there such a letter?  Only "invalid drive" says no - a
   drive with nothing in the drive is still a drive */
int sys_drive_valid( int drv )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)drv + 1;
    call21( &regs, 0x4409 );
    return !(CF( regs ) && AX( regs ) == ERR_BADDRIVE);
}

int sys_get_cwd( int drv, char *buf )
{
    REGS regs;

    buf[0] = 0;
    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (u32)drv + 1;
        regs.esi = (u32)buf;
        call21( &regs, 0x7147 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)drv + 1;
    regs.esi = (u32)buf;
    call21( &regs, 0x4700 );
    return result( &regs );
}

int sys_truename( const char *path, char *out, int form )
{
    REGS regs;

    out[0] = 0;
    if ( lfn_ok ) {
        clear( &regs );
        regs.ecx = (u32)form;
        regs.esi = (u32)path;
        regs.edi = (u32)out;
        call21( &regs, 0x7160 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.esi = (u32)path;
    regs.edi = (u32)out;
    call21( &regs, 0x6000 );
    return result( &regs );
}

/* F1h/06h: the volume table's entry for a letter - the BIOS drive, the
   partition type, DRVF_* flags and where the volume is, twelve bytes.
   Nothing is mounted to answer, so a diskette drive with no diskette
   in the drive is still a diskette drive. */
int sys_drive_kind( int drv )
{
    REGS regs;
    u8 rec[12];

    mem_set( rec, 0, sizeof( rec ) );
    clear( &regs );
    regs.edx = (u32)drv;
    regs.edi = (u32)rec;
    call21( &regs, 0xF106 );
    if ( CF( regs ) || !(rec[2] & 0x01) ) {
        return DK_NONE;
    }
    if ( rec[2] & 0x08 ) {
        return DK_CDROM;
    }
    if ( rec[2] & 0x02 ) {
        return DK_FLOPPY;
    }
    if ( rec[2] & 0x14 ) {
        return DK_OTHER;
    }
    return DK_FIXED;
}

static void to_kb( u32 cluster_bytes, u32 free_clusters, u32 total_clusters,
                   u32 *free_kb, u32 *total_kb )
{
    if ( cluster_bytes >= 1024 ) {
        *free_kb = free_clusters * (cluster_bytes / 1024);
        *total_kb = total_clusters * (cluster_bytes / 1024);
    } else {
        *free_kb = free_clusters / (1024 / cluster_bytes);
        *total_kb = total_clusters / (1024 / cluster_bytes);
    }
}

/* 7303h, which counts in 32 bits, and 36h where the kernel has not got
   it.  The record (F73_* in PMDOS.INC) is DOS 7.1's: its size, its
   version, then sectors a cluster, bytes a sector, clusters free and
   clusters in all, a dword each.  Up to v0.69 the size word was
   missing and everything sat two bytes early; a first word of 0 says
   which (SETUP's way). */
int sys_disk_space( int drv, u32 *free_kb, u32 *total_kb )
{
    static u8 ext[48];
    static char root[4] = "A:\\";
    REGS regs;
    u32 spc, bps, cluster;
    const u8 *at;

    *free_kb = *total_kb = 0;
    root[0] = (char)('A' + drv);
    mem_set( ext, 0, sizeof( ext ) );
    clear( &regs );
    regs.edx = (u32)root;
    regs.edi = (u32)ext;
    regs.ecx = sizeof( ext );
    call21( &regs, 0x7303 );
    at = *(u16 *)ext == 0 ? ext + 2 : ext + 4;
    spc = *(u32 *)at;
    bps = *(u32 *)(at + 4);
    if ( !CF( regs ) && spc && spc <= 0x100 && bps >= 128 && bps <= 0x1000 ) {
        to_kb( spc * bps, *(u32 *)(at + 8), *(u32 *)(at + 12), free_kb, total_kb );
        return 1;
    }
    clear( &regs );
    regs.edx = (u32)drv + 1;
    call21( &regs, 0x3600 );
    if ( (regs.eax & 0xFFFF) == 0xFFFF ) {
        return 0;
    }
    cluster = (regs.eax & 0xFFFF) * (regs.ecx & 0xFFFF);
    if ( cluster == 0 ) {
        return 0;
    }
    to_kb( cluster, regs.ebx & 0xFFFF, regs.edx & 0xFFFF, free_kb, total_kb );
    return 1;
}

/* ------------------------------------------------------------------ */
/* another program                                                     */
/* ------------------------------------------------------------------ */

/* 4B00h: EDX -> the program, EBX -> two pointers, the environment the
   child is given a copy of and its command tail in DOS's form - a
   length, the text, a carriage return.  4Dh has its exit code. */
int sys_exec( const char *prog, const char *args, int *error )
{
    static u8  tail_buf[132];
    static u32 parm[2];
    REGS regs;
    u32 len = str_len( args );

    if ( len > 126 ) {
        len = 126;
    }
    tail_buf[0] = (u8)len;
    mem_cpy( tail_buf + 1, args, len );
    tail_buf[1 + len] = 0x0D;
    parm[0] = *(u32 *)(psp + 0x2C);
    parm[1] = (u32)tail_buf;
    clear( &regs );
    regs.edx = (u32)prog;
    regs.ebx = (u32)parm;
    call21( &regs, 0x4B00 );
    if ( CF( regs ) ) {
        *error = (int)AX( regs );
        return -1;
    }
    *error = 0;
    clear( &regs );
    call21( &regs, 0x4D00 );
    return (int)(regs.eax & 0xFF);
}

/* F1h/03h: the interpreter the kernel started, which is the one that
   reads PATH and runs batch files */
void sys_shell_path( char *buf, u32 max )
{
    static char name[96];
    REGS regs;

    name[0] = 0;
    clear( &regs );
    regs.edx = (u32)name;
    call21( &regs, 0xF103 );
    if ( CF( regs ) || name[0] == 0 ) {
        str_cpy( name, "CMD.EXE" );
    }
    str_fit( buf, name, max );
}

/* F1h/00h: the kernel's version, which 30h cannot say - it answers as
   DOS 5 does, for the programs that ask */
void sys_version( int *major, int *minor )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF100 );
    *major = (int)((regs.ebx >> 8) & 0xFF);
    *minor = (int)(regs.ebx & 0xFF);
}

/* ------------------------------------------------------------------ */
/* a graphics screen                                                   */
/* ------------------------------------------------------------------ */

int sys_peek( u32 addr, void *buf, u32 len )
{
    REGS regs;

    clear( &regs );
    regs.ebx = addr;
    regs.ecx = len;
    regs.edi = (u32)buf;
    call21( &regs, F1( 0x20, 0 ) );
    if ( CF( regs ) ) {
        mem_set( buf, 0, len );
        return 0;
    }
    return 1;
}

int sys_bios( int vector, BREGS *bregs )
{
    REGS regs;

    bregs->es = bregs->ds = 0;
    bregs->flags = 0;
    clear( &regs );
    regs.edx = (u32)vector;
    regs.esi = (u32)bregs;
    call21( &regs, 0xF121 );
    return !CF( regs );
}

/* VBF_INFO: 4F00h's block, its mode list copied into it and its
   pointer made ours */
int sys_vbe_version( u16 *modes, int max, int *count )
{
    static u8 block[512];
    REGS regs;
    const u16 *list;
    int found = 0;

    *count = 0;
    mem_set( block, 0, sizeof( block ) );
    clear( &regs );
    regs.edi = (u32)block;
    call21( &regs, F1( 0x1F, 0 ) );
    if ( CF( regs ) ) {
        return 0;
    }
    list = (const u16 *)*(u32 *)(block + 0x0E);
    if ( list ) {
        while ( *list != 0xFFFF && found < max ) {
            modes[found++] = *list++;
        }
    }
    *count = found;
    return (int)(regs.eax & 0xFFFF);
}

int sys_vbe_mode( int mode, u8 *info )
{
    REGS regs;

    mem_set( info, 0, 256 );
    clear( &regs );
    regs.ecx = (u32)mode;
    regs.edi = (u32)info;
    call21( &regs, F1( 0x1F, 1 ) );
    return !CF( regs );
}

int sys_video_set( u32 mode, u32 *selector, u32 *bytes, u32 *pitch )
{
    REGS regs;

    clear( &regs );
    regs.ebx = mode;
    call21( &regs, F1( 0x1F, 2 ) );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *selector = regs.eax & 0xFFFF;
    *bytes = regs.ecx;
    *pitch = regs.edx;
    return 0;
}

void sys_video_restore( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F1( 0x1F, 3 ) );
}

int sys_video_palette( int first, int count, const u8 *entries )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 0;
    regs.ecx = (u32)count;
    regs.edx = (u32)first;
    regs.edi = (u32)entries;
    call21( &regs, F1( 0x1F, 4 ) );
    return !CF( regs );
}
