/*
 * SYS.H - every call DOSSHELL makes to the operating system.
 *
 * SYS_D32.C is the one implementation: PM-DOS, native.  The first part
 * is EDIT's - the text screen, the keyboard, the mouse and files,
 * through 21h/F0h, INT 33h and the long-name calls with the classic
 * ones behind them.  The rest is what a shell needs: running a
 * program, the drives there are, and a graphics screen - a mode set by
 * the kernel (21h/F1h AL=1Fh), which hands back a selector for the
 * frame buffer, and the ROM's fonts read out of the first megabyte
 * (21h/F1h AL=20h), because a program at ring 3 has no BIOS to ask.
 *
 * Errors are DOS error codes; 0 is success.  Drives are 0-based.
 */
#ifndef SYS_H
#define SYS_H

#include "types.h"

#define ATTR_READONLY   0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME     0x08
#define ATTR_DIR        0x10
#define ATTR_ARCHIVE    0x20

#define ERR_NOFILE      2
#define ERR_NOPATH      3
#define ERR_NOHANDLES   4
#define ERR_ACCESS      5
#define ERR_NOMEM       8
#define ERR_BADDRIVE    15
#define ERR_NOMORE      18
#define ERR_WRPROTECT   19
#define ERR_NOTREADY    21
#define ERR_EXISTS      80
#define ERR_DISKFULL    112

#define PATH_MAX        260

/* the shift state, 40:17's bits */
#define SH_RSHIFT       0x01
#define SH_LSHIFT       0x02
#define SH_SHIFT        0x03
#define SH_CTRL         0x04
#define SH_ALT          0x08
#define SH_KEYS         0x0F

typedef struct {
    u8   attr;
    u32  size;
    u16  time, date;            /* DOS's packed forms */
    char name[PATH_MAX];        /* the long name - the 8.3 when there is none */
    char alias[14];
} FINDREC;

int   sys_init( void );
const char *sys_cmdline( void );
const char *sys_getenv( const char *name );
const char *sys_program_dir( void );        /* where DOSSHELL.EXE is: "C:\DIR\" */
void  sys_exit( int code );
void *sys_mem_alloc( u32 size );
void  sys_stdout( const char *buf, u32 len );
int   sys_stdin_is_console( void );
int   sys_stdin_byte( void );               /* -1 at the end */
int   sys_crit_seen( void );                /* a critical error was failed since last asked */

/* the text screen */
void  sys_cls( void );
void  sys_screen_size( int *rows, int *cols );
void  sys_put_cells( u32 first, const u16 *cells, u32 count );
void  sys_cursor( int row, int col );
#define CUR_HIDE        0
#define CUR_LINE        1
#define CUR_BLOCK       2
void  sys_cursor_shape( int kind );
int   sys_set_lines( int lines );               /* -> rows now */

/* the keyboard: a key is AL = ASCII, AH = scan code */
#define KW_NOWAIT       0x01    /* sys_key_wait: look, and come back */
#define KW_MOUSE        0x02    /* ...and the mouse wakes it too */
int   sys_key_wait( int shift_seen, int *shift_now, int flags );
                                /* 1 = key waiting, 2 = the mouse moved */
int   sys_key_read( int *shift_now );
int   sys_shift( void );
u32   sys_hundredths( void );   /* the time of day, for double clicks */

typedef struct {
    int year, month, day, hour, minute;
} DATETIME;
void  sys_now( DATETIME *now );

/* how the country writes a date and a time (21h/38h) */
typedef struct {
    int  date_order;            /* 0 month-day-year, 1 day-month-year, 2 year-month-day */
    char date_sep, time_sep;
    int  clock24;
} COUNTRY;
extern COUNTRY country;

