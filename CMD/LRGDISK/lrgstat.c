/*====================================================================
 * LRGSTAT.C - LRGSTAT.EXE, a Windows 3.1 program that shows what
 * LRGDISK.386 has in use: for each drive how the drive is addressed,
 * bus-master DMA, block mode, the look before the interrupt, 32-bit
 * data and the shared cable, each with a lamp - green in use, grey
 * not, yellow stopped since Windows started - and the reason when
 * something is not used; under them the counts the driver keeps.
 *
 * Nothing is changed from here: the driver's API (lrgdisk.asm, ld_api)
 * only answers.  The switches are SYSTEM.INI's, read when Windows
 * starts.
 *
 *   LRGSTAT              the window; the counts move once a second
 *   LRGSTAT /S file      the same account written to a file as text,
 *                        and no window
 *
 * Small model, Open Watcom 1.9: see BUILD.CMD.
 *==================================================================*/
#include <windows.h>
#include <string.h>

/*--------------------------------------------------------------------
 * The account LRGDISK gives of itself: lrgdisk.asm's LI_*, LIC_* and
 * LIU_*, byte for byte
 *------------------------------------------------------------------*/
#pragma pack( 1 )
typedef struct {
    WORD    data;               /* the cable's data port */
    WORD    bm;                 /* its bus-master ports, 0 none */
    BYTE    flags;              /* CF_* */
    BYTE    ours;               /* bit n: unit n is LRGDISK's */
    BYTE    irq;
    BYTE    spare;
    DWORD   resets;             /* resets DOS programs gave the cable */
    DWORD   lent;               /* times the cable went to a DOS program */
} LICABLE;

typedef struct {
    BYTE    disk;               /* the BIOS's number, 80h to 83h; 0 none */
    BYTE    cable;
    BYTE    unit;               /* 0 master, 1 slave */
    BYTE    mode;               /* 0 CHS, 1 LBA, 2 LBA and LBA48 */
    BYTE    xfer;               /* the DMA mode in force; 0 PIO */
    BYTE    mult;               /* sectors a block; 0 one */
    BYTE    io32;
    BYTE    spare;
    DWORD   lastSector;
    DWORD   lastSectorHigh;
    DWORD   cyls;               /* the BIOS's highest cylinder plus 2 */
    DWORD   heads;
    DWORD   spt;
    BYTE    xfer0;              /* xfer and mult as start-up left them */
    BYTE    mult0;
    BYTE    multMax;            /* the most sectors a block the drive takes */
    BYTE    multSource;         /* 1 the drive's own setting, 2 LRGDISK's */
    BYTE    whyDma;             /* WD_*, WB_*, W3_*: 0 when in use */
    BYTE    whyBlock;
    BYTE    why32;
    BYTE    spare2;
    char    model[40];
    DWORD   count[16];          /* UC_* */
} LIUNIT;

typedef struct {
    WORD    size;
    WORD    version;
    WORD    flags;              /* LIF_* */
    WORD    bmBase;
    DWORD   pollLooks;
    WORD    lostMs;
    BYTE    burst;
    BYTE    blockMax;
    LICABLE cable[2];
    LIUNIT  unit[4];
} LIACCOUNT;
#pragma pack()

#define LRG_VERSION_MIN 0x0104

#define LIF_DMA         0x0001
#define LIF_BLOCK       0x0002
#define LIF_POLL        0x0004
#define LIF_IO32        0x0008
#define LIF_SERIAL      0x0010
#define LIF_PCIIDE      0x0020
#define LIF_CMD640      0x0040
#define LIF_TRACE       0x8000

#define CF_OTHER        0x02

#define WD_OFF          1
#define WD_NOCTRL       2
#define WD_NOCABLE      3
#define WD_NOMODE       4
#define WD_ERROR        5

#define WB_OFF          1
#define WB_ONE          2
#define WB_NONE         3
#define WB_REFUSED      4
#define WB_ERROR        5
#define WB_RESET        6

#define W3_OFF          1
#define W3_CMD640       2
#define W3_NONE         3
#define W3_BAD          4

#define UC_CMDS         0
#define UC_DMACMD       1
#define UC_DMASEC       2
#define UC_PIOCMD       3
#define UC_MULTCMD      4
#define UC_PIOSEC       5
#define UC_BLOCKS       6
#define UC_INTS         7
#define UC_POLLED       8
#define UC_WAITED       9
#define UC_SHORT        10
#define UC_SPARE        11
#define UC_KICK         12
#define UC_ERRORS       13
#define UC_RETRIES      14
#define UC_FAILED       15

/* lrgapi.asm */
extern unsigned long __cdecl LrgEntry( void );
extern unsigned __cdecl LrgVersion( unsigned long entry, unsigned __far *size );
extern unsigned __cdecl LrgAccount( unsigned long entry, void __far *buf, unsigned size );

/*--------------------------------------------------------------------
 * What there is to show
 *------------------------------------------------------------------*/
#define ST_OK           0
#define ST_STANDARD     1       /* Windows is not in 386 enhanced mode */
#define ST_NONE         2       /* no LRGDISK with an API */
#define ST_REFUSED      3       /* an LRGDISK that would not answer */

#define LAMP_OFF        0
#define LAMP_ON         1
#define LAMP_WAS        2       /* in use at start-up, stopped since */

#define ROW_ADDR        0
#define ROW_DMA         1
#define ROW_BLOCK       2
#define ROW_POLL        3
#define ROW_IO32        4
#define ROW_CABLE       5
#define ROW_COUNT       6

#define TALLY_COUNT     12      /* the counts shown: two columns of six */

typedef struct {
    int     lamp;
    char    name[28];
    char    state[72];
    char    detail[140];
} ROW;

typedef struct {
    char    label[36];
    char    value[20];
} TALLY;

