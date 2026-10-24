/*
 * SYS_D32.C - DEFRAG's system calls, PM-DOS native.  SCANDISK's, with
 * what a program that rearranges a drive needs more.
 *
 * DEFRAG.EXE is a D32 image loaded at offset 2000h of a program slot
 * and run at ring 3; INT 21h is the real-mode interface widened to 32
 * bits (CHKDSK's SYS_D32.C has the details the C programs share).
 *
 * THE DISK is CHKDSK's half: sectors through 21h/7305h, which goes
 * round the kernel's cache and reports its own errors - no critical
 * error is raised for a sector that will not read.  Before the first
 * sector is written that way the kernel writes out everything it owed
 * the volume and turns the volume's clean-shutdown flag off; after each
 * write it drops what it had cached of the volume.  Two things it does
 * not drop are a drive's current directory, which it knows by cluster,
 * and files that are open - MAIN.C says what is done about those.
 *
 * THE SCREEN, THE KEYBOARD AND THE MOUSE are EDIT's and PMD's: 21h/F0h
 * for cells, the cursor and keys, INT 33h for the mouse.  The graphics
 * screen is DOSSHELL's: 21h/F1h AL=1Fh sets a BIOS mode and answers
 * with a selector for its video memory, AL=20h reads the first megabyte
 * (the BIOS's fonts) and AL=21h asks the BIOS what the adapter is.
 *
 * ANOTHER PROGRAM is 4B00h, DOSSHELL's: ScanDisk, when a drive has
 * errors.  THE MACHINE IS RESTARTED, for /B, by 21h/F1h AL=1Bh, which
 * writes every cache first.
 */
#include "defrag.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void __cdecl int33( REGS *regs );
void __cdecl break_handler( void );
void __cdecl crit_handler( void );

extern char break_hit, crit_hit;

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)
#define F1( al, fn )    (0xF100UL | (al) | ((u32)(fn) << 16))

static u8   *psp;
static char  tail[300];

void __cdecl dos_entry( void )
{
    defrag_main();
}

static void call21( REGS *regs, u32 eax )
{
    regs->eax = eax;
    int21( regs );
}

static void clear( REGS *regs )
{
    mem_set( regs, 0, sizeof( *regs ) );
}

static int result( REGS *regs )
{
    if ( !CF( *regs ) ) {
        return 0;
    }
    return AX( *regs ) ? (int)AX( *regs ) : 31;
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
    /* a tail too long for the PSP is in CMDLINE, program name and all */
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

    /* a drive that will not answer is failed, not asked about */
    clear( &regs );
    regs.edx = (u32)crit_handler;
    call21( &regs, 0x2524 );
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

void sys_break_install( void )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)break_handler;
    call21( &regs, 0x2523 );
}

int sys_break_take( void )
{
    int hit = break_hit;

    break_hit = 0;
    return hit;
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

/* 4400h: bit 7 a device, bit 1 the console's output, bit 0 its input */
static u32 dev_info( u32 handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x4400 );
    return CF( regs ) ? 0 : regs.edx;
}

int sys_stdin_is_console( void )
{
    return (dev_info( 0 ) & 0x81) == 0x81;
}

/* a handle that is open and is not a device: redirected to a file */
int sys_handle_is_file( int handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    call21( &regs, 0x4400 );
    return !CF( regs ) && !(regs.edx & 0x80);
}

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

/* 21h/F1h AL=1Bh: every cache written, and the machine restarted.  It
   comes back only when something would not write. */
void sys_restart( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF11B );
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
/* the clock                                                           */
/* ------------------------------------------------------------------ */

/* 2Ch, as hundredths of a second since midnight */
u32 sys_hundredths( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x2C00 );
    return ((((regs.ecx >> 8) & 0xFF) * 60 + (regs.ecx & 0xFF)) * 60
            + ((regs.edx >> 8) & 0xFF)) * 100 + (regs.edx & 0xFF);
}

void sys_get_country( COUNTRY *ctry )
{
    REGS regs;
    u8 buf[48];

    mem_set( buf, 0, sizeof( buf ) );
    clear( &regs );
    regs.edx = (u32)buf;
    call21( &regs, 0x3800 );
    if ( CF( regs ) ) {
        return;
    }
    ctry->date_fmt = RD16( buf ) <= 2 ? RD16( buf ) : 0;
    if ( buf[7] ) {
        ctry->thou_sep = (char)buf[7];
    }
    if ( buf[11] ) {
        ctry->date_sep = (char)buf[11];
    }
    if ( buf[13] ) {
        ctry->time_sep = (char)buf[13];
    }
    ctry->time_24 = buf[17] & 1;
}

/* ------------------------------------------------------------------ */
/* drives                                                              */
/* ------------------------------------------------------------------ */

/* the volume table's flags (PMDOS.INC, DRVF_*) */
#define DRVF_PRESENT    0x01
#define DRVF_FLOPPY     0x02
#define DRVF_CDROM      0x08
#define DRVF_IMAGE      0x10

