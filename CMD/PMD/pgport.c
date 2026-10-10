/*
 * PGPORT.C - the pages Disk Drives, LPT Ports, COM Ports and IRQ
 * Status.
 *
 * THE DRIVES are the kernel's volume table (21h/F1h AL=6), a letter at
 * a time: what it is - a diskette drive, a partition of a fixed disk, a
 * CD-ROM, a file mounted as a letter, a loaded driver's - and for a
 * fixed disk the geometry of the disk it is on (AL=0Ch).  A drive whose
 * disk can be taken out is described and NOT read: asking an empty
 * drive how full it is costs seconds and an error.
 *
 * THE PORTS are where the BIOS data area says they are, and what they
 * say is read from them - IN and OUT, which the kernel carries out.
 * A serial port the mouse is on is looked at and not touched: finding
 * its speed means switching its first two registers to the divisor for
 * a moment, and a byte the mouse sent in that moment would be lost.
 *
 * THE INTERRUPT LINES have two owners to name.  The address is the
 * vector a DOS box has for the line, and "Handled By" is whoever that
 * address belongs to - unless the line is the kernel's own, which the
 * kernel says (F1h AL=12h, BUS_IRQSTAT) and a box's vector does not.
 */
#include "pmd.h"

/* ------------------------------------------------------------------ */
/* Disk Drives                                                         */
/* ------------------------------------------------------------------ */

static const char *floppy_name( int type )
{
    switch ( type ) {
    case 1: return "Floppy Drive, 5.25\" 360K";
    case 2: return "Floppy Drive, 5.25\" 1.2M";
    case 3: return "Floppy Drive, 3.5\" 720K";
    case 4: return "Floppy Drive, 3.5\" 1.44M";
    case 5: return "Floppy Drive, 3.5\" 2.88M";
    }
    return "Floppy Drive";
}

static void size_text( u32 kb, char *out, u32 max )
{
    if ( kb < 10240 ) {
        sfmt( out, max, "%uK", kb );
    } else if ( kb < 10240UL * 1024 ) {
        sfmt( out, max, "%uM", kb / 1024 );
    } else {
        sfmt( out, max, "%uG", kb / (1024UL * 1024) );
    }
}

static void drive_line( TEXT *text, int drv, const char *type, int sized )
{
    char free_text[16], total_text[16];
    u32 free_kb, total_kb;

    free_text[0] = total_text[0] = 0;
    if ( sized && sys_disk_space( drv, &free_kb, &total_kb ) && !crit_take() ) {
        size_text( free_kb, free_text, sizeof( free_text ) );
        size_text( total_kb, total_text, sizeof( total_text ) );
    }
    tx_addf( text, "  %c:   %-36s  %10s  %10s", 'A' + drv, type, free_text, total_text );
}

/* How many diskette drives the machine has: the equipment word.  With
   one, B: is the same drive under a second name, and is left out as
   MSD leaves it out. */
static int floppy_drives( void )
{
    u32 equipment = peek_w( 0x410 );

    return (equipment & 1) ? (int)((equipment >> 6) & 3) + 1 : 0;
}

static int phantom( const DRVREC *rec )
{
    return (rec->flags & DRVF_FLOPPY) && !(rec->flags & (DRVF_CDROM | DRVF_IMAGE | DRVF_DRIVER))
           && rec->bios < 0x80 && rec->bios >= floppy_drives();
}

