/*
 * SYS_D32.C - PMD's system calls, PM-DOS native.
 *
 * PMD.EXE is a D32 image loaded at offset 2000h of a program slot and
 * run at ring 3; INT 21h is the real-mode interface widened to 32 bits
 * (see XCOPY's SYS_D32.C for the details the C programs share).
 *
 * THE SCREEN, THE KEYBOARD AND THE MOUSE are EDIT's: 21h/F0h for cells,
 * the cursor and keys, INT 33h for the mouse.  Files go by their long
 * names (71xxh) and fall back to the classic calls the first time the
 * kernel answers 7100h.
 *
 * THE MACHINE is the second half of this file.  A program at ring 3
 * can see none of it for itself - its segment is its slot - so every
 * fact on every page is one of four things:
 *
 *   21h/F1h      the kernel's own account: memory, drives, disks, the
 *                interrupt lines, the PCI bus, the coprocessor
 *   F1h AL=20h   bytes of the first megabyte copied into a buffer: the
 *                BIOS data area, the vectors, the ROMs, and the memory
 *                blocks and device headers a DOS box's programs live
 *                among
 *   F1h AL=21h   a BIOS call out of the kernel's list of the ones that
 *                only ask
 *   IN and OUT   which fault at ring 3 and are carried out by the
 *                kernel, as they are for a DOS box (DOSINT.ASM)
 */
#include "pmd.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void __cdecl int33( REGS *regs );
void __cdecl break_handler( void );
void __cdecl crit_handler( void );

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)

static u8   *psp;
static char  tail[300];
static int   lfn_ok = 1;
static u8    find_rec[320];

