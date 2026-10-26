/*
 * SYS_D32.C - DEBUG's system calls, PM-DOS native.
 *
 * DEBUG.EXE is a D32 image (DOSINT.ASM with -dD32), loaded at offset
 * 2000h of a program slot (512K to start with; 48h grows it) and run
 * at ring 3 with every pointer a 32-bit offset within the slot.  INT
 * 21h is the real-mode interface widened to 32 bits: pointers in EDX,
 * ESI, EDI and EBX, counts in ECX, CF and AX for errors.  Where PM-DOS
 * differs from DOS:
 *
 *   - 48h takes BYTES in EBX and returns the block's offset in EAX.
 *   - DEBUG's slot is all it can address.  The first megabyte is read
 *     through 21h/F1h AL=20h, and never written; the program it debugs
 *     is read, written and run through 21h/F1h AL=29h, which the
 *     kernel answers for exactly this program (DOS\DBGAPI.INC).
 */
#include "rt.h"
#include "sys.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void __cdecl int25( REGS *regs );
void __cdecl int26( REGS *regs );
void __cdecl break_handler( void );

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)

#define TAIL_MAX    128

static u8   *psp;
static char  tail[TAIL_MAX];
static int   in_device = 1;     /* standard input is the keyboard */
static int   lfn_ok = 1;

void __cdecl dos_entry( void );

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
/* the program, its command line and its console                       */
/* ------------------------------------------------------------------ */

int sys_init( void )
{
    REGS regs;
    u32 len;

    clear( &regs );
    call21( &regs, 0x6200 );
    psp = (u8 *)regs.ebx;
    len = psp[0x80];
    if ( len > 126 ) {
        len = 126;
    }
    mem_cpy( tail, psp + 0x81, len );
    tail[len] = 0;
    in_device = sys_is_device( H_IN );
    return 0;
}

const char *sys_cmdline( void )
{
    return tail;
}

int sys_is_device( int handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    call21( &regs, 0x4400 );
    if ( CF( regs ) ) {
        return 0;
    }
    return ( regs.edx & 0x80 ) != 0;
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
    regs.ebx = size;
    call21( &regs, 0x4800 );
    if ( CF( regs ) ) {
        return NULL;
    }
    return (void *)regs.eax;
}

void sys_mem_free( void *block )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)block;
    call21( &regs, 0x4900 );
}

/* sys_get_key gave a CR from a file: the LF after it ends nothing */
static int lf_owed;

/* One byte of standard input as it is, through the handle: -1 at its
   end.  Used when the input is a file, where 0Ah would echo it but
   could not say the file had run out. */
static int read_byte( void )
{
    REGS regs;
    u8 ch;

    for ( ;; ) {
        clear( &regs );
        regs.ebx = H_IN;
        regs.ecx = 1;
        regs.edx = (u32)&ch;
        call21( &regs, 0x3F00 );
        if ( CF( regs ) || regs.eax == 0 ) {
            return -1;
        }
        if ( lf_owed ) {
            lf_owed = 0;
            if ( ch == '\n' ) {
                continue;
            }
        }
        return ch;
    }
}

/* A command line into BUF (MAX bytes with its NUL): 0Ah from the
   keyboard, which edits as the shell's prompt does; from a file, read
   to the end of the line and shown, as 6.22's 0Ah showed it.  -1 when
   a file has nothing left. */
int sys_read_line( char *buf, u32 max )
{
    static u8 line[2 + 256];
    REGS regs;
    u32 len = 0;
    int ch;

    if ( in_device ) {
        line[0] = (u8)( max - 1 > 255 ? 255 : max - 1 );
        line[1] = 0;
        clear( &regs );
        regs.edx = (u32)line;
        call21( &regs, 0x0A00 );
        len = line[1];
        mem_cpy( buf, line + 2, len );
        buf[len] = 0;
        out_ch( '\n' );
        out_flush();
        return (int)len;
    }
    for ( ;; ) {
        ch = read_byte();
        if ( ch < 0 ) {
            if ( len == 0 ) {
                return -1;
            }
            break;
        }
        if ( ch == '\r' ) {
            continue;
        }
        if ( ch == '\n' ) {
            break;
        }
        if ( ch == 0x1A ) {             /* ^Z: the end of a text file */
            if ( len == 0 ) {
                return -1;
            }
            break;
        }
        if ( len + 1 < max ) {
            buf[len++] = (char)ch;
        }
    }
    buf[len] = 0;
    out_str( buf );
    out_nl();
    return (int)len;
}

/* One key, unechoed: an extended key is 0 and then its code, as 08h
   gives them.  -1 at the end of a file. */
int sys_get_key( void )
{
    REGS regs;

    if ( !in_device ) {
        int ch = read_byte();
        lf_owed = ( ch == '\r' );       /* "CR LF" is one Enter */
        return ch;
    }
    clear( &regs );
    call21( &regs, 0x0800 );
    return (int)( regs.eax & 0xFF );
}

void sys_break_install( void )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)break_handler;
    call21( &regs, 0x2523 );
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

/* CF with AX = 7100h: the long-name call is not there */
static int lfn_missing( REGS *regs )
{
    if ( CF( *regs ) && AX( *regs ) == 0x7100 ) {
        lfn_ok = 0;
        return 1;
    }
    return 0;
}

static int open_mode( const char *name, int create, int *err )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = create ? 0x0002 : 0x0000;        /* read/write, or read */
        regs.ecx = 0;                               /* attributes */
        regs.edx = create ? 0x0012 : 0x0001;        /* replace or make / open */
        regs.esi = (u32)name;
        call21( &regs, 0x716C );
        if ( !lfn_missing( &regs ) ) {
            if ( CF( regs ) ) {
                *err = (int)AX( regs );
                return -1;
            }
            return (int)AX( regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)name;
    regs.ecx = 0;
    call21( &regs, create ? 0x3C00 : 0x3D00 );
    if ( CF( regs ) ) {
        *err = (int)AX( regs );
        return -1;
    }
    return (int)AX( regs );
}

