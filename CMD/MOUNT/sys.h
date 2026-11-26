/*
 * SYS.H - every call MOUNT makes to the operating system.
 *
 * SYS_D32.C is the one implementation: PM-DOS, native.  Errors are DOS
 * error codes (2 = file not found, 3 = path not found, 5 = access
 * denied ...); 0 is success.
 */
#ifndef SYS_H
#define SYS_H

#include "types.h"

#define H_IN    0
#define H_OUT   1

#define ATTR_READONLY   0x01

#define ERR_NOFILE      2
#define ERR_NOPATH      3
#define ERR_ACCESS      5
#define ERR_NOMEM       8
#define ERR_BADDATA     13
#define ERR_BADDRIVE    15
#define ERR_BADLEN      0x18    /* IMEM_COPY: the room is too small */
#define ERR_WRFAULT     0x1D
#define ERR_GENERAL     0x1F
#define ERR_NOTSUP      50      /* F1h AL=0Fh: no clusters to map */
#define ERR_DISKFULL    0x70
#define ERR_EXISTS      80

#define PATH_MAX        260     /* the long-name API's own limit, NUL included */

/* 21h/F1h AL=11h's report of one mount (IMAGE.INC img_report) */
#define IMG_MAX         8
#define IMF_RO          0x02
#define IMF_ISO         0x04
#define IMF_MEM         0x08    /* held in memory, loaded by this program */
#define IMF_CHANGED     0x10    /* ...and written to since */
#define IMF_NET         0x20    /* a file on a share, read where it is */
#define F1T_EXFAT       0xEF    /* F1h AL=1's type byte for exFAT */

typedef struct {
    uint8_t  letter;            /* 0 = A: */
    uint8_t  flags;             /* IMF_* */
    uint8_t  host;              /* the letter the file is on, FFh none */
    uint8_t  pad;
    uint32_t size;              /* the file, in bytes */
    uint32_t extents;           /* runs in its map; 0 when in memory */
    uint32_t start;             /* the volume's first sector */
    uint32_t secs;              /* ...and how many it has */
    char     path[80];          /* ASCIIZ */
} REPORT;

/* F1h AL=10h's BH */
#define UM_KEEP         0       /* refuse if a loaded image has changed */
#define UM_DISCARD      1       /* throw a loaded image's changes away */
#define UM_SAVED        2       /* the changes up to "gen" were saved */

int   sys_init( void );
const char *sys_cmdline( void );
void  sys_write( int handle, const char *buf, unsigned int len );
int   sys_stdin_is_console( void );
void  sys_key_flush( void );                /* drop what was typed ahead */
int   sys_read_key( void );                 /* no echo; -1 at end of input */
void  sys_exit( int code );

int   sys_truename( const char *path, char *out, int form );                /* 7160h CL */
int   sys_get_attr( const char *path, uint16_t *attr );
int   sys_open( const char *path, int mode, uint32_t *handle );             /* 3Dh AL */
int   sys_create( const char *path, uint32_t *handle );
int   sys_read( uint32_t handle, void *buf, uint32_t len, uint32_t *got );
int   sys_write_file( uint32_t handle, const void *buf, uint32_t len, uint32_t *put );
int   sys_seek( uint32_t handle, uint32_t pos, int how, uint32_t *where );  /* 42h AL */
int   sys_close( uint32_t handle );
int   sys_thousands( void );                /* 38h's separator, ',' if none */
void  sys_now( uint16_t *dos_date, uint16_t *dos_time );

/* the kernel's image calls, 21h/F1h */
int   sys_img_map( int letter, int ro, const char *path, int *took );
int   sys_img_load( int letter, int ro, const void *data, uint32_t len,
                    const char *path, int *took );
int   sys_img_copy( int letter, void *buf, uint32_t room, uint32_t *len, uint32_t *gen );
int   sys_img_unmount( int letter, int mode, uint32_t gen );
int   sys_img_report( int slot, REPORT *rep );
int   sys_drive_type( int letter );         /* F1h AL=1: -1 if it fails */

void *sys_mem_alloc( unsigned int size );

#endif
