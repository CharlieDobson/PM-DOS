/*
 * SYS.H - every call PMD makes to the operating system.
 *
 * SYS_D32.C is the one implementation: PM-DOS, native.  The first half
 * is EDIT's - the screen, the keyboard, the mouse and files, through
 * 21h/F0h, INT 33h and the long-name calls.  The second half is what a
 * diagnostics program needs and nothing else does: the kernel's own
 * accounts of the machine (21h/F1h), the first megabyte read into a
 * buffer (F1h AL=20h), a BIOS call that only asks (F1h AL=21h), the
 * ports, and the processor.
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
    u8   attr;
    u32  size;
    u16  time, date;            /* DOS's packed forms */
    char name[PATH_MAX];        /* the long name - the 8.3 when there is none */
    char alias[14];
} FINDREC;

int   sys_init( void );
const char *sys_cmdline( void );
const char *sys_getenv( const char *name );
const char *sys_env_block( void );          /* NAME=VALUE, NUL; a second NUL ends it */
const char *sys_program_path( void );       /* the path this program was run from */
void  sys_exit( int code );
void *sys_mem_alloc( u32 size );
u32   sys_mem_largest( void );
void  sys_break_install( void );
void  sys_stdout( const char *buf, u32 len );
int   sys_stdin_is_console( void );
int   sys_stdin_byte( void );               /* -1 at the end */
int   sys_stdin_line( char *buf, int max );  /* a line typed at the prompt */

/* the screen */
void  sys_screen_size( int *rows, int *cols );
void  sys_put_cells( u32 first, const u16 *cells, u32 count );
void  sys_get_cells( u32 first, u16 *cells, u32 count );
void  sys_cursor( int row, int col );
void  sys_get_cursor( int *row, int *col );
#define CUR_HIDE        0
#define CUR_LINE        1
#define CUR_BLOCK       2
void  sys_cursor_shape( int kind );

/* the keyboard: a key is AL = ASCII, AH = scan code */
#define KW_NOWAIT       0x01    /* sys_key_wait: look, and come back */
#define KW_MOUSE        0x02    /* ...and the mouse wakes it too */
int   sys_key_wait( int shift_seen, int *shift_now, int flags );
                                /* 1 = key waiting, 2 = the mouse moved */
int   sys_key_read( int *shift_now );
int   sys_shift( void );
u32   sys_hundredths( void );   /* the time of day, for double clicks */

typedef struct {
    int year, month, day, hour, minute;
} DATETIME;
void  sys_now( DATETIME *now );

/* the mouse: INT 33h, positions in the driver's virtual pixels */
int   sys_mouse_reset( void );  /* -> buttons, 0 for no mouse; hides it */
void  sys_mouse_show( int show );
void  sys_mouse_state( int *x, int *y, int *buttons );
int   sys_mouse_count( int release, int button, int *x, int *y );
void  sys_mouse_extent( int *xmax, int *ymax );
void  sys_mouse_move( int x, int y );
void  sys_mouse_inject( int buttons );  /* test mode: buttons as if pressed */

typedef struct {
    int present;
    int buttons;
    int version;                /* 0820h is 8.20 */
    int type;                   /* 1 bus, 2 serial, 3 InPort, 4 PS/2, 5 HP */
    int irq;
    int horiz, vert, threshold; /* sensitivity, 1..100 */
} MOUSEINFO;
void  sys_mouse_info( MOUSEINFO *info );

/* files and directories */
int   sys_lfn( void );
int   sys_open_read( const char *path, u32 *handle );
int   sys_open_write( const char *path, u32 *handle );    /* devices too */
int   sys_create( const char *path, u32 *handle );
int   sys_read( u32 handle, void *buf, u32 len, u32 *done );
int   sys_write( u32 handle, const void *buf, u32 len, u32 *done );
int   sys_close( u32 handle );
int   sys_file_size( u32 handle, u32 *size );   /* and back to the start */
int   sys_seek_end( u32 handle );
int   sys_get_attr( const char *path, u16 *attr );
int   sys_find_first( const char *pattern, u16 attrs, FINDREC *rec, u32 *handle );
int   sys_find_next( u32 handle, FINDREC *rec );
void  sys_find_close( u32 handle );
int   sys_get_drive( void );
int   sys_get_cwd( int drv, char *buf );    /* "DIR\SUB", no drive, no lead '\' */
int   sys_truename( const char *path, char *out, int form );
int   sys_is_device( u32 handle );
int   sys_share_active( void );                /* SHARE=ON: file sharing is enforced */

/* ------------------------------------------------------------------ */
/* the machine                                                         */
/* ------------------------------------------------------------------ */

