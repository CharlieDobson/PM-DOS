/*
 * SYS.H - every call DEBUG makes to the operating system.
 *
 * SYS_D32.C is the one implementation: PM-DOS, native.  Errors are DOS
 * error codes (2 = file not found, 5 = access denied ...); 0 is
 * success.  Drives are 0-based (0 = A:).
 */
#ifndef SYS_H
#define SYS_H

#include "types.h"

#define H_IN    0
#define H_OUT   1
#define H_ERR   2

#define ERR_NOFILE      2
#define ERR_ACCESS      5
#define ERR_NOMEM       8
#define ERR_EXEFMT      11
#define ERR_BADDATA     13

/* INC\PMDOS.INC's DRX_* record: the held program's registers, and why
   it stopped */
typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, efl;
    uint16_t cs, ds, es, ss, fs, gs;
    uint32_t vec, err, addr, dr6, limit;
} DRX;

#define DRX_NOVEC       0xFFFFFFFFUL
#define DBF_ENDED       0
#define DBF_STOPPED     1

#define SEL_PROG_CS     0x3B
#define SEL_PROG_DS     0x43

#define SLOT_PSP        0x1000UL        /* where a native program's PSP is */
#define SLOT_IMAGE      0x2000UL        /* ...and its image */

int   sys_init( void );
const char *sys_cmdline( void );        /* the command tail, NUL-ended */
void  sys_write( int handle, const char *buf, unsigned int len );
void  sys_exit( int code );
void *sys_mem_alloc( unsigned int size );
void  sys_mem_free( void *block );
int   sys_is_device( int handle );

int   sys_read_line( char *buf, unsigned int max );  /* -1 at the end of the input */
int   sys_get_key( void );                           /* -1 at the end of the input */

void  sys_break_install( void );
extern uint8_t break_hit;
extern uint8_t break_quiet;

/* files: a handle, or -1 with *err set */
int   sys_open( const char *name, int *err );
int   sys_create( const char *name, int *err );
void  sys_delete( const char *name );
int   sys_read( int handle, void *buf, unsigned int len, unsigned int *got );
int   sys_write_file( int handle, const void *buf, unsigned int len, unsigned int *done );
int   sys_size( int handle, uint32_t *size );
int   sys_seek( int handle, uint32_t pos ); /* from the start */
void  sys_close( int handle );

/* the first megabyte, read only (21h/F1h AL=20h) */
int      sys_low_read( uint32_t linear, void *buf, uint32_t len );
uint16_t sys_conv_seg( void );          /* the first free paragraph a DOS box would have */

/* raw sectors (INT 25h / 26h) */
int   sys_abs_read( int drive, uint32_t sector, uint32_t count, void *buf );
int   sys_abs_write( int drive, uint32_t sector, uint32_t count, const void *buf );

uint32_t __cdecl port_in( uint32_t port );
void     __cdecl port_out( uint32_t port, uint32_t value );

/* the held program (21h/F1h AL=29h) */
int   sys_dbg_load( const char *path, const char *tail, DRX *regs );
int   sys_dbg_go( DRX *regs, int *stopped );
int   sys_dbg_read( uint32_t offset, void *buf, uint32_t len );
int   sys_dbg_write( uint32_t offset, const void *buf, uint32_t len );
int   sys_dbg_kill( void );
int   sys_exit_code( void );            /* 4Dh: AL, and AH = how it ended */

#endif
