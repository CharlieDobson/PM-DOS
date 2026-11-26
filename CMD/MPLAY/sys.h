/*
 * SYS.H - every call PMWRITE makes to the operating system.
 *
 * SYS_D32.C is the implementation that ships: PM-DOS, native.  The text
 * screen and the keyboard are 21h/F0h, the mouse is INT 33h, files are
 * the long-name calls with the classic ones behind them, and a graphics
 * screen is a mode set by the kernel (21h/F1h AL=1Fh), which hands back
 * a selector for the frame buffer.  The ROM's font is read out of the
 * first megabyte (21h/F1h AL=20h), because a program at ring 3 has no
 * BIOS to ask.
 *
 * SYS_NT.C is the same calls for PWHOST.EXE, the test program built on
 * Windows: files and memory are real there, and the screen, keyboard
 * and mouse do nothing.
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

/* a path's room.  A fixed number on purpose: a structure that holds a
   path must be the same size in every file that sees it.  (Not called
   PATH_MAX: the C library PWHOST is built with has one of its own.) */
#define PATH_LEN        260

/* the shift state, 40:17's bits */
#define SH_RSHIFT       0x01
#define SH_LSHIFT       0x02
#define SH_SHIFT        0x03
#define SH_CTRL         0x04
#define SH_ALT          0x08
#define SH_KEYS         0x0F

typedef struct {
    uint8_t  attr;
    uint32_t size;
    uint16_t time, date;        /* DOS's packed forms */
    char     name[PATH_LEN];    /* the long name - the 8.3 when there is none */
    char     alias[14];
} FINDREC;

int          sys_init( void );
const char  *sys_cmdline( void );
const char  *sys_getenv( const char *name );
const char  *sys_program_dir( void );       /* where PMWRITE.EXE is: "C:\DIR\" */
void         sys_exit( int code );
void        *sys_mem_alloc( unsigned int size );
unsigned int sys_mem_largest( void );
void         sys_break_install( void );     /* Ctrl+C is a key, not an end */
void         sys_stdout( const char *buf, unsigned int len );
int          sys_stdout_is_console( void );
int          sys_stdin_is_console( void );
int          sys_stdin_byte( void );        /* -1 at the end */
int          sys_crit_seen( void );         /* a critical error was failed since last asked */

/* the text screen */
void  sys_cls( void );
void  sys_screen_size( int *rows, int *cols );
void  sys_put_cells( unsigned int first, const uint16_t *cells, unsigned int count );
void  sys_get_cells( unsigned int first, uint16_t *cells, unsigned int count );
void  sys_cursor( int row, int col );
void  sys_get_cursor( int *row, int *col );
#define CUR_HIDE        0
#define CUR_LINE        1
#define CUR_BLOCK       2
void  sys_cursor_shape( int kind );
int   sys_set_lines( int lines );               /* -> rows now */

/* the keyboard: a key is AL = ASCII, AH = scan code */
#define KW_NOWAIT       0x01      /* sys_key_wait: look, and come back */
#define KW_MOUSE        0x02      /* ...and the mouse wakes it too */
int      sys_key_wait( int shift_seen, int *shift_now, int flags );
                                  /* 1 = key waiting, 2 = the mouse moved */
int      sys_key_read( int *shift_now );
int      sys_shift( void );
uint32_t sys_hundredths( void );  /* the time of day, good to a tick (55 ms) */
uint32_t sys_clock_ms( void );    /* milliseconds, counted while it is asked often */

typedef struct {
    int year, month, day, hour, minute, second;
} DATETIME;
void  sys_now( DATETIME *now );

