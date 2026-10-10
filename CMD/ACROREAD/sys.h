/*
 * SYS.H - every call ACROREAD makes to the operating system.
 *
 * SYS_D32.C is the implementation that ships: PM-DOS, native.  The text
 * screen and the keyboard are 21h/F0h, as EDIT's are; a graphics screen
 * is 21h/F1h AL=1Fh, which sets the mode and hands back a selector for
 * its frame buffer, and gfx_put (DOSINT.ASM) is how a scan line gets
 * there.  SYS_NT.C is the same calls for a test program on Windows,
 * which has files and no screen.
 *
 * Errors are DOS error codes; 0 is success.
 */
#ifndef SYS_H
#define SYS_H

#include "types.h"

#define ERR_NOFILE      2
#define ERR_NOPATH      3
#define ERR_NOHANDLES   4
#define ERR_ACCESS      5
#define ERR_NOMEM       8

#define PATH_LEN        260

/* the shift state, 40:17's bits */
#define SH_SHIFT        0x03
#define SH_CTRL         0x04
#define SH_ALT          0x08

int   sys_init( void );
const char *sys_cmdline( void );
const char *sys_getenv( const char *name );
void  sys_exit( int code );
void *sys_mem_alloc( u32 size );
u32   sys_mem_largest( void );
void  sys_break_install( void );
void  sys_stdout( const char *buf, u32 len );
int   sys_stdout_is_console( void );
int   sys_stdin_is_console( void );
int   sys_stdin_byte( void );               /* -1 at the end */

/* the text screen */
void  sys_screen_size( int *rows, int *cols );
void  sys_put_cells( u32 first, const u16 *cells, u32 count );
void  sys_get_cells( u32 first, u16 *cells, u32 count );
void  sys_cursor( int row, int col );
void  sys_get_cursor( int *row, int *col );
#define CUR_HIDE        0
#define CUR_LINE        1
void  sys_cursor_shape( int kind );

/* the keyboard: a key is AL = ASCII, AH = scan code */
int   sys_key_ready( void );
int   sys_key_read( int *shift_now );
u32   sys_hundredths( void );

/* files */
int   sys_open_read( const char *path, u32 *handle );
int   sys_read( u32 handle, void *buf, u32 len, u32 *done );
int   sys_seek( u32 handle, u32 pos );
int   sys_close( u32 handle );
int   sys_file_size( u32 handle, u32 *size );   /* and back to the start */

/*
 * A graphics screen.  A mode below 100h is a VGA mode, and its window
 * is the 64K at A0000h; from 100h up it is a VBE mode and is always
 * set with its linear frame buffer.  sys_gfx_set gives the bytes the
 * selector covers and the bytes in a scan line.
 */
#define VBE_INFO_LEN    512
#define VBE_MODE_LEN    256
int   sys_vbe_info( u8 *block, int *version, int *modes );   /* 1 = there is one */
int   sys_vbe_mode( int mode, u8 *block );                   /* 1 = it answered  */
int   sys_gfx_set( int mode, u32 *bytes, u32 *pitch );       /* 0 = set          */
void  sys_gfx_restore( void );
void  sys_gfx_palette( int first, int count, const u8 *bgr0 );

void     __cdecl gfx_put( u32 offset, const void *src, u32 bytes );
void     __cdecl gfx_fill( u32 offset, u32 value, u32 bytes );
unsigned __cdecl port_in( unsigned port );
void     __cdecl port_out( unsigned port, unsigned value );

#endif
