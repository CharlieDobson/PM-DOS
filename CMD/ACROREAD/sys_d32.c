/*
 * SYS_D32.C - ACROREAD's system calls, PM-DOS native.
 *
 * ACROREAD.EXE is a D32 image loaded at offset 2000h of a program slot
 * (512K to start with; 48h grows it, which is where a page's canvas
 * comes from) and run at ring 3.  INT 21h is the real-mode interface
 * widened to 32 bits, as EDIT's SYS_D32.C says.
 *
 * THE TEXT SCREEN AND THE KEYBOARD are 21h/F0h, the kernel's console
 * calls for a full-screen program that cannot reach either itself.
 *
 * A GRAPHICS SCREEN is 21h/F1h AL=1Fh (DOS\VBE.INC).  The kernel sets
 * the mode - a VGA one through INT 10h, a VBE one with its linear
 * frame buffer - and describes the frame buffer with a selector,
 * SEL_VBE, which is the only thing a native program has that reaches
 * outside its slot.  gfx_put in DOSINT.ASM copies a scan line through
 * it.  The mode a program leaves set, its exit puts back.
 *
 * THE MOUSE is INT 33h, the kernel's own driver answering Microsoft's
 * calls with whole registers.
 *
 * Files go by their long names (71xxh) and fall back to the classic
 * calls the first time the kernel answers 7100h.
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
void acro_main( void );

#define CF( regs )      ((regs).flags & 1)
#define AX( regs )      ((regs).eax & 0xFFFF)
#define F1( al, fn )    (0xF100UL | (al) | ((u32)(fn) << 16))

#define VBF_INFO        0
#define VBF_MODE        1
#define VBF_SET         2
#define VBF_RESTORE     3
#define VBF_PALETTE     4
#define VBM_LFB         0x4000UL

static u8   *psp;
static char  tail[300];
static int   lfn_ok = 1;

void __cdecl dos_entry( void )
{
    acro_main();
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

static int result( REGS *regs )
{
    return CF( *regs ) ? (int)AX( *regs ) : 0;
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
        str_cpyn( tail, full, sizeof( tail ) );
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

/* ^C is not a way out of a program that has changed the screen mode,
   and a drive that will not answer is failed and reported */
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

/* ------------------------------------------------------------------ */
/* the text screen                                                     */
/* ------------------------------------------------------------------ */

static int char_height = 16;

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
    regs.edx = ((u32)row << 8) | (u32)col;
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

void sys_cursor_shape( int kind )
{
    REGS regs;

    clear( &regs );
    if ( kind == CUR_HIDE ) {
        regs.ecx = 0x2000;
    } else if ( char_height >= 14 ) {
        regs.ecx = ((u32)(char_height - 3) << 8) | (u32)(char_height - 2);
    } else {
        regs.ecx = ((u32)(char_height - 2) << 8) | (u32)(char_height - 1);
    }
    call21( &regs, 0xF006 );
}

/* ------------------------------------------------------------------ */
/* the keyboard                                                        */
/* ------------------------------------------------------------------ */

/* F0h/08h with DH bit 0: look, and come back.  AL = 1 if a key waits. */
int sys_key_ready( void )
{
    REGS regs;

    clear( &regs );
    regs.edx = 0x0100;
    call21( &regs, 0xF008 );
    return (regs.eax & 0xFF) == 1;
}

int sys_key_read( int *shift_now )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF007 );
    *shift_now = (int)(regs.ebx & 0xFFFF);
    return (int)(regs.eax & 0xFFFF);
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
 * A CLOCK TO HOLD A BUTTON DOWN BY, finer than 2Ch's.  When this was
 * written 2Ch was no use for it at all: the kernel asked the BIOS for
 * the real-time clock's time on every call, which has whole seconds and
 * no hundredths, and a loop that asked over and over while a button was
 * held kept the machine in the BIOS and never heard the button let go.
 * The kernel counts the timer's ticks on from the clock's time now
 * (DOS\INT21.INC, fn2C), and 2Ch is good to a tick, 55 milliseconds.
 * This is kept for being finer than that.
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

/* F0h/08h: DH bit 0 not to wait, bit 1 for the mouse to end the wait
   as well.  The call also comes back, with AL = 0, when a shift key has
   changed from what DL says was last seen - which is nothing here, so
   the wait goes round again with the shift keys as they now are. */
