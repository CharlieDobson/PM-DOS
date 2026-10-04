/*
 * ISAPNP.C - ISAPNP.EXE for PM-DOS: the ISA Plug and Play cards in the
 * machine, what they can be set to, and switching them on.
 *
 * A PLUG AND PLAY CARD STARTS WITH EVERY DEVICE ON IT OFF.  A machine
 * with a Plug and Play BIOS switches them on before DOS starts; on one
 * without - a 486 with ISA slots - a program run at boot does it, and
 * that was the card maker's: Creative's CTCM, Intel's ICU.  The kernel
 * numbers the cards at boot and switches on the one device it needs
 * itself, an IDE port for a CD-ROM (IO\ISAPNP.INC), and leaves the rest
 * off.  This is the rest.
 *
 *   ISAPNP          every card, every device on it, and what it is
 *                   using now.
 *   ISAPNP /A       every device that is off is given resources and
 *                   switched on: the first of the sets it offers in
 *                   which nothing collides with anything this can see.
 *   ISAPNP c.d ...  card c's device d given what is on the line - IO=
 *                   ports, IRQ=, DMA=, MEM= - and switched on; or just
 *                   ON or OFF.
 *
 * WHAT IS KNOWN TO BE IN USE, for /A, is everything that can be found
 * out without risk: the ports every AT has; the serial and printer
 * ports the BIOS found, and their lines; the lines PCI cards were routed
 * to; the lines the kernel keeps and the ones a resident driver has
 * unmasked; the DMA channels the kernel keeps; and whatever the other
 * Plug and Play devices are already using.  An old card that is not
 * Plug and Play is not on that list and cannot be: give its resources
 * to a device by hand instead, with c.d.
 *
 * The kernel's own IDE port is left alone by /A.  Memory is never
 * chosen by /A - a device that needs a window of it is reported, and
 * MEM= gives it one.
 *
 * Every number on the command line and in the listing is hexadecimal
 * for a port or an address and decimal for an IRQ or a DMA channel,
 * which is how BLASTER= and every card's manual write them.
 *
 * No C library.  DOSINT.ASM is the startup and the INT 21h thunk; the
 * few routines a library would have supplied are at the bottom.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );

#define CF( regs )   ((regs).flags & 1)
#define TRUE         1
#define FALSE        0
#define NULL         ((void *)0)

/* the kernel's bus calls: 21h AX=F112h, the function in bits 16-23 */
#define BUS_PCIHERE  0x00
#define BUS_PCIENUM  0x01
#define BUS_PCIIRQ   0x07
#define BUS_IRQSTAT  0x20
#define BUS_DMASTAT  0x21
#define BUS_PNPHERE  0x30
#define BUS_PNPRES   0x31
#define BUS_PNPREGS  0x32
#define BUS_PNPSET   0x33

#define IRQF_KERNEL  0x01
#define IRQF_HOOKED  0x02
#define IRQF_BOXWANT 0x04

/* the probe log (21h/F1h AL=15h): which device the kernel switched on */
#define PLOG_LEN     128
#define PLP_FIRST    112
#define PLP_STAGE    0
#define PLP_CSN      8
#define PLP_LD       9

/* a logical device's registers */
#define R_ACT        0x30
#define R_MEM0       0x40       /* four of 8 bytes: base 23-16, 15-8, ... */
#define R_IO0        0x60       /* eight of 2: high byte, low byte */
#define R_IRQ0       0x70       /* two of 2: the line, its type */
#define R_DMA0       0x74       /* two of 1: the channel, 4 = none */

#define MAX_CARDS    8
#define MAX_DEVS     8
#define MAX_FUNCS    8
#define MAX_DESC     8
#define MAX_IO       8
#define MAX_IRQ      2
#define MAX_DMA      2
#define MAX_MEM      4
#define RES_BUF      4096
#define MAX_USED     64
#define OUT_BUF      4096

#define RK_IO        1
#define RK_IRQ       2
#define RK_DMA       3
#define RK_MEM       4

/* one thing a device needs */
typedef struct {
    u8  kind;                   /* RK_* */
    u8  ports;                  /* I/O: how many */
    u8  decode16;               /* ...and it decodes all sixteen bits */
    u16 mask;                   /* IRQ: lines, DMA: channels */
    u32 low, high;              /* I/O, memory: lowest and highest base */
    u32 step;                   /* ...and the distance between bases */
    u32 size;                   /* memory: bytes */
} DESC;

/* one set of them */
typedef struct {
    int  count;
    DESC desc[MAX_DESC];
} OPTION;

typedef struct {
    u8     number;              /* the logical device's */
    u8     id[4];               /* EISA ID */
    u8     compat[4];           /* the first compatible ID, or zeroes */
    char   name[48];
    OPTION before;              /* outside any dependent function, */
    OPTION after;               /* before them and after them */
    int    nfunc;
    OPTION func[MAX_FUNCS];     /* the alternatives */
    u8     regs[256];           /* as they are now */
} DEVICE;

typedef struct {
    u8     csn;
    u8     ident[9];
    char   name[48];
    int    ndev;
    DEVICE dev[MAX_DEVS];
} CARD;

/* what a device has been given, or is to be */
typedef struct {
    int nio, nirq, ndma, nmem;
    u16 io[MAX_IO];
    u8  irq[MAX_IRQ];
    u8  dma[MAX_DMA];
    u32 mem[MAX_MEM];
    int on, off;
} SETTING;