static HINSTANCE    g_inst;
static unsigned long g_entry;           /* the driver's API, 0 none */
static int          g_looked;           /* ...and it has been asked for */
static int          g_state;
static LIACCOUNT    g_acc;
static LIACCOUNT    g_fresh;
static int          g_sel;              /* the unit shown, 0 to 3 */
static HFONT        g_fontText;
static HFONT        g_fontBold;
static int          g_lineH;
static int          g_charW;
static int          g_clientW;
static int          g_clientH;
static RECT         g_tab[4];
static char         g_sep[4];           /* the thousands separator */
static ROW          g_row[ROW_COUNT];
static TALLY        g_tally[TALLY_COUNT];
static char         g_line[400];        /* a line being put together: the
                                           longest is under 260 */

static char g_class[] = "LrgStat";
static char g_title[] = "LRGDISK Status";

/*--------------------------------------------------------------------
 * Numbers: digits in groups of three with the separator Control Panel
 * has (WIN.INI [intl] sThousand)
 *------------------------------------------------------------------*/
static void GroupDigits( const char *digits, char *out )
{
    int count = lstrlen( digits );
    int i;
    int pos = 0;

    for ( i = 0; i < count; i++ ) {
        out[pos++] = digits[i];
        if ( ( count - 1 - i ) > 0 && ( count - 1 - i ) % 3 == 0 && g_sep[0] ) {
            out[pos++] = g_sep[0];
        }
    }
    out[pos] = 0;
}

static void FmtNum( unsigned long value, char *out )
{
    char digits[16];

    wsprintf( digits, "%lu", value );
    GroupDigits( digits, out );
}

/* A drive's sectors, from its last sector's number */
static void FmtSectors( unsigned long lastSector, char *out )
{
    if ( lastSector == 0xFFFFFFFFUL ) {
        GroupDigits( "4294967296", out );
    } else {
        FmtNum( lastSector + 1, out );
    }
}

/* ...and its size: megabytes, or gigabytes with a tenth from 10GB up */
static void FmtSize( unsigned long lastSector, char *out )
{
    unsigned long megabytes = ( lastSector >> 11 ) + ( ( ( lastSector & 2047 ) + 1 ) >> 11 );
    char number[20];

    if ( megabytes >= 10240 ) {
        unsigned long tenths = megabytes * 10 / 1024;

        FmtNum( tenths / 10, number );
        wsprintf( out, "%s.%uGB", (LPSTR)number, (unsigned)( tenths % 10 ) );
    } else {
        FmtNum( megabytes, number );
        wsprintf( out, "%sMB", (LPSTR)number );
    }
}

static void FmtXfer( BYTE xfer, char *out )
{
    if ( xfer & 0x40 ) {
        wsprintf( out, "Ultra DMA mode %u", (unsigned)( xfer & 7 ) );
    } else {
        wsprintf( out, "Multiword DMA mode %u", (unsigned)( xfer & 7 ) );
    }
}

/*--------------------------------------------------------------------
 * Asking the driver.  The account comes back in *acc; the result is
 * an ST_*.
 *------------------------------------------------------------------*/
#ifdef LRGSTAT_DEMO
/* A rig's build (wcc -dLRGSTAT_DEMO): LRGSTAT /D n shows a made-up
   account, so that every line this program can write is seen on a
   screen once.  The shipped program has none of this. */
static int g_demo;

static void DemoUnit( LIACCOUNT *acc, int place, const char *model, int mode, unsigned long lastSector,
                      int xfer, int xfer0, int whyDma, int mult, int mult0, int source, int whyBlock,
                      int io32, int why32 )
{
    LIUNIT *unit = &acc->unit[place];
    int i;

    unit->disk = (BYTE)( 0x80 + place );
    unit->cable = (BYTE)( place >> 1 );
    unit->unit = (BYTE)( place & 1 );
    unit->mode = (BYTE)mode;
    unit->xfer = (BYTE)xfer;
    unit->xfer0 = (BYTE)xfer0;
    unit->whyDma = (BYTE)whyDma;
    unit->mult = (BYTE)mult;
    unit->mult0 = (BYTE)mult0;
    unit->multMax = 16;
    unit->multSource = (BYTE)source;
    unit->whyBlock = (BYTE)whyBlock;
    unit->io32 = (BYTE)io32;
    unit->why32 = (BYTE)why32;
    unit->lastSector = lastSector;
    unit->cyls = 1025;
    unit->heads = 255;
    unit->spt = 63;
    memset( unit->model, ' ', sizeof( unit->model ) );
    memcpy( unit->model, model, lstrlen( model ) );
    for ( i = 0; i < 16; i++ ) {
        unit->count[i] = 0;
    }
    unit->count[UC_CMDS] = 25829UL + GetTickCount() / 500;
    unit->count[UC_DMACMD] = xfer ? 20419UL : 0;
    unit->count[UC_DMASEC] = xfer ? 63310UL : 0;
    unit->count[UC_PIOCMD] = 5410;
    unit->count[UC_PIOSEC] = 23408UL + GetTickCount() / 250;
    unit->count[UC_INTS] = 25827;
    unit->count[UC_POLLED] = 1204;
    unit->count[UC_WAITED] = 1190;
    unit->count[UC_ERRORS] = ( whyDma == WD_ERROR || whyBlock == WB_ERROR ) ? 1 : 0;
    unit->count[UC_RETRIES] = unit->count[UC_ERRORS];
}

