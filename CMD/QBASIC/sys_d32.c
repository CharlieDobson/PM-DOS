/*
 * SYS_D32.C - QBASIC's system calls, PM-DOS native.
 *
 * QBASIC.EXE is a D32 image loaded at offset 2000h of a program slot and
 * run at ring 3, as EDIT is (see EDIT's SYS_D32.C, which most of this
 * is).  What QBASIC asks for beyond an editor's needs:
 *
 *   21h/F1h AL=1Fh  a graphics mode set by the kernel, and a selector
 *                   for its video memory; the DAC
 *   21h/F1h AL=20h  the first megabyte read: the BIOS data area, the
 *                   ROM's fonts, for PEEK and for drawing text
 *   21h/F1h AL=13h  is there a coprocessor?  The interpreter's numbers
 *                   are the x87's, and PM-DOS does not emulate one
 *   21h/F0h AL=0Ch  a text mode (WIDTH 40, the 43 and 50 line screens)
 *   2Ah-2Dh         the date and the time, read and set
 *   4B00h           SHELL: another program in a slot of its own
 *
 * Files go by their long names (71xxh) and fall back to the classic
 * calls the first time the kernel answers 7100h, as EDIT's do.
 */
#include "rt.h"
#include "sys.h"

typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi;
    uint32_t flags;             /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void __cdecl int33( REGS *regs );
void __cdecl break_handler( void );
void qbasic_main( void );

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)
#define F0( al )    (0xF000UL | (al))
#define F1( al, fn )    (0xF100UL | (al) | ((uint32_t)(fn) << 16))

static uint8_t *psp;
static char     tail[300];
static int      lfn_ok = 1;
static uint8_t  find_rec[320];

void __cdecl dos_entry( void )
{
    qbasic_main();
}

static void call21( REGS *regs, uint32_t eax )
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

/* ------------------------------------------------------------------ */
/* the program                                                         */
/* ------------------------------------------------------------------ */

int sys_init( void )
{
    REGS regs;
    const char *full;
    unsigned int len;
    int quoted;
    char probe[PATH_MAX];

    clear( &regs );
    call21( &regs, 0x6200 );
    psp = (uint8_t *)regs.ebx;
    len = psp[0x80];
    if ( len > 126 ) {
        len = 126;
    }
    mem_cpy( tail, psp + 0x81, len );
    tail[len] = 0;
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
    sys_get_cwd( sys_get_drive(), probe );     /* are there long names? */
    return 0;
}

const char *sys_cmdline( void )
{
    return tail;
}

const char *sys_environment( void )
{
    const char *env = (const char *)*(uint32_t *)(psp + 0x2C);

    return env != NULL ? env : "\0";
}

const char *sys_getenv( const char *name )
{
    const char *env = sys_environment();
    unsigned int len = str_len( name );

    while ( *env ) {
        if ( str_nicmp( env, name, len ) == 0 && env[len] == '=' ) {
            return env + len + 1;
        }
        env += str_len( env ) + 1;
    }
    return NULL;
}

/* F1h/03h: the command interpreter the kernel started, for SHELL when
   there is no COMSPEC */
const char *sys_shell( void )
{
    static char name[96];
    REGS regs;

    name[0] = 0;
    clear( &regs );
    regs.edx = (uint32_t)name;
    call21( &regs, F1( 0x03, 0 ) );
    if ( CF( regs ) || name[0] == 0 ) {
        str_cpy( name, "CMD.EXE" );
    }
    return name;
}

void sys_exit( int code )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x4C00 | ((uint32_t)code & 0xFF) );
}

void *sys_mem_alloc( unsigned int size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = size;                        /* bytes, on PM-DOS */
    call21( &regs, 0x4800 );
    return CF( regs ) ? NULL : (void *)regs.eax;
}

/* the largest block 48h could give now: it refuses the impossible and
   says in EBX what would have fitted, as DOS does */
unsigned int sys_mem_largest( void )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 0x7FFFFFF0UL;
    call21( &regs, 0x4800 );
    return CF( regs ) ? regs.ebx : 0;
}

void sys_break_install( void )
{
    REGS regs;

    clear( &regs );
    regs.edx = (uint32_t)break_handler;
    call21( &regs, 0x2523 );
}

