/*
 * PMDOS.H - PM-DOS itself, for a native program
 *
 * A native PM-DOS program is a D32 image: 32-bit code that runs at
 * ring 3 in a slot of its own, where every pointer is an offset from
 * the start of the slot.  The kernel's services are INT 21h - the
 * MS-DOS calls, widened so that pointers and counts are 32-bit
 * registers - plus INT 33h for the mouse.
 *
 * The C library covers what most programs need.  This header is for
 * the rest: a call the library does not make for you, or a PM-DOS
 * service with no C library equivalent.
 */
#ifndef _PMDOS_H
#define _PMDOS_H

#include <_pmdefs.h>
#include <stdint.h>

_PM_BEGIN_C

/*---------------------------------------------------------------------
 * The registers in and out of an interrupt.  Bit 0 of flags is the
 * carry flag: on the way in it is what the carry is set to before the
 * call (a few calls read it), on the way out what the call left.
 *-------------------------------------------------------------------*/
typedef struct pm_regs {
    uint32_t eax, ebx, ecx, edx, esi, edi;
    uint32_t flags;
} pm_regs;

#define PM_CF( regs )   ((int)((regs).flags & 1))
#define PM_AX( regs )   ((unsigned)((regs).eax & 0xFFFF))

/* Issue INT 21h or INT 33h with the registers in *regs and store what
   comes back there.  Returns the carry flag: 0 or 1. */
int _PM_ASMCALL pm_int21( pm_regs *regs );
int _PM_ASMCALL pm_int33( pm_regs *regs );

/* The DOS error code of the last call the library made that failed,
   as INT 21h gave it in AX.  errno has the C library's name for it. */
extern int _doserrno;

/* Map a DOS error code to an errno value. */
int pm_errno_of( int doserr );

/*---------------------------------------------------------------------
 * The program's slot.  Offset 0 is a page that is not there, so a
 * null pointer faults; the PSP, the environment and the image follow.
 *-------------------------------------------------------------------*/
#define PM_SLOT_PSP     0x1000UL        /* the PSP, 100h bytes */
#define PM_SLOT_ENV     0x1100UL        /* the environment, up to 0F00h */
#define PM_SLOT_IMAGE   0x2000UL        /* the D32 header, then the code */

/* The PSP, as the program sees it. */
#define pm_psp()        ((unsigned char *)PM_SLOT_PSP)

/*---------------------------------------------------------------------
 * The kernel.
 *-------------------------------------------------------------------*/

/* PM-DOS's own version (21h/F1h AL=0), as major * 100 + minor: 67 is
   0.67.  INT 21h AH=30h reports DOS 5 for the sake of older programs. */
int pm_version( void );

/* The D32 header, at PM_SLOT_IMAGE.  A program can read its own. */
typedef struct pm_d32_header {
    uint8_t  sig[2];                    /* 'C','D' */
    uint8_t  int20[2];                  /* CDh,20h */
    uint8_t  version;                   /* 1 */
    uint8_t  type;                      /* 0 = program, 1 = driver */
    uint16_t spare;
    uint32_t base;                      /* 2000h for a program */
    uint32_t entry;
    uint32_t bss;                       /* bytes zeroed after the image */
    uint32_t stack;                     /* stack size asked for */
    uint32_t reloc;                     /* drivers only */
    uint32_t nreloc;
} pm_d32_header;

#define pm_d32_self()   ((const pm_d32_header *)PM_SLOT_IMAGE)

_PM_END_C

#endif