int sys_get_drive( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x1900 );
    return (int)(regs.eax & 0xFF);
}

int sys_boot_drive( void )
{
    REGS regs;
    int drive;

    clear( &regs );
    call21( &regs, 0x3305 );
    drive = (int)(regs.edx & 0xFF) - 1;
    return drive >= 0 && drive < 26 ? drive : 2;
}

/* 21h/F1h AL=6: a letter's row of the volume table; 0 = no such row */
static int drive_flags( int drv )
{
    REGS regs;
    u8 rec[16];

    mem_set( rec, 0, sizeof( rec ) );
    clear( &regs );
    regs.edx = (u32)drv;
    regs.edi = (u32)rec;
    call21( &regs, 0xF106 );
    return CF( regs ) ? 0 : rec[2];
}

/* What a drive is to DEFRAG: nothing it can optimize, a diskette drive,
   or anything else with a FAT volume in it.  PM-DOS reads a CD-ROM
   itself instead of through a redirector, so 4409h calls one local;
   F101h answers 12, 16 or 32 for a FAT volume and 0 for ISO9660 or UDF.
   A diskette drive is one whether or not there is a diskette in it. */
int sys_drive_kind( int drv )
{
    REGS regs;
    char in[4], out[130];
    int flags = drive_flags( drv );

    if ( !(flags & DRVF_PRESENT) || (flags & DRVF_CDROM) ) {
        return DK_NONE;
    }
    clear( &regs );
    regs.ebx = (u32)drv + 1;
    call21( &regs, 0x4409 );
    if ( !CF( regs ) && (regs.edx & 0x9000) ) {
        return DK_NONE;                         /* SUBST, or a network drive */
    }
    in[0] = (char)('A' + drv);
    in[1] = ':';
    in[2] = '\\';
    in[3] = 0;
    out[0] = 0;
    clear( &regs );
    regs.esi = (u32)in;
    regs.edi = (u32)out;
    call21( &regs, 0x6000 );
    if ( !CF( regs ) && out[1] == ':' && ch_upper( out[0] ) != 'A' + drv ) {
        return DK_NONE;                         /* another drive under this letter */
    }
    if ( flags & DRVF_FLOPPY ) {
        return DK_FLOPPY;
    }
    clear( &regs );
    regs.edx = (u32)drv + 1;
    call21( &regs, 0xF101 );
    /* 0EFh is exFAT, which has no FAT for this program to rearrange */
    return !CF( regs ) && (regs.eax & 0xFF) != 0 && (regs.eax & 0xFF) != 0xEF ?
           DK_FIXED : DK_NONE;
}

/* 1 = the drive is an exFAT volume, which is why DK_NONE */
int sys_drive_exfat( int drv )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)drv + 1;
    call21( &regs, 0xF101 );
    return !CF( regs ) && (regs.eax & 0xFF) == 0xEF;
}

int sys_get_cwd( int drv, char *buf, int max )
{
    REGS regs;
    char tmp[270];

    tmp[0] = 0;
    clear( &regs );
    regs.edx = (u32)drv + 1;
    regs.esi = (u32)tmp;
    regs.flags = 1;
    call21( &regs, 0x7147 );                    /* long path (DOS 7) */
    if ( CF( regs ) ) {
        clear( &regs );
        regs.edx = (u32)drv + 1;
        regs.esi = (u32)tmp;
        call21( &regs, 0x4700 );
        if ( CF( regs ) ) {
            return -1;
        }
    }
    if ( (int)str_len( tmp ) >= max ) {
        return -1;
    }
    str_cpy( buf, tmp );
    return 0;
}

/* 3Bh: "dir" is what 47h gave - no drive, no leading backslash - and ""
   is the root */
int sys_chdir( int drv, const char *dir )
{
    REGS regs;
    char path[PATH_MAX + 4];

    sfmt( path, sizeof( path ), "%c:\\%s", 'A' + drv, dir );
    clear( &regs );
    regs.edx = (u32)path;
    regs.flags = 1;
    call21( &regs, 0x713B );                    /* long path (DOS 7) */
    if ( CF( regs ) ) {
        clear( &regs );
        regs.edx = (u32)path;
        call21( &regs, 0x3B00 );
    }
    return result( &regs );
}

/* 21h/7302h, as DOS 7.1 has it:
 the drive in DL (1 = A:), the buffer at
   EDI and its size in ECX, and back comes DOS 7.1's extended DPB behind
   a length word. */
int sys_get_bpb( int drv, u8 *sector )
{
    REGS regs;
    u8 buf[0x40];

    mem_set( buf, 0, sizeof( buf ) );
    WR16( buf, 0x3F );
    clear( &regs );
    regs.edx = (u32)drv + 1;
    regs.ecx = 0x3F;
    regs.edi = (u32)buf;
    call21( &regs, 0x7302 );
    if ( CF( regs ) ) {
        return -1;
    }
    bpb_from_dpb( buf + 2, sector );
    return 0;
}