static CARD   *card;
static u8     *resbuf;
static int     ncards;
static u8      plog[PLOG_LEN];
static u32     used_lo[MAX_USED], used_hi[MAX_USED];
static u8      used_ten[MAX_USED];
static int     nused;
static u16     irq_used;
static u8      dma_used;
static int     changed;
static char    tail[160];
static char   *argv[32];
static int     argc;
static u8      out_buf[OUT_BUF];
static u32     out_len;

static const char hexdig[] = "0123456789ABCDEF";

static const char msg_help[] =
    "Lists the ISA Plug and Play cards and switches their devices on.\r\n"
    "\r\n"
    "ISAPNP\r\n"
    "ISAPNP /A\r\n"
    "ISAPNP card.device [IO=port[,port...]] [IRQ=n[,n]] [DMA=n[,n]]\r\n"
    "       [MEM=address[,address...]] [ON | OFF]\r\n"
    "\r\n"
    "  (none)  Lists every card and device, and what each one is using.\r\n"
    "  /A      Switches on every device that is off, with resources that\r\n"
    "          nothing else in the machine is known to be using.\r\n"
    "  card.device\r\n"
    "          The device to set, numbered as the list numbers it.  With\r\n"
    "          resources it is given those and switched on; ON or OFF\r\n"
    "          alone switches it as it is.\r\n"
    "\r\n"
    "Ports and addresses are in hexadecimal, IRQ and DMA numbers in\r\n"
    "decimal.  A card that is not Plug and Play cannot be seen by /A: give\r\n"
    "a device its resources by hand if one of them is taken.\r\n";

/* ---------------------------------------------------------------- */
/* small routines a C library would have supplied                    */
/* ---------------------------------------------------------------- */

static u32 str_len( const char *str )
{
    u32 len = 0;

    while ( str[len] ) len++;
    return len;
}

static void mem_set( void *dst, u8 val, u32 len )
{
    u8 *at = (u8 *)dst;

    while ( len-- ) *at++ = val;
}

static void mem_cpy( void *dst, const void *src, u32 len )
{
    u8 *to = (u8 *)dst;
    const u8 *from = (const u8 *)src;

    while ( len-- ) *to++ = *from++;
}

static u8 up( u8 ch )
{
    return (u8)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
}

static void clear( REGS *regs )
{
    mem_set( regs, 0, sizeof( *regs ) );
}

/* ---------------------------------------------------------------- */
/* output                                                            */
/* ---------------------------------------------------------------- */

static void out_flush( void )
{
    REGS regs;

    if ( out_len ) {
        clear( &regs );
        regs.eax = 0x4000;
        regs.ebx = 1;
        regs.ecx = out_len;
        regs.edx = (u32)out_buf;
        int21( &regs );
        out_len = 0;
    }
}

static void pnp_exit( int code )
{
    REGS regs;

    out_flush();
    clear( &regs );
    regs.eax = 0x4C00 | (code & 0xFF);
    int21( &regs );
}

static void out_ch( char ch )
{
    if ( out_len == OUT_BUF ) out_flush();
    out_buf[out_len++] = (u8)ch;
}

static void out_str( const char *str )
{
    while ( *str ) out_ch( *str++ );
}

static void out_nl( void )
{
    out_str( "\r\n" );
}

static void out_hex( u32 val, int digits )
{
    int i;

    for ( i = digits - 1; i >= 0; i-- ) out_ch( hexdig[(val >> (i * 4)) & 15] );
}

static void out_dec( u32 val )
{
    char buf[12];
    int at = 11;

    buf[at] = 0;
    do {
        buf[--at] = (char)('0' + val % 10);
        val /= 10;
    } while ( val );
    out_str( buf + at );
}

/* a field padded with blanks to a width */
static void out_pad( const char *str, u32 width )
{
    u32 len = str_len( str );

    out_str( str );
    while ( len++ < width ) out_ch( ' ' );
}

static void *mem_alloc( u32 size )
{
    REGS regs;

    clear( &regs );
    regs.eax = 0x4800;
    regs.ebx = size;
    int21( &regs );
    return CF( regs ) ? NULL : (void *)regs.eax;
}

/* ---------------------------------------------------------------- */
/* the kernel                                                        */
/* ---------------------------------------------------------------- */

static int bus_call( int func, REGS *regs )
{
    regs->eax = 0xF112 | ((u32)func << 16);
    int21( regs );
    return !CF( *regs );
}

static int pnp_regs( u8 csn, u8 ldn, u8 *regs256 )
{
    REGS regs;

    clear( &regs );
    regs.edx = csn | ((u32)ldn << 8);
    regs.edi = (u32)regs256;
    return bus_call( BUS_PNPREGS, &regs );
}

static int pnp_set( u8 csn, u8 ldn, const u8 *pairs, int count )
{
    REGS regs;

    clear( &regs );
    regs.edx = csn | ((u32)ldn << 8);
    regs.esi = (u32)pairs;
    regs.ecx = (u32)count;
    return bus_call( BUS_PNPSET, &regs );
}

/* ---------------------------------------------------------------- */
/* reading a card                                                    */
/* ---------------------------------------------------------------- */

/* what one of a card's IDs is called: three letters and four digits */
static void id_text( const u8 *id, char *text )
{
    text[0] = (char)('@' + ((id[0] >> 2) & 31));
    text[1] = (char)('@' + (((id[0] & 3) << 3) | (id[1] >> 5)));
    text[2] = (char)('@' + (id[1] & 31));
    text[3] = hexdig[id[2] >> 4];
    text[4] = hexdig[id[2] & 15];
    text[5] = hexdig[id[3] >> 4];
    text[6] = hexdig[id[3] & 15];
    text[7] = 0;
}

