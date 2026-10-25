/*
 * PLAT32.C - SETUP's 32-bit half: PM-DOS, native.
 *
 * SETUP.EXE's D32 image is loaded at offset 2000h of a program slot and
 * run at ring 3; INT 21h is the real-mode interface widened to 32 bits
 * (CMD\CHKDSK\SYS_D32.C has the rules every native C program follows).
 * What Setup needs beyond the files:
 *
 *   21h/F0h       the screen, the cursor and the keys (a native program
 *                 has no INT 10h or 16h)
 *   21h/F1h AL=6  a drive letter: its BIOS disk, partition type, flags
 *                 and where its partition is
 *   AL=0Ch        a physical disk: its geometry and size
 *   AL=0Dh/0Eh    raw sectors of a disk, LBA 0 and all
 *   AL=2          remount a letter whose sectors were rewritten
 *   AL=14h        a diskette track at a time (FDF_*), FORMAT's way
 *   AL=16h        commit the cache
 *   AL=1Bh        the cache committed, then the restart
 *   21h/7305h     a volume's sectors by letter
 *
 * The program's own path is not given to a native program, so
 * plat_self() is empty and COPY.C looks for SETUP.INF instead.
 */
#include "setup.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void __cdecl crit_handler( void );
void __cdecl break_handler( void );
extern char crit_hit;

#define CF( regs )      ((regs).flags & 1)
#define AX( regs )      ((regs).eax & 0xFFFF)
#define F1( al, fn )    (0xF100UL | (al) | ((u32)(fn) << 16))

unsigned plat_caps = PC_32BIT | PC_PARTITION | PC_RESTART;

static u8   *psp;
static char  tail[130];
static int   char_height = 16;
static u16  *saved_screen;
static int   saved_row, saved_col;

void __cdecl dos_entry( void )
{
    plat_init();
    setup_main();
    plat_exit( 0 );
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

int plat_init( void )
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
    mem_cpy( tail, psp + 0x81, (u16)len );
    tail[len] = 0;
    clear( &regs );
    regs.edx = (u32)crit_handler;
    call21( &regs, 0x2524 );
    clear( &regs );
    regs.edx = (u32)break_handler;
    call21( &regs, 0x2523 );
    return 0;
}

const char *plat_tail( void )
{
    return tail;
}

const char *plat_self( void )
{
    return "";
}

const char *plat_getenv( const char *name )
{
    const char *env;
    int len = str_len( name );

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

void plat_exit( int code )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x4C00 | ((u32)code & 0xFF) );
}

void *plat_alloc( u32 size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = size;                    /* bytes, on PM-DOS */
    call21( &regs, 0x4800 );
    return CF( regs ) ? NULL : (void *)regs.eax;
}

void plat_free( void *ptr )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)ptr;
    call21( &regs, 0x4900 );
}

void plat_line( const char *text )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 1;
    regs.ecx = str_len( text );
    regs.edx = (u32)text;
    call21( &regs, 0x4000 );
    clear( &regs );
    regs.ebx = 1;
    regs.ecx = 2;
    regs.edx = (u32)"\r\n";
    call21( &regs, 0x4000 );
}

int plat_stdin_byte( void )
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
/* the system                                                          */
/* ------------------------------------------------------------------ */

void plat_dosver( int *major, int *minor )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x3000 );
    *major = (int)(regs.eax & 0xFF);
    *minor = (int)((regs.eax >> 8) & 0xFF);
}

/* F100h with BX = FFFFh: BH.BL = PM-DOS's version (0.65 = 0, 65) */
int plat_pmdos( void )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 0xFFFF;
    call21( &regs, 0xF100 );
    if ( (regs.ebx & 0xFFFF) == 0xFFFF ) {
        return 1;
    }
    return (int)(((regs.ebx >> 8) & 0xFF) * 100 + (regs.ebx & 0xFF));
}

int plat_windows( void )
{
    return 0;
}

int plat_cpu386( void )
{
    return 1;
}

int plat_boot_drive( void )
{
    REGS regs;
    int drive;

    clear( &regs );
    call21( &regs, 0x3305 );
    drive = (int)(regs.edx & 0xFF) - 1;
    return drive >= 0 && drive < 26 ? drive : 2;
}

int plat_country_code( void )
{
    REGS regs;
    u8 buf[48];

    clear( &regs );
    regs.edx = (u32)buf;
    call21( &regs, 0x3800 );
    return CF( regs ) ? 1 : (int)(regs.ebx & 0xFFFF);
}

