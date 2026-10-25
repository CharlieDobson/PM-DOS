/*
 * PLAT16.C - SETUP's 16-bit half: real-mode MS-DOS (and the DOSes that
 * answer like it).  Open Watcom, large model, with its C library for
 * the start-up, the heap and the INT calls; the rest of Setup uses
 * RT.C's functions in both halves alike.
 *
 * This half upgrades the DOS that is running, so it is careful to go
 * through that DOS: files by INT 21h, a volume's sectors by INT 25h and
 * 26h (the DOS 4 packet form, the old one before DOS 4), and a diskette
 * is formatted with DOS's own IOCTLs - 440Dh 0840h to say what the disk
 * is to be, 0842h for each track - so DOS's idea of the disk stays
 * true.  Afterwards the drive's DPB is marked for rebuilding (List of
 * Lists, DPB_FIRST_ACCESS = FFh) so the next access reads the new boot
 * sector.  The BIOS is used for the screen, the keys and, through INT
 * 13h, to ask what a diskette drive is.
 */
#include <dos.h>
#include <i86.h>
#include <bios.h>
#include <stdlib.h>
#include "setup.h"

unsigned plat_caps = 0;

static const char *self_path = "";
static char tail[130];
static u16 far *video;
static int dos_major, dos_minor;

int main( int argc, char **argv )
{
    if ( argc > 0 && argv[0] ) {
        self_path = argv[0];
    }
    plat_init();
    setup_main();
    plat_exit( 0 );
    return 0;
}

/* INT 24h: fail the call, as the 32-bit half does */
static int __far crit( unsigned deverr, unsigned errcode, unsigned __far *devhdr )
{
    (void)deverr;
    (void)errcode;
    (void)devhdr;
    return _HARDERR_FAIL;
}

static void __interrupt __far no_break( void )
{
}

int plat_init( void )
{
    u8 far *psp = (u8 far *)MK_FP( _psp, 0 );
    int len = psp[0x80], i;
    union REGS r;

    if ( len > 126 ) {
        len = 126;
    }
    for ( i = 0; i < len; i++ ) {
        tail[i] = (char)psp[0x81 + i];
    }
    tail[len] = 0;
    _harderr( crit );
    _dos_setvect( 0x23, no_break );

    r.x.ax = 0x3306;                    /* the true version (DOS 5 on) */
    r.x.bx = 0;
    intdos( &r, &r );
    if ( r.h.al != 0xFF && r.h.bl >= 5 ) {
        dos_major = r.h.bl;
        dos_minor = r.h.bh;
    } else {
        r.x.ax = 0x3000;
        intdos( &r, &r );
        dos_major = r.h.al;
        dos_minor = r.h.ah;
    }
    return 0;
}

const char *plat_tail( void )
{
    return tail;
}

const char *plat_self( void )
{
    return self_path;
}

const char *plat_getenv( const char *name )
{
    return getenv( name );
}

void plat_exit( int code )
{
    union REGS r;

    r.x.ax = 0x4C00 | (code & 0xFF);
    intdos( &r, &r );
}

void *plat_alloc( u32 size )
{
    if ( size > 0xFFF0ul ) {
        return NULL;
    }
    return malloc( (unsigned)size );
}

void plat_free( void *ptr )
{
    free( ptr );
}

static void put_out( const char *text, int len )
{
    unsigned done;

    _dos_write( 1, (void far *)text, (unsigned)len, &done );
}

void plat_line( const char *text )
{
    put_out( text, str_len( text ) );
    put_out( "\r\n", 2 );
}

int plat_stdin_byte( void )
{
    u8 ch;
    unsigned done;

    if ( _dos_read( 0, (void far *)&ch, 1, &done ) || done == 0 ) {
        return -1;
    }
    return ch;
}

/* ------------------------------------------------------------------ */
/* the system                                                          */
/* ------------------------------------------------------------------ */

void plat_dosver( int *major, int *minor )
{
    *major = dos_major;
    *minor = dos_minor;
}

