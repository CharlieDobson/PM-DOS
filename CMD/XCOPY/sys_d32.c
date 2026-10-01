/*
 * SYS_D32.C - XCOPY's system calls, PM-DOS native.
 *
 * XCOPY.EXE is a D32 image (DOSINT.ASM with -dD32), loaded at offset
 * 2000h of a 512K program slot and run at ring 3 with every pointer a
 * 32-bit offset within the slot.  INT 21h is the real-mode interface
 * widened to 32 bits: pointers in EDX, ESI, EDI and EBX, counts in ECX,
 * CF and AX for errors.  Where PM-DOS differs from DOS:
 *
 *   - 48h takes BYTES in EBX and returns the block's offset in EAX.
 *   - PSP+2Ch holds the environment as a 32-bit offset in the slot.
 *   - 714Eh always fills the 8.3 name at +130h, where Windows leaves it
 *     empty when the long name is a valid 8.3 one; both are handled.
 *
 * THE LONG-NAME CALLS FIRST.  Every call that takes a name tries its
 * 71xxh form, and the first "7100h, not supported" answer - which is
 * what the kernel says with LFN=OFF in CONFIG.SYS - switches the whole
 * program to the classic calls, which on PM-DOS accept long names
 * anyway (they are LFN-transparent) and on any other DOS do not.
 */
#include "rt.h"
#include "sys.h"
#include "xcopy.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void __cdecl break_handler( void );
extern u8 break_hit;

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)

static u8   *psp;
static char  tail[300];
static int   lfn_ok = 1;
static u8    find_rec[320];     /* 714Eh's record, or the DTA of 4Eh */

void __cdecl dos_entry( void )
{
    xcopy_main();
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

/* CF with AX = 7100h: the long-name call is not there */
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

int sys_lfn( void )
{
    return lfn_ok;
}

/* ------------------------------------------------------------------ */
/* the program, its command line and its environment                   */
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

    /* A command line longer than the PSP holds: Windows 95's convention
       is a length of 7Fh and the whole line in CMDLINE, program name
       first.  Skip the name and take the rest. */
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

    /* does this DOS have the long-name calls?  Ask for the current
       directory the long way and see. */
    {
        char probe[PATH_MAX];
        sys_get_cwd( sys_get_drive(), probe );
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

void sys_write( int handle, const char *buf, u32 len )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    call21( &regs, 0x4000 );
}

int sys_stdin_is_console( void )
{
    REGS regs;

    clear( &regs );
    regs.ebx = H_IN;
    call21( &regs, 0x4400 );
    return !CF( regs ) && (regs.edx & 0x81) == 0x81;
}

/* one byte of redirected input; -1 at the end */
static int read_byte( void )
{
    REGS regs;
    u8 ch;

    clear( &regs );
    regs.ebx = H_IN;
    regs.ecx = 1;
    regs.edx = (u32)&ch;
    call21( &regs, 0x3F00 );
    if ( CF( regs ) || regs.eax == 0 ) {
        return -1;
    }
    return ch;
}

/* A keystroke for a question.  From the keyboard it is 08h - no echo,
   and ^C is noticed - with an extended key read whole and answered as
   0.  From a file (a batch file's "< ANSWERS") line ends are skipped, so
   a file of one answer to a line works. */
int sys_read_key( void )
{
    REGS regs;
    int key;

    if ( sys_stdin_is_console() ) {
        clear( &regs );
        call21( &regs, 0x0800 );
        key = (int)(regs.eax & 0xFF);
        if ( key == 0 ) {
            clear( &regs );
            call21( &regs, 0x0800 );
        }
        return key;
    }
    do {
        key = read_byte();
    } while ( key == '\r' || key == '\n' );
    return key;
}

void sys_exit( int code )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x4C00 | ((u32)code & 0xFF) );
}

void sys_break_install( void )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)break_handler;
    call21( &regs, 0x2523 );
}

int sys_break_hit( void )
{
    return break_hit != 0;
}

/* The ^C handler is not going back to the DOS call it interrupted.
   21h/F105h tells the kernel so - it has that call and the handler
   both marked as still running, and until they are let go the next
   DOS call (the 4Ch included) waits for them for ever.  CMD.EXE says
   the same thing on its way back to the prompt. */
void sys_break_abandon( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF105 );
}

/* ------------------------------------------------------------------ */
/* drives, directories and names                                       */
/* ------------------------------------------------------------------ */

int sys_get_drive( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x1900 );
    return (int)(regs.eax & 0xFF);
}

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

int sys_mkdir( const char *path )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (u32)path;
        call21( &regs, 0x7139 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, 0x3900 );
    return result( &regs );
}

int sys_delete( const char *path )
{
    REGS regs;

    /* The only thing XCOPY deletes is a copy it could not finish, and
       PM-DOS would keep that (UNDELETE=ON).  F1h AL=19h deletes without
       keeping; a kernel that has no such call says "invalid function"
       and the ordinary calls below are made instead. */
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, 0xF119 );
    if ( !CF( regs ) || AX( regs ) != 0x0001 ) {
        return result( &regs );
    }

    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (u32)path;
        regs.esi = 0;                       /* the name, not a pattern */
        call21( &regs, 0x7141 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, 0x4100 );
    return result( &regs );
}

