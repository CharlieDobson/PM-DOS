/*
 * SYS.H - every call DEFRAG makes to the operating system.
 *
 *   SYS_D32.C   PM-DOS, native (the production build, DEFRAG.EXE)
 *   SYS_NT.C    Win32 console, working on a disk image (the host test
 *               build, DEFRAGNT.EXE - MKHOST.CMD makes it)
 *
 * SCANDISK's, with what a program that rearranges a drive needs more:
 * the current directory moved out of the way, another program run, the
 * machine restarted.  The host build has no screen: it is the reading
 * of a drive and the moving of its clusters, said in lines (HOSTFE.C).
 *
 * Errors are DOS error codes; 0 is success.  Drives are 0-based.
 */
#ifndef SYS_H
#define SYS_H

#include "types.h"

#define PATH_MAX        260

/* sys_drive_kind() results */
#define DK_NONE         0       /* no such drive, or not one to optimize:
                                   a CD-ROM, a network drive, SUBST */
#define DK_FLOPPY       1
#define DK_FIXED        2       /* a hard disk, or a file mounted as one */

/* sys_write_sec() kinds (INT 21h/7305h write flags) */
#define WR_UNKNOWN      0
#define WR_FAT          1
#define WR_DIR          2
#define WR_DATA         3

/* the largest single sector transfer DEFRAG asks for */
#define SYS_MAX_XFER    32768u

/* the shift state, 40:17's bits */
#define SH_SHIFT        0x03
#define SH_CTRL         0x04
#define SH_ALT          0x08
#define SH_KEYS         0x0F

/* sys_vol_flags(): 21h/F1h AL=1Dh's bits */
#define VLS_KEPT        0x01    /* the volume has a clean-shutdown flag */
#define VLS_DIRTY       0x02    /* ...and it is off */

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
void *sys_mem_alloc( u32 size );
void  sys_break_install( void );            /* ^C raises a flag... */
int   sys_break_take( void );               /* ...which this reads and lowers */
void  sys_stdout( const char *buf, u32 len );
int   sys_stdin_is_console( void );
int   sys_stdin_byte( void );               /* -1 at the end */
int   sys_handle_is_file( int handle );     /* 1 = open, and not a device */
/* another program, and its exit code; -1 and a DOS error when it could
   not be started */
int   sys_exec( const char *prog, const char *tail, int *error );
void  sys_restart( void );                  /* comes back only if it could not */

/* the clock */
u32   sys_hundredths( void );               /* the time of day */
void  sys_get_country( COUNTRY *ctry );

/* drives */
int   sys_get_drive( void );
int   sys_boot_drive( void );
int   sys_drive_kind( int drv );            /* DK_* */
int   sys_drive_exfat( int drv );           /* 1 = exFAT, so DK_NONE */
int   sys_get_cwd( int drv, char *buf, int max );   /* "DIR\SUB", 0 = ok */
int   sys_chdir( int drv, const char *dir );        /* "" = the root */
int   sys_get_bpb( int drv, u8 *sector );   /* a boot sector made from the DPB */
int   sys_lock( int drv, int fat32 );       /* 0 = locked, or no lock needed */
void  sys_unlock( int drv, int fat32 );
int   sys_vol_flags( int drv );             /* VLS_* */
int   sys_read_sec( int drv, u32 lba, u32 count, u32 bps, void *buf );
int   sys_write_sec( int drv, u32 lba, u32 count, u32 bps, const void *buf, int kind );
void  sys_reset_drive( int drv );           /* flush and forget */

/* files: the test log, a picture of the screen */
int   sys_create( const char *path, u32 *handle );
int   sys_write( u32 handle, const void *buf, u32 len, u32 *done );
int   sys_close( u32 handle );
int   sys_exists( const char *path );

/* ------------------------------------------------------------------ */
/* the screen, the keyboard and the mouse - SYS_D32.C only             */
/* ------------------------------------------------------------------ */
void  sys_screen_size( int *rows, int *cols );
void  sys_put_cells( u32 first, const u16 *cells, u32 count );
void  sys_get_cells( u32 first, u16 *cells, u32 count );
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
int   sys_peek( u32 addr, void *buf, u32 len );

/* a BIOS call that only asks */
typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u16 es, ds;
    u32 flags;
} BREGS;
int   sys_bios( int vector, BREGS *regs );  /* 1 = it was called */

/* Set a BIOS graphics mode.  -> the selector of its video memory and
   the bytes there; 0 when the mode was set. */
int   sys_video_set( u32 mode, u32 *selector, u32 *bytes );
void  sys_video_restore( void );            /* the text screen from before */

/* DOSINT.ASM: bytes to the video memory, and a port */
void     __cdecl vid_put( u32 selector, u32 offset, const void *src, u32 len );
void     __cdecl port_out( unsigned port, unsigned value );
unsigned __cdecl port_in( unsigned port );

#endif