/*
 * 440Dh, lock (4Ah) and unlock (6Ah).  DOS 7 takes permissions in DX
 * for these.  PM-DOS has one 440Dh function, which takes a block at
 * DS:DX, so the kernel reads EDX as a pointer for every 440Dh call and
 * refuses a native program whose EDX is not inside its slot - a zero
 * there came back "access denied" and looked like a lock refused.  So
 * EDX points at something, which costs nothing and is ignored.
 */
static int lock_call( int drv, u32 cx )
{
    static u8 unused[8];
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)drv + 1;                    /* BH = lock level 0 */
    regs.ecx = cx;
    regs.edx = (u32)unused;
    call21( &regs, 0x440D );
    if ( !CF( regs ) ) {
        return 0;
    }
    return AX( regs ) == 5 ? -1 : 1;            /* refused, or no such call */
}

/* PM-DOS answers a lock with "invalid function", which is taken as "no
   lock needed": the kernel serves one program at a time.  Only a lock
   that exists and is refused stops the check. */
int sys_lock( int drv, int fat32 )
{
    int rc = 1;

    if ( fat32 ) {
        rc = lock_call( drv, 0x484A );
    }
    if ( rc > 0 ) {
        rc = lock_call( drv, 0x084A );
    }
    return rc < 0 ? -1 : 0;
}

void sys_unlock( int drv, int fat32 )
{
    if ( !fat32 || lock_call( drv, 0x486A ) != 0 ) {
        lock_call( drv, 0x086A );
    }
}

static int disk_io( int drv, u32 lba, u32 count, void *buf, u32 mode )
{
    REGS regs;
    u8 pkt[10];

    WR32( pkt, lba );
    WR16( pkt + 4, count );
    WR32( pkt + 6, (u32)buf );
    clear( &regs );
    regs.ebx = (u32)pkt;
    regs.ecx = 0xFFFFFFFFul;
    regs.edx = (u32)drv + 1;
    regs.esi = mode;
    regs.flags = 1;
    call21( &regs, 0x7305 );
    return result( &regs );
}

int sys_read_sec( int drv, u32 lba, u32 count, u32 bps, void *buf )
{
    (void)bps;
    return disk_io( drv, lba, count, buf, 0 );
}

int sys_write_sec( int drv, u32 lba, u32 count, u32 bps, const void *buf, int kind )
{
    (void)bps;
    return disk_io( drv, lba, count, (void *)buf, 1u | ((u32)kind << 13) );
}

void sys_reset_drive( int drv )
{
    REGS regs;

    clear( &regs );
    regs.ecx = 1;                               /* flush and invalidate */
    regs.edx = (u32)drv + 1;
    call21( &regs, 0x710D );
    clear( &regs );
    call21( &regs, 0x0D00 );
}

/* 21h/F1h AL=1Dh, VLF_QUERY: whether the kernel keeps a clean-shutdown
   flag for the volume, and what it says */
int sys_vol_flags( int drv )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)drv + 1;
    call21( &regs, F1( 0x1D, 0 ) );
    return CF( regs ) ? 0 : (int)(regs.eax & 0xFF);
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

int sys_create( const char *path, u32 *handle )
{
    REGS regs;

    crit_hit = 0;
    clear( &regs );
    regs.ecx = 0;
    regs.edx = (u32)path;
    call21( &regs, 0x3C00 );
    *handle = AX( regs );
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
        return result( &regs );
    }
    *done = regs.eax;
    return regs.eax < len ? 112 : 0;            /* the disk is full */
}

int sys_close( u32 handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x3E00 );
    return result( &regs );
}

int sys_exists( const char *path )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, 0x4300 );
    return !CF( regs );
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
    int buttons;

    clear( &regs );
    call33( &regs, 0x0000 );
    buttons = (regs.eax & 0xFFFF) == 0xFFFF ? (int)(regs.ebx & 0xFFFF) : 0;
    if ( buttons == 0xFFFF ) {
        buttons = 2;
    }
    return buttons;
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

/* 07h and 08h: the driver is told how many pixels a graphics screen
   has, and answers in them */
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
/* a graphics screen                                                   */
/* ------------------------------------------------------------------ */

/* 21h/F1h AL=20h: bytes of the first megabyte, which is outside a
   native program's segment.  A kernel without the call reads nothing. */
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

/* 21h/F1h AL=21h: one of the BIOS calls that only report */
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

/* 21h/F1h AL=1Fh, function 2: a mode below 100h is the BIOS's own, and
   the selector is the 64K at A0000h */
int sys_video_set( u32 mode, u32 *selector, u32 *bytes )
{
    REGS regs;

    clear( &regs );
    regs.ebx = mode;
    call21( &regs, F1( 0x1F, 2 ) );
    if ( CF( regs ) ) {
        return AX( regs ) ? (int)AX( regs ) : 31;
    }
    *selector = regs.eax & 0xFFFF;
    *bytes = regs.ecx;
    return 0;
}

/* function 3: the text mode the first mode set found, with its lines */
void sys_video_restore( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F1( 0x1F, 3 ) );
}
