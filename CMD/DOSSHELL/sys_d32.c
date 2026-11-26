/*
 * SYS_D32.C - DOSSHELL's system calls, PM-DOS native.
 *
 * DOSSHELL.EXE is a D32 image run at ring 3, as EDIT.EXE is, and the
 * first two thirds of this file are EDIT's: INT 21h is the real-mode
 * interface widened to 32 bits, the screen and the keyboard are
 * 21h/F0h, the mouse is INT 33h, and files go by their long names
 * (71xxh) until the kernel answers 7100h.
 *
 * WHAT A SHELL ADDS:
 *
 *   4B00h         another program, in a slot of its own; this one waits
 *   F1h/03h       where the command interpreter is, to hand a line to
 *   F1h/06h       the volume table: which letters there are, and what
 *                 kind of drive each one is
 *   7303h, 36h    how full a disk is
 *
 * AND A GRAPHICS SCREEN.  A native program cannot set a video mode or
 * reach video memory.  21h/F1h AL=1Fh does the first (VBF_SET: a BIOS
 * mode, or a VBE one with its linear frame buffer) and answers with a
 * selector for the second, which VID_PUT in DOSINT.ASM writes through.
 * The planar modes also want the sequencer's map mask, and the ports
 * are reached with plain OUT instructions: they fault at ring 3 and the
 * kernel carries them out.  The ROM's fonts are read out of the first
 * megabyte with 21h/F1h AL=20h - see GFX.C for where they are found.
 */
#include "rt.h"
#include "sys.h"
#include "shell.h"

typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi;
    uint32_t flags;             /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void __cdecl int33( REGS *regs );
void __cdecl break_handler( void );
void __cdecl crit_handler( void );
int  __cdecl crit_take( void );

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)
#define F1( al, fn )    (0xF100UL | (al) | ((uint32_t)(fn) << 16))

COUNTRY country = { 0, '-', ':', 0 };

static uint8_t *psp;
static char     tail[300];
static int      lfn_ok = 1;
static uint8_t  find_rec[320];

void __cdecl dos_entry( void )
{
    shell_main();
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
        str_fit( tail, full, sizeof( tail ) );
    }
    sys_get_cwd( sys_get_drive(), probe );     /* are there long names? */

    /* ^C is a key, and a drive that will not answer is failed and the
       shell says so itself */
    clear( &regs );
    regs.edx = (uint32_t)break_handler;
    call21( &regs, 0x2523 );
    clear( &regs );
    regs.edx = (uint32_t)crit_handler;
    call21( &regs, 0x2524 );

    /* 38h: how this country writes a number, a date and a time */
    mem_set( probe, 0, 40 );
    clear( &regs );
    regs.edx = (uint32_t)probe;
    call21( &regs, 0x3800 );
    if ( !CF( regs ) ) {
        country.date_order = probe[0] <= 2 ? probe[0] : 0;
        if ( probe[7] ) {
            fmt_group_set( probe[7] );
        }
        if ( probe[0x0B] ) {
            country.date_sep = probe[0x0B];
        }
        if ( probe[0x0D] ) {
            country.time_sep = probe[0x0D];
        }
        country.clock24 = probe[0x11] & 1;
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
    unsigned int len = str_len( name );

    env = (const char *)*(uint32_t *)(psp + 0x2C);
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

/* A native program is not told where it was loaded from, so the
   program is looked for the way the shell found it - here, then along
   PATH - and DOSSHELL.INI is kept beside it. */
const char *sys_program_dir( void )
{
    static char found[PATH_MAX];
    static const char exe[] = "DOSSHELL.EXE";
    char trial[PATH_MAX];
    const char *path = sys_getenv( "PATH" );
    uint16_t attr;
    unsigned int len;

    if ( found[0] ) {
        return found;
    }
    if ( sys_get_attr( exe, &attr ) == 0 ) {
        sys_truename( exe, found, 0 );
    }
    while ( found[0] == 0 && path && *path ) {
        for ( len = 0; path[len] && path[len] != ';'; len++ ) {
        }
        if ( len && len < PATH_MAX - 16 ) {
            mem_cpy( trial, path, len );
            trial[len] = 0;
            path_join( trial, trial, exe, sizeof( trial ) );
            if ( sys_get_attr( trial, &attr ) == 0 ) {
                sys_truename( trial, found, 0 );
            }
        }
        path += len;
        if ( *path == ';' ) {
            path++;
        }
    }
    if ( found[0] == 0 ) {                      /* nowhere: beside the work, then */
        sys_truename( exe, found, 0 );
    }
    *(char *)path_name( found ) = 0;
    return found;
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

void sys_put_cells( unsigned int first, const uint16_t *cells, unsigned int count )
{
    REGS regs;

    clear( &regs );
    regs.ebx = first;
    regs.ecx = count;
    regs.esi = (uint32_t)cells;
    call21( &regs, 0xF004 );
}

void sys_cursor( int row, int col )
{
    REGS regs;

    clear( &regs );
    regs.edx = ((uint32_t)row << 8) | (uint32_t)col;
    call21( &regs, 0xF002 );
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
        regs.ecx = ((uint32_t)first << 8) | (uint32_t)last;
    }
    call21( &regs, 0xF006 );
}

int sys_set_lines( int lines )
{
    REGS regs;
    int rows, cols;

    clear( &regs );
    regs.ebx = (uint32_t)lines;
    call21( &regs, 0xF00A );
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
    regs.edx = ((uint32_t)(flags & 3) << 8) | (uint32_t)(shift_seen & 0xFF);
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
uint32_t sys_hundredths( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x2C00 );
    return ((((regs.ecx >> 8) & 0xFF) * 60 + (regs.ecx & 0xFF)) * 60
            + ((regs.edx >> 8) & 0xFF)) * 100 + (regs.edx & 0xFF);
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
    *x = (int)(regs.ecx & 0xFFFF);
    *y = (int)(regs.edx & 0xFFFF);
}

/* 05h and 06h: the presses (or releases) of a button since last asked,
   and where the last one happened */
int sys_mouse_count( int release, int button, int *x, int *y )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (uint32_t)button;
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
    regs.edx = (uint32_t)xmax;
    call33( &regs, 0x0007 );
    clear( &regs );
    regs.edx = (uint32_t)ymax;
    call33( &regs, 0x0008 );
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
    call21( &regs, 0xF00B );
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

int sys_open_read( const char *path, uint32_t *handle )
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

int sys_open_write( const char *path, uint32_t *handle )
{
    return open_classic( path, 0x3D01, handle );    /* PRN, LPT1, COM1... */
}

int sys_create( const char *path, uint32_t *handle )
{
    int rc;

    if ( lfn_ok ) {
        rc = open_ext( path, 0x0001, 0x0012, handle );
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
   32 bits each - NOT DOS's CX:DX and DX:AX. */
int sys_file_size( uint32_t handle, uint32_t *size )
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

int sys_seek( uint32_t handle, uint32_t pos )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = pos;
    call21( &regs, 0x4200 );
    return result( &regs );
}

int sys_get_ftime( uint32_t handle, uint16_t *time, uint16_t *date )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x5700 );
    *time = (uint16_t)regs.ecx;
    *date = (uint16_t)regs.edx;
    return result( &regs );
}