static void Demo( LIACCOUNT *acc )
{
    memset( acc, 0, sizeof( *acc ) );
    acc->size = sizeof( *acc );
    acc->version = 0x0104;
    acc->flags = LIF_DMA | LIF_BLOCK | LIF_POLL | LIF_IO32 | LIF_PCIIDE;
    acc->bmBase = 0xF000;
    acc->cable[0].data = 0x1F0;
    acc->cable[0].irq = 14;
    acc->cable[1].data = 0x170;
    acc->cable[1].irq = 15;
    switch ( g_demo ) {
    case 1:     /* everything in use, by PIO and by DMA */
        DemoUnit( acc, 0, "WDC AC22100H", 1, 4124735UL, 0, 0, WD_NOMODE, 16, 16, 2, 0, 1, 0 );
        DemoUnit( acc, 1, "Maxtor 6Y200P0", 2, 398297087UL, 0x42, 0x42, 0, 16, 16, 1, 0, 1, 0 );
        break;
    case 2:     /* everything switched off in SYSTEM.INI */
        acc->flags = LIF_PCIIDE | LIF_SERIAL;
        DemoUnit( acc, 0, "QUANTUM FIREBALL CR8.4A", 1, 16514063UL, 0, 0, WD_OFF, 0, 0, 0, WB_OFF, 0, W3_OFF );
        break;
    case 3:     /* things that stopped, and a shared cable */
        acc->cable[0].flags = CF_OTHER;
        acc->cable[0].lent = 1234;
        acc->cable[0].resets = 2;
        DemoUnit( acc, 0, "IBM-DTLA-307030", 1, 60036479UL, 0, 0x45, WD_ERROR, 0, 16, 1, WB_ERROR, 0, W3_BAD );
        DemoUnit( acc, 1, "ST38410A", 1, 16841663UL, 0, 0x22, WD_ERROR, 0, 8, 2, WB_RESET, 1, 0 );
        break;
    default:    /* the other reasons, and a drive by CHS */
        acc->flags = LIF_DMA | LIF_BLOCK | LIF_POLL | LIF_IO32;
        acc->bmBase = 0;
        DemoUnit( acc, 0, "Conner Peripherals 420MB - CFS420A", 0, 818495UL, 0, 0, WD_NOCTRL, 0, 0, 0, WB_ONE, 0, W3_NONE );
        DemoUnit( acc, 1, "", 1, 0xFFFFFFFFUL, 0, 0, WD_NOCABLE, 0, 0, 2, WB_REFUSED, 0, W3_CMD640 );
        DemoUnit( acc, 2, "SAMSUNG SV0432D", 1, 8421839UL, 0, 0, WD_NOMODE, 0, 0, 0, WB_NONE, 0, W3_NONE );
        break;
    }
}
#endif

static int Query( LIACCOUNT *acc )
{
    unsigned size = 0;
    unsigned version;
    unsigned got;

#ifdef LRGSTAT_DEMO
    if ( g_demo ) {
        Demo( acc );
        return ST_OK;
    }
#endif
    if ( !( GetWinFlags() & WF_ENHANCED ) ) {
        return ST_STANDARD;
    }
    if ( !g_looked ) {
        /* (once: a driver is loaded when Windows starts or not at all) */
        g_entry = LrgEntry();
        g_looked = 1;
    }
    if ( g_entry == 0 ) {
        return ST_NONE;
    }
    version = LrgVersion( g_entry, &size );
    if ( version < LRG_VERSION_MIN ) {
        return ST_REFUSED;
    }
    memset( acc, 0, sizeof( *acc ) );
    got = LrgAccount( g_entry, acc, sizeof( *acc ) );
    if ( got < sizeof( *acc ) ) {
        return ST_REFUSED;
    }
    return ST_OK;
}

static int UnitCount( const LIACCOUNT *acc )
{
    int i;
    int count = 0;

    for ( i = 0; i < 4; i++ ) {
        if ( acc->unit[i].disk ) {
            count++;
        }
    }
    return count;
}

/* The unit shown stays the one chosen while that drive is there */
static void FixSelection( void )
{
    int i;

    if ( g_state != ST_OK || g_acc.unit[g_sel].disk ) {
        return;
    }
    for ( i = 0; i < 4; i++ ) {
        if ( g_acc.unit[i].disk ) {
            g_sel = i;
            return;
        }
    }
}

/*--------------------------------------------------------------------
 * The account in words.  One line across the top...
 *------------------------------------------------------------------*/
static void DescribeDriver( const LIACCOUNT *acc, char *out )
{
    char part[80];
    int drives = UnitCount( acc );

    wsprintf( out, "32-bit disk access for %d %s.", drives, (LPSTR)( drives == 1 ? "drive" : "drives" ) );
    if ( !( acc->flags & LIF_PCIIDE ) ) {
        lstrcat( out, "  No PCI IDE controller." );
    } else if ( acc->bmBase ) {
        wsprintf( part, "  PCI IDE controller, bus-master ports at %04Xh.", acc->bmBase );
        lstrcat( out, part );
    } else {
        lstrcat( out, "  PCI IDE controller, bus-master ports not used." );
    }
    if ( acc->flags & LIF_SERIAL ) {
        lstrcat( out, "  One cable at a time." );
    }
    if ( acc->flags & LIF_TRACE ) {
        lstrcat( out, "  Trace build." );
    }
}

/* ...the drive's name for itself, without the spaces around the name */
static void DescribeModel( const LIUNIT *unit, char *out )
{
    int first = 0;
    int last = 39;
    int i;
    int pos = 0;

    while ( first < 40 && ( unit->model[first] == ' ' || unit->model[first] == 0 ) ) {
        first++;
    }
    while ( last >= first && ( unit->model[last] == ' ' || unit->model[last] == 0 ) ) {
        last--;
    }
    for ( i = first; i <= last; i++ ) {
        out[pos++] = (char)( ( unit->model[i] >= ' ' && unit->model[i] < 127 ) ? unit->model[i] : '?' );
    }
    out[pos] = 0;
    if ( pos == 0 ) {
        lstrcpy( out, "(the drive gave no name)" );
    }
}

/* ...where the drive is and how big */
static void DescribePlace( const LIACCOUNT *acc, const LIUNIT *unit, char *out )
{
    const LICABLE *cable = &acc->cable[unit->cable & 1];
    char sectors[20];
    char size[20];

    FmtSectors( unit->lastSector, sectors );
    FmtSize( unit->lastSector, size );
    wsprintf( out, "%s cable (%Xh, IRQ %u), %s.  %s sectors, %s.",
              (LPSTR)( unit->cable ? "Second" : "First" ), cable->data, (unsigned)cable->irq,
              (LPSTR)( unit->unit ? "slave" : "master" ), (LPSTR)sectors, (LPSTR)size );
}