/* PMDOS.INC, as DOSINT.ASM laid it out */
typedef struct {
    u32 lol;                    /* a DOS box's list of lists, flat */
    u32 dosdata;                /* the segment it is in, flat */
    u32 first_mcb;              /* the conventional arena's first block */
    u32 ems_version;            /* BCD */
    u32 dpmi_major, dpmi_minor;
    u32 lastdrive;              /* how many letters there are */
} KFACTS;
extern KFACTS kernel_facts;

/* the first megabyte: 1 if it was read.  A far pointer is seg:off. */
int   sys_peek( u32 addr, void *buf, u32 len );
u8    peek_b( u32 addr );
u16   peek_w( u32 addr );
u32   peek_d( u32 addr );
#define FAR_FLAT( seg, off )    (((u32)(seg) << 4) + (u32)(off))
int   sys_ram_test( u32 addr );             /* 1 = a byte written there is kept */

/* a BIOS call out of the kernel's list of the ones that only ask */
typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u16 es, ds;
    u32 flags;
} BREGS;
int   sys_bios( int vector, BREGS *regs );  /* 1 = it was called */

unsigned __cdecl port_in( unsigned port );
void     __cdecl port_out( unsigned port, unsigned value );
int      __cdecl cpu_flag_holds( u32 mask );
void     __cdecl cpu_id( u32 leaf, u32 *regs );
int      __cdecl cpu_div_keeps_flags( void );
int      __cdecl fpu_is_287( void );
int      __cdecl crit_take( void );

#define EFL_AC          0x00040000UL
#define EFL_ID          0x00200000UL

/* the kernel */
void  sys_version( int *major, int *minor );            /* PM-DOS's own */
void  sys_dos_version( int *major, int *minor, int *oem, u32 *serial );
void  sys_true_version( int *major, int *minor, int *revision, int *flags );
int   sys_boot_drive( void );                           /* 0 = A: */
void  sys_ext_memory( u32 *bios_kb, u32 *top );

#define ARENA_KERNEL    0       /* above 2MB: what native programs are given */
#define ARENA_CONV      1       /* the one a DOS box lives in */
#define ARENA_UPPER     2       /* and its upper memory */
typedef struct {
    u32 sig;                    /* 'M' used, 'Z' free, 'E' the end */
    u32 size;                   /* bytes */
    u32 owner;                  /* an owner id, or a PSP segment below 1MB */
    u32 addr;                   /* the block's header, flat */
} MEMBLK;
int   sys_mem_block( int arena, u32 index, MEMBLK *blk );   /* 0 past the end */

int   sys_ems( u32 *frame_seg, u32 *total_pages, u32 *free_pages );
u32   sys_xms_largest( void );                          /* bytes */

#define FPUF_X87        0x01
#define FPUF_FXSR       0x02
#define FPUF_SSE        0x04
#define FPUF_SSE2       0x08
#define FPUF_MMX        0x10
u32   sys_fpu_flags( void );

/* the volume table: a letter */
#define DRVF_PRESENT    0x01
#define DRVF_FLOPPY     0x02
#define DRVF_DRIVER     0x04
#define DRVF_CDROM      0x08
#define DRVF_IMAGE      0x10
typedef struct {
    u8  bios, type, flags, pidx;
    u32 start, sectors;
} DRVREC;
int   sys_drive_rec( int drv, DRVREC *rec, int *letters );  /* 0 past the table */
int   sys_fat_type( int drv );                              /* 12, 16, 32; 0 */
int   sys_disk_space( int drv, u32 *free_kb, u32 *total_kb );

/* a physical disk */
typedef struct {
    u8  bios, flags;
    u16 spt, heads, cyls;
    u32 total, chs;
} DSKREC;
int   sys_disk_rec( int bios, DSKREC *rec );            /* 1 = there is one */
int   sys_floppy_type( int drive, int *spt, int *cyls, int *heads );
int   sys_cdrom_units( void );

/* the buses */
#define IRQF_KERNEL     0x01
#define IRQF_HOOKED     0x02
#define IRQF_BOXWANT    0x04
#define IRQF_LEVEL      0x20
#define IRQF_OPEN       0x40
#define IRQF_DEAD       0x80
int   sys_irq_stat( int irq, u32 *flags, u32 *taken );
int   sys_pci_here( int *last_bus, int *functions );
int   sys_pci_enum( int index, u32 *addr, u32 *ident, u32 *class_rev );
int   sys_pci_irq( u32 addr, int *line, int *pin );
int   sys_pnp_cards( int *read_port );

/* VBE: 1 and the version (0200h is 2.0) when the video BIOS has it */
int   sys_vbe_info( int *version, char *oem, u32 oem_max, int *memory_64k, int *modes );

#endif