void pg_disk( TEXT *text )
{
    DRVREC rec;
    DSKREC disk;
    int drv, letters = 0, type, spt, cyls, heads, fat;
    char name[40];

    tx_add( text, "Drive  Type                                  Free Space  Total Size" );
    tx_add( text, "-----  ------------------------------------  ----------  ----------" );
    crit_take();
    for ( drv = 0; sys_drive_rec( drv, &rec, &letters ); drv++ ) {
        if ( !(rec.flags & DRVF_PRESENT) || phantom( &rec ) ) {
            continue;
        }
        if ( rec.flags & DRVF_CDROM ) {
            drive_line( text, drv, "CD-ROM Drive", 0 );
        } else if ( rec.flags & DRVF_IMAGE ) {
            drive_line( text, drv, "Mounted Image File", sys_fat_type( drv ) != 0 );
        } else if ( rec.flags & DRVF_DRIVER ) {
            drive_line( text, drv, "Block Device Driver", sys_fat_type( drv ) != 0 );
        } else if ( rec.flags & DRVF_FLOPPY ) {
            spt = cyls = heads = 0;
            type = sys_floppy_type( rec.bios, &spt, &cyls, &heads );
            drive_line( text, drv, floppy_name( type ), 0 );
            if ( cyls && heads ) {
                tx_addf( text, "         %u Cylinders, %u Heads", cyls, heads );
                tx_addf( text, "         512 Bytes/Sector, %u Sectors/Track", spt );
            }
        } else {
            fat = sys_fat_type( drv );
            if ( fat ) {
                sfmt( name, sizeof( name ), "Fixed Disk, FAT%u", fat );
            } else {
                str_cpy( name, "Fixed Disk" );
            }
            drive_line( text, drv, name, fat != 0 );
            if ( sys_disk_rec( rec.bios, &disk ) && disk.heads ) {
                tx_addf( text, "         %u Cylinders, %u Heads", disk.cyls, disk.heads );
                tx_addf( text, "         512 Bytes/Sector, %u Sectors/Track", disk.spt );
            }
        }
    }
    crit_take();
    if ( sys_share_active() ) {
        tx_add( text, "SHARE Installed" );
    }
    if ( sys_cdrom_units() ) {
        tx_add( text, "MSCDEX Version 2.25 Installed" );     /* CDX_VER_*, MSCDEX.INC */
    }
    tx_addf( text, "LASTDRIVE=%c:", 'A' + peek_b( kernel_facts.lol + 0x21 ) - 1 );
}