int sys_event( int flags )
{
    static u32 shift_seen;
    REGS regs;

    for ( ;; ) {
        clear( &regs );
        regs.edx = ((u32)(flags & 3) << 8) | (shift_seen & 0xFF);
        call21( &regs, 0xF008 );
        shift_seen = regs.ebx & 0xFF;
        if ( (regs.eax & 0xFF) != 0 || (flags & EVF_NOWAIT) ) {
            return (int)(regs.eax & 0xFF);
        }
    }
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

void sys_mouse_state( int *xpos, int *ypos, int *buttons )
{
    REGS regs;

    clear( &regs );
    call33( &regs, 0x0003 );
    *buttons = (int)(regs.ebx & 7);
    *xpos = (int)(regs.ecx & 0xFFFF);
    *ypos = (int)(regs.edx & 0xFFFF);
}

/* 05h: the presses of the left button since last asked, and where the
   last of them was */
int sys_mouse_presses( int *xpos, int *ypos )
{
    REGS regs;

    clear( &regs );
    call33( &regs, 0x0005 );
    *xpos = (int)(regs.ecx & 0xFFFF);
    *ypos = (int)(regs.edx & 0xFFFF);
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

void sys_mouse_move( int xpos, int ypos )
{
    REGS regs;

    clear( &regs );
    regs.ecx = (u32)xpos;
    regs.edx = (u32)ypos;
    call33( &regs, 0x0004 );
}

/* 21h/F0h AL=0Bh: the buttons as if the mouse had changed them where it
   stands - how a test's key file presses and lets go */
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

int sys_open_read( const char *path, u32 *handle )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = 0x0000;                  /* read */
        regs.edx = 0x0001;                  /* open it if it is there */
        regs.esi = (u32)path;
        call21( &regs, 0x716C );
        if ( CF( regs ) && AX( regs ) == 0x7100 ) {
            lfn_ok = 0;
        } else {
            *handle = AX( regs );
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, 0x3D00 );
    *handle = AX( regs );
    return result( &regs );
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

/* 42h, the native form: the offset in ECX and the new position in EAX,
   32 bits each - not DOS's CX:DX and DX:AX */
int sys_seek( u32 handle, u32 pos )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = pos;
    call21( &regs, 0x4200 );
    return result( &regs );
}

int sys_close( u32 handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x3E00 );
    return result( &regs );
}

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

/* ------------------------------------------------------------------ */
/* a graphics screen                                                   */
/* ------------------------------------------------------------------ */

int sys_vbe_info( u8 *block, int *version, int *modes )
{
    REGS regs;

    mem_set( block, 0, VBE_INFO_LEN );
    clear( &regs );
    regs.edi = (u32)block;
    call21( &regs, F1( 0x1F, VBF_INFO ) );
    if ( CF( regs ) ) {
        return 0;
    }
    *version = (int)(regs.eax & 0xFFFF);
    *modes = (int)regs.ecx;
    return 1;
}

int sys_vbe_mode( int mode, u8 *block )
{
    REGS regs;

    mem_set( block, 0, VBE_MODE_LEN );
    clear( &regs );
    regs.ecx = (u32)mode;
    regs.edi = (u32)block;
    call21( &regs, F1( 0x1F, VBF_MODE ) );
    return !CF( regs );
}

int sys_gfx_set( int mode, u32 *bytes, u32 *pitch )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)mode;
    if ( mode >= 0x100 ) {
        regs.ebx |= VBM_LFB;
    }
    call21( &regs, F1( 0x1F, VBF_SET ) );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *bytes = regs.ecx;
    *pitch = regs.edx;
    return 0;
}

void sys_gfx_restore( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F1( 0x1F, VBF_RESTORE ) );
}

/* "bgr0" is four bytes an entry: blue, green, red, nothing - in the
   width of the DAC, which is six bits unless something widened it */
void sys_gfx_palette( int first, int count, const u8 *bgr0 )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 0;
    regs.ecx = (u32)count;
    regs.edx = (u32)first;
    regs.edi = (u32)bgr0;
    call21( &regs, F1( 0x1F, VBF_PALETTE ) );
}
