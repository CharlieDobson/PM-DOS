/*
 * AUDIO.H - a sound card, as a 32-bit driver presents one: the CALL
 *           entry of a driver whose table carries NDTF_AUDIO.
 *
 * ONE SHAPE FOR EVERY CARD.  SBLASTER.SYS, ES1371.SYS and whatever
 * comes after answer the same functions with the same blocks, so a
 * program finds the first loaded driver flagged NDTF_AUDIO (21h/F1h
 * AL=25h, NDF_INFO) and talks to it without knowing which card it is.
 *
 * SAMPLES GO INTO A RING IN THE DRIVER'S MEMORY (AUD_WRITE), and the
 * card's interrupt moves them on into the buffer the card plays from.
 * The ring is made at AUD_OPEN and can hold seconds, so a program that
 * is busy for a while - reading a floppy, redrawing a big screen -
 * does not make the sound stop.
 *
 * EVENTS ARE WRITES TO A SYNTHESIZER AT A GIVEN MOMENT: an FM chip's
 * register or a byte for the MIDI port, each stamped with the frame
 * the card will have played when it is due (AUD_EVENTS).  The driver
 * sends each one from the card's own interrupt, so the timing is the
 * sample clock's and not the program's.  A program playing only
 * events still plays samples - silence, at a low rate - because the
 * samples are the clock.
 *
 * THE CARD IS SHARED WITH DOS PROGRAMS.  Between AUD_CLOSE and the
 * next AUD_OPEN the driver leaves every port alone and claims none of
 * the card's interrupts, so a game in a DOS box finds the card as it
 * would without the driver.  A program that ends without AUD_CLOSE
 * is caught by a watchdog: after ten seconds with no call at all, the
 * driver stops the card, puts the mixer and the MIDI port back and
 * passes the interrupts on.  A program that was only slow gets the
 * card back at its next call, paused where it was or playing again.
 *
 * Every number here is the drivers' and the programs' contract: a
 * change is a new AUD_VERSION.
 */
#ifndef AUDIO_H
#define AUDIO_H

#ifndef NDTF_AUDIO
#define NDTF_AUDIO   0x0040              /* the driver table's flag (NDRV.H) */
#endif

/* THE FUNCTIONS - the fn of an audio driver's CALL entry */
#define AUD_INFO     0        /* buf -> AUD_INFOBLOCK, answer AUDI_LEN.
                                 Finds the card the first time */
#define AUD_OPEN     1        /* buf -> AUD_OPENBLOCK: the format wanted
                                 in, the format given out; answer 0.  The
                                 card is the program's until AUD_CLOSE */
#define AUD_WRITE    2        /* buf -> len bytes of samples for the
                                 ring; answer the bytes taken (whole
                                 frames, perhaps fewer than len) */
#define AUD_STATUS   3        /* buf -> AUD_STATBLOCK, answer AUDS_LEN */
#define AUD_START    4        /* play (again, after AUD_PAUSE) */
#define AUD_PAUSE    5        /* stop where it is; the ring and the
                                 queued events are kept */
#define AUD_STOP     6        /* stop; empty the ring and the event
                                 queue; the frame count starts at 0
                                 again */
#define AUD_CLOSE    7        /* stop, free the ring, and leave the card
                                 to DOS programs */
#define AUD_VOLUME   8        /* buf -> 2 bytes, left and right, 0..255:
                                 an amplitude - 255 is the card's full
                                 level, 128 about 6 dB down, 0 off */
#define AUD_EVENTS   9        /* buf -> AUDEV records (len / 8 of them)
                                 for the queue, in frame order; answer
                                 the records taken */
#define AUD_SENDNOW  10       /* buf -> AUDEV records, sent to the chip
                                 or port now whatever their frames say;
                                 answer the records sent */
#define AUD_DROPEV   11       /* every queued event not yet sent is
                                 dropped (the program then silences the
                                 synthesizer with AUD_SENDNOW) */

#define AUD_SIGVAL   0x49445541UL        /* 'A','U','D','I' */
#define AUD_VERSION  1

