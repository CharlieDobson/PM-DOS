/*
 * NETDRV.H - the shared base for a PM-DOS network card driver
 *
 * NE2000.SYS was written out whole, by hand, as the first card driver
 * and the proof of the contract (INC\NIC.INC).  Every card after it is
 * written on this base instead: the parts that are the same for every
 * card - the receive queue, the counts, the NIC_ functions (with the
 * bindings NETBEUI.SYS rides on: NIC_BIND, NIC_RECVB), the DEVICE= line, finding a card on the PCI bus, the port and memory
 * instructions - are here and in NETCORE.C, and a card's own source
 * provides only what is particular to its chip.
 *
 * A CARD'S SOURCE provides, and NETCORE.C calls:
 *   int  card_find( void )     0 and the globals below are filled, or
 *                              nonzero: there is no such card
 *   int  card_setup( void )    the rings and the registers, started; 0
 *                              ok, nonzero a card that would not start
 *   void card_stop( void )     quiet, for a park or a halt
 *   int  card_send( const uint8_t *frame, unsigned int len )   1 if the card took it
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

uint8_t  inb( uint16_t port );
#pragma aux inb = "in al,dx" parm [dx] value [al] modify exact [al];
uint16_t inw( uint16_t port );
#pragma aux inw = "in ax,dx" parm [dx] value [ax] modify exact [ax];
uint32_t inl( uint16_t port );
#pragma aux inl = "in eax,dx" parm [dx] value [eax] modify exact [eax];
void     outb( uint16_t port, uint8_t value );
#pragma aux outb = "out dx,al" parm [dx] [al] modify exact [];
void     outw( uint16_t port, uint16_t value );
#pragma aux outw = "out dx,ax" parm [dx] [ax] modify exact [];
void     outl( uint16_t port, uint32_t value );
#pragma aux outl = "out dx,eax" parm [dx] [eax] modify exact [];
void     insb( uint16_t port, void *buf, unsigned int bytes );
#pragma aux insb = "rep insb" parm [dx] [edi] [ecx] modify exact [edi ecx];
void     insw( uint16_t port, void *buf, unsigned int words );
#pragma aux insw = "rep insw" parm [dx] [edi] [ecx] modify exact [edi ecx];
void     outsb( uint16_t port, const void *buf, unsigned int bytes );
#pragma aux outsb = "rep outsb" parm [dx] [esi] [ecx] modify exact [esi ecx];
void     outsw( uint16_t port, const void *buf, unsigned int words );
#pragma aux outsw = "rep outsw" parm [dx] [esi] [ecx] modify exact [esi ecx];
uint32_t int_off( void );
#pragma aux int_off = "pushfd" "pop eax" "cli" value [eax] modify exact [eax];
void     int_back( uint32_t flags );
#pragma aux int_back = "push eax" "popfd" parm [eax] modify exact [];

/* the memory of a card whose registers or buffers are memory-mapped:
   the identity map means a physical address is reachable as it is */
#define MMIO8( base, off )     (*(volatile uint8_t  *)( (base) + (off) ))
#define MMIO16( base, off )    (*(volatile uint16_t *)( (base) + (off) ))
#define MMIO32( base, off )    (*(volatile uint32_t *)( (base) + (off) ))

/* ---------------------------------------------------------------- */
/* the globals a card fills at card_find()                           */
/* ---------------------------------------------------------------- */

extern uint16_t net_port;       /* the first I/O port, or 0 */
extern uint32_t net_mem;        /* a memory base, or 0 */
extern uint8_t  net_irq;        /* the line, or NIC_NOIRQ (polled) */
extern uint8_t  net_bus;        /* NICB_* */
extern uint8_t  net_flags;      /* NICF_* */
extern uint32_t net_pciaddr;    /* the PCI function, or 0 on ISA */
extern uint8_t  net_mac[6];
extern char     net_name[32];

/* the options on the DEVICE= line */
extern uint32_t net_opt_port;   /* /P: in hex, 0 = none */
extern uint32_t net_opt_mem;    /* /M: in hex, 0 = none */
extern uint8_t  net_opt_irq;    /* /I:, NIC_NOIRQ = none */
extern unsigned char net_opt_poll; /* /POLL */
extern const char *net_line;    /* the whole DEVICE= line, for a card's own
                                   switches: set while card_find and the
                                   first card_setup run, 0 after */

/* ---------------------------------------------------------------- */
/* what NETCORE.C gives a card's source                              */
/* ---------------------------------------------------------------- */

/* the receive queue: where to put the next frame (never 0, the spare
   slot when the queue is full), then that it holds one of len bytes */
uint8_t *net_rx_begin( void );
void     net_rx_commit( unsigned int len );

extern NIC_STATBLOCK net_stats;         /* the card bumps the error and
                                           overflow fields; the core keeps
                                           rx/tx frames, bytes, dropped */

void    *net_alloc( unsigned int bytes );   /* the kernel's, never reclaimed */
void     net_free( void *block );
void     net_print( const char *text );
uint32_t net_ticks( void );
void     net_delay_us( unsigned int usec ); /* at least that long, by the PIT */

/* the PCI bus, for a card that lives on it.  net_pci_find takes a list
   of (vendor, device) pairs ended by {0,0} and returns the first
   function that is one of them, with net_pciaddr set, or 0 */
typedef struct { uint16_t vendor, device; } PCI_ID;
uint32_t net_pci_find( const PCI_ID *ids, const PCI_ID **which );
uint32_t net_pci_bar( uint32_t addr, int bar, int *is_io );  /* a base, 0 if none or above 4GB */
uint32_t net_pci_map( uint32_t phys, unsigned int bytes );   /* device memory, reachable; 0 if not */
int      net_pci_irqline( uint32_t addr );                   /* a line, or NIC_NOIRQ */
void     net_pci_enable( uint32_t addr );                    /* I/O, memory, bus master on */
uint32_t net_pci_cfg_read( uint32_t addr, int reg, int bytes );
void     net_pci_cfg_write( uint32_t addr, int reg, int bytes, uint32_t value );

/* for an ISA card that finds its line by making the card interrupt, in
   card_find while net_line is set (interrupts are on then, and only
   then): set net_probe_isr to a function that answers 1 - and quiets
   the card - when the card is interrupting, call net_irq_probe_begin
   (every line it could be on is hooked), make the card interrupt, wait
   a few ticks for net_probe_hit, and call net_irq_probe_end, which
   keeps the line that was hit hooked, drops the others and sets
   net_irq.  NETCORE then runs the card on that line, unless /I: or
   /POLL says otherwise.  Lines are the usual NE2000 set */
extern volatile unsigned char net_probing;
extern volatile uint8_t net_probe_hit;
extern int ( *net_probe_isr )( void );
void net_irq_probe_begin( void );
void net_irq_probe_end( void );

/* what the card's source provides */
int  card_find( void );
int  card_setup( void );
void card_stop( void );
int  card_send( const uint8_t *frame, unsigned int len );
int  card_service( void );

/* ---------------------------------------------------------------- */
/* what a library would have supplied - defined in NETCORE.C, so a    */
/* file that does not use one is not warned about it (-we would make  */
/* an unused static fatal)                                            */
/* ---------------------------------------------------------------- */

void net_memcpy( void *to, const void *from, unsigned int len );
void net_memzero( void *to, unsigned int len );
void net_strcpy( char *to, const char *from );
int  net_mac_ok( const uint8_t *mac );          /* a usable station address */

#endif /* NETDRV_H */