/* the mouse: INT 33h, positions in the driver's virtual pixels */
int   sys_mouse_reset( void );  /* -> buttons, 0 for no mouse; hides it */
void  sys_mouse_show( int show );
void  sys_mouse_state( int *x, int *y, int *buttons );
int   sys_mouse_count( int release, int button, int *x, int *y );
void  sys_mouse_extent( int *xmax, int *ymax );
void  sys_mouse_range( int xmax, int ymax );    /* 0..xmax, 0..ymax */
void  sys_mouse_move( int x, int y );
void  sys_mouse_inject( int buttons );  /* test mode: buttons as if pressed */

/* files and directories */
int   sys_lfn( void );
int   sys_open_read( const char *path, u32 *handle );
int   sys_open_write( const char *path, u32 *handle );    /* devices too */
int   sys_create( const char *path, u32 *handle );
int   sys_read( u32 handle, void *buf, u32 len, u32 *done );
int   sys_write( u32 handle, const void *buf, u32 len, u32 *done );
int   sys_close( u32 handle );
int   sys_file_size( u32 handle, u32 *size );   /* and back to the start */
int   sys_seek( u32 handle, u32 pos );
int   sys_get_ftime( u32 handle, u16 *time, u16 *date );
int   sys_set_ftime( u32 handle, u16 time, u16 date );
int   sys_get_attr( const char *path, u16 *attr );
int   sys_set_attr( const char *path, u16 attr );
int   sys_delete( const char *path );
int   sys_rename( const char *from, const char *to );
int   sys_mkdir( const char *path );
int   sys_rmdir( const char *path );
int   sys_find_first( const char *pattern, u16 attrs, FINDREC *rec, u32 *handle );
int   sys_find_next( u32 handle, FINDREC *rec );
void  sys_find_close( u32 handle );
int   sys_get_drive( void );
void  sys_set_drive( int drv );
int   sys_get_cwd( int drv, char *buf );    /* "DIR\SUB", no drive, no lead '\' */
int   sys_chdir( const char *path );
int   sys_truename( const char *path, char *out, int form );

/* the drives */
#define DK_NONE         0
#define DK_FLOPPY       1
#define DK_FIXED        2
#define DK_CDROM        3
#define DK_OTHER        4       /* a block driver's, or an image file's */
int   sys_drive_kind( int drv );
int   sys_disk_space( int drv, u32 *free_kb, u32 *total_kb );
int   sys_volume_label( int drv, char *label );     /* 12 bytes; "" for none */

/* another program: "prog" loaded and run with "tail" as its command
   tail -> its exit code, or -1 and *error = why it could not be run */
int   sys_exec( const char *prog, const char *tail, int *error );
void  sys_shell_path( char *buf, u32 max );         /* the command interpreter */
void  sys_version( int *major, int *minor );        /* PM-DOS's own */

/* ------------------------------------------------------------------ */
/* a graphics screen                                                   */
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

/* VBE: its version (0200h is 2.0), or 0 when the video BIOS has none;
   and 4F01h's 256 bytes for one mode */
int   sys_vbe_version( u16 *modes, int max, int *count );
int   sys_vbe_mode( int mode, u8 *info );

#define VMODE_LFB       0x4000
/* set a mode: a BIOS one below 100h, a VBE one with VMODE_LFB.  ->
   the selector of its frame buffer, the bytes there and the bytes a
   scan line (0 for a planar mode); 0 when the mode was set */
int   sys_video_set( u32 mode, u32 *selector, u32 *bytes, u32 *pitch );
void  sys_video_restore( void );            /* the text screen from before */
/* "count" DAC entries from "first": blue, green, red and a zero each,
   six bits a colour */
int   sys_video_palette( int first, int count, const u8 *entries );

/* DOSINT.ASM: bytes to the frame buffer, and a port */
void     __cdecl vid_put( u32 selector, u32 offset, const void *src, u32 len );
void     __cdecl vid_fill( u32 selector, u32 offset, u32 value, u32 len );
void     __cdecl port_out( unsigned port, unsigned value );

#endif
