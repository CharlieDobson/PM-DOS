/*
 * SYS.H - every call SCANDISK makes to the operating system.
 *
 *   SYS_D32.C   PM-DOS, native (the production build, SCANDISK.EXE)
 *   SYS_NT.C    Win32 console, working on a disk image (the host test
 *               build, SCANDKNT.EXE - MKHOST.CMD makes it)
 *
 * The disk half is CHKDSK's, the screen half is EDIT's, PMD's and, for
 * the graphics screen, DOSSHELL's.  The host build has no screen: it
 * is the checks, the repairs, the Undo file and the log, driven line by
 * line (HOSTFE.C).
 *
 * Errors are DOS error codes; 0 is success.  Drives are 0-based.
 */
#ifndef SYS_H
#define SYS_H

#include "types.h"

#define PATH_MAX        260

/* sys_drive_type() results */
#define DRV_INVALID     (-1)
#define DRV_LOCAL       0
#define DRV_REMOTE      1       /* a network drive, a CD-ROM              */
#define DRV_EXFAT       3       /* exFAT, which only PM-DOS's F101h names */
#define DRV_SUBST       2       /* SUBSTed                                */

/* sys_write_sec() kinds (INT 21h/7305h write flags) */
#define WR_UNKNOWN      0
#define WR_FAT          1
#define WR_DIR          2
#define WR_DATA         3

/* the largest single sector transfer SCANDISK asks for */
#define SYS_MAX_XFER    32768u

/* the shift state, 40:17's bits */
#define SH_SHIFT        0x03
#define SH_CTRL         0x04
#define SH_ALT          0x08
#define SH_KEYS         0x0F

typedef struct {
    int  date_fmt;              /* 0 = M D Y, 1 = D M Y, 2 = Y M D */
    char date_sep;
    char time_sep;
    char thou_sep;
    int  time_24;               /* nonzero = 24-hour clock */
} COUNTRY;

/* the program */
int   sys_init( void );                     /* 0 = ok */
const char *sys_cmdline( void );            /* the command tail */
const char *sys_getenv( const char *name );
void  sys_exit( int code );
void *sys_mem_alloc( unsigned int size );
void  sys_break_install( void );            /* ^C raises a flag... */
int   sys_break_take( void );               /* ...which this reads and lowers */
void  sys_stdout( const char *buf, unsigned int len );
int   sys_stdin_is_console( void );
int   sys_stdin_byte( void );               /* -1 at the end */
#ifdef HOSTBUILD
int   sys_read_line( char *buf, int max );  /* echoes; -1 = end of input */
#endif

/* the clock */
uint32_t sys_hundredths( void );            /* the time of day */
void     sys_get_datetime( uint16_t *dos_date, uint16_t *dos_time );
void     sys_get_country( COUNTRY *ctry );

/* drives */
int   sys_get_drive( void );
int   sys_boot_drive( void );
int   sys_drive_type( int drv );
int   sys_drive_fixed( int drv );               /* 1 = a FAT volume that is not a
                                                   diskette: what /ALL checks */
int   sys_drive_floppy( int drv );              /* 1 = there is such a diskette drive */
int   sys_truename_drive( int drv );            /* real drive of "X:\", -1 = unknown */
int   sys_get_cwd( int drv, char *buf, int max );   /* "DIR\SUB", 0 = ok */
int   sys_get_bpb( int drv, uint8_t *sector );  /* a boot sector made from the DPB */
int   sys_lock( int drv, int fat32 );           /* 0 = locked, or no lock needed */
void  sys_unlock( int drv, int fat32 );
int   sys_read_sec( int drv, uint32_t lba, uint32_t count, uint32_t bps, void *buf );
int   sys_write_sec( int drv, uint32_t lba, uint32_t count, uint32_t bps, const void *buf, int kind );
void  sys_reset_drive( int drv );               /* flush and forget */
void  sys_mark_checked( int drv );              /* the volume is sound */

/* files: SCANDISK.INI, the log, the Undo file */
int   sys_open_read( const char *path, uint32_t *handle );
int   sys_open_rw( const char *path, uint32_t *handle );
int   sys_create( const char *path, uint32_t *handle );
int   sys_read( uint32_t handle, void *buf, unsigned int len, unsigned int *done );
int   sys_write( uint32_t handle, const void *buf, unsigned int len, unsigned int *done );
int   sys_close( uint32_t handle );
int   sys_file_size( uint32_t handle, uint32_t *size ); /* and back to the start */
int   sys_seek( uint32_t handle, uint32_t pos );
int   sys_seek_end( uint32_t handle );
int   sys_commit( uint32_t handle );        /* what was written is on the disk */
int   sys_exists( const char *path );

/* ------------------------------------------------------------------ */
/* the screen, the keyboard and the mouse - SYS_D32.C only             */
/* ------------------------------------------------------------------ */
void  sys_screen_size( int *rows, int *cols );
void  sys_put_cells( unsigned int first, const uint16_t *cells, unsigned int count );
void  sys_get_cells( unsigned int first, uint16_t *cells, unsigned int count );
void  sys_cursor( int row, int col );
void  sys_get_cursor( int *row, int *col );
#define CUR_HIDE        0
#define CUR_LINE        1
#define CUR_BLOCK       2
void  sys_cursor_shape( int kind );

/* the keyboard: a key is AL = ASCII, AH = scan code */
#define KW_NOWAIT       0x01    /* sys_key_wait: look, and come back */
#define KW_MOUSE        0x02    /* ...and the mouse wakes it too */
int   sys_key_wait( int shift_seen, int *shift_now, int flags );
                                /* 1 = key waiting, 2 = the mouse moved */
int   sys_key_read( int *shift_now );
int   sys_shift( void );

/* the mouse: INT 33h, positions in the driver's virtual pixels */
int   sys_mouse_reset( void );  /* -> buttons, 0 for no mouse; hides it */
void  sys_mouse_show( int show );
void  sys_mouse_state( int *x, int *y, int *buttons );
int   sys_mouse_count( int release, int button, int *x, int *y );
void  sys_mouse_extent( int *xmax, int *ymax );
void  sys_mouse_range( int xmax, int ymax );    /* 0..xmax, 0..ymax */
void  sys_mouse_move( int x, int y );
void  sys_mouse_inject( int buttons );  /* test mode: buttons as if pressed */

/* ------------------------------------------------------------------ */
/* a graphics screen - SYS_D32.C only                                  */
/* ------------------------------------------------------------------ */

/* the first megabyte: 1 if it was read */
int   sys_peek( uint32_t addr, void *buf, unsigned int len );

/* a BIOS call that only asks */
typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi;
    uint16_t es, ds;
    uint32_t flags;
} BREGS;
int   sys_bios( int vector, BREGS *regs );  /* 1 = it was called */

/* Set a BIOS graphics mode.  -> the selector of its video memory and
   the bytes there; 0 when the mode was set. */
int   sys_video_set( uint32_t mode, uint32_t *selector, uint32_t *bytes );
void  sys_video_restore( void );            /* the text screen from before */

/* DOSINT.ASM: bytes to the video memory, and a port */
void     __cdecl vid_put( uint32_t selector, uint32_t offset, const void *src, uint32_t len );
void     __cdecl port_out( unsigned port, unsigned value );
unsigned __cdecl port_in( unsigned port );

#endif