/* AUD_INFO's buffer */
typedef struct {
    uint32_t sig;             /* AUD_SIGVAL */
    uint16_t version;         /* AUD_VERSION */
    uint16_t flags;           /* AUDF_* */
    uint32_t rate_min;        /* frames a second the card can play */
    uint32_t rate_max;        /* ...one channel */
    uint32_t rate_max2;       /* ...two channels (lower on some cards) */
    uint8_t  bits;            /* AUDB_*: the sample sizes it plays */
    uint8_t  channels;        /* 1 or 2 */
    uint8_t  irq;             /* the line, or AUD_NONE */
    uint8_t  dma8;            /* ISA DMA channels, or AUD_NONE */
    uint8_t  dma16;
    uint8_t  spare;
    uint16_t port;            /* the card's first port, 0 if none */
    uint16_t fm_port;         /* the FM chip's, 0 if none */
    uint16_t mpu_port;        /* the MIDI port's, 0 if none */
    uint16_t version_hw;      /* the card's own version (a Sound
                                 Blaster's DSP: major * 256 + minor) */
    uint16_t spare2;
    uint32_t pciaddr;         /* BUS_PCIENUM's address, 0 on ISA */
    uint32_t ring_max;        /* the biggest ring AUD_OPEN makes */
    char     name[32];        /* ASCIIZ: "Sound Blaster 16" */
} AUD_INFOBLOCK;

#define AUDI_LEN     76

#define AUDF_FOUND   0x0001   /* a card is there and answers */
#define AUDF_PCM     0x0002   /* it plays samples */
#define AUDF_OPL2    0x0004   /* an OPL2 FM chip */
#define AUDF_OPL3    0x0008   /* an OPL3 (two register sets) */
#define AUDF_MPU     0x0010   /* an MPU-401 MIDI port, UART mode */
#define AUDF_MIXER   0x0020   /* AUD_VOLUME sets the card's own mixer */
#define AUDF_ANYRATE 0x0040   /* every rate in the range is exact; else
                                 AUD_OPEN gives the nearest it has */
#define AUDF_BUSY    0x0080   /* a program has the card open */

#define AUDB_8       0x01     /* 8 bits, unsigned */
#define AUDB_16      0x02     /* 16 bits, signed, little-endian */

#define AUD_NONE     0xFF

/* AUD_OPEN's buffer: what is wanted goes in, what the card does comes
   back.  Samples are interleaved, left first. */
typedef struct {
    uint32_t rate;            /* frames a second */
    uint8_t  bits;            /* 8 or 16 */
    uint8_t  channels;        /* 1 or 2 */
    uint16_t spare;
    uint32_t ring;            /* bytes of ring wanted, 0 for the driver's
                                 choice; out: the ring made */
    uint32_t block;           /* bytes the card plays between two
                                 interrupts, 0 for the driver's choice -
                                 smaller times events more finely; out:
                                 what was set */
    uint32_t events;          /* event records the queue should hold, 0
                                 for none; out: what it holds */
} AUD_OPENBLOCK;

#define AUDO_LEN     20

/* AUD_STATUS's buffer */
typedef struct {
    uint32_t played;          /* frames played since AUD_OPEN or AUD_STOP
                                 (counts on past 2^32) */
    uint32_t queued;          /* bytes in the ring not yet played */
    uint32_t room;            /* bytes AUD_WRITE would take now */
    uint32_t underruns;       /* times the ring ran dry while playing,
                                 since AUD_OPEN */
    uint32_t irqs;            /* the card's interrupts */
    uint32_t ev_queued;       /* events waiting */
    uint32_t ev_room;         /* events AUD_EVENTS would take now */
    uint8_t  state;           /* AUDST_* */
    uint8_t  spare[3];
} AUD_STATBLOCK;

#define AUDS_LEN     32

#define AUDST_CLOSED  0
#define AUDST_IDLE    1       /* open, not started (or stopped) */
#define AUDST_PLAYING 2
#define AUDST_PAUSED  3
#define AUDST_DRY     4       /* started, but the ring ran dry: playing
                                 silence until more comes */

/* An event: 8 bytes, the layout of MPLAY's DECEV */
typedef struct {
    uint32_t frame;           /* due when "played" reaches this */
    uint8_t  kind;            /* AUDE_* */
    uint8_t  reg;             /* AUDE_OPL: the register */
    uint8_t  val;             /* AUDE_OPL: the value; AUDE_MPU: the byte */
    uint8_t  bank;            /* AUDE_OPL: 0, or 1 for the OPL3's second
                                 register set */
} AUDEV;

#define AUDE_OPL     1
#define AUDE_MPU     2

#endif /* AUDIO_H */