static DESC *add_desc( OPTION *opt, u8 kind )
{
    DESC *desc;

    if ( opt == NULL || opt->count == MAX_DESC ) return NULL;
    desc = &opt->desc[opt->count++];
    mem_set( desc, 0, sizeof( *desc ) );
    desc->kind = kind;
    return desc;
}

static void set_name( char *name, const u8 *text, u32 len )
{
    u32 i;

    if ( len > 47 ) len = 47;
    for ( i = 0; i < len && text[i]; i++ ) name[i] = (char)text[i];
    while ( i && name[i - 1] == ' ' ) i--;
    name[i] = 0;
}

/* The resource data, into the devices and the sets of things each one
   can be given.  The descriptors outside the dependent functions come
   before them or after them, and their registers are numbered in that
   order, which is the specification's: the order they are written. */
static void parse_card( CARD *crd, const u8 *data, u32 len )
{
    u32 pos = 9, size;
    DEVICE *dev = NULL;
    OPTION *opt = NULL;
    const u8 *body;
    DESC *desc;
    u8 tag, kind;

    while ( pos < len ) {
        tag = data[pos++];
        if ( tag & 0x80 ) {
            if ( pos + 2 > len ) break;
            size = data[pos] | ((u32)data[pos + 1] << 8);
            pos += 2;
            if ( pos + size > len ) break;
            body = data + pos;
            pos += size;
            switch ( tag ) {
            case 0x82:                  /* a name */
                if ( dev == NULL ) {
                    if ( crd->name[0] == 0 ) set_name( crd->name, body, size );
                } else if ( dev->name[0] == 0 ) {
                    set_name( dev->name, body, size );
                }
                break;
            case 0x81:                  /* a 24-bit memory range */
                desc = add_desc( opt, RK_MEM );
                if ( desc && size >= 9 ) {
                    desc->low = ((u32)body[1] | ((u32)body[2] << 8)) << 8;
                    desc->high = ((u32)body[3] | ((u32)body[4] << 8)) << 8;
                    desc->step = (u32)body[5] | ((u32)body[6] << 8);
                    if ( desc->step == 0 ) desc->step = 0x10000;
                    desc->size = ((u32)body[7] | ((u32)body[8] << 8)) << 8;
                }
                break;
            case 0x85:                  /* 32-bit memory: taken as a range */
            case 0x86:                  /* that can be listed and not chosen */
                desc = add_desc( opt, RK_MEM );
                if ( desc ) desc->step = 0;
                break;
            }
            continue;
        }
        kind = (u8)((tag >> 3) & 15);
        size = tag & 7;
        if ( pos + size > len ) break;
        body = data + pos;
        pos += size;
        switch ( kind ) {
        case 2:                         /* a logical device */
            if ( crd->ndev == MAX_DEVS || size < 4 ) {
                dev = NULL;
                opt = NULL;
                break;
            }
            dev = &crd->dev[crd->ndev];
            mem_set( dev, 0, sizeof( *dev ) );
            dev->number = (u8)crd->ndev++;
            mem_cpy( dev->id, body, 4 );
            opt = &dev->before;
            break;
        case 3:                         /* a compatible ID */
            if ( dev && size >= 4 && dev->compat[0] == 0 ) mem_cpy( dev->compat, body, 4 );
            break;
        case 4:                         /* an IRQ */
            desc = add_desc( opt, RK_IRQ );
            if ( desc && size >= 2 ) desc->mask = (u16)(body[0] | (body[1] << 8));
            break;
        case 5:                         /* a DMA channel */
            desc = add_desc( opt, RK_DMA );
            if ( desc && size >= 1 ) desc->mask = body[0];
            break;
        case 6:                         /* a dependent function begins */
            if ( dev == NULL ) break;
            if ( dev->nfunc == MAX_FUNCS ) {
                opt = NULL;
                break;
            }
            opt = &dev->func[dev->nfunc++];
            opt->count = 0;
            break;
        case 7:                         /* and they end */
            if ( dev ) opt = &dev->after;
            break;
        case 8:                         /* a range of ports */
            desc = add_desc( opt, RK_IO );
            if ( desc && size >= 7 ) {
                desc->low = body[1] | ((u32)body[2] << 8);
                desc->high = body[3] | ((u32)body[4] << 8);
                desc->step = body[5] ? body[5] : 1;
                desc->ports = body[6];
                desc->decode16 = (u8)(body[0] & 1);
            }
            break;
        case 9:                         /* ports at one address */
            desc = add_desc( opt, RK_IO );
            if ( desc && size >= 3 ) {
                desc->low = (body[0] | ((u32)body[1] << 8)) & 0x3FF;
                desc->high = desc->low;
                desc->step = 1;
                desc->ports = body[2];
            }
            break;
        case 15:                        /* the end */
            return;
        }
    }
}

/* card csn's resource data and every device's registers into card */
static int read_card( u8 csn )
{
    REGS regs;
    int i;

    mem_set( card, 0, sizeof( *card ) );
    card->csn = csn;
    clear( &regs );
    regs.edx = csn;
    regs.edi = (u32)resbuf;
    regs.ecx = RES_BUF;
    if ( !bus_call( BUS_PNPRES, &regs ) || regs.ecx < 9 ) return FALSE;
    mem_cpy( card->ident, resbuf, 9 );
    parse_card( card, resbuf, regs.ecx );
    for ( i = 0; i < card->ndev; i++ ) {
        pnp_regs( csn, card->dev[i].number, card->dev[i].regs );
    }
    return TRUE;
}

/* how many of a kind the device has registers for: as many as its own
   resource data describes, which is the most any one choice of it uses */