void sys_stdout( const char *buf, unsigned int len )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 1;
    regs.ecx = len;
    regs.edx = (uint32_t)buf;
    call21( &regs, 0x4000 );
}

int sys_stdin_is_console( void )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 0;
    call21( &regs, 0x4400 );
    return !CF( regs ) && (regs.edx & 0x81) == 0x81;
}

int sys_stdin_byte( void )
{
    REGS regs;
    uint8_t ch;

    clear( &regs );
    regs.ebx = 0;
    regs.ecx = 1;
    regs.edx = (uint32_t)&ch;
    call21( &regs, 0x3F00 );
    if ( CF( regs ) || regs.eax == 0 ) {
        return -1;
    }
    return ch;
}

/* F1h AL=13h: the kernel's coprocessor flags; FPUF_X87 is bit 0 */
int sys_fpu_present( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F1( 0x13, 0 ) );
    return !CF( regs ) && (regs.eax & 0x01) != 0;
}

/* 4B00h: EDX -> the program, EBX -> two pointers, the environment the
   child gets and its command tail.  This program waits in its slot */
/* "env": a block of ASCIIZ strings and an empty one (the kernel copies it
   into the child), or NULL for the one QBasic was given */
int sys_exec( const char *program, const char *args, const char *env )
{
    static uint8_t  tail_buf[132];
    static uint32_t parm[2];
    REGS regs;
    unsigned int len = str_len( args );

    if ( len > 126 ) {
        len = 126;
    }
    tail_buf[0] = (uint8_t)len;
    mem_cpy( tail_buf + 1, args, len );
    tail_buf[1 + len] = 0x0D;
    parm[0] = env != NULL ? (uint32_t)env : *(uint32_t *)(psp + 0x2C);
    parm[1] = (uint32_t)tail_buf;
    clear( &regs );
    regs.edx = (uint32_t)program;
    regs.ebx = (uint32_t)parm;
    call21( &regs, 0x4B00 );
    return result( &regs );
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

static int char_height = 16;

void sys_screen_size( int *rows, int *cols )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F0( 0x03 ) );
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

void sys_put_cells( unsigned int first, const uint16_t *cells, unsigned int count )
{
    REGS regs;

    clear( &regs );
    regs.ebx = first;
    regs.ecx = count;
    regs.esi = (uint32_t)cells;
    call21( &regs, F0( 0x04 ) );
}

void sys_get_cells( unsigned int first, uint16_t *cells, unsigned int count )
{
    REGS regs;

    clear( &regs );
    regs.ebx = first;
    regs.ecx = count;
    regs.edi = (uint32_t)cells;
    call21( &regs, F0( 0x05 ) );
}

void sys_cursor( int row, int col )
{
    REGS regs;

    clear( &regs );
    regs.edx = ((uint32_t)row << 8) | (uint32_t)col;
    call21( &regs, F0( 0x02 ) );
}

void sys_get_cursor( int *row, int *col )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F0( 0x01 ) );
    *row = (int)((regs.edx >> 8) & 0xFF);
    *col = (int)(regs.edx & 0xFF);
}

/* The shapes in the controller's scan lines for the font in use, as a
   VGA BIOS would set them: an underline two lines up from the bottom of
   a 14- or 16-line cell and on the last two of an 8-line one; a block
   from the top of the cell to the second-last line. */
void sys_cursor_shape( int kind )
{
    REGS regs;
    int first, last;

    clear( &regs );
    if ( kind == CUR_HIDE ) {
        regs.ecx = 0x2000;
    } else {
        if ( kind == CUR_BLOCK ) {
            first = 1;
            last = char_height - 1;
        } else if ( char_height >= 14 ) {
            first = char_height - 2;
            last = char_height - 1;
        } else {
            first = char_height - 2;
            last = char_height - 1;
        }
        regs.ecx = ((uint32_t)first << 8) | (uint32_t)last;
    }
    call21( &regs, F0( 0x06 ) );
}

/* LOCATE's own start and stop lines, as the program gave them */
void sys_cursor_lines( int first, int last )
{
    REGS regs;

    clear( &regs );
    regs.ecx = ((uint32_t)(first & 0x1F) << 8) | (uint32_t)(last & 0x1F);
    call21( &regs, F0( 0x06 ) );
}