void __cdecl dos_entry( void )
{
    pmd_main();
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

/* ------------------------------------------------------------------ */
/* the program                                                         */
/* ------------------------------------------------------------------ */

int sys_init( void )
{
    REGS regs;
    const char *full;
    u32 len;
    int quoted;
    char probe[PATH_MAX];

    clear( &regs );
    call21( &regs, 0x6200 );
    psp = (u8 *)regs.ebx;
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

    /* a drive that will not answer is failed, not asked about */
    clear( &regs );
    regs.edx = (u32)crit_handler;
    call21( &regs, 0x2524 );

    /* the thousands separator: 38h's block, byte 7 */
    mem_set( probe, 0, 40 );
    clear( &regs );
    regs.edx = (u32)probe;
    call21( &regs, 0x3800 );
    if ( !CF( regs ) && probe[7] ) {
        fmt_group_set( probe[7] );
    }
    return 0;
}

const char *sys_env_block( void )
{
    return (const char *)*(u32 *)(psp + 0x2C);
}

/* A native program is not told where it was loaded from: the
   environment EXEC copies in ends at its last string.  So it is looked
   for the way the shell found it - here, then along PATH. */
const char *sys_program_path( void )
{
    static char found[PATH_MAX];
    char trial[PATH_MAX];
    const char *path = sys_getenv( "PATH" );
    u16 attr;
    u32 len, end;

    if ( found[0] ) {
        return found;
    }
    if ( sys_get_attr( "PMD.EXE", &attr ) == 0 ) {
        sys_truename( "PMD.EXE", found, 0 );
    }
    while ( found[0] == 0 && path && *path ) {
        for ( len = 0; path[len] && path[len] != ';'; len++ ) {
        }
        if ( len && len < PATH_MAX - 10 ) {
            mem_cpy( trial, path, len );
            end = len;
            if ( trial[end - 1] != '\\' && trial[end - 1] != ':' ) {
                trial[end++] = '\\';
            }
            str_cpy( trial + end, "PMD.EXE" );
            if ( sys_get_attr( trial, &attr ) == 0 ) {
                sys_truename( trial, found, 0 );
            }
        }
        path += len;
        if ( *path == ';' ) {
            path++;
        }
    }
    if ( found[0] == 0 ) {
        str_cpy( found, "PMD.EXE" );
    }
    return found;
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
   says in EBX what would have fitted, as DOS does.  For the test log. */
u32 sys_mem_largest( void )
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
    regs.edx = (u32)break_handler;
    call21( &regs, 0x2523 );
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
/* the screen                                                          */
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

/* The shapes in the controller's scan lines for the font in use, as a
   VGA BIOS would set them: an underline two lines up from the bottom of
   a 14- or 16-line cell (0Dh-0Eh on the usual one) and on the last two
   of an 8-line one; a block the whole height. */
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

/* ------------------------------------------------------------------ */
/* the keyboard                                                        */
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

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

static void call33( REGS *regs, u32 eax )
{
    regs->eax = eax;
    int33( regs );
}

static int mouse_asked, mouse_buttons;

int sys_mouse_reset( void )
{
    REGS regs;

    clear( &regs );
    call33( &regs, 0x0000 );
    mouse_asked = 1;
    mouse_buttons = (regs.eax & 0xFFFF) == 0xFFFF ? (int)(regs.ebx & 0xFFFF) : 0;
    if ( mouse_buttons == 0xFFFF ) {
        mouse_buttons = 2;
    }
    return mouse_buttons;
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
    regs.ebx = (u32)button;
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
        rc = open_ext( path, 0x0001, 0x0012, handle );
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
   32 bits each - NOT DOS's CX:DX and DX:AX.  Read as DOS's, every file
   past 64K came back its size modulo 64K, and the loader sized its
   pools by that and made thousands of tiny ones. */
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
    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x4200 );                /* and back to the start */
    return result( &regs );
}

int sys_seek_end( u32 handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = 0;
    call21( &regs, 0x4202 );
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

/* ------------------------------------------------------------------ */
/* searching                                                           */
/* ------------------------------------------------------------------ */

static u8 dta_required;

static void rec_from_lfn( FINDREC *rec )
{
    rec->attr = find_rec[0];
    rec->size = *(u32 *)(find_rec + 0x20);
    rec->time = *(u16 *)(find_rec + 0x14);      /* last written, DOS's forms */
    rec->date = *(u16 *)(find_rec + 0x16);
    str_cpyn( rec->name, (const char *)find_rec + 0x2C, sizeof( rec->name ) );
    str_cpyn( rec->alias, (const char *)find_rec + 0x130, sizeof( rec->alias ) );
    if ( rec->alias[0] == 0 ) {
        str_cpyn( rec->alias, rec->name, sizeof( rec->alias ) );
    }
}

static void rec_from_dta( FINDREC *rec )
{
    rec->attr = find_rec[0x15];
    rec->size = *(u32 *)(find_rec + 0x1A);
    rec->time = *(u16 *)(find_rec + 0x16);
    rec->date = *(u16 *)(find_rec + 0x18);
    str_cpyn( rec->name, (const char *)find_rec + 0x1E, 13 );
    str_cpyn( rec->alias, rec->name, sizeof( rec->alias ) );
}

static int dta_skip( void )
{
    return (find_rec[0x15] & dta_required) != dta_required;
}

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

int sys_is_device( u32 handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x4400 );
    return !CF( regs ) && (regs.edx & 0x80) != 0;
}

/* 0Ah: a line as the user types it, with DOS's editing.  For the
   customer's details /F asks for before the screen is taken over. */
int sys_stdin_line( char *buf, int max )
{
    static u8 line[130];
    REGS regs;
    int len;

    line[0] = (u8)(max > 126 ? 126 : max);
    line[1] = 0;
    clear( &regs );
    regs.edx = (u32)line;
    call21( &regs, 0x0A00 );
    len = line[1];
    if ( len >= max ) {
        len = max - 1;
    }
    mem_cpy( buf, line + 2, (u32)len );
    buf[len] = 0;
    sys_stdout( "\r\n", 2 );
    return len;
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

/* What the reset at the start said - asking again would hide the
   pointer the screen is using - then 24h: the version, the kind of
   mouse and its line; 1Bh: how far the pointer goes for a movement */
void sys_mouse_info( MOUSEINFO *info )
{
    REGS regs;

    mem_set( info, 0, sizeof( *info ) );
    if ( mouse_asked == 0 ) {
        sys_mouse_reset();
    }
    if ( mouse_buttons == 0 ) {
        return;
    }
    info->present = 1;
    info->buttons = mouse_buttons;
    clear( &regs );
    call33( &regs, 0x0024 );
    info->version = (int)(regs.ebx & 0xFFFF);
    info->type = (int)((regs.ecx >> 8) & 0xFF);
    info->irq = (int)(regs.ecx & 0xFF);
    clear( &regs );
    call33( &regs, 0x001B );
    info->horiz = (int)(regs.ebx & 0xFFFF);
    info->vert = (int)(regs.ecx & 0xFFFF);
    info->threshold = (int)(regs.edx & 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* the first megabyte, and the BIOS                                    */
/* ------------------------------------------------------------------ */

#define F1( al, fn )    (0xF100UL | (al) | ((u32)(fn) << 16))

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

u8 peek_b( u32 addr )
{
    u8 val;

    sys_peek( addr, &val, 1 );
    return val;
}

u16 peek_w( u32 addr )
{
    u16 val;

    sys_peek( addr, &val, 2 );
    return val;
}

u32 peek_d( u32 addr )
{
    u32 val;

    sys_peek( addr, &val, 4 );
    return val;
}

int sys_ram_test( u32 addr )
{
    REGS regs;

    clear( &regs );
    regs.ebx = addr;
    call21( &regs, F1( 0x20, 1 ) );
    return !CF( regs ) && regs.eax == 1;
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

/* ------------------------------------------------------------------ */
/* the kernel's accounts                                               */
/* ------------------------------------------------------------------ */

void sys_version( int *major, int *minor )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF100 );
    *major = (int)((regs.ebx >> 8) & 0xFF);
    *minor = (int)(regs.ebx & 0xFF);
}

void sys_dos_version( int *major, int *minor, int *oem, u32 *serial )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x3000 );
    *major = (int)(regs.eax & 0xFF);
    *minor = (int)((regs.eax >> 8) & 0xFF);
    *oem = (int)((regs.ebx >> 8) & 0xFF);
    *serial = ((regs.ebx & 0xFF) << 16) | (regs.ecx & 0xFFFF);
}

void sys_true_version( int *major, int *minor, int *revision, int *flags )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x3306 );
    *major = (int)(regs.ebx & 0xFF);
    *minor = (int)((regs.ebx >> 8) & 0xFF);
    *revision = (int)(regs.edx & 0x07);
    *flags = (int)((regs.edx >> 8) & 0xFF);
}