static int how_many( const DEVICE *dev, u8 kind )
{
    const OPTION *opts[2];
    int most = 0, f, nf, n, i, j;

    nf = dev->nfunc ? dev->nfunc : 1;
    for ( f = 0; f < nf; f++ ) {
        opts[0] = &dev->before;
        opts[1] = &dev->after;
        n = 0;
        for ( i = 0; i < 2; i++ ) {
            for ( j = 0; j < opts[i]->count; j++ ) {
                if ( opts[i]->desc[j].kind == kind ) n++;
            }
        }
        if ( dev->nfunc ) {
            for ( j = 0; j < dev->func[f].count; j++ ) {
                if ( dev->func[f].desc[j].kind == kind ) n++;
            }
        }
        if ( n > most ) most = n;
    }
    return most;
}

/* The device's current resources, out of its registers - those of them
   it has.  A register a device does not implement reads as whatever the
   card likes (all ones, often), and a port of 0 is no port. */
static void current( const DEVICE *dev, SETTING *set )
{
    int i, count;
    u32 val;

    mem_set( set, 0, sizeof( *set ) );
    count = how_many( dev, RK_IO );
    for ( i = 0; i < count && i < MAX_IO; i++ ) {
        val = ((u32)dev->regs[R_IO0 + i * 2] << 8) | dev->regs[R_IO0 + i * 2 + 1];
        if ( val ) set->io[set->nio++] = (u16)val;
    }
    count = how_many( dev, RK_IRQ );
    for ( i = 0; i < count && i < MAX_IRQ; i++ ) {
        val = dev->regs[R_IRQ0 + i * 2] & 15;
        if ( val ) set->irq[set->nirq++] = (u8)val;
    }
    count = how_many( dev, RK_DMA );
    for ( i = 0; i < count && i < MAX_DMA; i++ ) {
        val = dev->regs[R_DMA0 + i] & 7;
        if ( val != 4 ) set->dma[set->ndma++] = (u8)val;
    }
    count = how_many( dev, RK_MEM );
    for ( i = 0; i < count && i < MAX_MEM; i++ ) {
        val = (((u32)dev->regs[R_MEM0 + i * 8] << 8) | dev->regs[R_MEM0 + i * 8 + 1]) << 8;
        if ( val ) set->mem[set->nmem++] = val;
    }
}

static int is_on( const DEVICE *dev )
{
    return dev->regs[R_ACT] & 1;
}

/* the one the kernel switched on for its CD-ROM drive */
static int kernels( const CARD *crd, const DEVICE *dev )
{
    return plog[PLP_FIRST + PLP_STAGE] == '+' || plog[PLP_FIRST + PLP_STAGE] == 'K'
        ? plog[PLP_FIRST + PLP_CSN] == crd->csn && plog[PLP_FIRST + PLP_LD] == dev->number
        : FALSE;
}

/* ---------------------------------------------------------------- */
/* what is in use                                                    */
/* ---------------------------------------------------------------- */

/* ten = TRUE if whatever is there decodes only ten bits of the address,
   as an ISA card does unless it says otherwise, and answers at every
   copy of its ports 400h apart */
static void use_io( u32 low, u32 count, int ten )
{
    if ( count == 0 || nused == MAX_USED ) return;
    used_lo[nused] = low;
    used_hi[nused] = low + count - 1;
    used_ten[nused] = (u8)ten;
    nused++;
}

static int overlap( u32 alow, u32 ahigh, u32 blow, u32 bhigh )
{
    return alow <= bhigh && blow <= ahigh;
}

/* The same ports, or the same ports in ten bits when either side looks
   at no more than ten: a card at 220h that decodes ten bits is at 620h
   as well, and one that decodes sixteen is not. */
static int io_free( u32 low, u32 count, int ten )
{
    u32 high, low10;
    int i;

    if ( count == 0 ) return TRUE;
    high = low + count - 1;
    low10 = low & 0x3FF;
    for ( i = 0; i < nused; i++ ) {
        if ( overlap( low, high, used_lo[i], used_hi[i] ) ) return FALSE;
        if ( !ten && !used_ten[i] ) continue;
        if ( overlap( low10, low10 + count - 1, used_lo[i] & 0x3FF,
                      (used_lo[i] & 0x3FF) + (used_hi[i] - used_lo[i]) ) ) return FALSE;
    }
    return TRUE;
}

/* What a device's port register number index is for, which only what
   it asked for can say: its first set says it well enough, the sets of
   one device differing in where and not in how many. */
static const DESC *io_desc( const DEVICE *dev, int index )
{
    const OPTION *opts[3];
    int set, desc, seen = 0;

    opts[0] = &dev->before;
    opts[1] = dev->nfunc ? &dev->func[0] : NULL;
    opts[2] = &dev->after;
    for ( set = 0; set < 3; set++ ) {
        if ( opts[set] == NULL ) continue;
        for ( desc = 0; desc < opts[set]->count; desc++ ) {
            if ( opts[set]->desc[desc].kind != RK_IO ) continue;
            if ( seen++ == index ) return &opts[set]->desc[desc];
        }
    }
    return NULL;
}

static void use_setting( const SETTING *set, const DEVICE *dev )
{
    const DESC *desc;
    int i;

    for ( i = 0; i < set->nio; i++ ) {
        desc = io_desc( dev, i );
        if ( desc ) {
            use_io( set->io[i], desc->ports ? desc->ports : 1, !desc->decode16 );
        } else {
            use_io( set->io[i], 8, TRUE );
        }
    }
    for ( i = 0; i < set->nirq; i++ ) irq_used |= (u16)(1 << set->irq[i]);
    for ( i = 0; i < set->ndma; i++ ) dma_used |= (u8)(1 << set->dma[i]);
}