/* ...and a row for each thing LRGDISK may or may not be using */
static void DescribeRows( const LIACCOUNT *acc, const LIUNIT *unit, ROW *row )
{
    const LICABLE *cable = &acc->cable[unit->cable & 1];
    char first[24];
    char second[24];
    char mode[32];

    /* how the drive is addressed */
    lstrcpy( row[ROW_ADDR].name, "Addressing" );
    row[ROW_ADDR].lamp = LAMP_ON;
    if ( unit->mode == 2 ) {
        lstrcpy( row[ROW_ADDR].state, "LBA, with 48-bit commands past 128GB" );
    } else if ( unit->mode == 1 ) {
        lstrcpy( row[ROW_ADDR].state, "LBA, 28-bit" );
    } else {
        row[ROW_ADDR].lamp = LAMP_OFF;
        lstrcpy( row[ROW_ADDR].state, "CHS, by the BIOS's geometry" );
    }
    wsprintf( row[ROW_ADDR].detail, "The BIOS's geometry is %lu x %lu x %lu.",
              unit->cyls ? unit->cyls - 1 : 0, unit->heads, unit->spt );

    /* bus-master DMA */
    lstrcpy( row[ROW_DMA].name, "Bus-master DMA" );
    if ( unit->xfer ) {
        row[ROW_DMA].lamp = LAMP_ON;
        FmtXfer( unit->xfer, row[ROW_DMA].state );
        FmtNum( unit->count[UC_DMASEC], first );
        wsprintf( row[ROW_DMA].detail, "%s sectors moved by DMA.", (LPSTR)first );
        if ( unit->count[UC_PIOCMD] ) {
            FmtNum( unit->count[UC_PIOCMD], first );
            wsprintf( g_line, "  %s commands went by PIO.", (LPSTR)first );
            lstrcat( row[ROW_DMA].detail, g_line );
        }
    } else if ( unit->whyDma == WD_ERROR ) {
        row[ROW_DMA].lamp = LAMP_WAS;
        lstrcpy( row[ROW_DMA].state, "Stopped after an error" );
        FmtXfer( unit->xfer0, mode );
        wsprintf( row[ROW_DMA].detail, "A DMA transfer went wrong (%s was set).  PIO until Windows restarts.", (LPSTR)mode );
    } else {
        row[ROW_DMA].lamp = LAMP_OFF;
        lstrcpy( row[ROW_DMA].state, "Not used" );
        switch ( unit->whyDma ) {
        case WD_OFF:
            lstrcpy( row[ROW_DMA].detail, "LRGDiskDMA=off is in SYSTEM.INI." );
            break;
        case WD_NOCTRL:
            lstrcpy( row[ROW_DMA].detail, "There is no PCI bus-master IDE controller." );
            break;
        case WD_NOCABLE:
            lstrcpy( row[ROW_DMA].detail, "The controller has no bus-master ports for this cable." );
            break;
        default:
            lstrcpy( row[ROW_DMA].detail, "No DMA mode was set on the drive before Windows started." );
            break;
        }
    }

    /* block mode */
    lstrcpy( row[ROW_BLOCK].name, "Block mode" );
    if ( unit->mult >= 2 ) {
        row[ROW_BLOCK].lamp = LAMP_ON;
        wsprintf( row[ROW_BLOCK].state, "%u sectors a block", (unsigned)unit->mult );
        if ( unit->multSource == 2 ) {
            wsprintf( row[ROW_BLOCK].detail, "LRGDISK set the size.  The drive takes up to %u.", (unsigned)unit->multMax );
        } else {
            lstrcpy( row[ROW_BLOCK].detail, "The drive was already set to this size." );
        }
        if ( unit->xfer ) {
            lstrcat( row[ROW_BLOCK].detail, "  For the commands that go by PIO." );
        }
    } else if ( unit->whyBlock == WB_ERROR ) {
        row[ROW_BLOCK].lamp = LAMP_WAS;
        lstrcpy( row[ROW_BLOCK].state, "Stopped after an error" );
        wsprintf( row[ROW_BLOCK].detail, "A block command went wrong (%u sectors a block was set).  One sector a block now.", (unsigned)unit->mult0 );
    } else if ( unit->whyBlock == WB_RESET ) {
        row[ROW_BLOCK].lamp = LAMP_WAS;
        lstrcpy( row[ROW_BLOCK].state, "Stopped after a reset" );
        lstrcpy( row[ROW_BLOCK].detail, "A DOS program reset the cable, and the drive would not take the block size back." );
    } else {
        row[ROW_BLOCK].lamp = LAMP_OFF;
        lstrcpy( row[ROW_BLOCK].state, "Not used" );
        switch ( unit->whyBlock ) {
        case WB_OFF:
            lstrcpy( row[ROW_BLOCK].detail, "LRGDiskBlockMode=off is in SYSTEM.INI." );
            break;
        case WB_ONE:
            lstrcpy( row[ROW_BLOCK].detail, "The drive is set to blocks of one sector, and LRGDISK leaves that setting alone." );
            break;
        case WB_REFUSED:
            lstrcpy( row[ROW_BLOCK].detail, "The drive refused the block size." );
            break;
        default:
            lstrcpy( row[ROW_BLOCK].detail, "The drive has no block mode." );
            break;
        }
    }

    /* the look for the next block before its interrupt */
    lstrcpy( row[ROW_POLL].name, "Look before interrupt" );
    if ( acc->flags & LIF_POLL ) {
        row[ROW_POLL].lamp = LAMP_ON;
        lstrcpy( row[ROW_POLL].state, "On" );
        FmtNum( unit->count[UC_POLLED], first );
        FmtNum( unit->count[UC_WAITED], second );
        wsprintf( row[ROW_POLL].detail, "%s blocks were ready without an interrupt; %s were left to one.", (LPSTR)first, (LPSTR)second );
    } else {
        row[ROW_POLL].lamp = LAMP_OFF;
        lstrcpy( row[ROW_POLL].state, "Not used" );
        lstrcpy( row[ROW_POLL].detail, "LRGDiskPoll=off is in SYSTEM.INI." );
    }

    /* the data port 32 bits at a time */
    lstrcpy( row[ROW_IO32].name, "32-bit data" );
    if ( unit->io32 ) {
        row[ROW_IO32].lamp = LAMP_ON;
        lstrcpy( row[ROW_IO32].state, "On" );
        lstrcpy( row[ROW_IO32].detail, "PIO data moves 32 bits at a time." );
    } else {
        row[ROW_IO32].lamp = LAMP_OFF;
        lstrcpy( row[ROW_IO32].state, "Not used" );
        switch ( unit->why32 ) {
        case W3_OFF:
            lstrcpy( row[ROW_IO32].detail, "LRGDisk32BitIO=off is in SYSTEM.INI." );
            break;
        case W3_CMD640:
            lstrcpy( row[ROW_IO32].detail, "The controller is a CMD640, which corrupts 32-bit transfers." );
            break;
        case W3_BAD:
            lstrcpy( row[ROW_IO32].detail, "A 32-bit test read of the drive came back different." );
            break;
        default:
            lstrcpy( row[ROW_IO32].detail, "There is no PCI IDE controller, and the drive does not report 32-bit I/O." );
            break;
        }
    }

    /* the cable, shared with something DOS programs drive */
    lstrcpy( row[ROW_CABLE].name, "Cable sharing" );
    if ( cable->flags & CF_OTHER ) {
        row[ROW_CABLE].lamp = LAMP_ON;
        lstrcpy( row[ROW_CABLE].state, "Shared with a CD-ROM drive or another device" );
        FmtNum( cable->lent, first );
        FmtNum( cable->resets, second );
        wsprintf( row[ROW_CABLE].detail, "DOS programs were given the cable %s times and reset the cable %s times.", (LPSTR)first, (LPSTR)second );
    } else {
        row[ROW_CABLE].lamp = LAMP_OFF;
        lstrcpy( row[ROW_CABLE].state, "Not needed" );
        lstrcpy( row[ROW_CABLE].detail, "Only LRGDISK's drives are on this cable." );
    }
}