int sys_set_ftime( uint32_t handle, uint16_t time, uint16_t date )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = time;
    regs.edx = date;
    call21( &regs, 0x5701 );
    return result( &regs );
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

int sys_set_attr( const char *path, uint16_t attr )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = 1;
        regs.ecx = attr;
        regs.edx = (uint32_t)path;
        call21( &regs, 0x7143 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.ecx = attr;
    regs.edx = (uint32_t)path;
    call21( &regs, 0x4301 );
    return result( &regs );
}

/* one path in EDX, by its long name and then by the classic call */
static int path_call( const char *path, uint32_t lfn_func, uint32_t func )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (uint32_t)path;
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

/* ------------------------------------------------------------------ */
/* searching                                                           */
/* ------------------------------------------------------------------ */

static uint8_t dta_required;

static void rec_from_lfn( FINDREC *rec )
{
    rec->attr = find_rec[0];
    rec->time = *(uint16_t *)(find_rec + 0x14); /* last written, DOS's form */
    rec->date = *(uint16_t *)(find_rec + 0x16);
    rec->size = *(uint32_t *)(find_rec + 0x20);
    str_fit( rec->name, (const char *)find_rec + 0x2C, sizeof( rec->name ) );
    str_fit( rec->alias, (const char *)find_rec + 0x130, sizeof( rec->alias ) );
    if ( rec->alias[0] == 0 ) {
        str_fit( rec->alias, rec->name, sizeof( rec->alias ) );
    }
}

static void rec_from_dta( FINDREC *rec )
{
    rec->attr = find_rec[0x15];
    rec->time = *(uint16_t *)(find_rec + 0x16);
    rec->date = *(uint16_t *)(find_rec + 0x18);
    rec->size = *(uint32_t *)(find_rec + 0x1A);
    str_fit( rec->name, (const char *)find_rec + 0x1E, 13 );
    str_fit( rec->alias, rec->name, sizeof( rec->alias ) );
}

static int dta_skip( void )
{
    return (find_rec[0x15] & dta_required) != dta_required;
}

/* "attrs": the attributes a name may have in the low byte, and the
   ones it must have in the high byte.  SI = 1 asks for DOS's dates. */
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