/* Everything an AT has, and everything the kernel can say about the
   rest of the machine. */
static void machine_used( void )
{
    static const u16 fixed[] = {
        0x000, 0x20,  0x020, 0x20,  0x040, 0x20,  0x060, 0x10,  0x070, 0x10,
        0x080, 0x20,  0x0A0, 0x20,  0x0C0, 0x20,  0x0F0, 0x10,
        0x170, 8,     0x1F0, 8,     0x376, 2,     0x3F6, 2,
        0x3B0, 0x30,  0x3F0, 8,     0x279, 1,     0x000, 0
    };
    REGS regs;
    int i;

    for ( i = 0; fixed[i + 1]; i += 2 ) use_io( fixed[i], fixed[i + 1], TRUE );
    /* the timer, the keyboard, the cascade, the floppy, the clock, the
       coprocessor and both IDE channels */
    irq_used = 0x0001 | 0x0002 | 0x0004 | 0x0040 | 0x0100 | 0x2000 | 0x4000 | 0x8000;
    dma_used = 0x04 | 0x10;     /* the floppy's, and the cascade */

    clear( &regs );             /* the port the cards are read at */
    if ( bus_call( BUS_PNPHERE, &regs ) && regs.ebx ) use_io( regs.ebx, 1, TRUE );

    /* the lines the kernel keeps, a driver has hooked, or a resident
       DOS driver has unmasked */
    for ( i = 0; i < 16; i++ ) {
        clear( &regs );
        regs.edx = (u32)i;
        if ( bus_call( BUS_IRQSTAT, &regs ) ) {
            if ( regs.eax & (IRQF_KERNEL | IRQF_HOOKED | IRQF_BOXWANT) ) irq_used |= (u16)(1 << i);
        }
    }

    /* the lines the PCI cards were routed to */
    clear( &regs );
    if ( bus_call( BUS_PCIHERE, &regs ) && (regs.eax & 1) ) {
        u32 count = regs.ecx;
        u32 n;
        for ( n = 0; n < count; n++ ) {
            u32 addr;
            clear( &regs );
            regs.esi = n;
            if ( !bus_call( BUS_PCIENUM, &regs ) ) break;
            addr = regs.ebx;
            clear( &regs );
            regs.ebx = addr;
            if ( bus_call( BUS_PCIIRQ, &regs ) ) {
                if ( (regs.eax & 0xFF00) && (regs.eax & 0xFF) < 16 ) irq_used |= (u16)(1 << (regs.eax & 0xFF));
            }
        }
    }

    /* the DMA channels the kernel keeps for a bus master */
    clear( &regs );
    if ( bus_call( BUS_DMASTAT, &regs ) ) dma_used |= (u8)regs.ebx;
}

/* The serial and printer ports: all four of one and all three of the
   other, there or not.  A native program cannot read the BIOS data area
   to find out which are - and a card put at a serial port's address
   because the port was quiet that moment is the mistake this is here
   to prevent.  The seven addresses are no loss. */
static void ports_used( void )
{
    static const u16 com[] = { 0x3F8, 0x2F8, 0x3E8, 0x2E8 };
    static const u16 lpt[] = { 0x378, 0x278, 0x3BC };
    int i;

    for ( i = 0; i < 4; i++ ) use_io( com[i], 8, TRUE );
    for ( i = 0; i < 3; i++ ) use_io( lpt[i], 8, TRUE );
}

/* ---------------------------------------------------------------- */
/* choosing                                                          */
/* ---------------------------------------------------------------- */

/* The order IRQs are tried in: the ones nothing on a PC is wired to
   first, then the ones that are spare only if their usual owner is not
   there. */
static const u8 irq_order[] = { 5, 10, 11, 9, 15, 7, 12, 3, 4, 14 };

static int choose_desc( const DESC *desc, SETTING *set )
{
    u32 base;
    int i;

    switch ( desc->kind ) {
    case RK_IO:
        if ( set->nio == MAX_IO ) return FALSE;
        if ( desc->ports == 0 ) {
            set->io[set->nio++] = 0;
            return TRUE;
        }
        for ( base = desc->low; base <= desc->high && base < 0x10000; base += desc->step ) {
            if ( io_free( base, desc->ports, !desc->decode16 ) ) {
                set->io[set->nio++] = (u16)base;
                use_io( base, desc->ports, !desc->decode16 );
                return TRUE;
            }
            if ( desc->step == 0 ) break;
        }
        return FALSE;
    case RK_IRQ:
        if ( set->nirq == MAX_IRQ ) return FALSE;
        if ( desc->mask == 0 ) {
            set->irq[set->nirq++] = 0;
            return TRUE;
        }
        for ( i = 0; i < (int)sizeof( irq_order ); i++ ) {
            u8 line = irq_order[i];
            if ( (desc->mask & (1 << line)) && !(irq_used & (1 << line)) ) {
                set->irq[set->nirq++] = line;
                irq_used |= (u16)(1 << line);
                return TRUE;
            }
        }
        return FALSE;
    case RK_DMA:
        if ( set->ndma == MAX_DMA ) return FALSE;
        if ( desc->mask == 0 ) {
            set->dma[set->ndma++] = 4;
            return TRUE;
        }
        for ( i = 0; i < 8; i++ ) {
            if ( (desc->mask & (1 << i)) && !(dma_used & (1 << i)) ) {
                set->dma[set->ndma++] = (u8)i;
                dma_used |= (u8)(1 << i);
                return TRUE;
            }
        }
        return FALSE;
    case RK_MEM:
        return FALSE;           /* never chosen: see the header */
    }
    return FALSE;
}