void sum_disk( char *line1, char *line2 )
{
    DRVREC rec;
    int drv, letters = 0;
    char *line = line1;
    u32 len;

    line1[0] = line2[0] = 0;
    for ( drv = 0; sys_drive_rec( drv, &rec, &letters ); drv++ ) {
        if ( !(rec.flags & DRVF_PRESENT) || phantom( &rec ) ) {
            continue;
        }
        len = str_len( line );
        if ( len + 3 > SUM_WIDTH ) {
            if ( line == line2 ) {
                break;
            }
            line = line2;
            len = 0;
        }
        if ( len ) {
            line[len++] = ' ';
        }
        line[len++] = (char)('A' + drv);
        line[len++] = ':';
        line[len] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* LPT Ports                                                           */
/* ------------------------------------------------------------------ */

int lpt_port( int index )
{
    return index >= 0 && index < 3 ? peek_w( 0x408 + (u32)index * 2 ) : 0;
}

void pg_lpt( TEXT *text )
{
    int index, port;
    unsigned status;

    tx_add( text, "         Port     On     Paper    I/O    Time" );
    tx_add( text, "Port   Address   Line     Out    Error    Out    Busy     ACK" );
    tx_add( text, "-----  -------   ----    -----   -----   ----    ----     ---" );
    for ( index = 0; index < 3; index++ ) {
        port = lpt_port( index );
        if ( port == 0 ) {
            tx_addf( text, "LPT%u:     -        -       -       -       -       -       -", index + 1 );
            continue;
        }
        /* the status register: selected, out of paper, and three lines
           that are low when they mean something */
        status = port_in( (unsigned)port + 1 );
        tx_addf( text, "LPT%u:   %04XH     %-3s     %-3s     %-3s     %-3s     %-3s     %-3s", index + 1,
                 port, (status & 0x10) ? "Yes" : "No", (status & 0x20) ? "Yes" : "No",
                 (status & 0x08) ? "No" : "Yes", "No", (status & 0x80) ? "No" : "Yes",
                 (status & 0x40) ? "No" : "Yes" );
    }
}

void sum_lpt( char *line1, char *line2 )
{
    int index, count = 0;

    for ( index = 0; index < 3; index++ ) {
        if ( lpt_port( index ) ) {
            count++;
        }
    }
    sfmt( line1, SUM_WIDTH + 1, "%u", count );
    line2[0] = 0;
}

/* ------------------------------------------------------------------ */
/* COM Ports                                                           */
/* ------------------------------------------------------------------ */

int com_port( int index )
{
    return index >= 0 && index < 4 ? peek_w( 0x400 + (u32)index * 2 ) : 0;
}

typedef struct {
    char address[8], baud[8], parity[8], data[4], stop[4];
    char cd[4], ri[4], dsr[4], cts[4], uart[10];
} COMINFO;

/* 8250, 16450, 16550 or 16550AF: switch the FIFOs on and see whether
   the chip says it has any, and if not whether it has the scratch
   register an 8250 lacks.  Whatever it was doing, it goes back to. */
static const char *uart_chip( unsigned base )
{
    unsigned before = port_in( base + 2 ), now, scratch, kept;
    const char *name;

    port_out( base + 2, 0x01 );
    now = port_in( base + 2 );
    port_out( base + 2, (before & 0xC0) ? 0x01 : 0x00 );
    if ( (now & 0xC0) == 0xC0 ) {
        return "16550AF";
    }
    if ( now & 0x80 ) {
        return "16550";
    }
    scratch = port_in( base + 7 );
    port_out( base + 7, 0x5A );
    kept = port_in( base + 7 );
    name = kept == 0x5A ? "16450" : "8250";
    port_out( base + 7, scratch );
    return name;
}

static void com_read( int index, COMINFO *info )
{
    static const char *const parity[] = { "None", "Odd", "None", "Even", "None", "Mark",
                                          "None", "Space" };
    unsigned base = (unsigned)com_port( index ), lcr, msr, divisor;
    int is_mouse = mouse_com_port() == index + 1;

    mem_set( info, 0, sizeof( *info ) );
    if ( base == 0 ) {
        str_cpy( info->address, "N/A" );
        return;
    }
    sfmt( info->address, sizeof( info->address ), "%04XH", base );
    lcr = port_in( base + 3 );
    str_cpy( info->parity, parity[(lcr >> 3) & 7] );
    sfmt( info->data, sizeof( info->data ), "%u", 5 + (lcr & 3) );
    str_cpy( info->stop, !(lcr & 4) ? "1" : (lcr & 3) == 0 ? "1.5" : "2" );
    msr = port_in( base + 6 );
    str_cpy( info->cd, (msr & 0x80) ? "Yes" : "No" );
    str_cpy( info->ri, (msr & 0x40) ? "Yes" : "No" );
    str_cpy( info->dsr, (msr & 0x20) ? "Yes" : "No" );
    str_cpy( info->cts, (msr & 0x10) ? "Yes" : "No" );
    if ( is_mouse ) {
        str_cpy( info->uart, "Mouse" );
        return;
    }
    port_out( base + 3, lcr | 0x80 );           /* the divisor, for a moment */
    divisor = port_in( base ) | (port_in( base + 1 ) << 8);
    port_out( base + 3, lcr );
    if ( divisor ) {
        sfmt( info->baud, sizeof( info->baud ), "%u", 115200UL / divisor );
    }
    str_cpy( info->uart, uart_chip( base ) );
}

void pg_com( TEXT *text )
{
    COMINFO info[4];
    int index;

    for ( index = 0; index < 4; index++ ) {
        com_read( index, &info[index] );
    }
    tx_add( text, "                           COM1:      COM2:      COM3:      COM4:" );
    tx_add( text, "                           -----      -----      -----      -----" );
#define COM_ROW( label, member ) \
    tx_addf( text, "%-27s%5s      %5s      %5s      %5s", label, info[0].member, info[1].member, \
             info[2].member, info[3].member )
    COM_ROW( "Port Address", address );
    COM_ROW( "Baud Rate", baud );
    COM_ROW( "Parity", parity );
    COM_ROW( "Data Bits", data );
    COM_ROW( "Stop Bits", stop );
    COM_ROW( "Carrier Detect (CD)", cd );
    COM_ROW( "Ring Indicator (RI)", ri );
    COM_ROW( "Data Set Ready (DSR)", dsr );
    COM_ROW( "Clear To Send (CTS)", cts );
    COM_ROW( "UART Chip Used", uart );
#undef COM_ROW
}

void sum_com( char *line1, char *line2 )
{
    int index, count = 0;

    for ( index = 0; index < 4; index++ ) {
        if ( com_port( index ) ) {
            count++;
        }
    }
    sfmt( line1, SUM_WIDTH + 1, "%u", count );
    line2[0] = 0;
}

/* ------------------------------------------------------------------ */
/* IRQ Status                                                          */
/* ------------------------------------------------------------------ */

static const char *const irq_names[16] = {
    "Timer Click", "Keyboard", "Second 8259A", "COM2: COM4:", "COM1: COM3:", "LPT2:",
    "Floppy Disk", "LPT1:", "Real-Time Clock", "Redirected IRQ2", "(Reserved)", "(Reserved)",
    "(Reserved)", "Math Coprocessor", "Fixed Disk", "(Reserved)"
};

/* a bit for every line a PCI card has been routed to */
static u32 pci_lines( void )
{
    u32 addr, ident, class_rev, lines = 0;
    int index, line, pin;

    for ( index = 0; index < 256 && sys_pci_enum( index, &addr, &ident, &class_rev ); index++ ) {
        line = pin = 0;
        if ( sys_pci_irq( addr, &line, &pin ) && pin && line > 0 && line < 16 ) {
            lines |= 1UL << line;
        }
    }
    return lines;
}

static void irq_detected( int irq, u32 pci, char *out )
{
    MOUSEINFO mouse_info;
    int port = mouse_com_port();
    const char *found = "";

    out[0] = 0;
    switch ( irq ) {
    case 0: case 1: case 2: case 8: case 9:
        found = "Yes";
        break;
    case 3:
        found = com_port( 1 ) || com_port( 3 ) ? "Yes" : "No";
        if ( port == 2 ) {
            found = "COM2: Serial Mouse";
        }
        break;
    case 4:
        found = com_port( 0 ) || com_port( 2 ) ? "Yes" : "No";
        if ( port == 1 ) {
            found = "COM1: Serial Mouse";
        }
        break;
    case 5:
        found = lpt_port( 1 ) ? "Yes" : "No";
        break;
    case 6:
        found = (peek_w( 0x410 ) & 1) ? "Yes" : "No";
        break;
    case 7:
        found = lpt_port( 0 ) ? "Yes" : "No";
        break;
    case 12:
        sys_mouse_info( &mouse_info );
        if ( mouse_info.present && mouse_info.type == 4 ) {
            found = "PS/2 Style Mouse";
        }
        break;
    case 13:
        found = (sys_fpu_flags() & FPUF_X87) ? "Yes" : "No";
        break;
    case 14:
        found = peek_b( 0x475 ) ? "Yes" : "No";
        break;
    }
    if ( (pci & (1UL << irq)) && (found[0] == 0 || str_cmp( found, "No" ) == 0) ) {
        found = "PCI Device";
    }
    str_cpy( out, found );
}

static void irq_handler( u32 vector, u32 flags, char *out, u32 max )
{
    u32 flat = FAR_FLAT( vector >> 16, vector & 0xFFFF );
    const char *owner;

    if ( flags & IRQF_KERNEL ) {
        str_cpyn( out, "PM-DOS Kernel", max );
        return;
    }
    if ( vector == 0 ) {
        str_cpyn( out, "None", max );
        return;
    }
    owner = mem_owner_name( flat );
    if ( owner[0] ) {
        str_cpyn( out, owner, max );
    } else if ( flat >= 0xC0000UL ) {
        str_cpyn( out, "BIOS", max );
    } else if ( flat < kernel_facts.first_mcb ) {
        str_cpyn( out, "System Area", max );
    } else {
        str_cpyn( out, "???", max );
    }
}

void pg_irq( TEXT *text )
{
    u32 pci = pci_lines(), vector, flags, taken;
    int irq;
    char detected[24], handler[24];

    tx_add( text, "IRQ  Address    Description       Detected            Handled By" );
    tx_add( text, "---  ---------  ----------------  ------------------  ----------------" );
    for ( irq = 0; irq < 16; irq++ ) {
        vector = peek_d( (u32)(irq < 8 ? 0x08 + irq : 0x70 + irq - 8) * 4 );
        flags = taken = 0;
        sys_irq_stat( irq, &flags, &taken );
        irq_detected( irq, pci, detected );
        irq_handler( vector, flags, handler, sizeof( handler ) );
        tx_addf( text, "%3u  %04X:%04X  %-16s  %-18s  %s", irq, vector >> 16, vector & 0xFFFF,
                 irq_names[irq], detected, handler );
    }
}