void plat_get_date( int *year, int *month, int *day )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x2A00 );
    *year = (int)(regs.ecx & 0xFFFF);
    *month = (int)((regs.edx >> 8) & 0xFF);
    *day = (int)(regs.edx & 0xFF);
}

void plat_get_time( int *hour, int *minute )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x2C00 );
    *hour = (int)((regs.ecx >> 8) & 0xFF);
    *minute = (int)(regs.ecx & 0xFF);
}

int plat_set_date( int year, int month, int day )
{
    REGS regs;

    clear( &regs );
    regs.ecx = (u32)year;
    regs.edx = ((u32)month << 8) | (u32)day;
    call21( &regs, 0x2B00 );
    return (regs.eax & 0xFF) != 0;
}

int plat_set_time( int hour, int minute )
{
    REGS regs;

    clear( &regs );
    regs.ecx = ((u32)hour << 8) | (u32)minute;
    regs.edx = 0;
    call21( &regs, 0x2D00 );
    return (regs.eax & 0xFF) != 0;
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

void plat_screen_open( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF003 );
    char_height = (int)(regs.ecx & 0xFF);
    if ( char_height < 4 || char_height > 32 ) {
        char_height = 16;
    }
    if ( ((regs.edx >> 8) & 0xFF) != SCR_ROWS || (regs.edx & 0xFF) != SCR_COLS ) {
        clear( &regs );
        regs.edx = 25;
        call21( &regs, 0xF00A );        /* back to 25 lines */
    }
    saved_screen = (u16 *)plat_alloc( SCR_ROWS * SCR_COLS * 2 );
    if ( saved_screen ) {
        plat_screen_get( 0, saved_screen, SCR_ROWS * SCR_COLS );
    }
    clear( &regs );
    call21( &regs, 0xF001 );
    saved_row = (int)((regs.edx >> 8) & 0xFF);
    saved_col = (int)(regs.edx & 0xFF);
}

void plat_screen_put( int pos, const u16 *cells, int count )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)pos;
    regs.ecx = (u32)count;
    regs.esi = (u32)cells;
    call21( &regs, 0xF004 );
}

void plat_screen_get( int pos, u16 *cells, int count )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)pos;
    regs.ecx = (u32)count;
    regs.edi = (u32)cells;
    call21( &regs, 0xF005 );
}

/* shown: an underline in the last two scan lines, as 6.22's fields */
void plat_cursor( int row, int col, int shown )
{
    REGS regs;

    clear( &regs );
    if ( shown ) {
        regs.edx = ((u32)row << 8) | (u32)col;
        call21( &regs, 0xF002 );
        clear( &regs );
        regs.ecx = ((u32)(char_height - 2) << 8) | (u32)(char_height - 1);
    } else {
        regs.ecx = 0x2000;
    }
    call21( &regs, 0xF006 );
}

/* Setup ends on a clean screen, as 6.22's did */
void plat_screen_close( void )
{
    REGS regs;
    u16 *blank = (u16 *)plat_alloc( SCR_ROWS * SCR_COLS * 2 );
    int i;

    if ( blank ) {
        for ( i = 0; i < SCR_ROWS * SCR_COLS; i++ ) {
            blank[i] = 0x0720;
        }
        plat_screen_put( 0, blank, SCR_ROWS * SCR_COLS );
    }
    clear( &regs );
    call21( &regs, 0xF002 );
    clear( &regs );
    regs.ecx = ((u32)(char_height - 2) << 8) | (u32)(char_height - 1);
    call21( &regs, 0xF006 );
    (void)saved_row;
    (void)saved_col;
}

/* ------------------------------------------------------------------ */
/* the keyboard                                                        */
/* ------------------------------------------------------------------ */

static int shift_now( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF009 );
    return (int)(regs.ebx & 0xFF);
}

int plat_key_ready( void )
{
    REGS regs;

    clear( &regs );
    regs.edx = 0x0100 | (u32)shift_now();       /* DH bit 0: do not wait */
    call21( &regs, 0xF008 );
    return (regs.eax & 0xFF) == 1;
}

int plat_key_read( void )
{
    REGS regs;
    int ascii, scan;

    clear( &regs );
    call21( &regs, 0xF007 );
    ascii = (int)(regs.eax & 0xFF);
    scan = (int)((regs.eax >> 8) & 0xFF);
    if ( (ascii == 0 || ascii == 0xE0) && scan ) {
        return K_EXT( scan );
    }
    return ascii;
}

/* wait for a key: DH = 0 */
void plat_idle( void )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)shift_now();
    call21( &regs, 0xF008 );
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