static int choose_option( const OPTION *opt, SETTING *set )
{
    int i;

    for ( i = 0; i < opt->count; i++ ) {
        if ( !choose_desc( &opt->desc[i], set ) ) return FALSE;
    }
    return TRUE;
}

static int needs_mem( const OPTION *opt )
{
    int i;

    for ( i = 0; i < opt->count; i++ ) {
        if ( opt->desc[i].kind == RK_MEM ) return TRUE;
    }
    return FALSE;
}

/* The first set that can be had whole.  What a set that failed took
   on the way is given back before the next is tried. */
static int choose( const DEVICE *dev, SETTING *set, int *memory )
{
    u32 save_lo[MAX_USED], save_hi[MAX_USED];
    u8 save_ten[MAX_USED];
    int save_n = nused, f, nf;
    u16 save_irq = irq_used;
    u8 save_dma = dma_used;

    mem_cpy( save_lo, used_lo, sizeof( save_lo ) );
    mem_cpy( save_hi, used_hi, sizeof( save_hi ) );
    mem_cpy( save_ten, used_ten, sizeof( save_ten ) );
    *memory = FALSE;
    nf = dev->nfunc ? dev->nfunc : 1;
    for ( f = 0; f < nf; f++ ) {
        mem_set( set, 0, sizeof( *set ) );
        if ( needs_mem( &dev->before ) || needs_mem( &dev->after )
          || (dev->nfunc && needs_mem( &dev->func[f] )) ) {
            *memory = TRUE;
            continue;
        }
        if ( choose_option( &dev->before, set )
          && (dev->nfunc == 0 || choose_option( &dev->func[f], set ))
          && choose_option( &dev->after, set ) ) {
            return TRUE;
        }
        nused = save_n;
        mem_cpy( used_lo, save_lo, sizeof( save_lo ) );
        mem_cpy( used_hi, save_hi, sizeof( save_hi ) );
        mem_cpy( used_ten, save_ten, sizeof( save_ten ) );
        irq_used = save_irq;
        dma_used = save_dma;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- */
/* setting                                                           */
/* ---------------------------------------------------------------- */

/* Off, the registers, and on: a device is moved with its decoding
   switched off, as the specification says it must be. */
static int apply( const CARD *crd, const DEVICE *dev, const SETTING *set )
{
    u8 pairs[64 * 2];
    int count = 0, i;

#define PUT( reg, val )  ( pairs[count * 2] = (u8)(reg), pairs[count * 2 + 1] = (u8)(val), count++ )

    /* As many of each register as the device has - or as were given,
       for a device whose resource data says less than it means - and
       no more: a register a device does not implement is not one to
       write to. */
    PUT( R_ACT, 0 );
    if ( set->nio || set->nirq || set->ndma || set->nmem ) {
        int has = how_many( dev, RK_IO );
        if ( set->nio > has ) has = set->nio;
        for ( i = 0; i < has && i < MAX_IO; i++ ) {
            u16 port = (u16)(i < set->nio ? set->io[i] : 0);
            PUT( R_IO0 + i * 2, port >> 8 );
            PUT( R_IO0 + i * 2 + 1, port & 0xFF );
        }
        has = how_many( dev, RK_IRQ );
        if ( set->nirq > has ) has = set->nirq;
        for ( i = 0; i < has && i < MAX_IRQ; i++ ) {
            PUT( R_IRQ0 + i * 2, i < set->nirq ? set->irq[i] : 0 );
            PUT( R_IRQ0 + i * 2 + 1, 2 );           /* edge, high: ISA's */
        }
        has = how_many( dev, RK_DMA );
        if ( set->ndma > has ) has = set->ndma;
        for ( i = 0; i < has && i < MAX_DMA; i++ ) {
            PUT( R_DMA0 + i, i < set->ndma ? set->dma[i] : 4 );
        }
        for ( i = 0; i < set->nmem; i++ ) {
            PUT( R_MEM0 + i * 8, (set->mem[i] >> 16) & 0xFF );
            PUT( R_MEM0 + i * 8 + 1, (set->mem[i] >> 8) & 0xFF );
        }
    }
    if ( !set->off ) PUT( R_ACT, 1 );
#undef PUT

    changed = TRUE;
    return pnp_set( crd->csn, dev->number, pairs, count );
}

/* ---------------------------------------------------------------- */
/* listing                                                           */
/* ---------------------------------------------------------------- */

static void list_setting( const SETTING *set )
{
    int i;

    if ( set->nio ) {
        out_str( "  I/O" );
        for ( i = 0; i < set->nio; i++ ) {
            out_ch( ' ' );
            out_hex( set->io[i], 3 );
        }
    }
    if ( set->nirq ) {
        out_str( "  IRQ" );
        for ( i = 0; i < set->nirq; i++ ) {
            out_ch( ' ' );
            out_dec( set->irq[i] );
        }
    }
    if ( set->ndma ) {
        out_str( "  DMA" );
        for ( i = 0; i < set->ndma; i++ ) {
            out_ch( ' ' );
            out_dec( set->dma[i] );
        }
    }
    if ( set->nmem ) {
        out_str( "  MEM" );
        for ( i = 0; i < set->nmem; i++ ) {
            out_ch( ' ' );
            out_hex( set->mem[i], 5 );
        }
    }
}

static void list_card( void )
{
    char text[8];
    SETTING set;
    int i;

    out_str( "Card " );
    out_dec( card->csn );
    out_str( "  " );
    id_text( card->ident, text );
    out_str( text );
    out_str( "  serial " );
    out_hex( ((u32)card->ident[7] << 24) | ((u32)card->ident[6] << 16)
           | ((u32)card->ident[5] << 8) | card->ident[4], 8 );
    if ( card->name[0] ) {
        out_str( "  " );
        out_str( card->name );
    }
    out_nl();
    for ( i = 0; i < card->ndev; i++ ) {
        DEVICE *dev = &card->dev[i];
        char numtext[8];

        out_str( "  " );
        numtext[0] = (char)('0' + card->csn);
        numtext[1] = '.';
        numtext[2] = (char)('0' + dev->number);
        numtext[3] = 0;
        out_pad( numtext, 5 );
        id_text( dev->id, text );
        out_pad( text, 9 );
        out_pad( dev->name[0] ? dev->name : "-", 26 );
        out_str( is_on( dev ) ? "on " : "off" );
        if ( is_on( dev ) ) {
            current( dev, &set );
            list_setting( &set );
        }
        if ( kernels( card, dev ) ) out_str( "  (the CD-ROM's)" );
        out_nl();
    }
}

/* ---------------------------------------------------------------- */
/* the command line                                                  */
/* ---------------------------------------------------------------- */

static void split_args( void )
{
    REGS regs;
    u8 *psp;
    u32 len, i;
    char *in, *out;

    clear( &regs );
    regs.eax = 0x6200;
    int21( &regs );
    psp = (u8 *)regs.ebx;
    len = psp[0x80];
    if ( len > 127 ) len = 127;
    for ( i = 0; i < len; i++ ) tail[i] = (char)psp[0x81 + i];
    tail[len] = 0;

    in = out = tail;
    argc = 0;
    for ( ;; ) {
        while ( *in == ' ' || *in == '\t' ) in++;
        if ( *in == 0 || *in == '\r' ) break;
        if ( argc == 31 ) break;
        argv[argc++] = out;
        while ( *in && *in != '\r' && *in != ' ' && *in != '\t' ) *out++ = *in++;
        if ( *in ) in++;
        *out++ = 0;
    }
}

/* a number in base 10 or 16 at *text, which is moved past it; FALSE if
   there are no digits */
static int get_num( const char **text, int base, u32 *val )
{
    const char *at = *text;
    u32 num = 0;
    int digits = 0;

    for ( ;; ) {
        u8 ch = up( (u8)*at );
        u32 dig;
        if ( ch >= '0' && ch <= '9' ) {
            dig = ch - '0';
        } else if ( base == 16 && ch >= 'A' && ch <= 'F' ) {
            dig = ch - 'A' + 10;
        } else {
            break;
        }
        if ( dig >= (u32)base ) break;
        num = num * base + dig;
        digits++;
        at++;
    }
    if ( base == 16 && up( (u8)*at ) == 'H' ) at++;
    *text = at;
    *val = num;
    return digits > 0;
}

/* "NAME=" at the front of word, ignoring case */
static const char *keyword( const char *word, const char *name )
{
    while ( *name ) {
        if ( up( (u8)*word ) != (u8)*name ) return NULL;
        word++;
        name++;
    }
    return word;
}

/* IO=a,b... and the rest into set; FALSE and a message if it is not
   one of them, or is not numbers */
static int parse_setting( const char *word, SETTING *set )
{
    const char *at;
    u32 val;

    if ( (at = keyword( word, "IO=" )) != NULL ) {
        do {
            if ( set->nio == MAX_IO || !get_num( &at, 16, &val ) || val > 0xFFFF ) return FALSE;
            set->io[set->nio++] = (u16)val;
        } while ( *at++ == ',' );
        return at[-1] == 0;
    }
    if ( (at = keyword( word, "IRQ=" )) != NULL ) {
        do {
            if ( set->nirq == MAX_IRQ || !get_num( &at, 10, &val ) || val > 15 ) return FALSE;
            set->irq[set->nirq++] = (u8)val;
        } while ( *at++ == ',' );
        return at[-1] == 0;
    }
    if ( (at = keyword( word, "DMA=" )) != NULL ) {
        do {
            if ( set->ndma == MAX_DMA || !get_num( &at, 10, &val ) || val > 7 ) return FALSE;
            set->dma[set->ndma++] = (u8)val;
        } while ( *at++ == ',' );
        return at[-1] == 0;
    }
    if ( (at = keyword( word, "MEM=" )) != NULL ) {
        do {
            if ( set->nmem == MAX_MEM || !get_num( &at, 16, &val ) || val > 0xFFFFFF ) return FALSE;
            set->mem[set->nmem++] = val & 0xFFFF00;
        } while ( *at++ == ',' );
        return at[-1] == 0;
    }
    if ( keyword( word, "ON" ) && word[2] == 0 ) {
        set->on = TRUE;
        return TRUE;
    }
    if ( keyword( word, "OFF" ) && word[3] == 0 ) {
        set->off = TRUE;
        return TRUE;
    }
    return FALSE;
}

static void say( const char *what, const char *word )
{
    out_str( "ISAPNP: " );
    out_str( what );
    if ( word ) {
        out_str( " - " );
        out_str( word );
    }
    out_nl();
}

/* ---------------------------------------------------------------- */
/* the three things it does                                          */
/* ---------------------------------------------------------------- */

static void do_list( void )
{
    int c;

    for ( c = 1; c <= ncards; c++ ) {
        if ( !read_card( (u8)c ) ) {
            out_str( "Card " );
            out_dec( (u32)c );
            out_str( "  does not answer" );
            out_nl();
            continue;
        }
        list_card();
    }
}

static void out_line_none( void )
{
    out_str( "Every device is on already." );
    out_nl();
}

static void do_auto( void )
{
    SETTING set;
    int c, i, memory, any = FALSE;
    char text[8];

    /* What the machine has, and then what every device that is on is
       using, before anything is chosen. */
    machine_used();
    ports_used();
    for ( c = 1; c <= ncards; c++ ) {
        if ( !read_card( (u8)c ) ) continue;
        for ( i = 0; i < card->ndev; i++ ) {
            if ( !is_on( &card->dev[i] ) ) continue;
            current( &card->dev[i], &set );
            use_setting( &set, &card->dev[i] );
        }
    }

    for ( c = 1; c <= ncards; c++ ) {
        if ( !read_card( (u8)c ) ) continue;
        for ( i = 0; i < card->ndev; i++ ) {
            DEVICE *dev = &card->dev[i];
            if ( is_on( dev ) || kernels( card, dev ) ) continue;
            any = TRUE;
            out_dec( (u32)c );
            out_ch( '.' );
            out_dec( dev->number );
            out_str( "  " );
            id_text( dev->id, text );
            out_pad( text, 9 );
            out_pad( dev->name[0] ? dev->name : "-", 26 );
            if ( !choose( dev, &set, &memory ) ) {
                out_str( memory ? "needs memory: give it MEM=" : "nothing free that it can use" );
                out_nl();
                continue;
            }
            if ( !apply( card, dev, &set ) ) {
                out_str( "could not be set" );
                out_nl();
                continue;
            }
            out_str( "on " );
            list_setting( &set );
            out_nl();
        }
    }
    if ( !any ) out_line_none();
}

static int do_one( int argn )
{
    SETTING set;
    const char *at = argv[argn];
    u32 cnum, dnum;
    int i;

    if ( !get_num( &at, 10, &cnum ) || *at++ != '.' || !get_num( &at, 10, &dnum ) || *at ) {
        say( "Invalid parameter", argv[argn] );
        return 1;
    }
    mem_set( &set, 0, sizeof( set ) );
    for ( i = argn + 1; i < argc; i++ ) {
        if ( !parse_setting( argv[i], &set ) ) {
            say( "Invalid parameter", argv[i] );
            return 1;
        }
    }
    if ( cnum == 0 || cnum > (u32)ncards || !read_card( (u8)cnum ) ) {
        say( "There is no such card", argv[argn] );
        return 1;
    }
    if ( dnum >= (u32)card->ndev ) {
        say( "That card has no such device", argv[argn] );
        return 1;
    }
    if ( set.on && set.off ) {
        say( "ON and OFF together", NULL );
        return 1;
    }
    /* WHAT IS NOT GIVEN STAYS AS IT IS: IO= alone moves the ports and
       leaves the device its interrupt and its channels. */
    if ( set.nio || set.nirq || set.ndma || set.nmem ) {
        SETTING now;
        current( &card->dev[dnum], &now );
        if ( set.nio == 0 ) {
            set.nio = now.nio;
            mem_cpy( set.io, now.io, sizeof( set.io ) );
        }
        if ( set.nirq == 0 ) {
            set.nirq = now.nirq;
            mem_cpy( set.irq, now.irq, sizeof( set.irq ) );
        }
        if ( set.ndma == 0 ) {
            set.ndma = now.ndma;
            mem_cpy( set.dma, now.dma, sizeof( set.dma ) );
        }
        if ( set.nmem == 0 ) {
            set.nmem = now.nmem;
            mem_cpy( set.mem, now.mem, sizeof( set.mem ) );
        }
    }
    if ( !apply( card, &card->dev[dnum], &set ) ) {
        say( "The device could not be set", argv[argn] );
        return 1;
    }
    read_card( (u8)cnum );
    list_card();
    return 0;
}

void __cdecl dos_entry( void )
{
    REGS regs;
    int i, code = 0;

    split_args();
    for ( i = 0; i < argc; i++ ) {
        if ( argv[i][0] == '/' && argv[i][1] == '?' ) {
            out_str( msg_help );
            pnp_exit( 0 );
        }
    }

    card = mem_alloc( sizeof( CARD ) );
    resbuf = mem_alloc( RES_BUF );
    if ( card == NULL || resbuf == NULL ) {
        say( "Not enough memory", NULL );
        pnp_exit( 1 );
    }

    clear( &regs );
    regs.eax = 0xF115;          /* the probe log: whose IDE port is whose */
    regs.edi = (u32)plog;
    int21( &regs );

    clear( &regs );
    if ( !bus_call( BUS_PNPHERE, &regs ) ) {
        say( "This kernel has no Plug and Play calls", NULL );
        pnp_exit( 1 );
    }
    ncards = (int)regs.eax;
    if ( ncards > MAX_CARDS ) ncards = MAX_CARDS;
    if ( ncards == 0 ) {
        out_str( "There are no ISA Plug and Play cards in this machine." );
        out_nl();
        pnp_exit( argc ? 1 : 0 );
    }

    if ( argc == 0 ) {
        do_list();
    } else if ( argv[0][0] == '/' && up( (u8)argv[0][1] ) == 'A' && argv[0][2] == 0 && argc == 1 ) {
        do_auto();
    } else if ( argv[0][0] != '/' ) {
        code = do_one( 0 );
    } else {
        say( "Invalid switch", argv[0] );
        code = 1;
    }
    pnp_exit( code );
}