int sys_set_lines( int lines )
{
    REGS regs;
    int rows, cols;

    clear( &regs );
    regs.ebx = (uint32_t)lines;
    call21( &regs, F0( 0x0A ) );
    sys_screen_size( &rows, &cols );
    return rows;
}

/* F0h AL=0Ch: a text mode (1 for 40 columns, 3 for 80), with the scan
   lines and the font for 25, 43 or 50 lines.  -> 1 if it was set */
int sys_text_mode( int mode, int lines )
{
    REGS regs;
    int scan = lines == 43 ? 2 : 3;     /* 350 lines for 43, else 400 */
    int rows, cols;

    clear( &regs );
    regs.ebx = (uint32_t)(mode & 0xFF) | ((uint32_t)scan << 8);
    regs.ecx = lines > 25 ? 1 : 0;      /* the 8x8 font */
    call21( &regs, F0( 0x0C ) );
    if ( CF( regs ) ) {
        return 0;
    }
    sys_screen_size( &rows, &cols );    /* the character height with it */
    return rows == lines;               /* 0: the adapter gave another size */
}

/* ------------------------------------------------------------------ */
/* graphics and the first megabyte                                     */
/* ------------------------------------------------------------------ */

/* VBF_SET: a VGA mode by its BIOS number.  -> 1, with the selector for
   its window on video memory (A0000h, or B8000h for the CGA modes) */
int sys_video_set( uint32_t mode, uint32_t *selector, uint32_t *bytes, uint32_t *pitch )
{
    REGS regs;

    clear( &regs );
    regs.ebx = mode;
    call21( &regs, F1( 0x1F, 2 ) );
    if ( CF( regs ) ) {
        return 0;
    }
    *selector = regs.eax & 0xFFFF;
    *bytes = regs.ecx;
    *pitch = regs.edx;
    return 1;
}

void sys_video_restore( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F1( 0x1F, 3 ) );
}

/* VBF_PALETTE: entries of four bytes, blue, green, red and 0, in the
   DAC's six bits */
int sys_video_palette( int first, int count, const uint8_t *entries )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 0;
    regs.ecx = (uint32_t)count;
    regs.edx = (uint32_t)first;
    regs.edi = (uint32_t)entries;
    call21( &regs, F1( 0x1F, 4 ) );
    return !CF( regs );
}

int sys_video_palette_get( int first, int count, uint8_t *entries )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 1;
    regs.ecx = (uint32_t)count;
    regs.edx = (uint32_t)first;
    regs.edi = (uint32_t)entries;
    call21( &regs, F1( 0x1F, 4 ) );
    return !CF( regs );
}

/* LMF_READ: bytes of the first megabyte.  -> 1 if they were read */
int sys_peek( uint32_t addr, void *buf, unsigned int len )
{
    REGS regs;

    clear( &regs );
    regs.ebx = addr;
    regs.ecx = len;
    regs.edi = (uint32_t)buf;
    call21( &regs, F1( 0x20, 0 ) );
    if ( CF( regs ) ) {
        mem_set( buf, 0, len );
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* the keyboard                                                        */
/* ------------------------------------------------------------------ */

/* DH: bit 0 not to wait, bit 1 for the mouse to end the wait as well
   (AL = 2 then: INT 33h says what happened) */
int sys_key_wait( int shift_seen, int *shift_now, int flags )
{
    REGS regs;

    clear( &regs );
    regs.edx = ((uint32_t)(flags & 3) << 8) | (uint32_t)(shift_seen & 0xFF);
    call21( &regs, F0( 0x08 ) );
    *shift_now = (int)(regs.ebx & 0xFFFF);
    return (int)(regs.eax & 0xFF);
}

int sys_key_read( int *shift_now )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F0( 0x07 ) );
    *shift_now = (int)(regs.ebx & 0xFFFF);
    return (int)(regs.eax & 0xFFFF);
}