/* PM-DOS's real-mode DOS (Windows parked) answers F100h; MS-DOS not */
int plat_pmdos( void )
{
    union REGS r;

    r.x.ax = 0xF100;
    r.x.bx = 0xFFFF;
    intdos( &r, &r );
    if ( r.x.bx == 0xFFFF ) {
        return 0;
    }
    return r.h.bh * 100 + r.h.bl + (r.x.bx == 0 ? 1 : 0);
}

/* Windows 3.x enhanced mode (2Fh/1600h), or standard / real (4680h) */
int plat_windows( void )
{
    union REGS r;

    r.x.ax = 0x1600;
    int86( 0x2F, &r, &r );
    if ( (r.h.al & 0x7F) != 0 ) {
        return 1;
    }
    r.x.ax = 0x4680;
    int86( 0x2F, &r, &r );
    return r.x.ax == 0;
}

/* An 8086 keeps flags bits 12-15 set, a 286 in real mode keeps them
   clear; a 386 lets them be set and cleared */
extern unsigned flags_probe( void );
#pragma aux flags_probe = \
    "pushf"                 \
    "pop ax"                \
    "mov cx,ax"             \
    "or ax,0F000h"          \
    "push ax"               \
    "popf"                  \
    "pushf"                 \
    "pop ax"                \
    "push cx"               \
    "popf"                  \
    value [ax] modify [cx];

int plat_cpu386( void )
{
    unsigned bits = flags_probe() & 0xF000;

    return bits != 0xF000 && bits != 0;
}

int plat_boot_drive( void )
{
    union REGS r;

    if ( dos_major < 4 ) {
        return 2;
    }
    r.x.ax = 0x3305;
    intdos( &r, &r );
    return r.h.dl >= 1 && r.h.dl <= 26 ? r.h.dl - 1 : 2;
}

int plat_country_code( void )
{
    union REGS r;
    struct SREGS s;
    static u8 buf[48];

    segread( &s );
    r.x.ax = 0x3800;
    r.x.dx = FP_OFF( buf );
    s.ds = FP_SEG( buf );
    intdosx( &r, &r, &s );
    return r.x.cflag ? 1 : r.x.bx;
}

void plat_get_date( int *year, int *month, int *day )
{
    union REGS r;

    r.h.ah = 0x2A;
    intdos( &r, &r );
    *year = r.x.cx;
    *month = r.h.dh;
    *day = r.h.dl;
}

void plat_get_time( int *hour, int *minute )
{
    union REGS r;

    r.h.ah = 0x2C;
    intdos( &r, &r );
    *hour = r.h.ch;
    *minute = r.h.cl;
}

int plat_set_date( int year, int month, int day )
{
    union REGS r;

    r.h.ah = 0x2B;
    r.x.cx = (unsigned)year;
    r.h.dh = (u8)month;
    r.h.dl = (u8)day;
    intdos( &r, &r );
    return r.h.al != 0;
}