/* CON, NUL, PRN...: a name that opens as a character device */
int sys_is_device( const char *path )
{
    REGS regs;
    u32 handle;
    int device;

    if ( sys_open_read( path, &handle ) != 0 ) {
        return 0;
    }
    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x4400 );
    device = !CF( regs ) && (regs.edx & 0x80);
    sys_close( handle );
    return device;
}

/* ------------------------------------------------------------------ */
/* searching                                                           */
/* ------------------------------------------------------------------ */

static void rec_from_lfn( FINDREC *rec )
{
    rec->attr = find_rec[0];
    rec->time = (u16)(find_rec[0x14] | (find_rec[0x15] << 8));
    rec->date = (u16)(find_rec[0x16] | (find_rec[0x17] << 8));
    rec->size = *(u32 *)(find_rec + 0x20);
    str_cpyn( rec->name, (const char *)find_rec + 0x2C, sizeof( rec->name ) );
    str_cpyn( rec->alias, (const char *)find_rec + 0x130, sizeof( rec->alias ) );
    if ( rec->alias[0] == 0 ) {             /* Windows: the name IS 8.3 */
        str_cpyn( rec->alias, rec->name, sizeof( rec->alias ) );
    }
}

static void rec_from_dta( FINDREC *rec )
{
    rec->attr = find_rec[0x15];
    rec->time = (u16)(find_rec[0x16] | (find_rec[0x17] << 8));
    rec->date = (u16)(find_rec[0x18] | (find_rec[0x19] << 8));
    rec->size = *(u32 *)(find_rec + 0x1A);
    str_cpyn( rec->name, (const char *)find_rec + 0x1E, 13 );
    str_cpyn( rec->alias, rec->name, sizeof( rec->alias ) );
}

/* The classic search cannot ask for a REQUIRED attribute, so the
   directory-only searches filter here, as 714Eh would have. */
static u8 dta_required;

static int dta_skip( void )
{
    return (find_rec[0x15] & dta_required) != dta_required;
}

int sys_find_first( const char *pattern, u16 attrs, FINDREC *rec, u32 *handle )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ecx = attrs;                   /* CL allowed, CH required */
        regs.edx = (u32)pattern;
        regs.edi = (u32)find_rec;
        regs.esi = 1;                       /* times in DOS form */
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
/* files                                                               */
/* ------------------------------------------------------------------ */

static int open_ext( const char *path, u32 mode, u32 action, u32 *handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = mode;
    regs.ecx = 0;                           /* a new file is a plain one */
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

int sys_open_read( const char *path, u32 *handle )
{
    REGS regs;
    int rc;

    if ( lfn_ok ) {
        rc = open_ext( path, 0x0000, 0x0001, handle );
        if ( rc >= 0 ) {
            return rc;
        }
    }
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, 0x3D00 );
    *handle = AX( regs );
    return result( &regs );
}

int sys_create( const char *path, u32 *handle )
{
    REGS regs;
    int rc;

    if ( lfn_ok ) {
        rc = open_ext( path, 0x0001, 0x0012, handle );     /* replace or create */
        if ( rc >= 0 ) {
            return rc;
        }
    }
    clear( &regs );
    regs.ecx = 0;
    regs.edx = (u32)path;
    call21( &regs, 0x3C00 );
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

int sys_write_file( u32 handle, const void *buf, u32 len, u32 *done )
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

int sys_get_ftime( u32 handle, u16 *date, u16 *time )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x5700 );
    *time = (u16)regs.ecx;
    *date = (u16)regs.edx;
    return result( &regs );
}

int sys_set_ftime( u32 handle, u16 date, u16 time )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = time;
    regs.edx = date;
    call21( &regs, 0x5701 );
    return result( &regs );
}

/* ------------------------------------------------------------------ */
/* the rest                                                            */
/* ------------------------------------------------------------------ */

int sys_get_verify( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x5400 );
    return (int)(regs.eax & 0xFF);
}

void sys_set_verify( int on )
{
    REGS regs;

    clear( &regs );
    call21( &regs, on ? 0x2E01 : 0x2E00 );
}

void sys_date_format( int *order, char *sep )
{
    REGS regs;
    u8 info[64];

    mem_set( info, 0, sizeof( info ) );
    clear( &regs );
    regs.edx = (u32)info;
    call21( &regs, 0x3800 );
    if ( CF( regs ) ) {
        *order = 0;
        *sep = '-';
        return;
    }
    *order = info[0] <= 2 ? info[0] : 0;
    *sep = info[0x0B] ? (char)info[0x0B] : '-';
}

void *sys_mem_alloc( u32 size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = size;                        /* bytes, on PM-DOS */
    call21( &regs, 0x4800 );
    return CF( regs ) ? NULL : (void *)regs.eax;
}
