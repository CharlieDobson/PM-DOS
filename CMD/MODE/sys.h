/*
 * SYS.H - every call MODE makes to the operating system.
 *
 * SYS_D32.C is the one implementation: PM-DOS, native.  A native
 * program has no BIOS, no video memory and no BIOS data area of its
 * own, so the machine is reached through the kernel: 21h/F0h for the
 * screen and the keyboard, 21h/F1h for low memory, the BIOS's answers
 * and the printer and serial settings, and IN and OUT, which the
 * kernel carries out for a native program.
 *
 * Errors are DOS error codes (1 = invalid function, 2 = file not
 * found, 5 = access denied ...); 0 is success.
 */
#ifndef SYS_H
#define SYS_H

#include "types.h"

#define H_OUT   1
#define H_ERR   2

/* the BIOS data area, as low memory addresses it */
#define BDA_COM1    0x400       /* 4 words: COM1-COM4 base ports, 0 = none */
#define BDA_LPT1    0x408       /* 3 words: LPT1-LPT3                     */
#define BDA_EQUIP   0x410       /* the equipment word                     */
#define BDA_VMODE   0x449
#define BDA_VCOLS   0x44A

/* a BIOS call that only asks (21h/F1h AL=21h) */
typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u16 es, ds;
    u32 flags;                  /* out: bit 0 is CF */
} BQ;

/* the printer and serial settings the kernel keeps (21h/F1h AL=28h) */
typedef struct {
    u8 route[4];                /* LPT1-3: 0, or the COM port 1-4      */
    u8 lretry[4];               /* LPT1-3: RETRY_*                     */
    u8 cretry[4];               /* COM1-4: RETRY_*                     */
} PORTSET;

#define RETRY_NONE  0
#define RETRY_E     1
#define RETRY_B     2
#define RETRY_R     3
#define RETRY_P     4

int   sys_init( void );
const char *sys_cmdline( void );
void  sys_write( int handle, const char *buf, u32 len );
void  sys_exit( int code );

u16   sys_low_word( u32 addr );
u8    sys_low_byte( u32 addr );
int   sys_low_ram( u32 addr );                  /* 1 if a byte there keeps */
int   sys_bios( int intno, BQ *bq );            /* 0 = done */

u32 __cdecl port_in( u32 port );
void __cdecl port_out( u32 port, u32 value );

/* the screen and the keyboard */
void  sys_scr_size( int *rows, int *cols );
int   sys_text_mode( int mode, int scan, int font8, int equip );
int   sys_equip( int bits );                    /* 10h, 20h or 30h */
int   sys_kb_rate( int byte );

/* the printers and serial ports */
int   sys_port_get( PORTSET *set );
int   sys_port_route( int lpt, int com );       /* lpt 0-2, com 1-4 or 0 */
int   sys_port_retry( int dev, int retry );     /* dev 0-2 LPT, 4-7 COM */
int   sys_lpt_out( int lpt, int byte, int *status );

/* files and devices, for the code pages and ANSI.SYS */
int   sys_open( const char *name, int mode, int *handle );
void  sys_close( int handle );
int   sys_read( int handle, void *buf, u32 len, u32 *got );
int   sys_devinfo( int handle, u16 *info );     /* 4400h */
int   sys_setinfo( int handle, u16 info );      /* 4401h */
int   sys_ioctl_write( int handle, const void *buf, u32 len );   /* 4403h */
int   sys_genioctl( int handle, int cat, int func, void *block );  /* 440Ch */
int   sys_break( int set, int on );             /* -> the setting before */
void *sys_mem_alloc( u32 size );

#endif