int sys_shift( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F0( 0x09 ) );
    return (int)(regs.ebx & 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* the time                                                            */
/* ------------------------------------------------------------------ */

/* 2Ch, as hundredths of a second since midnight */
uint32_t sys_hundredths( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x2C00 );
    return ((((regs.ecx >> 8) & 0xFF) * 60 + (regs.ecx & 0xFF)) * 60
            + ((regs.edx >> 8) & 0xFF)) * 100 + (regs.edx & 0xFF);
}

void sys_date( int *year, int *month, int *day )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x2A00 );
    *year = (int)(regs.ecx & 0xFFFF);
    *month = (int)((regs.edx >> 8) & 0xFF);
    *day = (int)(regs.edx & 0xFF);
}

void sys_time( int *hour, int *minute, int *second, int *hundredth )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x2C00 );
    *hour = (int)((regs.ecx >> 8) & 0xFF);
    *minute = (int)(regs.ecx & 0xFF);
    *second = (int)((regs.edx >> 8) & 0xFF);
    *hundredth = (int)(regs.edx & 0xFF);
}

/* 2Bh and 2Dh: AL = FFh for a date or time DOS will not take.  -> 0 */
int sys_set_date( int year, int month, int day )
{
    REGS regs;

    clear( &regs );
    regs.ecx = (uint32_t)year;
    regs.edx = ((uint32_t)month << 8) | (uint32_t)day;
    call21( &regs, 0x2B00 );
    return (regs.eax & 0xFF) == 0 ? 0 : ERR_FUNC;
}

int sys_set_time( int hour, int minute, int second )
{
    REGS regs;

    clear( &regs );
    regs.ecx = ((uint32_t)hour << 8) | (uint32_t)minute;
    regs.edx = (uint32_t)second << 8;
    call21( &regs, 0x2D00 );
    return (regs.eax & 0xFF) == 0 ? 0 : ERR_FUNC;
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

static void call33( REGS *regs, uint32_t eax )
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

void sys_mouse_state( int *x, int *y, int *buttons )
{
    REGS regs;

    clear( &regs );
    call33( &regs, 0x0003 );
    *buttons = (int)(regs.ebx & 7);
    *x = (int)regs.ecx;
    *y = (int)regs.edx;
}

/* 05h and 06h: the presses (or releases) of a button since last asked,
   and where the last one happened */
int sys_mouse_count( int release, int button, int *x, int *y )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (uint32_t)button;
    call33( &regs, release ? 0x0006 : 0x0005 );
    *x = (int)regs.ecx;
    *y = (int)regs.edx;
    return (int)(regs.ebx & 0xFFFF);
}

/* 26h: the largest position the driver will give on this screen */
void sys_mouse_extent( int *xmax, int *ymax )
{
    REGS regs;

    clear( &regs );
    call33( &regs, 0x0026 );
    *xmax = (int)regs.ecx;
    *ymax = (int)regs.edx;
}

void sys_mouse_move( int x, int y )
{
    REGS regs;

    clear( &regs );
    regs.ecx = (uint32_t)x;
    regs.edx = (uint32_t)y;
    call33( &regs, 0x0004 );
}

/* 21h/F0h AL=0Bh: the buttons as if the mouse had changed them where it
   stands - how the test key files press and let go */
void sys_mouse_inject( int buttons )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (uint32_t)(buttons & 7);
    call21( &regs, F0( 0x0B ) );
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

int sys_lfn( void )
{
    return lfn_ok;
}

static int open_ext( const char *path, uint32_t mode, uint32_t action, uint32_t *handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = mode;
    regs.ecx = 0;
    regs.edx = action;
    regs.esi = (uint32_t)path;
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

static int open_classic( const char *path, uint32_t func, uint32_t *handle )
{
    REGS regs;

    clear( &regs );
    regs.ecx = 0;
    regs.edx = (uint32_t)path;
    call21( &regs, func );
    *handle = AX( regs );
    return result( &regs );
}

/* an existing file: OPEN_READ, OPEN_WRITE or OPEN_RW */
int sys_open( const char *path, int mode, uint32_t *handle )
{
    int rc;

    if ( lfn_ok ) {
        rc = open_ext( path, (uint32_t)mode, 0x0001, handle );
        if ( rc >= 0 ) {
            return rc;
        }
    }
    return open_classic( path, 0x3D00 | (uint32_t)mode, handle );
}

int sys_open_read( const char *path, uint32_t *handle )
{
    return sys_open( path, OPEN_READ, handle );
}

int sys_open_write( const char *path, uint32_t *handle )
{
    return open_classic( path, 0x3D01, handle );    /* PRN, LPT1, COM1... */
}

int sys_create( const char *path, uint32_t *handle )
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

int sys_read( uint32_t handle, void *buf, unsigned int len, unsigned int *done )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = len;
    regs.edx = (uint32_t)buf;
    call21( &regs, 0x3F00 );
    *done = CF( regs ) ? 0 : regs.eax;
    return result( &regs );
}

