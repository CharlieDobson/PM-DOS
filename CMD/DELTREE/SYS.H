/*
 * SYS.H - every call DELTREE makes to the operating system.
 *
 * SYS_D32.C is the one implementation: PM-DOS, native.  The long-name
 * calls (71xxh) are used when the kernel has them and the classic ones
 * when it answers 7100h - LFN=OFF in CONFIG.SYS - so the program still
 * works, in 8.3 names, on a system without long names.
 *
 * Errors are DOS error codes (2 = file not found, 3 = path not found,
 * 5 = access denied ...); 0 is success.
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
#define ERR_ACCESS      5
#define ERR_CURDIR      16
#define ERR_NOMORE      18
#define ERR_SHARE       32
#define ERR_LOCK        33
#define ERR_FAIL        83      /* "Fail" was the answer to a disk error */

#define PATH_MAX        260     /* the long-name API's own limit, NUL included */

typedef struct {
    u8   attr;
    char name[PATH_MAX];        /* the long name - the 8.3 when there is none */
    char alias[14];             /* the 8.3 name */
} FINDREC;

int   sys_init( void );
const char *sys_cmdline( void );
const char *sys_getenv( const char *name );
void  sys_write( int handle, const char *buf, u32 len );
int   sys_stdin_is_console( void );
void  sys_key_flush( void );                /* drop what was typed ahead */
int   sys_read_key( void );                 /* no echo; -1 at end of input */
void  sys_exit( int code );

int   sys_truename( const char *path, char *out, int form );   /* 7160h CL */
int   sys_set_attr( const char *path, u16 attr );
int   sys_delete( const char *path );
int   sys_rmdir( const char *path );

int   sys_find_first( const char *pattern, u16 attrs, FINDREC *rec, u32 *handle );
int   sys_find_next( u32 handle, FINDREC *rec );
void  sys_find_close( u32 handle );

void *sys_mem_alloc( u32 size );

#endif
