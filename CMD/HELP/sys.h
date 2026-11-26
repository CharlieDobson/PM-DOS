/*
 * SYS.H - every call HELP makes to the operating system.
 *
 * SYS_D32.C is the one implementation: PM-DOS, native.  Files go by
 * their long names when the kernel has the long-name calls (71xxh) and
 * by the classic ones when it says 7100h (LFN=OFF).  The screen, the
 * cursor and the keyboard go through 21h/F0h, because a program at
 * ring 3 has no video memory, no BIOS data area and no BIOS of its own.
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
    uint8_t  attr;
    uint32_t size;
    char     name[PATH_MAX];    /* the long name - the 8.3 when there is none */
    char     alias[14];
} FINDREC;

int          sys_init( void );
const char *sys_cmdline( void );
const char *sys_getenv( const char *name );
const char *sys_program_path( void );       /* the path this program was run from */
void         sys_exit( int code );
void        *sys_mem_alloc( unsigned int size );
unsigned int sys_mem_largest( void );
void         sys_break_install( void );
void         sys_stdout( const char *buf, unsigned int len );
int          sys_stdin_is_console( void );
int          sys_stdin_byte( void );            /* -1 at the end */

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
int   sys_set_lines( int lines );               /* -> rows now */

/* the keyboard: a key is AL = ASCII, AH = scan code */
#define KW_NOWAIT       0x01      /* sys_key_wait: look, and come back */
#define KW_MOUSE        0x02      /* ...and the mouse wakes it too */
int      sys_key_wait( int shift_seen, int *shift_now, int flags );
                                  /* 1 = key waiting, 2 = the mouse moved */
int      sys_key_read( int *shift_now );
int      sys_shift( void );
uint32_t sys_hundredths( void );  /* the time of day, for double clicks */

/* the mouse: INT 33h, positions in the driver's virtual pixels */
int   sys_mouse_reset( void );  /* -> buttons, 0 for no mouse; hides it */
void  sys_mouse_show( int show );
void  sys_mouse_state( int *x, int *y, int *buttons );
int   sys_mouse_count( int release, int button, int *x, int *y );
void  sys_mouse_extent( int *xmax, int *ymax );
void  sys_mouse_move( int x, int y );
void  sys_mouse_inject( int buttons );  /* test mode: buttons as if pressed */

/* files and directories */
int   sys_lfn( void );
int   sys_open_read( const char *path, uint32_t *handle );
int   sys_open_write( const char *path, uint32_t *handle ); /* devices too */
int   sys_create( const char *path, uint32_t *handle );
int   sys_read( uint32_t handle, void *buf, unsigned int len, unsigned int *done );
int   sys_write( uint32_t handle, const void *buf, unsigned int len, unsigned int *done );
int   sys_close( uint32_t handle );
int   sys_file_size( uint32_t handle, uint32_t *size ); /* and back to the start */
int   sys_rewind( uint32_t handle );
int   sys_get_attr( const char *path, uint16_t *attr );
int   sys_find_first( const char *pattern, uint16_t attrs, FINDREC *rec, uint32_t *handle );
int   sys_find_next( uint32_t handle, FINDREC *rec );
void  sys_find_close( uint32_t handle );
int   sys_get_drive( void );
void  sys_set_drive( int drv );
int   sys_drive_valid( int drv );
int   sys_get_cwd( int drv, char *buf );    /* "DIR\SUB", no drive, no lead '\' */
int   sys_chdir( const char *path );
int   sys_truename( const char *path, char *out, int form );

#endif
