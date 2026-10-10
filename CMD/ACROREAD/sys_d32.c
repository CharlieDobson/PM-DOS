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