/* F1h/06h: the volume table's entry for a letter - the BIOS drive, the
   partition type, DRVF_* flags and where the volume is, twelve bytes.
   Nothing is mounted to answer, so a diskette drive with no diskette
   in the drive is still a diskette drive. */
int sys_drive_kind( int drv )
{
    REGS regs;
    uint8_t rec[12];

    mem_set( rec, 0, sizeof( rec ) );
    clear( &regs );
    regs.edx = (uint32_t)drv;
    regs.edi = (uint32_t)rec;
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

static void to_kb( uint32_t cluster_bytes, uint32_t free_clusters, uint32_t total_clusters,
                   uint32_t *free_kb, uint32_t *total_kb )
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
int sys_disk_space( int drv, uint32_t *free_kb, uint32_t *total_kb )
{
    static uint8_t ext[48];
    static char root[4] = "A:\\";
    REGS regs;
    uint32_t spc, bps, cluster;
    const uint8_t *at;

    *free_kb = *total_kb = 0;
    root[0] = (char)('A' + drv);
    mem_set( ext, 0, sizeof( ext ) );
    clear( &regs );
    regs.edx = (uint32_t)root;
    regs.edi = (uint32_t)ext;
    regs.ecx = sizeof( ext );
    call21( &regs, 0x7303 );
    at = *(uint16_t *)ext == 0 ? ext + 2 : ext + 4;
    spc = *(uint32_t *)at;
    bps = *(uint32_t *)(at + 4);
    if ( !CF( regs ) && spc && spc <= 0x100 && bps >= 128 && bps <= 0x1000 ) {
        to_kb( spc * bps, *(uint32_t *)(at + 8), *(uint32_t *)(at + 12), free_kb, total_kb );
        return 1;
    }
    clear( &regs );
    regs.edx = (uint32_t)drv + 1;
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

/* the volume's label: the one root entry with the label attribute,
   which the classic search hands over when asked for that alone */
int sys_volume_label( int drv, char *label )
{
    static uint8_t dta[64];
    static char pattern[8] = "A:\\*.*";
    REGS regs;
    const char *name;
    int pos = 0;

    label[0] = 0;
    pattern[0] = (char)('A' + drv);
    clear( &regs );
    regs.edx = (uint32_t)dta;
    call21( &regs, 0x1A00 );
    clear( &regs );
    regs.ecx = ATTR_VOLUME;
    regs.edx = (uint32_t)pattern;
    call21( &regs, 0x4E00 );
    if ( CF( regs ) || !(dta[0x15] & ATTR_VOLUME) ) {
        return 0;
    }
    for ( name = (const char *)dta + 0x1E; *name && pos < 11; name++ ) {
        if ( *name != '.' ) {                   /* eleven characters, kept as 8.3 */
            label[pos++] = *name;
        }
    }
    label[pos] = 0;
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
    parm[0] = *(uint32_t *)(psp + 0x2C);
    parm[1] = (uint32_t)tail_buf;
    clear( &regs );
    regs.edx = (uint32_t)prog;
    regs.ebx = (uint32_t)parm;
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
void sys_shell_path( char *buf, unsigned int max )
{
    static char name[96];
    REGS regs;

    name[0] = 0;
    clear( &regs );
    regs.edx = (uint32_t)name;
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

int sys_bios( int vector, BREGS *bregs )
{
    REGS regs;

    bregs->es = bregs->ds = 0;
    bregs->flags = 0;
    clear( &regs );
    regs.edx = (uint32_t)vector;
    regs.esi = (uint32_t)bregs;
    call21( &regs, 0xF121 );
    return !CF( regs );
}

/* VBF_INFO: 4F00h's block, its mode list copied into it and its
   pointer made ours */
int sys_vbe_version( uint16_t *modes, int max, int *count )
{
    static uint8_t block[512];
    REGS regs;
    const uint16_t *list;
    int found = 0;

    *count = 0;
    mem_set( block, 0, sizeof( block ) );
    clear( &regs );
    regs.edi = (uint32_t)block;
    call21( &regs, F1( 0x1F, 0 ) );
    if ( CF( regs ) ) {
        return 0;
    }
    list = (const uint16_t *)*(uint32_t *)(block + 0x0E);
    if ( list ) {
        while ( *list != 0xFFFF && found < max ) {
            modes[found++] = *list++;
        }
    }
    *count = found;
    return (int)(regs.eax & 0xFFFF);
}

int sys_vbe_mode( int mode, uint8_t *info )
{
    REGS regs;

    mem_set( info, 0, 256 );
    clear( &regs );
    regs.ecx = (uint32_t)mode;
    regs.edi = (uint32_t)info;
    call21( &regs, F1( 0x1F, 1 ) );
    return !CF( regs );
}

int sys_video_set( uint32_t mode, uint32_t *selector, uint32_t *bytes, uint32_t *pitch )
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