int plat_open( const char *path, int write, u16 *handle )
{
    REGS regs;

    crit_hit = 0;
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, write ? 0x3D02 : 0x3D00 );
    *handle = (u16)AX( regs );
    return result( &regs );
}

int plat_create( const char *path, u16 *handle )
{
    REGS regs;

    crit_hit = 0;
    clear( &regs );
    regs.ecx = 0;
    regs.edx = (u32)path;
    call21( &regs, 0x3C00 );
    *handle = (u16)AX( regs );
    return result( &regs );
}

int plat_read( u16 handle, void *buf, u16 len, u16 *done )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    call21( &regs, 0x3F00 );
    *done = CF( regs ) ? 0 : (u16)regs.eax;
    return result( &regs );
}

int plat_write( u16 handle, const void *buf, u16 len, u16 *done )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    call21( &regs, 0x4000 );
    *done = CF( regs ) ? 0 : (u16)regs.eax;
    return result( &regs );
}

int plat_close( u16 handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x3E00 );
    return result( &regs );
}

/* 42h, the native form: the offset in ECX, the position back in EAX */
int plat_seek( u16 handle, u32 pos )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = pos;
    call21( &regs, 0x4200 );
    return result( &regs );
}

int plat_size( u16 handle, u32 *size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = 0;
    call21( &regs, 0x4202 );
    if ( CF( regs ) ) {
        return result( &regs );
    }
    *size = regs.eax;
    return plat_seek( handle, 0 );
}

int plat_get_ftime( u16 handle, u16 *date, u16 *time )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x5700 );
    *time = (u16)regs.ecx;
    *date = (u16)regs.edx;
    return result( &regs );
}

int plat_set_ftime( u16 handle, u16 date, u16 time )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    regs.ecx = time;
    regs.edx = date;
    call21( &regs, 0x5701 );
    return result( &regs );
}

static int path_call( u32 func, const char *path )
{
    REGS regs;

    crit_hit = 0;
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, func );
    return result( &regs );
}

int plat_delete( const char *path )
{
    return path_call( 0x4100, path );
}

int plat_mkdir( const char *path )
{
    return path_call( 0x3900, path );
}

int plat_rmdir( const char *path )
{
    return path_call( 0x3A00, path );
}

int plat_rename( const char *from, const char *to )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)from;
    regs.edi = (u32)to;
    call21( &regs, 0x5600 );
    return result( &regs );
}

int plat_get_attr( const char *path, u16 *attr )
{
    REGS regs;

    crit_hit = 0;
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, 0x4300 );
    *attr = (u16)regs.ecx;
    return result( &regs );
}

int plat_set_attr( const char *path, u16 attr )
{
    REGS regs;

    clear( &regs );
    regs.ecx = attr;
    regs.edx = (u32)path;
    call21( &regs, 0x4301 );
    return result( &regs );
}

static void find_fill( FINDDATA *fd, const u8 *dta )
{
    fd->attr = dta[0x15];
    fd->time = RD16( dta + 0x16 );
    fd->date = RD16( dta + 0x18 );
    fd->size = RD32( dta + 0x1A );
    str_cpyn( fd->name, (const char *)dta + 0x1E, sizeof( fd->name ) );
}

/* the search state lives in the record's own reserved bytes */
int plat_find_first( const char *spec, u16 attr, FINDDATA *fd )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)fd->reserved;
    call21( &regs, 0x1A00 );
    clear( &regs );
    regs.ecx = attr;
    regs.edx = (u32)spec;
    call21( &regs, 0x4E00 );
    if ( CF( regs ) ) {
        return result( &regs );
    }
    find_fill( fd, fd->reserved );
    return 0;
}

int plat_find_next( FINDDATA *fd )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)fd->reserved;
    call21( &regs, 0x1A00 );
    clear( &regs );
    call21( &regs, 0x4F00 );
    if ( CF( regs ) ) {
        return result( &regs );
    }
    find_fill( fd, fd->reserved );
    return 0;
}

/* 7303h first - every number in it is 32 bits - then 36h.
   PM-DOS's record (PMDOS.INC F73_*) starts with the version word, so it
   is two bytes ahead of DOS 7.1's, which starts with its size: a first
   word of 0 says which (and keeps this right if the kernel moves to
   DOS 7.1's layout) */
