/*
 * SYS.H - host interface for CHKDSK/32.
 *
 * Every call CHKDSK makes to the operating system goes through the
 * functions below.  Three implementations exist:
 *
 *   SYS_D32.C   PM-DOS, native (the production build, CHKDSK.EXE)
 *   SYS_DPMI.C  DOS/32A extender on real-mode DOS (test build, CHKD32A.EXE)
 *   SYS_NT.C    Win32 console, operating on a disk image (host test build)
 *
 * Drive numbers are 0-based (0 = A:) throughout.
 */
#ifndef SYS_H
#define SYS_H

#include "types.h"

#define H_OUT   1               /* standard output */
#define H_ERR   2               /* standard error  */

/* sys_drive_type() results */
#define DRV_INVALID  (-1)
#define DRV_LOCAL      0
#define DRV_REMOTE     1        /* network redirector, CD-ROM (MSCDEX) */
#define DRV_SUBST      2        /* SUBSTed drive                        */

/* sys_write_sec() kinds (INT 21h/7305h write flags) */
#define WR_UNKNOWN   0
#define WR_FAT       1
#define WR_DIR       2
#define WR_DATA      3

/* largest single sector transfer CHKDSK ever requests */
#define SYS_MAX_XFER 32768u

typedef struct {
    int  date_fmt;              /* 0 = M D Y, 1 = D M Y, 2 = Y M D */
    char date_sep;
    char time_sep;
    char thou_sep;
    int  time_24;               /* nonzero = 24-hour clock */
} COUNTRY;

int   sys_init( void );                     /* 0 = ok */
const char *sys_cmdline( void );            /* command tail, NUL-terminated */
void  sys_write( int handle, const char *buf, u32 len );
int   sys_read_line( char *buf, int max );  /* echoes; -1 = end of input */
void  sys_exit( int code );

int   sys_get_drive( void );
int   sys_drive_type( int drv );
int   sys_truename_drive( int drv );        /* real drive of "X:\", -1 = unknown */
int   sys_get_cwd( int drv, char *buf, int max ); /* "DIR\SUB", 0 = ok */
int   sys_get_bpb( int drv, u8 *sector );   /* synthetic boot sector, 0 = ok */

int   sys_lock( int drv, int fat32 );       /* 0 = locked or locking not needed */
void  sys_unlock( int drv, int fat32 );
int   sys_read_sec( int drv, u32 lba, u32 count, u32 bps, void *buf );
int   sys_write_sec( int drv, u32 lba, u32 count, u32 bps, const void *buf,
                     int kind );
void  sys_reset_drive( int drv );           /* flush and invalidate buffers */
void  sys_mark_checked( int drv );          /* the volume is sound: PM-DOS
                                               may set its clean bit */

void *sys_mem_alloc( u32 size );
void  sys_mem_info( u32 *total, u32 *avail );
void  sys_get_datetime( u16 *dos_date, u16 *dos_time );
void  sys_get_country( COUNTRY *ctry );

#endif
