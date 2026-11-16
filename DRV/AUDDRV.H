/*
 * AUDDRV.H - what DRV\AUDCORE.C, the part every sound card driver
 *            shares, asks of a card's own file and gives it.
 *
 * THE CORE IS THE DRIVER'S FACE: the CALL functions of INC\AUDIO.H,
 * the ring the program writes samples into, the queue of timed
 * events, and the interrupt and clock entries.  A CARD FILE
 * (SBLASTER.C, ES1371.C) knows its chip: how to find it, how to make
 * it play the DMA buffer round and round in blocks with an interrupt
 * after each, where in the buffer it is, and how to write to its FM
 * chip and MIDI port.  The card file also provides nd_init, because
 * only it knows what the DEVICE= line can say.
 *
 * THE DMA BUFFER IS PLAYED AS A RING OF BLOCKS.  The core fills every
 * block before the card starts; the card interrupts at the end of
 * each block, the core refills that block from the ring and counts
 * the frames as played.  card_open chooses the block size and how
 * many blocks, so that blocks * block fits the buffer.
 */
#ifndef AUDDRV_H
#define AUDDRV_H

/* ---- the processor, in ring 0 ---------------------------------- */
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
u32  int_off( void );
#pragma aux int_off = "pushfd" "pop eax" "cli" value [eax] modify exact [eax];
void int_back( u32 flags );
#pragma aux int_back = "push eax" "popfd" parm [eax] modify exact [];

/* ---- what the core keeps, for the card file to read and set ---- */
extern AUD_INFOBLOCK aud_info;          /* the card file fills it in card_find */
extern u8  *dma_buf;                    /* the card plays from here: linear */
extern u32  dma_phys;                   /* ...physical */
extern u32  dma_size;                   /* bytes there are */
extern u32  blk_bytes;                  /* card_open sets: a block */
extern u32  blk_count;                  /* ...and how many (2 to AUD_MAXBLK) */
extern u32  frame_bytes;                /* the core sets from the format */

#define AUD_MAXBLK   32

/* the core's own helpers for the card file */
void  aud_print( const char *text );            /* nds_print */
void  aud_say_card( const char *driver );       /* "NAME.SYS: Sound Blaster 16 at 220h..." */
u32   aud_ticks( void );
void  aud_delay_ticks( u32 ticks );             /* at INIT and CALL only */
int   aud_alloc_dma( u32 bytes, u32 limit );    /* the buffer: in one piece and below "limit"
                                                   (physical), not across a 64K line when
                                                   limit is 16MB; 1 if it was had */
void  mem_zero( void *dst, u32 len );
void  mem_copy( void *dst, const void *src, u32 len );
int   mem_same( const void *lhs, const void *rhs, u32 len );
u32   str_len( const char *str );
void  str_cpy( char *dst, const char *src );
void  str_cat( char *dst, const char *src );
void  str_hex( char *dst, u32 value, int digits );     /* appended */
void  str_dec( char *dst, u32 value );                 /* appended */
u32   num_after( const char *text, int hex, u32 none );
u32   aud_atten( u8 level );             /* AUD_VOLUME's level as an attenuation in
                                           half decibels: 0 for 255, 255 for 0 (off) */

/* ---- what the card file provides ------------------------------- */
int   card_find( void );                /* 1: the card is there; aud_info filled */
int   card_open( AUD_OPENBLOCK *fmt );  /* the nearest format it plays into *fmt,
                                           blk_bytes and blk_count chosen (the core
                                           passes the block wanted in fmt->block);
                                           0, or an ERR_ */
void  card_start( void );               /* play from the start of the buffer */
void  card_halt( void );                /* stop now */
int   card_pause( void );               /* stop where it is: 1 if card_resume can go on */
void  card_resume( void );
void  card_close( void );               /* the card as DOS programs expect it again */
u32   card_dma_pos( void );             /* the byte in the buffer it plays next */
int   card_irq( void );                 /* CARD_* below */
void  card_volume( u8 left, u8 right );
void  card_opl( u8 bank, u8 reg, u8 val );      /* AUDF_OPL2/3 */
int   card_mpu_put( u8 byte );          /* AUDF_MPU: 1 sent, 0 not ready yet */
void  card_mpu_drain( void );           /* read and drop what the MIDI port received */
void  card_park( int back );            /* Windows takes the machine, and gives it back */
int   card_probe_irq( u32 line );       /* while no program has the card: an interrupt
                                           the card file may be waiting for (finding
                                           its line); 1 if it was */

#define CARD_NOTMINE  0                 /* card_irq: not the card's */
#define CARD_BLOCK    1                 /* a block ended (acknowledged) */
#define CARD_OTHER    2                 /* the card's, but not a block end */

/* the core's entries the card file calls from nd_init */
int   aud_core_init( void );            /* the core's memory; 0 if there is none */

#endif /* AUDDRV_H */
