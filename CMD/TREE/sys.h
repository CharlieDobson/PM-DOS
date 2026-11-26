/*
 * SYS.H - every call TREE makes to the operating system.
 *
 * SYS_D32.C is the one implementation: PM-DOS, native.  The long-name
 * calls (71xxh) are used when the kernel has them and the classic ones
 * when it answers 7100h - LFN=OFF in CONFIG.SYS - so the tree is still
 * drawn, in 8.3 names, on a system without long names.
 *
 * Errors are DOS error codes (2 = file not found, 3 = path not found,
 * 5 = access denied ...); 0 is success.  Drives are 0-based (0 = A:).
 */
#ifndef SYS_H
#define SYS_H

#include "types.h"

#define H_OUT   1
#ifdef ERR_TO_OUT               /* a test build, for a shell with no 2> */
#define H_ERR   1
#else
#define H_ERR   2
#endif

#define ATTR_READONLY   0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME     0x08
#define ATTR_DIR        0x10
#define ATTR_ARCHIVE    0x20

#define ERR_NOFILE      2
#define ERR_NOPATH      3
#define ERR_NOMEM       8
#define ERR_BADDRIVE    15
#define ERR_NOMORE      18

#define PATH_MAX        260     /* the long-name API's own limit, NUL included */
#define LABEL_LEN       11      /* a volume label, as the directory holds it */

typedef struct {
    uint8_t attr;
    char    name[PATH_MAX];     /* the long name - the 8.3 when there is none */
} FINDREC;

int   sys_init( void );
const char *sys_cmdline( void );
const char *sys_getenv( const char *name );
void  sys_write( int handle, const char *buf, unsigned int len );
void  sys_exit( int code );
void  sys_break_install( void );
int   sys_break_hit( void );

int   sys_get_drive( void );
int   sys_drive_valid( int drv );
int   sys_get_cwd( int drv, char *buf );     /* "DIR\SUB", no drive, no lead '\' */
int   sys_get_attr( const char *path, uint16_t *attr );
int   sys_volume_label( int drv, char *label );     /* LABEL_LEN + 1 bytes */
int   sys_volume_serial( int drv, uint32_t *serial );
void  sys_upper( char *str, unsigned int len );  /* the country's capitals */

int   sys_find_first( const char *pattern, uint16_t attrs, FINDREC *rec, uint32_t *handle );
int   sys_find_next( uint32_t handle, FINDREC *rec );
void  sys_find_close( uint32_t handle );

void *sys_mem_alloc( unsigned int size );

#endif