/* ...and the counts: six down the left, six down the right */
static void DescribeTallies( const LIUNIT *unit, TALLY *tally )
{
    static const char *label[TALLY_COUNT] = {
        "Commands from Windows", "Sectors by DMA", "Sectors by PIO",
        "Errors from the drive", "Commands sent again", "Commands given up",
        "Interrupts", "Sectors an interrupt", "Blocks without an interrupt",
        "Blocks the drive ended early", "Interrupts with nothing to do", "Lost interrupts recovered"
    };
    static const int which[TALLY_COUNT] = {
        UC_CMDS, UC_DMASEC, UC_PIOSEC, UC_ERRORS, UC_RETRIES, UC_FAILED,
        UC_INTS, -1, UC_POLLED, UC_SHORT, UC_SPARE, UC_KICK
    };
    int i;

    for ( i = 0; i < TALLY_COUNT; i++ ) {
        lstrcpy( tally[i].label, label[i] );
        if ( which[i] >= 0 ) {
            FmtNum( unit->count[which[i]], tally[i].value );
        } else if ( unit->count[UC_INTS] == 0 ) {
            lstrcpy( tally[i].value, "-" );
        } else {
            /* (tenths: the sum of two counts times ten stays in 32
               bits for the first 400 million sectors) */
            unsigned long sectors = unit->count[UC_DMASEC] + unit->count[UC_PIOSEC];
            unsigned long tenths;

            if ( sectors > 400000000UL ) {
                tenths = ( sectors / unit->count[UC_INTS] ) * 10;
            } else {
                tenths = sectors * 10 / unit->count[UC_INTS];
            }
            wsprintf( tally[i].value, "%lu.%u", tenths / 10, (unsigned)( tenths % 10 ) );
        }
    }
}

/* What to say when there is no account to show.  The text itself is
   handed back, not a copy: the longest is longer than g_line. */
static const char *TroubleText( int state )
{
    if ( state == ST_STANDARD ) {
        return "Windows is running in standard mode.\n\n"
               "LRGDISK works in 386 enhanced mode only.";
    }
    if ( state == ST_REFUSED ) {
        return "LRGDISK.386 is loaded but did not answer.\n\n"
               "LRGSTAT needs LRGDISK 1.4 or later.";
    }
    return "LRGDISK 1.4 or later is not running.\n\n"
           "LRGDISK is loaded from SYSTEM.INI, under [386Enh]:\n\n"
           "    device=lrgdisk.386      in place of device=*wdctrl\n"
           "    device=*int13               if that line is not there already\n"
           "    32BitDiskAccess=on\n\n"
           "With LRGDiskReport=on there as well, LRGDISK writes what the\n"
           "start-up checks found to LRGDISK.TXT in the boot drive's root.";
}

/*--------------------------------------------------------------------
 * LRGSTAT /S file: the account as text
 *------------------------------------------------------------------*/
static void Emit( HFILE file, const char *text )
{
    _lwrite( file, text, lstrlen( text ) );
    _lwrite( file, "\r\n", 2 );
}