int plat_set_time( int hour, int minute )
{
    union REGS r;

    r.h.ah = 0x2D;
    r.h.ch = (u8)hour;
    r.h.cl = (u8)minute;
    r.x.dx = 0;
    intdos( &r, &r );
    return r.h.al != 0;
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

void plat_screen_open( void )
{
    union REGS r;

    r.h.ah = 0x0F;
    int86( 0x10, &r, &r );
    if ( r.h.al != 2 && r.h.al != 3 && r.h.al != 7 ) {
        r.x.ax = 0x0003;
        int86( 0x10, &r, &r );
        r.h.al = 3;
    }
    video = (u16 far *)MK_FP( r.h.al == 7 ? 0xB000 : 0xB800, 0 );
}

void plat_screen_put( int pos, const u16 *cells, int count )
{
    u16 far *out = video + pos;

    while ( count-- > 0 ) {
        *out++ = *cells++;
    }
}

void plat_screen_get( int pos, u16 *cells, int count )
{
    u16 far *in = video + pos;

    while ( count-- > 0 ) {
        *cells++ = *in++;
    }
}

void plat_cursor( int row, int col, int shown )
{
    union REGS r;

    if ( shown ) {
        r.h.ah = 0x02;
        r.h.bh = 0;
        r.h.dh = (u8)row;
        r.h.dl = (u8)col;
        int86( 0x10, &r, &r );
        r.h.ah = 0x01;
        r.x.cx = 0x0607;                /* 6.22's underline */
    } else {
        r.h.ah = 0x01;
        r.x.cx = 0x2000;
    }
    int86( 0x10, &r, &r );
}

void plat_screen_close( void )
{
    union REGS r;
    int i;

    for ( i = 0; i < SCR_ROWS * SCR_COLS; i++ ) {
        video[i] = 0x0720;
    }
    r.h.ah = 0x02;
    r.h.bh = 0;
    r.x.dx = 0;
    int86( 0x10, &r, &r );
    r.h.ah = 0x01;
    r.x.cx = 0x0607;
    int86( 0x10, &r, &r );
}

/* ------------------------------------------------------------------ */
/* the keyboard                                                        */
/* ------------------------------------------------------------------ */

int plat_key_ready( void )
{
    return _bios_keybrd( _NKEYBRD_READY ) != 0;
}

int plat_key_read( void )
{
    unsigned key = _bios_keybrd( _NKEYBRD_READ );
    int ascii = key & 0xFF, scan = key >> 8;

    if ( (ascii == 0 || ascii == 0xE0) && scan ) {
        return K_EXT( scan );
    }
    return ascii;
}

/* nothing to do: give the time away under a multitasker (2Fh/1680h) */
void plat_idle( void )
{
    union REGS r;

    r.x.ax = 0x1680;
    int86( 0x2F, &r, &r );
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

int plat_open( const char *path, int write, u16 *handle )
{
    int h;
    unsigned rc = _dos_open( path, write ? 2 : 0, &h );

    *handle = (u16)h;
    return (int)rc;
}

int plat_create( const char *path, u16 *handle )
{
    int h;
    unsigned rc = _dos_creat( path, _A_NORMAL, &h );

    *handle = (u16)h;
    return (int)rc;
}

int plat_read( u16 handle, void *buf, u16 len, u16 *done )
{
    unsigned got = 0, rc = _dos_read( handle, buf, len, &got );

    *done = (u16)got;
    return (int)rc;
}

int plat_write( u16 handle, const void *buf, u16 len, u16 *done )
{
    unsigned put = 0, rc = _dos_write( handle, (void far *)buf, len, &put );

    *done = (u16)put;
    return (int)rc;
}

int plat_close( u16 handle )
{
    return (int)_dos_close( handle );
}

static int seek_to( u16 handle, u32 pos, int how, u32 *at )
{
    union REGS r;

    r.h.ah = 0x42;
    r.h.al = (u8)how;
    r.x.bx = handle;
    r.x.cx = (unsigned)(pos >> 16);
    r.x.dx = (unsigned)pos;
    intdos( &r, &r );
    if ( r.x.cflag ) {
        return r.x.ax;
    }
    if ( at ) {
        *at = ((u32)r.x.dx << 16) | r.x.ax;
    }
    return 0;
}

int plat_seek( u16 handle, u32 pos )
{
    return seek_to( handle, pos, 0, NULL );
}

int plat_size( u16 handle, u32 *size )
{
    int rc = seek_to( handle, 0, 2, size );

    return rc ? rc : seek_to( handle, 0, 0, NULL );
}

int plat_get_ftime( u16 handle, u16 *date, u16 *time )
{
    unsigned d, t, rc = _dos_getftime( handle, &d, &t );

    *date = (u16)d;
    *time = (u16)t;
    return (int)rc;
}

int plat_set_ftime( u16 handle, u16 date, u16 time )
{
    return (int)_dos_setftime( handle, date, time );
}

static int path_call( u8 func, const char *path, const char *other, u16 cx )
{
    union REGS r;
    struct SREGS s;

    segread( &s );
    r.h.ah = func;
    r.h.al = 0;
    r.x.cx = cx;
    r.x.dx = FP_OFF( path );
    s.ds = FP_SEG( path );
    if ( other ) {
        r.x.di = FP_OFF( other );
        s.es = FP_SEG( other );
    }
    intdosx( &r, &r, &s );
    return r.x.cflag ? (int)r.x.ax : 0;
}

int plat_delete( const char *path )
{
    return path_call( 0x41, path, NULL, 0 );
}

int plat_rename( const char *from, const char *to )
{
    return path_call( 0x56, from, to, 0 );
}

int plat_mkdir( const char *path )
{
    return path_call( 0x39, path, NULL, 0 );
}

int plat_rmdir( const char *path )
{
    return path_call( 0x3A, path, NULL, 0 );
}

int plat_get_attr( const char *path, u16 *attr )
{
    unsigned a = 0, rc = _dos_getfileattr( path, &a );

    *attr = (u16)a;
    return (int)rc;
}

int plat_set_attr( const char *path, u16 attr )
{
    return (int)_dos_setfileattr( path, attr );
}

/* DOS's search record lives in FINDDATA's reserved bytes: the compile
   fails here if it would not fit */
typedef char find_t_fits[sizeof( struct find_t ) <= sizeof( ((FINDDATA *)0)->reserved ) ? 1 : -1];

static void find_fill( FINDDATA *fd, struct find_t *ft )
{
    fd->attr = ft->attrib;
    fd->time = ft->wr_time;
    fd->date = ft->wr_date;
    fd->size = ft->size;
    str_cpyn( fd->name, ft->name, sizeof( fd->name ) );
}

int plat_find_first( const char *spec, u16 attr, FINDDATA *fd )
{
    struct find_t *ft = (struct find_t *)fd->reserved;
    unsigned rc = _dos_findfirst( spec, attr, ft );

    if ( rc == 0 ) {
        find_fill( fd, ft );
    }
    return (int)rc;
}

int plat_find_next( FINDDATA *fd )
{
    struct find_t *ft = (struct find_t *)fd->reserved;
    unsigned rc = _dos_findnext( ft );

    if ( rc == 0 ) {
        find_fill( fd, ft );
    }
    return (int)rc;
}

int plat_disk_free( int drive, u32 *free_kb, u32 *cluster_bytes )
{
    struct diskfree_t df;

    if ( _dos_getdiskfree( drive + 1, &df ) ) {
        return 15;
    }
    *cluster_bytes = (u32)df.sectors_per_cluster * df.bytes_per_sector;
    *free_kb = *cluster_bytes >= 1024 ? (u32)df.avail_clusters * (*cluster_bytes / 1024)
                                 : (u32)df.avail_clusters / 2;
    return 0;
}

int plat_current_dir( char *buf, int max )
{
    union REGS r;
    struct SREGS s;
    static char dir[70];
    int drive;

    r.h.ah = 0x19;
    intdos( &r, &r );
    drive = r.h.al;
    segread( &s );
    r.h.ah = 0x47;
    r.h.dl = 0;
    r.x.si = FP_OFF( dir );
    s.ds = FP_SEG( dir );
    intdosx( &r, &r, &s );
    if ( r.x.cflag ) {
        return 1;
    }
    sfmt( buf, max, "%c:\\%s", 'A' + drive, dir );
    if ( dir[0] ) {
        str_catn( buf, "\\", max );
    }
    return 0;
}

void plat_flush( void )
{
    union REGS r;

    r.h.ah = 0x0D;
    intdos( &r, &r );
}

/* ------------------------------------------------------------------ */
/* drives and disks                                                    */
/* ------------------------------------------------------------------ */

/* MSCDEX: 2Fh/150Bh says whether a letter is a CD-ROM */
static int is_cdrom( int drive )
{
    union REGS r;

    r.x.ax = 0x150B;
    r.x.bx = 0;
    r.x.cx = (unsigned)drive;
    int86( 0x2F, &r, &r );
    return r.x.bx == 0xADAD && r.x.ax != 0;
}

int plat_drive( int drive, DRIVEINFO *di )
{
    union REGS r;

    mem_set( di, 0, sizeof( *di ) );
    di->bios = -1;
    r.x.ax = 0x4409;                    /* remote? and is there a drive */
    r.h.bl = (u8)(drive + 1);
    intdos( &r, &r );
    if ( r.x.cflag ) {
        return 1;
    }
    di->flags = DI_PRESENT;
    if ( is_cdrom( drive ) ) {
        di->flags |= DI_CDROM;
        return 0;
    }
    if ( r.x.dx & 0x1000 ) {
        di->flags |= DI_REMOTE;
        return 0;
    }
    r.x.ax = 0x4408;                    /* removable? */
    r.h.bl = (u8)(drive + 1);
    intdos( &r, &r );
    if ( !r.x.cflag && r.x.ax == 0 ) {
        if ( drive < 2 ) {
            di->flags |= DI_FLOPPY;
            di->bios = drive;
        } else {
            di->flags |= DI_REMOVABLE;
        }
    } else {
        di->flags |= DI_FIXED;
    }
    return 0;
}

/* the BIOS's geometry of a disk (INT 13h AH=08h) */
static int geometry( int bios, u16 *spt, u16 *heads, u16 *cyls )
{
    union REGS r;
    struct SREGS s;

    segread( &s );
    r.h.ah = 0x08;
    r.h.dl = (u8)bios;
    r.x.di = 0;
    s.es = 0;
    int86x( 0x13, &r, &r, &s );
    if ( r.x.cflag || (r.h.cl & 0x3F) == 0 ) {
        return 1;
    }
    *spt = r.h.cl & 0x3F;
    *heads = (u16)r.h.dh + 1;
    *cyls = (u16)(((r.h.cl & 0xC0) << 2) | r.h.ch) + 1;
    return 0;
}

int plat_disk( int bios, DISKINFO *dk )
{
    u16 cyls;

    mem_set( dk, 0, sizeof( *dk ) );
    if ( geometry( bios, &dk->spt, &dk->heads, &cyls ) ) {
        return 1;
    }
    dk->present = 1;
    dk->total = (u32)cyls * dk->heads * dk->spt;
    return 0;
}

static int bios_io( u8 func, int bios, u32 lba, u16 count, void far *buf )
{
    union REGS r;
    struct SREGS s;
    u16 spt, heads, cyls, cyl, tries;
    u8 far *at = (u8 far *)buf;

    if ( geometry( bios, &spt, &heads, &cyls ) ) {
        return 21;
    }
    while ( count-- ) {
        cyl = (u16)(lba / ((u32)spt * heads));
        for ( tries = 0; tries < 3; tries++ ) {
            segread( &s );
            r.h.ah = func;
            r.h.al = 1;
            r.h.ch = (u8)cyl;
            r.h.cl = (u8)((lba % spt) + 1) | (u8)((cyl >> 2) & 0xC0);
            r.h.dh = (u8)((lba / spt) % heads);
            r.h.dl = (u8)bios;
            r.x.bx = FP_OFF( at );
            s.es = FP_SEG( at );
            int86x( 0x13, &r, &r, &s );
            if ( !r.x.cflag ) {
                break;
            }
            r.h.ah = 0;
            r.h.dl = (u8)bios;
            int86( 0x13, &r, &r );
        }
        if ( tries == 3 ) {
            return 21;
        }
        lba++;
        at += 512;
    }
    return 0;
}

int plat_disk_read( int bios, u32 lba, u16 count, void *buf )
{
    return bios_io( 0x02, bios, lba, count, buf );
}

int plat_disk_write( int bios, u32 lba, u16 count, const void *buf )
{
    return bios_io( 0x03, bios, lba, count, (void far *)buf );
}

/* INT 25h and 26h leave the flags on the stack; nonzero = an error,
   AH the BIOS's status (03h: write-protected) */
extern unsigned abs_rd( u8 drive, void far *packet );
#pragma aux abs_rd =        \
    "push bp"               \
    "push ds"               \
    "mov ds,dx"             \
    "mov cx,0FFFFh"         \
    "int 25h"               \
    "pop cx"                \
    "sbb cx,cx"             \
    "and ax,cx"             \
    "neg cx"                \
    "add ax,cx"             \
    "pop ds"                \
    "pop bp"                \
    parm [al] [dx bx] value [ax] modify [bx cx dx si di es];

extern unsigned abs_wr( u8 drive, void far *packet );
#pragma aux abs_wr =        \
    "push bp"               \
    "push ds"               \
    "mov ds,dx"             \
    "mov cx,0FFFFh"         \
    "int 26h"               \
    "pop cx"                \
    "sbb cx,cx"             \
    "and ax,cx"             \
    "neg cx"                \
    "add ax,cx"             \
    "pop ds"                \
    "pop bp"                \
    parm [al] [dx bx] value [ax] modify [bx cx dx si di es];

/* before DOS 4: sector in DX, count in CX, DS:BX the buffer */
extern unsigned old_rd( u8 drive, unsigned count, unsigned sector, void far *buf );
#pragma aux old_rd =        \
    "push bp"               \
    "push ds"               \
    "mov ds,si"             \
    "int 25h"               \
    "pop cx"                \
    "sbb cx,cx"             \
    "and ax,cx"             \
    "neg cx"                \
    "add ax,cx"             \
    "pop ds"                \
    "pop bp"                \
    parm [al] [cx] [dx] [si bx] value [ax] modify [bx cx dx si di es];

extern unsigned old_wr( u8 drive, unsigned count, unsigned sector, void far *buf );
#pragma aux old_wr =        \
    "push bp"               \
    "push ds"               \
    "mov ds,si"             \
    "int 26h"               \
    "pop cx"                \
    "sbb cx,cx"             \
    "and ax,cx"             \
    "neg cx"                \
    "add ax,cx"             \
    "pop ds"                \
    "pop bp"                \
    parm [al] [cx] [dx] [si bx] value [ax] modify [bx cx dx si di es];

static int vol_io( int drive, u32 sector, u16 count, void far *buf, int write )
{
    static u8 packet[10];
    unsigned rc;

    if ( dos_major < 4 ) {
        rc = write ? old_wr( (u8)drive, count, (unsigned)sector, buf )
                   : old_rd( (u8)drive, count, (unsigned)sector, buf );
    } else {
        WR32( packet, sector );
        WR16( packet + 4, count );
        WR16( packet + 6, FP_OFF( buf ) );
        WR16( packet + 8, FP_SEG( buf ) );
        rc = write ? abs_wr( (u8)drive, (void far *)packet )
                   : abs_rd( (u8)drive, (void far *)packet );
    }
    if ( rc == 0 ) {
        return 0;
    }
    return (rc >> 8) == 0x03 ? 19 : 31;
}

int plat_vol_read( int drive, u32 sector, u16 count, void *buf )
{
    return vol_io( drive, sector, count, buf, 0 );
}

int plat_vol_write( int drive, u32 sector, u16 count, const void *buf )
{
    return vol_io( drive, sector, count, (void far *)buf, 1 );
}

/* The drive's DPB marked "not yet accessed", found through the List of
   Lists rather than by 32h, which would read the disk to answer */
static void dpb_forget( int drive )
{
    union REGS r;
    struct SREGS s;
    u8 far *dpb;
    int steps;

    if ( dos_major < 4 ) {
        return;
    }
    segread( &s );
    r.h.ah = 0x52;
    intdosx( &r, &r, &s );
    dpb = *(u8 far * far *)MK_FP( s.es, r.x.bx );
    for ( steps = 0; steps < 32 && FP_OFF( dpb ) != 0xFFFF; steps++ ) {
        if ( dpb[0] == drive ) {
            dpb[0x18] = 0xFF;           /* DPB_FIRST_ACCESS */
            return;
        }
        dpb = *(u8 far * far *)(dpb + 0x19);
    }
}

int plat_vol_remount( int drive )
{
    plat_flush();
    dpb_forget( drive );
    return 0;
}

/* no other program is running under DOS 5 or 6: nothing to lock */
int plat_lock( int drive )
{
    (void)drive;
    return 0;
}

void plat_unlock( int drive )
{
    (void)drive;
}

/* ------------------------------------------------------------------ */
/* diskettes: DOS's generic IOCTL, as FORMAT does it                   */
/* ------------------------------------------------------------------ */

int plat_fd_type( int drive )
{
    union REGS r;
    struct SREGS s;

    segread( &s );
    r.h.ah = 0x08;
    r.h.dl = (u8)drive;
    r.x.di = 0;
    s.es = 0;
    int86x( 0x13, &r, &r, &s );
    if ( r.x.cflag ) {
        return FDT_NONE;
    }
    return r.h.bl == 6 ? FDT_2880 : r.h.bl;
}

static u8 params[0x26 + 2 + 36 * 4];    /* the device parameters      */
static u8 saved[sizeof( params )];
static int have_saved;

static int ioctl( int drive, unsigned cx, void far *block )
{
    union REGS r;
    struct SREGS s;

    segread( &s );
    r.x.ax = 0x440D;
    r.h.bl = (u8)(drive + 1);
    r.x.cx = cx;
    r.x.dx = FP_OFF( block );
    s.ds = FP_SEG( block );
    intdosx( &r, &r, &s );
    return r.x.cflag ? (int)r.x.ax : 0;
}

int plat_fd_begin( int drive, int spt, int cyls, int heads, u16 total )
{
    int i, rootents = spt >= 15 ? 224 : 112;
    u8 *bpb = params + 7;

    mem_set( saved, 0, sizeof( saved ) );
    have_saved = ioctl( drive, 0x0860, (void far *)saved ) == 0;
    mem_set( params, 0, sizeof( params ) );
    params[0] = 5;                      /* the BPB for the disk in it, and */
                                        /* every sector the same size      */
    if ( have_saved ) {
        params[1] = saved[1];           /* the device type and attributes */
        params[2] = saved[2];
        params[3] = saved[3];
    }
    WR16( params + 4, cyls );
    WR16( bpb + 0, 512 );
    bpb[2] = (u8)(spt >= 15 ? 1 : 2);
    WR16( bpb + 3, 1 );
    bpb[5] = 2;
    WR16( bpb + 6, rootents );
    WR16( bpb + 8, total );
    bpb[10] = (u8)(spt == 9 ? 0xF9 : spt == 15 ? 0xF9 : 0xF0);
    WR16( bpb + 11, spt == 9 ? 3 : spt == 15 ? 7 : 9 );
    WR16( bpb + 13, spt );
    WR16( bpb + 15, heads );
    WR16( params + 0x26, spt );
    for ( i = 0; i < spt; i++ ) {
        WR16( params + 0x28 + i * 4, i + 1 );
        WR16( params + 0x2A + i * 4, 512 );
    }
    return ioctl( drive, 0x0840, (void far *)params ) ? 1 : 0;
}

int plat_fd_track( int drive, int cyl, int head, int spt )
{
    static u8 block[7];
    int rc;

    (void)spt;
    block[0] = 0;
    WR16( block + 1, head );
    WR16( block + 3, cyl );
    WR16( block + 5, 1 );
    rc = ioctl( drive, 0x0842, (void far *)block );
    if ( rc == 0 ) {
        return 0;
    }
    return rc == 0x13 ? 3 : 1;          /* 13h: write-protected */
}

void plat_fd_end( int drive, int spt, int heads, u16 total )
{
    (void)spt;
    (void)heads;
    (void)total;
    if ( have_saved ) {
        saved[0] = 4;                   /* the default back as it was */
        ioctl( drive, 0x0840, (void far *)saved );
    }
    dpb_forget( drive );
}

/* the BIOS's warm start: 1234h at 40:72 skips the memory test */
void plat_restart( void )
{
    union REGS r;
    void (far *reset)( void ) = (void (far *)( void ))MK_FP( 0xFFFF, 0 );

    r.h.ah = 0x0D;
    intdos( &r, &r );
    *(u16 far *)MK_FP( 0x40, 0x72 ) = 0x1234;
    reset();
}
