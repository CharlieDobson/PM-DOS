/*
 * NETDRV.H - the shared base for a PM-DOS network card driver
 *
 * NE2000.SYS was written out whole, by hand, as the first card driver
 * and the proof of the contract (INC\NIC.INC).  Every card after it is
 * written on this base instead: the parts that are the same for every
 * card - the receive queue, the counts, the four NIC_ functions, the
 * DEVICE= line, finding a card on the PCI bus, the port and memory
 * instructions - are here and in NETCORE.C, and a card's own source
 * provides only what is particular to its chip.
 *
 * A CARD'S SOURCE provides, and NETCORE.C calls:
 *   int  card_find( void )     0 and the globals below are filled, or
 *                              nonzero: there is no such card
 *   int  card_setup( void )    the rings and the registers, started; 0
 *                              ok, nonzero a card that would not start
 *   void card_stop( void )     quiet, for a park or a halt
 *   int  card_send( const u8 *frame, u32 len )   1 if the card took it
 *   int  card_service( void )  what the card has to say, answered;
 *                              drains arrived frames into the queue
 *                              with net_rx_*; 1 if any was the card's
 * and a table (ND_TABLE) with its own name, pointing at NDSTART.ASM's
 * four ndv_ entries.
 *
 * DMA IS THE KERNEL'S OWN POINTERS.  PM-DOS is identity-mapped above
 * the megabyte (IO\PAGING.INC): a linear address is a physical one, so
 * a bus-master card is handed the address net_alloc returned, as the
 * kernel's USB drivers hand theirs.  net_alloc's memory is contiguous
 * and never moves.
 *
 * Compiled: wcc386 -mf -3r -s -zl, behind DRV\NDSTART.ASM and NETCORE.C.
 */
#ifndef NETDRV_H
#define NETDRV_H

#define ND_DRIVER
#include "ndrv.h"
#include "nic.h"

/* ---------------------------------------------------------------- */
/* the processor: ports (8, 16 and 32 bits) and the interrupt flag   */
/* ---------------------------------------------------------------- */

u8   inb( u16 port );
#pragma aux inb = "in al,dx" parm [dx] value [al] modify exact [al];
u16  inw( u16 port );
#pragma aux inw = "in ax,dx" parm [dx] value [ax] modify exact [ax];
u32  inl( u16 port );
#pragma aux inl = "in eax,dx" parm [dx] value [eax] modify exact [eax];
void outb( u16 port, u8 value );
#pragma aux outb = "out dx,al" parm [dx] [al] modify exact [];
void outw( u16 port, u16 value );
#pragma aux outw = "out dx,ax" parm [dx] [ax] modify exact [];
void outl( u16 port, u32 value );
#pragma aux outl = "out dx,eax" parm [dx] [eax] modify exact [];
void insb( u16 port, void *buf, u32 bytes );
#pragma aux insb = "rep insb" parm [dx] [edi] [ecx] modify exact [edi ecx];
void insw( u16 port, void *buf, u32 words );
#pragma aux insw = "rep insw" parm [dx] [edi] [ecx] modify exact [edi ecx];
void outsb( u16 port, const void *buf, u32 bytes );
#pragma aux outsb = "rep outsb" parm [dx] [esi] [ecx] modify exact [esi ecx];
void outsw( u16 port, const void *buf, u32 words );
#pragma aux outsw = "rep outsw" parm [dx] [esi] [ecx] modify exact [esi ecx];
u32  int_off( void );
#pragma aux int_off = "pushfd" "pop eax" "cli" value [eax] modify exact [eax];
void int_back( u32 flags );
#pragma aux int_back = "push eax" "popfd" parm [eax] modify exact [];

/* the memory of a card whose registers or buffers are memory-mapped:
   the identity map means a physical address is reachable as it is */
#define MMIO8( base, off )     (*(volatile u8  *)( (base) + (off) ))
#define MMIO16( base, off )    (*(volatile u16 *)( (base) + (off) ))
#define MMIO32( base, off )    (*(volatile u32 *)( (base) + (off) ))

/* ---------------------------------------------------------------- */
/* the globals a card fills at card_find()                           */
/* ---------------------------------------------------------------- */

extern u16  net_port;           /* the first I/O port, or 0 */
extern u32  net_mem;            /* a memory base, or 0 */
extern u8   net_irq;            /* the line, or NIC_NOIRQ (polled) */
extern u8   net_bus;            /* NICB_* */
extern u8   net_flags;          /* NICF_* */
extern u32  net_pciaddr;        /* the PCI function, or 0 on ISA */
extern u8   net_mac[6];
extern char net_name[32];

/* the options on the DEVICE= line */
extern u32  net_opt_port;       /* /P: in hex, 0 = none */
extern u32  net_opt_mem;        /* /M: in hex, 0 = none */
extern u8   net_opt_irq;        /* /I:, NIC_NOIRQ = none */

/* ---------------------------------------------------------------- */
/* what NETCORE.C gives a card's source                              */
/* ---------------------------------------------------------------- */

/* the receive queue: where to put the next frame (never 0, the spare
   slot when the queue is full), then that it holds one of len bytes */
u8  *net_rx_begin( void );
void net_rx_commit( u32 len );

extern NIC_STATBLOCK net_stats;         /* the card bumps the error and
                                           overflow fields; the core keeps
                                           rx/tx frames, bytes, dropped */

void *net_alloc( u32 bytes );           /* the kernel's, never reclaimed */
void  net_free( void *block );
void  net_print( const char *text );
u32   net_ticks( void );

/* the PCI bus, for a card that lives on it.  net_pci_find takes a list
   of (vendor, device) pairs ended by {0,0} and returns the first
   function that is one of them, with net_pciaddr set, or 0 */
typedef struct { u16 vendor, device; } PCI_ID;
u32  net_pci_find( const PCI_ID *ids, const PCI_ID **which );
u32  net_pci_bar( u32 addr, int bar, int *is_io );   /* a base, 0 if none */
u32  net_pci_map( u32 phys, u32 bytes );             /* device memory, reachable; 0 if not */
int  net_pci_irqline( u32 addr );                    /* a line, or NIC_NOIRQ */
void net_pci_enable( u32 addr );                     /* I/O, memory, bus master on */
u32  net_pci_cfg_read( u32 addr, int reg, int bytes );
void net_pci_cfg_write( u32 addr, int reg, int bytes, u32 value );

/* for an ISA card that finds its line by making the card interrupt:
   hook every line it could be on, the caller makes it interrupt, and
   the one the handler was called on is kept.  net_irq_probe_begin
   returns the count hooked; the card sets net_probe_hit from its ISR
   (net_probing is nonzero meanwhile); net_irq_probe_end keeps the hit
   line hooked and drops the others.  Lines are the usual NE2000 set */
extern volatile u8 net_probing;
extern volatile u8 net_probe_hit;
void net_irq_probe_begin( void );
void net_irq_probe_end( void );

/* what the card's source provides */
int  card_find( void );
int  card_setup( void );
void card_stop( void );
int  card_send( const u8 *frame, u32 len );
int  card_service( void );

/* ---------------------------------------------------------------- */
/* what a library would have supplied - defined in NETCORE.C, so a    */
/* file that does not use one is not warned about it (-we would make  */
/* an unused static fatal)                                            */
/* ---------------------------------------------------------------- */

void net_memcpy( void *to, const void *from, u32 len );
void net_memzero( void *to, u32 len );
void net_strcpy( char *to, const char *from );
int  net_mac_ok( const u8 *mac );               /* a usable station address */

#endif /* NETDRV_H */