static int Report( LPSTR path )
{
    HFILE file = _lcreat( path, 0 );
    static const char *lampName[3] = { "no ", "yes", "was" };
    int i;
    int j;

    if ( file == HFILE_ERROR ) {
        return 1;
    }
    g_state = Query( &g_acc );
    if ( g_state != ST_OK ) {
        /* (the text's first line says it; the rest is for a window) */
        const char *text = TroubleText( g_state );

        for ( i = 0; text[i] && text[i] != '\n' && i < (int)sizeof( g_line ) - 1; i++ ) {
            g_line[i] = text[i];
        }
        g_line[i] = 0;
        Emit( file, g_line );
        _lclose( file );
        return 2;
    }
    wsprintf( g_line, "LRGDISK %u.%u", g_acc.version >> 8, g_acc.version & 0xFF );
    Emit( file, g_line );
    DescribeDriver( &g_acc, g_line );
    Emit( file, g_line );
    for ( i = 0; i < 4; i++ ) {
        const LIUNIT *unit = &g_acc.unit[i];

        if ( unit->disk == 0 ) {
            continue;
        }
        Emit( file, "" );
        wsprintf( g_line, "Disk %02Xh: ", (unsigned)unit->disk );
        DescribeModel( unit, g_line + lstrlen( g_line ) );
        Emit( file, g_line );
        lstrcpy( g_line, "  " );
        DescribePlace( &g_acc, unit, g_line + 2 );
        Emit( file, g_line );
        DescribeRows( &g_acc, unit, g_row );
        for ( j = 0; j < ROW_COUNT; j++ ) {
            /* (g_line is DescribeRows' own scratch: put together after) */
            wsprintf( g_line, "  [%s] %s: %s.  %s", (LPSTR)lampName[g_row[j].lamp],
                      (LPSTR)g_row[j].name, (LPSTR)g_row[j].state, (LPSTR)g_row[j].detail );
            Emit( file, g_line );
        }
        DescribeTallies( unit, g_tally );
        for ( j = 0; j < TALLY_COUNT; j++ ) {
            wsprintf( g_line, "  %s: %s", (LPSTR)g_tally[j].label, (LPSTR)g_tally[j].value );
            Emit( file, g_line );
        }
    }
    _lclose( file );
    return 0;
}

/*--------------------------------------------------------------------
 * The window
 *------------------------------------------------------------------*/
static void Put( HDC hdc, int x, int y, const char *text )
{
    TextOut( hdc, x, y, text, lstrlen( text ) );
}

static int TextWidth( HDC hdc, const char *text )
{
    return (int)LOWORD( GetTextExtent( hdc, text, lstrlen( text ) ) );
}

static void PutRight( HDC hdc, int right, int y, const char *text )
{
    Put( hdc, right - TextWidth( hdc, text ), y, text );
}

/* A lamp: green in use, yellow stopped since start-up, grey not used.
   The 16 colours a VGA has, so none of them is dithered. */
static void DrawLamp( HDC hdc, int x, int y, int size, int lamp )
{
    COLORREF fill = RGB( 192, 192, 192 );
    COLORREF edge = RGB( 128, 128, 128 );
    HBRUSH brush;
    HPEN pen;
    HGDIOBJ oldBrush;
    HGDIOBJ oldPen;

    if ( lamp == LAMP_ON ) {
        fill = RGB( 0, 255, 0 );
        edge = RGB( 0, 0, 0 );
    } else if ( lamp == LAMP_WAS ) {
        fill = RGB( 255, 255, 0 );
        edge = RGB( 0, 0, 0 );
    }
    brush = CreateSolidBrush( fill );
    pen = CreatePen( PS_SOLID, 1, edge );
    oldBrush = SelectObject( hdc, brush );
    oldPen = SelectObject( hdc, pen );
    Ellipse( hdc, x, y, x + size, y + size );
    SelectObject( hdc, oldBrush );
    SelectObject( hdc, oldPen );
    DeleteObject( brush );
    DeleteObject( pen );
}

static void DrawFrame( HDC hdc, const RECT *rect )
{
    HGDIOBJ oldBrush = SelectObject( hdc, GetStockObject( NULL_BRUSH ) );
    HGDIOBJ oldPen = SelectObject( hdc, GetStockObject( BLACK_PEN ) );

    Rectangle( hdc, rect->left, rect->top, rect->right, rect->bottom );
    SelectObject( hdc, oldBrush );
    SelectObject( hdc, oldPen );
}