int sys_boot_drive( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x3305 );
    return (int)(regs.edx & 0xFF) - 1;
}

void sys_ext_memory( u32 *bios_kb, u32 *top )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF104 );
    *bios_kb = regs.eax;
    *top = regs.ebx;
}

int sys_mem_block( int arena, u32 index, MEMBLK *blk )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)arena;
    regs.ebx = index;
    regs.edi = (u32)blk;
    call21( &regs, 0xF108 );
    return !CF( regs );
}

int sys_ems( u32 *frame_seg, u32 *total_pages, u32 *free_pages )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF109 );
    *frame_seg = regs.ebx & 0xFFFF;
    *total_pages = regs.ecx;
    *free_pages = regs.edx;
    return !CF( regs ) && regs.eax == 1;
}

u32 sys_xms_largest( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F1( 0x1A, 4 ) );
    return CF( regs ) ? 0 : regs.ecx;
}

u32 sys_fpu_flags( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF113 );
    return CF( regs ) ? 0 : regs.eax;
}

int sys_drive_rec( int drv, DRVREC *rec, int *letters )
{
    REGS regs;

    mem_set( rec, 0, sizeof( *rec ) );
    clear( &regs );
    regs.edx = (u32)drv;
    regs.edi = (u32)rec;
    call21( &regs, 0xF106 );
    if ( letters ) {
        *letters = (int)regs.ecx;
    }
    return !CF( regs );
}

/* this MOUNTS the volume, so it is not asked of a drive whose medium
   may not be there */
int sys_fat_type( int drv )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)drv + 1;
    call21( &regs, 0xF101 );
    return CF( regs ) ? 0 : (int)(regs.eax & 0xFF);
}

static void to_kb( u32 cluster_bytes, u32 free_clusters, u32 total_clusters,
                   u32 *free_kb, u32 *total_kb )
{
    if ( cluster_bytes == 0 ) {
        *free_kb = *total_kb = 0;
        return;
    }
    if ( cluster_bytes >= 1024 ) {
        *free_kb = free_clusters * (cluster_bytes / 1024);
        *total_kb = total_clusters * (cluster_bytes / 1024);
    } else {
        *free_kb = free_clusters / (1024 / cluster_bytes);
        *total_kb = total_clusters / (1024 / cluster_bytes);
    }
}

/* 7303h, which counts in 32 bits, and 36h where the kernel has not got
   it - whose 16 bits are full at 65,535 clusters.  The record is
   PM-DOS's (F73_* in PMDOS.INC): a word, then sectors a cluster, bytes
   a sector, clusters free and clusters in all, a dword each. */
