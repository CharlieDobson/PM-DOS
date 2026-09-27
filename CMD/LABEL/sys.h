/*
 * SYS.H - host interface for LABEL/32.
 *
 * Every call LABEL makes to the operating system goes through the
 * functions below.  Three implementations exist:
 *
 *   SYS_D32.C   PM-DOS, native (the production build, LABEL.EXE)
 *   SYS_DPMI.C  DOS/32A extender on real-mode DOS (test build, LABEL32A.EXE)
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

/*
 * Disk errors.  sys_read_sec() and sys_write_sec() return 0 or one of
 * these DOS extended error codes, which are the INT 24h critical error
 * codes plus 19.  MSG.C holds their text.
 */
#define DE_FIRST    19
#define DE_WRPROT   19          /* Write protect error */
#define DE_NOTREADY 21          /* Not ready           */
#define DE_DATA     23          /* Data error          */
#define DE_MEDIA    26          /* Invalid media type  */
#define DE_WRFAULT  29          /* Write fault error   */
#define DE_GENERAL  31          /* General failure     */
#define DE_LAST     31

/* largest single sector transfer LABEL ever requests */
#define SYS_MAX_XFER 32768u

int   sys_init( void );                     /* 0 = ok */
const char *sys_cmdline( void );            /* command tail, NUL-terminated */
void  sys_write( int handle, const char *buf, u32 len );
int   sys_read_line( char *buf, int max );  /* echoes; -1 = end of input */
int   sys_read_key( void );                 /* one key, echoed; -1 = end */
void  sys_exit( int code );

int   sys_get_drive( void );
int   sys_drive_type( int drv );
int   sys_truename_drive( int drv );        /* real drive of "X:\", -1 = unknown */
int   sys_get_bpb( int drv, u8 *sector );   /* synthetic boot sector, 0 = ok */

int   sys_lock( int drv, int fat32 );       /* 0 = locked or locking not needed */
void  sys_unlock( int drv, int fat32 );
int   sys_read_sec( int drv, u32 lba, u32 count, u32 bps, void *buf );
int   sys_write_sec( int drv, u32 lba, u32 count, u32 bps, const void *buf,
                     int kind );
void  sys_reset_drive( int drv );           /* flush and invalidate buffers */

void *sys_mem_alloc( u32 size );
void  sys_get_datetime( u16 *dos_date, u16 *dos_time );
int   sys_upcase( int ch );                 /* country upper case, ch >= 80h */

#endif