/* The whole client area, into hdc */
static void PaintAll( HDC hdc )
{
    RECT rect;
    RECT panel;
    HBRUSH back = CreateSolidBrush( GetSysColor( COLOR_WINDOW ) );
    COLORREF ink = GetSysColor( COLOR_WINDOWTEXT );
    COLORREF grey = RGB( 128, 128, 128 );
    HGDIOBJ oldFont;
    int margin = g_charW * 2;
    int x;
    int y;
    int i;
    int nameW;
    int lamp = g_lineH - 4;
    int tabH = g_lineH + 8;
    int half;
    const LIUNIT *unit;

    SetRect( &rect, 0, 0, g_clientW, g_clientH );
    FillRect( hdc, &rect, back );
    DeleteObject( back );
    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, ink );
    oldFont = SelectObject( hdc, g_fontText );

    if ( g_state != ST_OK ) {
        SetRect( &rect, margin * 2, margin * 2, g_clientW - margin * 2, g_clientH - margin );
        DrawText( hdc, TroubleText( g_state ), -1, &rect, DT_LEFT | DT_NOPREFIX | DT_EXPANDTABS );
        SelectObject( hdc, oldFont );
        return;
    }

    /* the heading: the driver, and a line about all of it */
    y = margin / 2 + 2;
    SelectObject( hdc, g_fontBold );
    wsprintf( g_line, "LRGDISK %u.%u", g_acc.version >> 8, g_acc.version & 0xFF );
    Put( hdc, margin, y, g_line );
    x = margin + TextWidth( hdc, g_line ) + g_charW * 3;
    SelectObject( hdc, g_fontText );
    DescribeDriver( &g_acc, g_line );
    Put( hdc, x, y, g_line );
    y += g_lineH + 6;

    /* a tab for each drive; the one shown stands open onto the panel */
    x = margin;
    for ( i = 0; i < 4; i++ ) {
        SetRectEmpty( &g_tab[i] );
        if ( g_acc.unit[i].disk == 0 ) {
            continue;
        }
        wsprintf( g_line, "Disk %02Xh", (unsigned)g_acc.unit[i].disk );
        SelectObject( hdc, i == g_sel ? g_fontBold : g_fontText );
        SetRect( &g_tab[i], x, y, x + TextWidth( hdc, g_line ) + g_charW * 5, y + tabH );
        if ( i != g_sel ) {
            HBRUSH shade = CreateSolidBrush( RGB( 192, 192, 192 ) );

            FillRect( hdc, &g_tab[i], shade );
            DeleteObject( shade );
        }
        DrawFrame( hdc, &g_tab[i] );
        Put( hdc, x + g_charW * 5 / 2, y + 4, g_line );
        x = g_tab[i].right + 4;
    }
    y += tabH - 1;
    SetRect( &panel, margin, y, g_clientW - margin, g_clientH - margin / 2 );
    DrawFrame( hdc, &panel );
    if ( !IsRectEmpty( &g_tab[g_sel] ) ) {
        /* (the open tab's lower edge, rubbed out) */
        HPEN wipe = CreatePen( PS_SOLID, 1, GetSysColor( COLOR_WINDOW ) );
        HGDIOBJ oldPen = SelectObject( hdc, wipe );

        MoveTo( hdc, g_tab[g_sel].left + 1, y );
        LineTo( hdc, g_tab[g_sel].right - 1, y );
        SelectObject( hdc, oldPen );
        DeleteObject( wipe );
    }

    /* the drive: its name, where the drive is and how big */
    unit = &g_acc.unit[g_sel];
    x = panel.left + g_charW * 2;
    y += 7;
    SelectObject( hdc, g_fontBold );
    DescribeModel( unit, g_line );
    Put( hdc, x, y, g_line );
    y += g_lineH;
    SelectObject( hdc, g_fontText );
    DescribePlace( &g_acc, unit, g_line );
    Put( hdc, x, y, g_line );
    y += g_lineH + 6;

    /* a row for each thing: the lamp, its name, how it stands, why */
    DescribeRows( &g_acc, unit, g_row );
    SelectObject( hdc, g_fontBold );
    nameW = 0;
    for ( i = 0; i < ROW_COUNT; i++ ) {
        int width = TextWidth( hdc, g_row[i].name );

        if ( width > nameW ) {
            nameW = width;
        }
    }
    nameW += g_charW * 3;
    for ( i = 0; i < ROW_COUNT; i++ ) {
        DrawLamp( hdc, x, y + 2, lamp, g_row[i].lamp );
        SelectObject( hdc, g_fontBold );
        SetTextColor( hdc, ink );
        Put( hdc, x + lamp + g_charW * 2, y, g_row[i].name );
        Put( hdc, x + lamp + g_charW * 2 + nameW, y, g_row[i].state );
        SelectObject( hdc, g_fontText );
        SetTextColor( hdc, grey );
        Put( hdc, x + lamp + g_charW * 2 + nameW, y + g_lineH, g_row[i].detail );
        y += g_lineH * 2 + 3;
    }
    SetTextColor( hdc, ink );

    /* the counts */
    y += 3;
    MoveTo( hdc, panel.left, y );
    LineTo( hdc, panel.right, y );
    y += 5;
    SelectObject( hdc, g_fontBold );
    Put( hdc, x, y, "Since Windows started" );
    y += g_lineH + 1;
    SelectObject( hdc, g_fontText );
    DescribeTallies( unit, g_tally );
    half = ( panel.right - panel.left ) / 2;
    for ( i = 0; i < TALLY_COUNT; i++ ) {
        int column = i / ( TALLY_COUNT / 2 );
        int line = i % ( TALLY_COUNT / 2 );
        int left = x + column * half;

        Put( hdc, left, y + line * g_lineH, g_tally[i].label );
        PutRight( hdc, left + half - g_charW * 8, y + line * g_lineH, g_tally[i].value );
    }
    SelectObject( hdc, oldFont );
}

/* Off the screen first, so the counts do not flicker as they move */
static void Paint( HWND hwnd )
{
    PAINTSTRUCT paint;
    HDC hdc = BeginPaint( hwnd, &paint );
    HDC memory = CreateCompatibleDC( hdc );
    HBITMAP bitmap = CreateCompatibleBitmap( hdc, g_clientW, g_clientH );

    if ( memory && bitmap ) {
        HGDIOBJ oldBitmap = SelectObject( memory, bitmap );

        PaintAll( memory );
        BitBlt( hdc, 0, 0, g_clientW, g_clientH, memory, 0, 0, SRCCOPY );
        SelectObject( memory, oldBitmap );
    } else {
        PaintAll( hdc );
    }
    if ( bitmap ) {
        DeleteObject( bitmap );
    }
    if ( memory ) {
        DeleteDC( memory );
    }
    EndPaint( hwnd, &paint );
}

/* The drive before or after the one shown, among those there are */
static void Step( HWND hwnd, int by )
{
    int i;
    int unit = g_sel;

    if ( g_state != ST_OK ) {
        return;
    }
    for ( i = 0; i < 4; i++ ) {
        unit = ( unit + by + 4 ) % 4;
        if ( g_acc.unit[unit].disk ) {
            break;
        }
    }
    if ( unit != g_sel && g_acc.unit[unit].disk ) {
        g_sel = unit;
        InvalidateRect( hwnd, NULL, FALSE );
    }
}

static void Refresh( HWND hwnd )
{
    int state = Query( &g_fresh );

    if ( state != g_state || memcmp( &g_fresh, &g_acc, sizeof( g_acc ) ) != 0 ) {
        g_state = state;
        g_acc = g_fresh;
        FixSelection();
        InvalidateRect( hwnd, NULL, FALSE );
    }
}