int sys_disk_space( int drv, u32 *free_kb, u32 *total_kb )
{
    static u8 ext[48];
    u32 spc, bps;
    static char root[4];
    REGS regs;
    u32 cluster;

    root[0] = (char)('A' + drv);
    root[1] = ':';
    root[2] = 0x5C;
    root[3] = 0;
    mem_set( ext, 0, sizeof( ext ) );
    clear( &regs );
    regs.edx = (u32)root;
    regs.edi = (u32)ext;
    regs.ecx = sizeof( ext );
    call21( &regs, 0x7303 );
    spc = *(u32 *)(ext + 0x02);
    bps = *(u32 *)(ext + 0x06);
    if ( !CF( regs ) && spc && spc <= 0x100 && bps >= 128 && bps <= 0x1000 ) {
        to_kb( spc * bps, *(u32 *)(ext + 0x0A), *(u32 *)(ext + 0x0E), free_kb, total_kb );
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

int sys_disk_rec( int bios, DSKREC *rec )
{
    REGS regs;

    mem_set( rec, 0, sizeof( *rec ) );
    clear( &regs );
    regs.edx = (u32)bios;
    regs.edi = (u32)rec;
    call21( &regs, 0xF10C );
    return !CF( regs ) && (rec->flags & 1);
}

/* FDF_TYPE: what CMOS says the drive is - 1 360K, 2 1.2M, 3 720K,
   4 1.44M, 5 2.88M - and its largest format */
int sys_floppy_type( int drive, int *spt, int *cyls, int *heads )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)drive;
    call21( &regs, F1( 0x14, 0 ) );
    if ( CF( regs ) ) {
        return 0;
    }
    *spt = (int)(regs.ecx & 0xFF);
    *cyls = (int)((regs.ecx >> 8) & 0xFF);
    *heads = (int)((regs.edx >> 8) & 0xFF);
    return (int)(regs.ebx & 0xFF);
}

int sys_cdrom_units( void )
{
    static u8 unit[64];
    REGS regs;

    clear( &regs );
    regs.edx = 0;
    regs.edi = (u32)unit;
    call21( &regs, 0xF117 );
    return regs.ecx > 8 ? 0 : (int)regs.ecx;
}

int sys_irq_stat( int irq, u32 *flags, u32 *taken )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)irq;
    call21( &regs, F1( 0x12, 0x20 ) );
    *flags = regs.eax;
    *taken = regs.ebx;
    return !CF( regs );
}

int sys_pci_here( int *last_bus, int *functions )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F1( 0x12, 0x00 ) );
    *last_bus = (int)regs.ebx;
    *functions = (int)regs.ecx;
    return !CF( regs ) && regs.eax == 1;
}

int sys_pci_enum( int index, u32 *addr, u32 *ident, u32 *class_rev )
{
    REGS regs;

    clear( &regs );
    regs.esi = (u32)index;
    call21( &regs, F1( 0x12, 0x01 ) );
    *addr = regs.ebx;
    *ident = regs.eax;
    *class_rev = regs.ecx;
    return !CF( regs );
}

int sys_pci_irq( u32 addr, int *line, int *pin )
{
    REGS regs;

    clear( &regs );
    regs.ebx = addr;
    call21( &regs, F1( 0x12, 0x07 ) );
    *line = (int)(regs.eax & 0xFF);
    *pin = (int)((regs.eax >> 8) & 0xFF);
    return !CF( regs );
}

int sys_pnp_cards( int *read_port )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F1( 0x12, 0x30 ) );
    *read_port = (int)regs.ebx;
    return CF( regs ) ? 0 : (int)regs.eax;
}

/* VBF_INFO: 4F00h's block, with its strings copied into it and its
   pointers made ours */
int sys_vbe_info( int *version, char *oem, u32 oem_max, int *memory_64k, int *modes )
{
    static u8 block[512];
    REGS regs;
    const char *name;
    u32 len;

    mem_set( block, 0, sizeof( block ) );
    clear( &regs );
    regs.edi = (u32)block;
    call21( &regs, F1( 0x1F, 0 ) );
    if ( CF( regs ) ) {
        return 0;
    }
    *version = (int)(regs.eax & 0xFFFF);
    *modes = (int)regs.ecx;
    *memory_64k = *(u16 *)(block + 0x12);
    name = (const char *)*(u32 *)(block + 0x06);
    oem[0] = 0;
    if ( name && oem_max ) {
        len = str_len( name );
        if ( len > oem_max - 1 ) {
            len = oem_max - 1;
        }
        mem_cpy( oem, name, len );
        oem[len] = 0;
    }
    return 1;
}

/* SHARE is part of the kernel and CONFIG.SYS can switch it off; no
   call says which.  So it is tried: a file opened to the exclusion of
   everybody cannot be opened again where sharing is enforced, and can
   where it is not.  The file is this program's own. */
int sys_share_active( void )
{
    const char *path = sys_program_path();
    u32 first, second;
    int rc;

    if ( open_classic( path, 0x3D10, &first ) ) {       /* read, deny all */
        return 0;
    }
    rc = open_classic( path, 0x3D40, &second );         /* read, deny none */
    if ( rc == 0 ) {
        sys_close( second );
    }
    sys_close( first );
    crit_take();                        /* a refusal may arrive as an INT 24h too */
    return rc != 0;
}
