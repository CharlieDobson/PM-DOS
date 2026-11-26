/*
 * SYS.H - every call QBASIC makes to the operating system.
 *
 * SYS_D32.C is the implementation that ships: PM-DOS, native.  Files go
 * by their long names when the kernel has the long-name calls (71xxh)
 * and by the classic ones when it says 7100h (LFN=OFF).  The screen,
 * the cursor and the keyboard go through 21h/F0h, because a program at
 * ring 3 has no video memory, no BIOS data area and no BIOS of its own;
 * a graphics mode and its frame buffer through 21h/F1h AL=1Fh; the
 * first megabyte is read through AL=20h.
 *
 * SYS_NT.C is the same calls on Windows, for QBHOST.EXE (MKHOST.CMD):
 * the interpreter run from a console with the screen kept in memory.
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

#define ERR_FUNC        1
#define ERR_NOFILE      2
#define ERR_NOPATH      3
#define ERR_NOHANDLES   4
#define ERR_ACCESS      5
#define ERR_BADHANDLE   6
#define ERR_NOMEM       8
#define ERR_BADDRIVE    15
#define ERR_CURDIR      16
#define ERR_NOTSAME     17
#define ERR_NOMORE      18
#define ERR_WRPROT      19
#define ERR_NOTREADY    21
#define ERR_SHARE       32
#define ERR_EXISTS      80
#define ERR_DISKFULL    112

#ifdef PATH_MAX
#undef PATH_MAX                 /* the host build's C library has its own */
#endif
#define PATH_MAX        260

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
    uint16_t date, time;
    char     name[PATH_MAX];    /* the long name - the 8.3 when there is none */
    char     alias[14];
} FINDREC;

int          sys_init( void );
const char  *sys_cmdline( void );
const char  *sys_getenv( const char *name );
const char  *sys_environment( void );       /* the block itself */
void         sys_exit( int code );
void        *sys_mem_alloc( unsigned int size );
unsigned int sys_mem_largest( void );
void         sys_break_install( void );
void         sys_stdout( const char *buf, unsigned int len );
int          sys_stdin_is_console( void );
int          sys_stdin_byte( void );        /* -1 at the end */
int          sys_fpu_present( void );
int          sys_exec( const char *program, const char *tail, const char *env ); /* DOS error; env NULL: QBasic's own */
const char  *sys_shell( void );             /* the command interpreter, for SHELL */

/* the screen */
void  sys_screen_size( int *rows, int *cols );
void  sys_put_cells( unsigned int first, const uint16_t *cells, unsigned int count );
void  sys_get_cells( unsigned int first, uint16_t *cells, unsigned int count );
void  sys_cursor( int row, int col );
void  sys_get_cursor( int *row, int *col );
#define CUR_HIDE        0
#define CUR_LINE        1
#define CUR_BLOCK       2
void  sys_cursor_shape( int kind );
void  sys_cursor_lines( int first, int last );
int   sys_set_lines( int lines );               /* -> rows now */
int   sys_text_mode( int mode, int lines );     /* 21h/F0h AL=0Ch: 0-3, 7 */

/* graphics: 21h/F1h AL=1Fh */
int   sys_video_set( uint32_t mode, uint32_t *selector, uint32_t *bytes, uint32_t *pitch );
void  sys_video_restore( void );
int   sys_video_palette( int first, int count, const uint8_t *entries ); /* B G R 0 */
int   sys_video_palette_get( int first, int count, uint8_t *entries );
int   sys_peek( uint32_t addr, void *buf, unsigned int len ); /* the first megabyte: 1 = read */
void  __cdecl vid_put( uint32_t selector, uint32_t offset, const void *src, unsigned int len );
void  __cdecl vid_fill( uint32_t selector, uint32_t offset, uint32_t value, unsigned int len );
void  __cdecl port_out( uint32_t port, uint32_t value );
uint32_t   __cdecl port_in( uint32_t port );

/* the keyboard: a key is AL = ASCII, AH = scan code */
#define KW_NOWAIT       0x01    /* sys_key_wait: look, and come back */
#define KW_MOUSE        0x02    /* ...and the mouse wakes it too */
int   sys_key_wait( int shift_seen, int *shift_now, int flags );
                                /* 1 = key waiting, 2 = the mouse moved */
int   sys_key_read( int *shift_now );
int   sys_shift( void );

/* the time */
uint32_t sys_hundredths( void ); /* the time of day */
void     sys_date( int *year, int *month, int *day );
void     sys_time( int *hour, int *minute, int *second, int *hundredth );
int      sys_set_date( int year, int month, int day );
int      sys_set_time( int hour, int minute, int second );

/* the mouse: INT 33h, positions in the driver's virtual pixels */
int   sys_mouse_reset( void );  /* -> buttons, 0 for no mouse; hides it */
void  sys_mouse_show( int show );
void  sys_mouse_state( int *x, int *y, int *buttons );
int   sys_mouse_count( int release, int button, int *x, int *y );
void  sys_mouse_extent( int *xmax, int *ymax );
void  sys_mouse_move( int x, int y );
void  sys_mouse_inject( int buttons );  /* test mode: buttons as if pressed */

/* files and directories */
#define OPEN_READ       0
#define OPEN_WRITE      1
#define OPEN_RW         2
int   sys_lfn( void );
int   sys_open( const char *path, int mode, uint32_t *handle );
int   sys_open_read( const char *path, uint32_t *handle );
int   sys_open_write( const char *path, uint32_t *handle ); /* devices too */
int   sys_create( const char *path, uint32_t *handle );
int   sys_read( uint32_t handle, void *buf, unsigned int len, unsigned int *done );
int   sys_write( uint32_t handle, const void *buf, unsigned int len, unsigned int *done );
int   sys_close( uint32_t handle );
int   sys_seek( uint32_t handle, uint32_t pos );
int   sys_tell( uint32_t handle, uint32_t *pos );
int   sys_file_size( uint32_t handle, uint32_t *size );  /* and back to the start */
int   sys_size_keep( uint32_t handle, uint32_t *size );  /* the position kept */
int   sys_truncate( uint32_t handle );                   /* the file ends here */
int   sys_rewind( uint32_t handle );
int   sys_is_device( uint32_t handle );
int   sys_get_attr( const char *path, uint16_t *attr );
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
int   sys_disk_free( int drv, uint32_t *bytes );

#endif
