/*
 * NDRV.H - a 32-bit DEVICE= driver, for C
 *
 * The C face of INC\NDRV.INC: the table a driver begins with, the
 * kernel's row for it, the functions 21h/F1h AL=25h answers - and,
 * for a driver whose body is C, what DRV\NDSTART.ASM asks of that
 * body and what it gives back.  Every number here is NDRV.INC's; a
 * change there is a change here.
 *
 * A DRIVER IN C defines ND_DRIVER before including this, provides the
 * six nd_* functions and the table, and is linked behind NDSTART.ASM
 * as BUILD.CMD does for NE2000.SYS.  Its memory is the kernel's and
 * its code runs in ring 0 with interrupts wherever the kernel left
 * them: on at INIT and CALL, off in nd_irq and nd_tick.  It may call
 * nothing but the nds_* functions below, and nds_alloc, nds_free and
 * nds_print at INIT and CALL time only.
 *
 * A PROGRAM that asks the kernel about drivers includes this for the
 * row and the NDF_* numbers; REGS is the register block every C
 * native's INT 21h thunk takes.
 */
#ifndef NDRV_H
#define NDRV_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

/* the registers in and out of an INT 21h or a service with a register
   contract: flags bit 0 is CF - on the way in, what to set it to */
typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;
} REGS;

#define CF( regs )   ((regs).flags & 1)

/* THE DRIVER TABLE - what the D32 header's entry field names */
#define NDT_SIGVAL   0x5652444EL         /* 'N','D','R','V' */
#define NDT_VERSION  1
#define NDTF_NIC     0x0001              /* CALL answers INC\NIC.H */
#define NDTF_STACK   0x0002              /* CALL answers INC\SOCKET.H */
#define NDTF_REDIR   0x0004              /* CALL answers INC\REDIR.H */
#define NDTF_DISK    0x0010              /* units are drive letters */
#define NDT_DISKVER  2                   /* the version a disk driver's
                                            table carries */
#define NDUF_MEMORY  0x01                /* a unit that is memory */

/* A DISK DRIVER's table goes on past ND_TABLE with a unit count and a
   pointer to its units, and each unit is an NDU_* record (NDRV.INC).
   Not declared here: the unit's transfer entry has a register contract
   NDSTART.ASM has no C bridge for, and RAMDISK.SYS, the one disk driver
   so far, is assembly.  NOR IS A TYPEDEF ADDED FOR IT BEFORE ONE IS
   NEEDED: a new type in this header re-orders the data Watcom lays out
   in every module that includes it, and every card driver and network
   program changes by a byte here and there for nothing */

typedef struct {
    u32  sig;
    u16  ver;
    u16  flags;
    char name[8];                        /* blank padded, no NUL */
    void (__cdecl *init)( void );
    void (__cdecl *park)( void );
    void (__cdecl *halt)( void );
    void (__cdecl *call)( void );
} ND_TABLE;

/* THE KERNEL'S ROW for a loaded driver (ND_*) */
typedef struct {
    char name[8];
    u32  base;                           /* where the image is */
    u32  size;                           /* bytes kept */
    u32  table;                          /* its driver table */
    u32  flags;                          /* the table's NDT_FLAGS */
    u32  parks, unparks;
} ND_ROW;

#define ND_MAX       8

/* 21h/F1h AL=25h: EAX = (NDF_x << 16) | 0xF125, DL = a row */
#define NDF_COUNT    0                   /* -> ECX = loaded, EAX = ND_MAX */
#define NDF_INFO     1                   /* EDI -> an ND_ROW */
#define NDF_CALL     2                   /* EBX = fn, EDI -> ECX bytes */
#define NDF_MEM      3                   /* -> ECX = bytes NDS_ALLOC'd,
                                            EDX = blocks */

/* the errors a CALL entry answers with (PMDOS.INC's ERR_*) */
#define ERR_FUNC     1
#define ERR_BADDATA  13
#define ERR_BADDRIVE 15
#define ERR_BADLEN   0x18
#define ERR_GENERAL  0x1F

/* IOS_BUS and 21h/F1h AL=12h functions (PMDOS.INC's BUS_*): in AH for
   nds_bus, in bits 16-23 of EAX for a program */
#define BUS_PCIHERE  0x00
#define BUS_PCIENUM  0x01
#define BUS_PCICLASS 0x02
#define BUS_PCIID    0x03
#define BUS_PCIREAD  0x04
#define BUS_PCIWRITE 0x05
#define BUS_PCIBAR   0x06
#define BUS_PCIIRQ   0x07
#define BUS_IRQSTAT  0x20
#define BUS_PNPHERE  0x30
#define BUS_PNPRES   0x31
#define BUS_PNPREGS  0x32
#define BUS_PNPSET   0x33

#define BARF_IO      0x01

#define IRQF_KERNEL  0x01                /* BUS_IRQSTAT: the kernel's own */
#define IRQF_HOOKED  0x02                /* a driver has it */
#define IRQF_OPEN    0x40                /* unmasked now */
#define IRQF_DEAD    0x80                /* masked for good */

#ifdef ND_DRIVER

/* WHAT THE C SIDE PROVIDES to NDSTART.ASM */
int  __cdecl nd_init( const char *line );    /* 0: installed; else refused */
void __cdecl nd_park( int back );            /* 0 going down, 1 come back */
void __cdecl nd_halt( void );
int  __cdecl nd_call( u32 fn, void *buf, u32 len, u32 *answer );
                                             /* 0: EAX = *answer; else ERR_* */
int  __cdecl nd_irq( u32 line );             /* 1: the device was asking */
void __cdecl nd_tick( void );

/* the entries the table names: NDSTART.ASM's, which call the above */
void __cdecl ndv_init( void );
void __cdecl ndv_park( void );
void __cdecl ndv_halt( void );
void __cdecl ndv_call( void );

/* WHAT NDSTART.ASM GIVES IT: the kernel's services (NDS_*) */
void *__cdecl nds_alloc( u32 bytes );        /* 0 if none; never reclaimed */
void  __cdecl nds_free( void *block );
void  __cdecl nds_print( const char *text ); /* ASCIIZ, to the console */
u32   __cdecl nds_ticks( void );             /* the 18.2Hz count */
int   __cdecl nds_bus( REGS *regs );         /* AH = BUS_*; 1 if CF clear */
int   __cdecl nds_irqhook( u32 line );       /* nd_irq on it; 1 if hooked */
int   __cdecl nds_irqunhook( u32 line );
int   __cdecl nds_tickhook( void );          /* nd_tick every tick; 1 if hooked */
int   __cdecl nds_tickunhook( void );

/* the TCP/IP stack rides on the NIC drivers: these reach them, the same
   way a program reaches a driver through 21h/F1h AL=25h NDF_CALL */
u32   __cdecl nds_niccount( void );          /* loaded drivers that are NICs */
int   __cdecl nds_niccall( u32 idx, u32 fn, void *buf, u32 len, u32 *answer );
                                             /* 0: EAX = *answer; else ERR_* */

/* a redirector reaches the one TCP/IP stack the same way: EBX = a socket
   function (INC\SOCKET.H), the request buffer is the driver's own memory */
int   __cdecl nds_sockcall( u32 fn, void *buf, u32 len, u32 *answer );
                                             /* 0: EAX = *answer; else ERR_* */

extern ND_ROW *nds_row;                      /* the kernel's row for us */

#endif /* ND_DRIVER */

#endif /* NDRV_H */