int plat_disk_free( int drive, u32 *free_kb, u32 *cluster_bytes )
{
    REGS regs;
    u8 rec[44], *at;
    char root[4];

    root[0] = (char)('A' + drive);
    root[1] = ':';
    root[2] = '\\';
    root[3] = 0;
    mem_set( rec, 0, sizeof( rec ) );
    WR16( rec, sizeof( rec ) );
    clear( &regs );
    regs.edx = (u32)root;
    regs.edi = (u32)rec;
    regs.ecx = sizeof( rec );
    regs.flags = 1;
    crit_hit = 0;
    call21( &regs, 0x7303 );
    if ( !CF( regs ) ) {
        at = RD16( rec ) == 0 ? rec + 2 : rec + 4;
        *cluster_bytes = RD32( at ) * RD32( at + 4 );
        *free_kb = *cluster_bytes >= 1024 ? RD32( at + 8 ) * (*cluster_bytes / 1024)
                                     : RD32( at + 8 ) / 2;
        return 0;
    }
    clear( &regs );
    regs.edx = (u32)drive + 1;
    call21( &regs, 0x3600 );
    if ( AX( regs ) == 0xFFFF ) {
        return 15;
    }
    *cluster_bytes = AX( regs ) * (regs.ecx & 0xFFFF);
    *free_kb = *cluster_bytes >= 1024 ? (regs.ebx & 0xFFFF) * (*cluster_bytes / 1024)
                                 : (regs.ebx & 0xFFFF) / 2;
    return 0;
}

int plat_current_dir( char *buf, int max )
{
    REGS regs;
    char dir[70];
    int drive;

    clear( &regs );
    call21( &regs, 0x1900 );
    drive = (int)(regs.eax & 0xFF);
    clear( &regs );
    regs.edx = 0;
    regs.esi = (u32)dir;
    call21( &regs, 0x4700 );
    if ( CF( regs ) ) {
        return 1;
    }
    sfmt( buf, max, "%c:\\%s", 'A' + drive, dir );
    if ( dir[0] ) {
        str_catn( buf, "\\", max );
    }
    return 0;
}

/* the commit of every cache, so a raw read sees what DOS wrote */
void plat_flush( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x0D00 );
    clear( &regs );
    call21( &regs, F1( 0x16, 1 ) );     /* CCF_COMMIT */
}

/* ------------------------------------------------------------------ */
/* drives and disks                                                    */
/* ------------------------------------------------------------------ */

/* the volume table's row (PMDOS.INC DRV_*, DRVF_*) */
#define DRVF_PRESENT    0x01
#define DRVF_FLOPPY     0x02
#define DRVF_CDROM      0x08
#define DRVF_USB        0x80
#define NET_DRV_TYPE    0xFE

int plat_drive( int drive, DRIVEINFO *di )
{
    REGS regs;
    u8 row[16];

    mem_set( di, 0, sizeof( *di ) );
    di->bios = -1;
    mem_set( row, 0, sizeof( row ) );
    clear( &regs );
    regs.edx = (u32)drive;
    regs.edi = (u32)row;
    call21( &regs, F1( 6, 0 ) );
    if ( CF( regs ) || !(row[2] & DRVF_PRESENT) ) {
        return 1;
    }
    di->flags = DI_PRESENT;
    di->bios = row[0];
    di->ptype = row[1];
    di->start = RD32( row + 4 );
    di->secs = RD32( row + 8 );
    if ( row[1] == NET_DRV_TYPE ) {
        di->flags |= DI_REMOTE;
    } else if ( row[2] & DRVF_CDROM ) {
        di->flags |= DI_CDROM;
    } else if ( row[2] & DRVF_USB ) {
        di->flags |= DI_REMOVABLE;
    } else if ( (row[2] & DRVF_FLOPPY) && row[0] < 0x80 ) {
        di->flags |= DI_FLOPPY;
    } else if ( row[0] >= 0x80 ) {
        di->flags |= DI_FIXED;
    }
    return 0;
}

int plat_disk( int bios, DISKINFO *dk )
{
    REGS regs;
    u8 rec[16];

    mem_set( dk, 0, sizeof( *dk ) );
    mem_set( rec, 0, sizeof( rec ) );
    clear( &regs );
    regs.edx = (u32)bios;
    regs.edi = (u32)rec;
    call21( &regs, F1( 0x0C, 0 ) );
    if ( CF( regs ) || !(rec[1] & 1) ) {
        return 1;
    }
    dk->present = 1;
    dk->removable = (rec[1] & 2) != 0;
    dk->spt = RD16( rec + 2 );
    dk->heads = RD16( rec + 4 );
    dk->total = RD32( rec + 8 );
    return 0;
}