int sys_write( uint32_t handle, const void *buf, unsigned int len, unsigned int *done )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = len;
    regs.edx = (uint32_t)buf;
    call21( &regs, 0x4000 );
    if ( CF( regs ) ) {
        *done = 0;
        return (int)AX( regs );
    }
    *done = regs.eax;
    return regs.eax < len ? ERR_DISKFULL : 0;
}

int sys_close( uint32_t handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x3E00 );
    return result( &regs );
}

/* 42h, the native form: the offset in ECX and the new position in EAX,
   32 bits each - not DOS's CX:DX and DX:AX */
static int seek( uint32_t handle, uint32_t how, uint32_t offset, uint32_t *pos )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = offset;
    call21( &regs, 0x4200 | how );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    if ( pos != NULL ) {
        *pos = regs.eax;
    }
    return 0;
}

int sys_seek( uint32_t handle, uint32_t pos )
{
    return seek( handle, 0, pos, NULL );
}

int sys_tell( uint32_t handle, uint32_t *pos )
{
    return seek( handle, 1, 0, pos );
}

int sys_file_size( uint32_t handle, uint32_t *size )
{
    int rc = seek( handle, 2, 0, size );

    if ( rc ) {
        return rc;
    }
    return seek( handle, 0, 0, NULL );
}

/* the size, and the position put back where it was */
int sys_size_keep( uint32_t handle, uint32_t *size )
{
    uint32_t at;
    int rc = seek( handle, 1, 0, &at );

    if ( rc == 0 ) {
        rc = seek( handle, 2, 0, size );
    }
    if ( rc == 0 ) {
        rc = seek( handle, 0, at, NULL );
    }
    return rc;
}

/* a write of nothing ends the file where the position is */
int sys_truncate( uint32_t handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = 0;
    regs.edx = (uint32_t)find_rec;
    call21( &regs, 0x4000 );
    return result( &regs );
}

int sys_rewind( uint32_t handle )
{
    return seek( handle, 0, 0, NULL );
}

/* 4400h: bit 7 of DX is a device */
int sys_is_device( uint32_t handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x4400 );
    return !CF( regs ) && (regs.edx & 0x80) != 0;
}

int sys_get_attr( const char *path, uint16_t *attr )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = 0;
        regs.edx = (uint32_t)path;
        call21( &regs, 0x7143 );
        if ( !lfn_missing( &regs ) ) {
            *attr = (uint16_t)regs.ecx;
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (uint32_t)path;
    call21( &regs, 0x4300 );
    *attr = (uint16_t)regs.ecx;
    return result( &regs );
}

/* a call that takes one path in EDX, the long-name one first */
static int path_call( const char *path, uint32_t lfn_func, uint32_t func )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (uint32_t)path;
        regs.esi = 0;
        call21( &regs, lfn_func );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (uint32_t)path;
    call21( &regs, func );
    return result( &regs );
}

int sys_delete( const char *path )
{
    return path_call( path, 0x7141, 0x4100 );
}

int sys_mkdir( const char *path )
{
    return path_call( path, 0x7139, 0x3900 );
}

int sys_rmdir( const char *path )
{
    return path_call( path, 0x713A, 0x3A00 );
}

int sys_rename( const char *from, const char *to )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (uint32_t)from;
        regs.edi = (uint32_t)to;
        call21( &regs, 0x7156 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (uint32_t)from;
    regs.edi = (uint32_t)to;
    call21( &regs, 0x5600 );
    return result( &regs );
}

/* 36h: clusters free x sectors a cluster x bytes a sector, kept below
   4G */