int sys_open( const char *name, int *err )
{
    return open_mode( name, 0, err );
}

int sys_create( const char *name, int *err )
{
    return open_mode( name, 1, err );
}

void sys_delete( const char *name )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (u32)name;
        regs.esi = 0;                               /* no wildcards */
        call21( &regs, 0x7141 );
        if ( !lfn_missing( &regs ) ) {
            return;
        }
    }
    clear( &regs );
    regs.edx = (u32)name;
    call21( &regs, 0x4100 );
}

int sys_read( int handle, void *buf, u32 len, u32 *got )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    call21( &regs, 0x3F00 );
    *got = CF( regs ) ? 0 : regs.eax;
    return result( &regs );
}

int sys_write_file( int handle, const void *buf, u32 len, u32 *done )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    call21( &regs, 0x4000 );
    *done = CF( regs ) ? 0 : regs.eax;
    return result( &regs );
}

int sys_size( int handle, u32 *size )
{
    REGS regs;
    int rc;

    clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = 0;
    call21( &regs, 0x4202 );        /* to the end: EAX = the length */
    rc = result( &regs );
    *size = rc ? 0 : regs.eax;
    clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = 0;
    call21( &regs, 0x4200 );        /* and back to the start */
    return rc;
}

int sys_seek( int handle, u32 pos )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = pos;
    call21( &regs, 0x4200 );
    return result( &regs );
}

void sys_close( int handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    call21( &regs, 0x3E00 );
}

/* ------------------------------------------------------------------ */
/* the first megabyte                                                  */
/* ------------------------------------------------------------------ */

int sys_low_read( u32 linear, void *buf, u32 len )
{
    REGS regs;

    clear( &regs );
    regs.ebx = linear;
    regs.ecx = len;
    regs.edi = (u32)buf;
    call21( &regs, 0x0000F120UL );          /* LMF_READ = 0 */
    return result( &regs );
}

/* The first free block of the chain a DOS box runs in (21h/F1h AL=8,
   F8_CONV), as a paragraph: where a 16-bit program would be loaded, so
   that the work area for one stands where it would stand and the real
   memory below it - the vectors, the BIOS's data, DOS's - is still
   there to be read.  1000h if the chain will not say. */
u16 sys_conv_seg( void )
{
    REGS regs;
    u32 rec[4];                     /* F8_SIG, F8_SIZE, F8_OWNER, F8_ADDR */
    u32 index;

    for ( index = 0; index < 64; index++ ) {
        clear( &regs );
        regs.ebx = index;
        regs.edx = 1;               /* F8_CONV */
        regs.edi = (u32)rec;
        call21( &regs, 0xF108 );
        if ( CF( regs ) || rec[0] == 'E' ) {
            break;
        }
        if ( rec[0] == 'Z' && rec[3] < 0x90000UL && rec[1] >= 0x10000UL ) {
            return (u16)( ( rec[3] + 15 ) >> 4 );
        }
    }
    return 0x1000;
}

/* ------------------------------------------------------------------ */
/* raw sectors                                                         */
/* ------------------------------------------------------------------ */

int sys_abs_read( int drive, u32 sector, u32 count, void *buf )
{
    REGS regs;

    clear( &regs );
    regs.eax = (u32)drive;
    regs.edx = sector;
    regs.ecx = count;
    regs.ebx = (u32)buf;
    int25( &regs );
    return result( &regs );
}

int sys_abs_write( int drive, u32 sector, u32 count, const void *buf )
{
    REGS regs;

    clear( &regs );
    regs.eax = (u32)drive;
    regs.edx = sector;
    regs.ecx = count;
    regs.ebx = (u32)buf;
    int26( &regs );
    return result( &regs );
}

/* ------------------------------------------------------------------ */
/* the held program                                                    */
/* ------------------------------------------------------------------ */

static int dbg_call( u32 fn, REGS *regs )
{
    regs->eax = ( fn << 16 ) | 0xF129;
    regs->flags = 0;
    int21( regs );
    return result( regs );
}

/* TAIL is the command tail as DOS keeps it: a length, the text, CR */
int sys_dbg_load( const char *path, const char *tail, DRX *regs )
{
    static u32 block[2];            /* 4Bh's: the environment (ours), the tail */
    REGS r;

    block[0] = 0;
    block[1] = (u32)tail;
    clear( &r );
    r.edx = (u32)path;
    r.ebx = (u32)block;
    r.esi = (u32)regs;
    return dbg_call( 0, &r );
}

int sys_dbg_go( DRX *regs, int *stopped )
{
    REGS r;
    int rc;

    clear( &r );
    r.esi = (u32)regs;
    rc = dbg_call( 1, &r );
    *stopped = rc == 0 && AX( r ) == DBF_STOPPED;
    return rc;
}

int sys_dbg_read( u32 offset, void *buf, u32 len )
{
    REGS r;

    clear( &r );
    r.ebx = offset;
    r.ecx = len;
    r.edi = (u32)buf;
    return dbg_call( 2, &r );
}

int sys_dbg_write( u32 offset, const void *buf, u32 len )
{
    REGS r;

    clear( &r );
    r.ebx = offset;
    r.ecx = len;
    r.esi = (u32)buf;
    return dbg_call( 3, &r );
}

int sys_dbg_kill( void )
{
    REGS r;

    clear( &r );
    return dbg_call( 4, &r );
}

int sys_exit_code( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x4D00 );
    return (int)AX( regs );
}