long FAR PASCAL __export MainWndProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
{
    int i;
    POINT point;

    switch ( message ) {
    case WM_CREATE:
        SetTimer( hwnd, 1, 1000, NULL );
        return 0;

    case WM_TIMER:
        Refresh( hwnd );
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Paint( hwnd );
        return 0;

    case WM_LBUTTONDOWN:
        point.x = (int)LOWORD( lParam );
        point.y = (int)HIWORD( lParam );
        for ( i = 0; i < 4; i++ ) {
            if ( g_state == ST_OK && g_acc.unit[i].disk && PtInRect( &g_tab[i], point ) && i != g_sel ) {
                g_sel = i;
                InvalidateRect( hwnd, NULL, FALSE );
            }
        }
        return 0;

    case WM_KEYDOWN:
        if ( wParam == VK_LEFT || wParam == VK_UP ) {
            Step( hwnd, -1 );
        } else if ( wParam == VK_RIGHT || wParam == VK_DOWN || wParam == VK_TAB ) {
            Step( hwnd, 1 );
        } else if ( wParam == VK_F5 ) {
            Refresh( hwnd );
        } else if ( wParam == VK_ESCAPE ) {
            PostMessage( hwnd, WM_CLOSE, 0, 0L );
        }
        return 0;

    case WM_DESTROY:
        KillTimer( hwnd, 1 );
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProc( hwnd, message, wParam, lParam );
}

/* The two fonts - Windows' small one and the same in bold - and the
   client area's size worked out from them */
static void Measure( void )
{
    HDC hdc = GetDC( NULL );
    TEXTMETRIC metric;
    LOGFONT logFont;
    HGDIOBJ oldFont;

    g_fontText = GetStockObject( ANSI_VAR_FONT );
    GetObject( g_fontText, sizeof( logFont ), &logFont );
    logFont.lfWeight = FW_BOLD;
    g_fontBold = CreateFontIndirect( &logFont );
    if ( g_fontBold == NULL ) {
        g_fontBold = g_fontText;
    }
    oldFont = SelectObject( hdc, g_fontBold );
    GetTextMetrics( hdc, &metric );
    SelectObject( hdc, oldFont );
    ReleaseDC( NULL, hdc );
    g_lineH = metric.tmHeight + 2;
    g_charW = metric.tmAveCharWidth;
    g_clientW = g_charW * 104;
    /* the heading, the tabs, two lines for the drive, the rows, the
       counts' heading and their six lines, and the gaps PaintAll
       leaves between them */
    g_clientH = ( g_charW + 2 ) + ( g_lineH + 6 ) + ( g_lineH + 7 ) + 7 + ( g_lineH * 2 + 6 )
              + ROW_COUNT * ( g_lineH * 2 + 3 ) + 8 + ( g_lineH + 1 ) + ( TALLY_COUNT / 2 ) * g_lineH + 6 + g_charW;
}

int PASCAL WinMain( HINSTANCE instance, HINSTANCE previous, LPSTR commandLine, int show )
{
    WNDCLASS windowClass;
    HWND hwnd;
    MSG message;
    RECT rect;
    int width;
    int height;
    int x;
    int y;

    g_inst = instance;
    GetProfileString( "intl", "sThousand", ",", g_sep, sizeof( g_sep ) );

    while ( *commandLine == ' ' ) {
        commandLine++;
    }
#ifdef LRGSTAT_DEMO
    if ( commandLine[0] == '/' && commandLine[1] == 'D' ) {
        commandLine += 2;
        while ( *commandLine == ' ' ) {
            commandLine++;
        }
        g_demo = *commandLine >= '1' && *commandLine <= '9' ? *commandLine - '0' : 1;
        if ( *commandLine ) {
            commandLine++;
        }
        while ( *commandLine == ' ' ) {
            commandLine++;
        }
        if ( *commandLine >= '0' && *commandLine <= '3' ) {
            g_sel = *commandLine - '0';
            commandLine++;
        }
        while ( *commandLine == ' ' ) {
            commandLine++;
        }
    }
#endif
    if ( commandLine[0] == '/' && ( commandLine[1] == 'S' || commandLine[1] == 's' ) ) {
        commandLine += 2;
        while ( *commandLine == ' ' ) {
            commandLine++;
        }
        return Report( *commandLine ? commandLine : (LPSTR)"LRGSTAT.TXT" );
    }

    if ( previous == NULL ) {
        windowClass.style = 0;
        windowClass.lpfnWndProc = MainWndProc;
        windowClass.cbClsExtra = 0;
        windowClass.cbWndExtra = 0;
        windowClass.hInstance = instance;
        windowClass.hIcon = LoadIcon( instance, MAKEINTRESOURCE( 1 ) );
        windowClass.hCursor = LoadCursor( NULL, IDC_ARROW );
        windowClass.hbrBackground = NULL;
        windowClass.lpszMenuName = NULL;
        windowClass.lpszClassName = g_class;
        if ( !RegisterClass( &windowClass ) ) {
            return 1;
        }
    }

    Measure();
    g_state = Query( &g_acc );
    FixSelection();

    SetRect( &rect, 0, 0, g_clientW, g_clientH );
    AdjustWindowRect( &rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE );
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    x = ( GetSystemMetrics( SM_CXSCREEN ) - width ) / 2;
    y = ( GetSystemMetrics( SM_CYSCREEN ) - height ) / 3;
    if ( x < 0 ) {
        x = 0;
    }
    if ( y < 0 ) {
        y = 0;
    }
    hwnd = CreateWindow( g_class, g_title, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                         x, y, width, height, NULL, NULL, instance, NULL );
    if ( hwnd == NULL ) {
        return 1;
    }
    ShowWindow( hwnd, show );
    UpdateWindow( hwnd );
    while ( GetMessage( &message, NULL, 0, 0 ) ) {
        TranslateMessage( &message );
        DispatchMessage( &message );
    }
    if ( g_fontBold != g_fontText ) {
        DeleteObject( g_fontBold );
    }
    return (int)message.wParam;
}