int sys_disk_free( int drv, uint32_t *bytes )
{
    REGS regs;
    uint32_t per_cluster, total;

    clear( &regs );
    regs.edx = (uint32_t)drv + 1;
    call21( &regs, 0x3600 );
    if ( AX( regs ) == 0xFFFF ) {
        *bytes = 0;
        return ERR_BADDRIVE;
    }
    per_cluster = AX( regs ) * (regs.ecx & 0xFFFF);
    total = regs.ebx & 0xFFFF;
    if ( per_cluster != 0 && total > 0xFFFFFFFFUL / per_cluster ) {
        *bytes = 0xFFFFFFFFUL;
    } else {
        *bytes = total * per_cluster;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* searching                                                           */
/* ------------------------------------------------------------------ */

static uint8_t dta_required;

static void rec_from_lfn( FINDREC *rec )
{
    rec->attr = find_rec[0];
    rec->time = *(uint16_t *)(find_rec + 0x14); /* SI = 1: DOS's form */
    rec->date = *(uint16_t *)(find_rec + 0x16);
    rec->size = *(uint32_t *)(find_rec + 0x20);
    rec->name[0] = 0;                           /* a name too long for it: none */
    find_rec[0x130 + 13] = 0;
    str_cpyn( rec->name, (const char *)find_rec + 0x2C, sizeof( rec->name ) );
    str_cpyn( rec->alias, (const char *)find_rec + 0x130, sizeof( rec->alias ) );
    if ( rec->alias[0] == 0 ) {
        str_cpyn( rec->alias, rec->name, sizeof( rec->alias ) );
    }
}

static void rec_from_dta( FINDREC *rec )
{
    rec->attr = find_rec[0x15];
    rec->time = *(uint16_t *)(find_rec + 0x16);
    rec->date = *(uint16_t *)(find_rec + 0x18);
    rec->size = *(uint32_t *)(find_rec + 0x1A);
    find_rec[0x1E + 12] = 0;
    str_cpyn( rec->name, (const char *)find_rec + 0x1E, 13 );
    str_cpyn( rec->alias, rec->name, sizeof( rec->alias ) );
}

static int dta_skip( void )
{
    return (find_rec[0x15] & dta_required) != dta_required;
}

int sys_find_first( const char *pattern, uint16_t attrs, FINDREC *rec, uint32_t *handle )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ecx = attrs;
        regs.edx = (uint32_t)pattern;
        regs.edi = (uint32_t)find_rec;
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
    regs.edx = (uint32_t)find_rec;
    call21( &regs, 0x1A00 );
    dta_required = (uint8_t)(attrs >> 8);
    clear( &regs );
    regs.ecx = attrs & 0xFF;
    regs.edx = (uint32_t)pattern;
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

int sys_find_next( uint32_t handle, FINDREC *rec )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = handle;
        regs.edi = (uint32_t)find_rec;
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

void sys_find_close( uint32_t handle )
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
    regs.edx = (uint32_t)drv;
    call21( &regs, 0x0E00 );
}

int sys_drive_valid( int drv )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (uint32_t)drv + 1;
    call21( &regs, 0x4409 );
    return !(CF( regs ) && AX( regs ) == ERR_BADDRIVE);
}

int sys_get_cwd( int drv, char *buf )
{
    REGS regs;

    buf[0] = 0;
    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (uint32_t)drv + 1;
        regs.esi = (uint32_t)buf;
        call21( &regs, 0x7147 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (uint32_t)drv + 1;
    regs.esi = (uint32_t)buf;
    call21( &regs, 0x4700 );
    return result( &regs );
}

int sys_chdir( const char *path )
{
    return path_call( path, 0x713B, 0x3B00 );
}

int sys_truename( const char *path, char *out, int form )
{
    REGS regs;

    out[0] = 0;
    if ( lfn_ok ) {
        clear( &regs );
        regs.ecx = (uint32_t)form;
        regs.esi = (uint32_t)path;
        regs.edi = (uint32_t)out;
        call21( &regs, 0x7160 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.esi = (uint32_t)path;
    regs.edi = (uint32_t)out;
    call21( &regs, 0x6000 );
    return result( &regs );
}