/* how the country writes a date, a time and a number (21h/38h) */
typedef struct {
    int  date_order;            /* 0 month-day-year, 1 day-month-year, 2 year-month-day */
    char date_sep, time_sep;
    char thousands, decimal;
    int  clock24;
    int  codepage;              /* the screen's: 437, 850 ... (21h/6601h) */
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
int   sys_mouse_wheel( void );          /* notches the wheel turned since last asked; 0 for a
                                           driver that has no wheel */

/* files and directories */
int   sys_lfn( void );
int   sys_open_read( const char *path, uint32_t *handle );
int   sys_open_write( const char *path, uint32_t *handle ); /* devices too: PRN */
int   sys_create( const char *path, uint32_t *handle );    /* made, or emptied; read and write */
int   sys_read( uint32_t handle, void *buf, uint32_t len, uint32_t *done );
int   sys_write( uint32_t handle, const void *buf, uint32_t len, uint32_t *done );
int   sys_close( uint32_t handle );
int   sys_file_size( uint32_t handle, uint32_t *size ); /* and back to the start */
int   sys_seek( uint32_t handle, uint32_t pos );
int   sys_get_ftime( uint32_t handle, uint16_t *time, uint16_t *date );
int   sys_set_ftime( uint32_t handle, uint16_t time, uint16_t date );
int   sys_get_attr( const char *path, uint16_t *attr );
int   sys_set_attr( const char *path, uint16_t attr );
int   sys_delete( const char *path );
int   sys_rename( const char *from, const char *to );
int   sys_mkdir( const char *path );
int   sys_rmdir( const char *path );
int   sys_find_first( const char *pattern, uint16_t attrs, FINDREC *rec, uint32_t *handle );
int   sys_find_next( uint32_t handle, FINDREC *rec );
void  sys_find_close( uint32_t handle );
int   sys_get_drive( void );
void  sys_set_drive( int drv );
int   sys_drive_valid( int drv );
int   sys_get_cwd( int drv, char *buf );    /* "DIR\SUB", no drive, no lead '\' */
int   sys_chdir( const char *path );
int   sys_truename( const char *path, char *out, int form );

/* the drives */
#define DK_NONE         0
#define DK_FLOPPY       1
#define DK_FIXED        2
#define DK_CDROM        3
#define DK_OTHER        4       /* a block driver's, an image file's, a network one */
int   sys_drive_kind( int drv );
int   sys_disk_space( int drv, uint32_t *free_kb, uint32_t *total_kb );

/* another program: "prog" loaded and run with "tail" as its command
   tail -> its exit code, or -1 and *error = why it could not be run */
int   sys_exec( const char *prog, const char *tail, int *error );
void  sys_shell_path( char *buf, unsigned int max ); /* the command interpreter */
void  sys_version( int *major, int *minor );        /* PM-DOS's own */

/* the 32-bit drivers DEVICE= loaded (21h/F1h AL=25h): the first whose
   table carries "flag" (NDRV.H's NDTF_*) -> its row, or -1; and a call
   to one -> 0, or the driver's error */
int   sys_driver_find( uint32_t flag );
int   sys_driver_call( int row, uint32_t fn, void *buf, uint32_t len, uint32_t *answer );

/* the PCI bus and the interrupt lines (21h/F1h AL=12h): "fn" is a BUS_*
   function; regs[0..5] are EAX, EBX, ECX, EDX, ESI, EDI going in and
   coming out.  -> 1 if the carry came back clear */
int   sys_bus( uint32_t fn, uint32_t *regs );

/* ------------------------------------------------------------------ */
/* a graphics screen                                                   */
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

/* VBE: its version (0200h is 2.0), or 0 when the video BIOS has none;
   and 4F01h's 256 bytes for one mode */
int   sys_vbe_version( uint16_t *modes, int max, int *count );
int   sys_vbe_mode( int mode, uint8_t *info );

#define VMODE_LFB       0x4000
/* set a mode: a BIOS one below 100h, a VBE one with VMODE_LFB.  ->
   the selector of its frame buffer, the bytes there and the bytes a
   scan line (0 for a planar mode); 0 when the mode was set */
int   sys_video_set( uint32_t mode, uint32_t *selector, uint32_t *bytes, uint32_t *pitch );
void  sys_video_restore( void );            /* the text screen from before */
/* "count" DAC entries from "first": blue, green, red and a zero each,
   six bits a colour */
int   sys_video_palette( int first, int count, const uint8_t *entries );

/* DOSINT.ASM: bytes to the frame buffer, and a port */
void     __cdecl vid_put( uint32_t selector, uint32_t offset, const void *src, uint32_t len );
void     __cdecl vid_fill( uint32_t selector, uint32_t offset, uint32_t value, uint32_t len );
void     __cdecl port_out( unsigned port, unsigned value );
unsigned __cdecl port_in( unsigned port );

#endif