static int raw_io( u32 al, int bios, u32 lba, u16 count, const void *buf )
{
    REGS regs;
    u16 step;

    while ( count ) {
        step = count > 64 ? 64 : count;
        clear( &regs );
        regs.edx = (u32)bios;
        regs.ebx = lba;
        regs.ecx = step;
        regs.edi = (u32)buf;
        crit_hit = 0;
        call21( &regs, al );
        if ( CF( regs ) ) {
            return result( &regs );
        }
        lba += step;
        count -= step;
        buf = (const u8 *)buf + (u32)step * 512;
    }
    return 0;
}

int plat_disk_read( int bios, u32 lba, u16 count, void *buf )
{
    return raw_io( 0xF10D, bios, lba, count, buf );
}

int plat_disk_write( int bios, u32 lba, u16 count, const void *buf )
{
    return raw_io( 0xF10E, bios, lba, count, buf );
}

static int vol_io( int drive, u32 sector, u16 count, const void *buf, u32 mode )
{
    REGS regs;
    u8 pkt[10];

    WR32( pkt, sector );
    WR16( pkt + 4, count );
    WR32( pkt + 6, (u32)buf );
    clear( &regs );
    regs.ebx = (u32)pkt;
    regs.ecx = 0xFFFFFFFFul;
    regs.edx = (u32)drive + 1;
    regs.esi = mode;
    regs.flags = 1;
    crit_hit = 0;
    call21( &regs, 0x7305 );
    return result( &regs );
}

int plat_vol_read( int drive, u32 sector, u16 count, void *buf )
{
    return vol_io( drive, sector, count, buf, 0 );
}

int plat_vol_write( int drive, u32 sector, u16 count, const void *buf )
{
    return vol_io( drive, sector, count, buf, 1 );
}

int plat_vol_remount( int drive )
{
    REGS regs;

    plat_flush();
    clear( &regs );
    regs.edx = (u32)drive + 1;
    call21( &regs, 0xF102 );
    return result( &regs );
}

int plat_lock( int drive )
{
    (void)drive;
    return 0;                           /* one program at a time: no locks */
}

void plat_unlock( int drive )
{
    (void)drive;
}

/* ------------------------------------------------------------------ */
/* diskettes: 21h/F1h AL=14h, FORMAT's calls                           */
/* ------------------------------------------------------------------ */

#define FDF_TYPE        0
#define FDF_SETGEO      1
#define FDF_BEGIN       2
#define FDF_TRACK       3
#define FDF_END         4

int plat_fd_type( int drive )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)drive;
    call21( &regs, F1( 0x14, FDF_TYPE ) );
    return CF( regs ) ? FDT_NONE : (int)(regs.ebx & 0xFF);
}

static u8 medium( int spt, int cyls )
{
    if ( cyls <= 40 ) {
        return 1;                       /* FDM_360 */
    }
    return spt >= 36 ? 5 : spt >= 18 ? 4 : spt >= 15 ? 2 : 3;
}

int plat_fd_begin( int drive, int spt, int cyls, int heads, u16 total )
{
    REGS regs;

    (void)heads;
    (void)total;
    clear( &regs );
    regs.edx = (u32)drive;
    regs.ecx = ((u32)cyls << 8) | (u32)spt;
    regs.ebx = medium( spt, cyls );
    call21( &regs, F1( 0x14, FDF_BEGIN ) );
    if ( CF( regs ) ) {
        return (regs.eax & 0xFF) == 3 ? 3 : 1;
    }
    return 0;
}

int plat_fd_track( int drive, int cyl, int head, int spt )
{
    REGS regs;

    clear( &regs );
    regs.edx = ((u32)head << 8) | (u32)drive;
    regs.ecx = ((u32)cyl << 8) | (u32)spt;
    call21( &regs, F1( 0x14, FDF_TRACK ) );
    if ( CF( regs ) ) {
        return (regs.eax & 0xFF) == 3 ? 3 : 1;
    }
    return 0;
}

void plat_fd_end( int drive, int spt, int heads, u16 total )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)drive;
    call21( &regs, F1( 0x14, FDF_END ) );
    /* what the disk now is, for the sectors that follow */
    clear( &regs );
    regs.edx = ((u32)heads << 8) | (u32)drive;
    regs.ecx = (u32)spt;
    regs.ebx = total;
    call21( &regs, F1( 0x14, FDF_SETGEO ) );
}

/* 21h/F1h AL=1Bh: every cache committed, then the restart */
void plat_restart( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0xF11B );
    plat_exit( 0 );
}
