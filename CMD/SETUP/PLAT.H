/*
 * PLAT.H - what SETUP asks of the operating system underneath it.
 *
 *   PLAT16.C   real-mode MS-DOS (and anything that answers like it):
 *              INT 21h, INT 25h/26h, the BIOS, the screen at B800h
 *   PLAT32.C   PM-DOS native: INT 21h widened to 32 bits, 21h/F0h for
 *              the screen and keys, 21h/F1h for disks and the restart
 *
 * Errors are DOS error codes, 0 is success.  Drives are 0-based
 * (0 = A:).  A length is u16: nothing moves more than COPYBUF at once,
 * so the 16-bit half needs no huge pointers.
 */
#ifndef PLAT_H
#define PLAT_H

#include "types.h"

/* what this half may do (plat_caps) */
#define PC_32BIT        0x0001  /* the PM-DOS half                       */
#define PC_PARTITION    0x0002  /* may write a partition table           */
#define PC_RESTART      0x0004  /* may restart the machine itself        */
extern unsigned plat_caps;

/* file attributes */
#define FA_RDONLY       0x01
#define FA_HIDDEN       0x02
#define FA_SYSTEM       0x04
#define FA_LABEL        0x08
#define FA_DIR          0x10
#define FA_ARCH         0x20

/* DOS error codes SETUP looks at */
#define E_NOFILE        2
#define E_NOPATH        3
#define E_ACCESS        5
#define E_EXISTS        80

typedef struct {
    u8   attr;
    u16  time, date;
    u32  size;
    char name[13];
    u8   reserved[48];          /* the platform's own search state       */
} FINDDATA;

/* a drive letter (plat_drive) */
#define DI_PRESENT      0x01
#define DI_FLOPPY       0x02    /* a diskette drive                      */
#define DI_CDROM        0x04
#define DI_REMOTE       0x08    /* a network drive (or anything not ours) */
#define DI_REMOVABLE    0x10    /* a USB disk, a Zip                     */
#define DI_FIXED        0x20    /* a hard disk partition                 */

typedef struct {
    unsigned flags;             /* DI_*                                  */
    int  bios;                  /* its BIOS drive, -1 = unknown          */
    u8   ptype;                 /* the partition type byte, 0 = unknown  */
    u32  start, secs;           /* the partition, when it is one         */
} DRIVEINFO;

/* a physical disk (plat_disk) */
typedef struct {
    int  present;
    int  removable;             /* a USB disk                            */
    u16  spt, heads;
    u32  total;                 /* its size in sectors                   */
} DISKINFO;

/* the program */
int   plat_init( void );                /* 0 = ok                        */
const char *plat_tail( void );          /* the command tail              */
const char *plat_self( void );          /* SETUP.EXE's own path, "" when
                                           the system does not say       */
const char *plat_getenv( const char *name );
void  plat_exit( int code );
void *plat_alloc( u32 size );           /* NULL = no memory              */
void  plat_free( void *ptr );
void  plat_line( const char *text );    /* to standard output            */
int   plat_stdin_byte( void );          /* the test mode's keys; -1 = end */

/* the DOS underneath */
void  plat_dosver( int *major, int *minor );
int   plat_pmdos( void );               /* PM-DOS's version * 100, or 0  */
int   plat_windows( void );             /* nonzero under Windows         */
int   plat_cpu386( void );
int   plat_boot_drive( void );
int   plat_country_code( void );

/* the clock */
void  plat_get_date( int *year, int *month, int *day );
void  plat_get_time( int *hour, int *minute );
int   plat_set_date( int year, int month, int day );
int   plat_set_time( int hour, int minute );

/* the screen: cells are character | attribute << 8 */
void  plat_screen_open( void );
void  plat_screen_put( int pos, const u16 *cells, int count );
void  plat_screen_get( int pos, u16 *cells, int count );
void  plat_cursor( int row, int col, int shown );
void  plat_screen_close( void );

/* the keyboard: ASCII, or 100h + scan code */
int   plat_key_ready( void );
int   plat_key_read( void );

void  plat_idle( void );                /* nothing to do just now        */

/* files */
int   plat_open( const char *path, int write, u16 *handle );
int   plat_create( const char *path, u16 *handle );
int   plat_read( u16 handle, void *buf, u16 len, u16 *done );
int   plat_write( u16 handle, const void *buf, u16 len, u16 *done );
int   plat_close( u16 handle );
int   plat_seek( u16 handle, u32 pos );
int   plat_size( u16 handle, u32 *size );
int   plat_get_ftime( u16 handle, u16 *date, u16 *time );
int   plat_set_ftime( u16 handle, u16 date, u16 time );
int   plat_delete( const char *path );
int   plat_rename( const char *from, const char *to );
int   plat_get_attr( const char *path, u16 *attr );
int   plat_set_attr( const char *path, u16 attr );
int   plat_mkdir( const char *path );
int   plat_find_first( const char *spec, u16 attr, FINDDATA *fd );
int   plat_find_next( FINDDATA *fd );
int   plat_disk_free( int drive, u32 *free_kb, u32 *cluster_bytes );  /* KB: no 4GB ceiling */
int   plat_current_dir( char *buf, int max );   /* "C:\SUB\", 0 = ok   */
int   plat_rmdir( const char *path );
void  plat_flush( void );               /* everything onto the disks     */

/* drives and disks */
int   plat_drive( int drive, DRIVEINFO *di );   /* 0 = there is one      */
int   plat_disk( int bios, DISKINFO *dk );      /* 0 = there is one      */
int   plat_disk_read( int bios, u32 lba, u16 count, void *buf );
int   plat_disk_write( int bios, u32 lba, u16 count, const void *buf );
int   plat_vol_read( int drive, u32 sector, u16 count, void *buf );
int   plat_vol_write( int drive, u32 sector, u16 count, const void *buf );
int   plat_vol_remount( int drive );    /* what is on it was rewritten   */
int   plat_lock( int drive );           /* DOS 7's lock; 0 = held or none */
void  plat_unlock( int drive );

/* diskettes: a track at a time (FLOPPY.C) */
int   plat_fd_type( int drive );        /* FDT_ as CMOS calls it, 0 none */
/* begin, a track, end: 0 = done, 3 = write-protected, else failed.  The
   end tells the system what the disk now is (and must follow a begin) */
int   plat_fd_begin( int drive, int spt, int cyls, int heads, u16 total );
int   plat_fd_track( int drive, int cyl, int head, int spt );
void  plat_fd_end( int drive, int spt, int heads, u16 total );
#define FDT_NONE        0
#define FDT_360         1
#define FDT_1200        2
#define FDT_720         3
#define FDT_1440        4
#define FDT_2880        5

void  plat_restart( void );

#endif
