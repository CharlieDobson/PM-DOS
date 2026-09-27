/*
 * SYS.H - every call XCOPY makes to the operating system.
 *
 * SYS_D32.C is the one implementation: PM-DOS, native.  The long-name
 * calls (71xxh) are used when the kernel has them and the classic ones
 * when it answers 7100h - LFN=OFF in CONFIG.SYS - so the program still
 * copies, in 8.3 names, on a system without long names.
 *
 * Errors are DOS error codes (2 = file not found, 3 = path not found,
 * 5 = access denied ...); 0 is success.  Drives are 0-based (0 = A:).
 */
#ifndef SYS_H
#define SYS_H

#include "types.h"

#define H_IN    0
#define H_OUT   1

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
#define ERR_SHARE       32
#define ERR_LOCK        33
#define ERR_DISKFULL    112     /* a short write: "Insufficient disk space" */
#define ERR_EXISTS      80

#define PATH_MAX        260     /* the long-name API's own limit, NUL included */

typedef struct {
    u8   attr;
    u16  time, date;            /* last written, DOS form */
    u32  size;
    char name[PATH_MAX];        /* the long name - the 8.3 when there is none */
    char alias[14];             /* the 8.3 name */
} FINDREC;

int   sys_init( void );
const char *sys_cmdline( void );
const char *sys_getenv( const char *name );
void  sys_write( int handle, const char *buf, u32 len );
int   sys_stdin_is_console( void );
int   sys_read_key( void );                 /* no echo; -1 at end of input */
void  sys_exit( int code );
void  sys_break_install( void );
int   sys_break_hit( void );
void  sys_break_abandon( void );            /* from inside the handler */
int   sys_lfn( void );                      /* the long-name calls answer */

int   sys_get_drive( void );
int   sys_drive_valid( int drv );
int   sys_get_cwd( int drv, char *buf );    /* "DIR\SUB", no drive, no lead '\' */
int   sys_truename( const char *path, char *out, int form );   /* 7160h CL */
int   sys_get_attr( const char *path, u16 *attr );
int   sys_set_attr( const char *path, u16 attr );
int   sys_mkdir( const char *path );
int   sys_delete( const char *path );
int   sys_is_device( const char *path );    /* a character device by name */

int   sys_find_first( const char *pattern, u16 attrs, FINDREC *rec, u32 *handle );
int   sys_find_next( u32 handle, FINDREC *rec );
void  sys_find_close( u32 handle );

int   sys_open_read( const char *path, u32 *handle );
int   sys_create( const char *path, u32 *handle );
int   sys_read( u32 handle, void *buf, u32 len, u32 *done );
int   sys_write_file( u32 handle, const void *buf, u32 len, u32 *done );
int   sys_close( u32 handle );
int   sys_get_ftime( u32 handle, u16 *date, u16 *time );
int   sys_set_ftime( u32 handle, u16 date, u16 time );

int   sys_get_verify( void );
void  sys_set_verify( int on );
void  sys_date_format( int *order, char *sep );     /* 0 MDY, 1 DMY, 2 YMD */
void *sys_mem_alloc( u32 size );

#endif
