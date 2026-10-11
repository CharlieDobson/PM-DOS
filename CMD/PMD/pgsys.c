/*
 * PGSYS.C - the pages Computer, Video, OS Version, Network, Mouse and
 * Other Adapters, and the table that says which routine builds which
 * page.
 *
 * Each page is asked for twice: for the two lines beside its button on
 * the main screen and for the page itself.  What was found the first
 * time is kept, so the second asking costs nothing and says the same.
 *
 * WHERE THE FACTS COME FROM, since none can be read directly: the BIOS
 * data area and both ROMs through sys_peek, the configuration table and
 * the video and game-port calls through sys_bios, the processor from
 * CPU.C, and PM-DOS's own account of itself from 21h/F1h.
 */
#include "pmd.h"

const char *const page_names[PG_COUNT] = {
    "Computer", "Memory", "Video", "Network", "OS Version", "Mouse", "Other Adapters",
    "Disk Drives", "LPT Ports", "COM Ports", "IRQ Status", "TSR Programs", "Device Drivers"
};

static void sum_set( char *dst, const char *src )
{
    u32 len = str_len( src );

    if ( len > SUM_WIDTH ) {
        len = SUM_WIDTH;
    }
    mem_cpy( dst, src, len );
    dst[len] = 0;
    while ( len && dst[len - 1] == ' ' ) {
        dst[--len] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Computer                                                            */
/* ------------------------------------------------------------------ */

/* who made the machine, where its BIOS says */
static const char *const computer_names[] = {
    "QEMU", "Bochs", "DOSBox", "86Box", "VirtualBox", "VMware",
    "Compaq", "Dell", "Hewlett-Packard", "Hewlett Packard", "Gateway", "Packard Bell",
    "Toshiba", "Zenith", "Tandy", "Epson", "Olivetti", "Northgate", "Everex", "Micron",
    "Digital Equipment", "AT&T", "Unisys", "Amstrad", "Commodore", "Leading Edge",
    "Samsung", "Hyundai", "Goldstar", "Mitsubishi", "Panasonic", "Siemens", "Tandon",
    "Texas Instruments", "CompuAdd", "Mitac", "Acer", "AST", "NEC", "NCR", "Wang",
    "Zeos", "Wyse", "ALR", "DTK", "Sharp", "Sony",
    "ASUS", "Gigabyte", "Micro-Star", "Supermicro", "Tyan",
    NULL
};

/* and who wrote the BIOS */
static const char *const bios_names[] = {
    "SeaBIOS", "Phoenix", "Award", "American Megatrends", "Microid Research", "Quadtel",
    "General Software", "Insyde", "Micronics", "Olivetti", "Western Digital", "Compaq",
    "Mylex", "Zenith", "Bochs", "DOSBox", "AMI", "DTK", "IBM",
    NULL
};

typedef struct {
    int  have;
    char name[24], maker[24];
    char versions[3][64];
    int  nversions;
    char date[12];
    int  config_ok;
    u8   model, submodel, revision, features;
    CPUINFO cpu;
    char copro[12];
    int  enhanced, dma, cascade, mca, eisa, pci;
    u32  ebda_seg, ebda_k;
} COMPUTER;

static COMPUTER computer;
static u8 *bios_rom;

static const u8 *system_rom( void )
{
    if ( bios_rom == NULL ) {
        bios_rom = rom_load( 0xF0000UL, 0x10000UL );
    }
    return bios_rom;
}

static const char *bios_category( const COMPUTER *comp )
{
    static const struct {
        u8          model, submodel;        /* submodel FFh: any */
        const char *name;
    } kinds[] = {
        { 0xFF, 0xFF, "IBM PC" },           { 0xFE, 0xFF, "IBM PC/XT" },
        { 0xFB, 0xFF, "IBM PC/XT" },        { 0xFD, 0xFF, "IBM PCjr" },
        { 0xF9, 0xFF, "IBM PC Convertible" },
        { 0xFC, 0x02, "IBM PC/XT-286" },    { 0xFC, 0x04, "IBM PS/2 Model 50" },
        { 0xFC, 0x05, "IBM PS/2 Model 60" },{ 0xFC, 0x09, "IBM PS/2 Model 30 286" },
        { 0xFC, 0x0B, "IBM PS/1" },         { 0xFC, 0xFF, "IBM PC/AT" },
        { 0xFA, 0x01, "IBM PS/2 Model 25" },{ 0xFA, 0xFF, "IBM PS/2 Model 30" },
        { 0xF8, 0x00, "IBM PS/2 Model 80" },{ 0xF8, 0x01, "IBM PS/2 Model 80" },
        { 0xF8, 0x0B, "IBM PS/2 Model P70" },
        { 0xF8, 0x0C, "IBM PS/2 Model 55SX" },
        { 0xF8, 0x14, "IBM PS/2 Model 90" },{ 0xF8, 0x16, "IBM PS/2 Model 90" },
        { 0xF8, 0x1C, "IBM PS/2 Model 65SX" },
        { 0xF8, 0xFF, "IBM PS/2 Model 70" }
    };
    int index;

    for ( index = 0; index < (int)(sizeof( kinds ) / sizeof( kinds[0] )); index++ ) {
        if ( kinds[index].model == comp->model
             && (kinds[index].submodel == 0xFF || kinds[index].submodel == comp->submodel) ) {
            return kinds[index].name;
        }
    }
    return "Unknown";
}

static COMPUTER *get_computer( void )
{
    COMPUTER *comp = &computer;
    const u8 *rom;
    BREGS regs;
    u8 table[10];
    int index, last, count;
    u32 fpu;

    if ( comp->have ) {
        return comp;
    }
    mem_set( comp, 0, sizeof( *comp ) );
    comp->have = 1;
    str_cpy( comp->name, "Unknown" );
    str_cpy( comp->maker, "Unknown" );
    rom = system_rom();
    if ( rom ) {
        index = rom_find_name( rom, 0x10000UL, computer_names );
        if ( index >= 0 ) {
            str_cpy( comp->name, computer_names[index] );
        }
        index = rom_find_name( rom, 0x10000UL, bios_names );
        if ( index >= 0 ) {
            str_cpy( comp->maker, bios_names[index] );
        }
        for ( index = (int)str_len( comp->name ); index > 0 && comp->name[index - 1] == ' '; index-- ) {
            comp->name[index - 1] = 0;
        }
        for ( index = (int)str_len( comp->maker ); index > 0 && comp->maker[index - 1] == ' '; index-- ) {
            comp->maker[index - 1] = 0;
        }
        comp->nversions = rom_versions( rom, 0x10000UL, comp->versions );
        /* the date a BIOS keeps at F000:FFF5 is THE date; failing that,
           the latest one anywhere in it */
        if ( rom[0xFFF7] == '/' && rom[0xFFFA] == '/' ) {
            mem_cpy( comp->date, rom + 0xFFF5, 8 );
            comp->date[8] = 0;
        } else {
            rom_date( rom, 0x10000UL, comp->date );
        }
        comp->eisa = mem_cmp( rom + 0xFFD9, "EISA", 4 ) == 0;
    }

    /* INT 15h/C0h: ES:BX -> the configuration table */
    mem_set( &regs, 0, sizeof( regs ) );
    regs.eax = 0xC000;
    if ( sys_bios( 0x15, &regs ) && !(regs.flags & 1) && ((regs.eax >> 8) & 0xFF) == 0
         && sys_peek( FAR_FLAT( regs.es, regs.ebx & 0xFFFF ), table, sizeof( table ) ) ) {
        comp->config_ok = 1;
        comp->model = table[2];
        comp->submodel = table[3];
        comp->revision = table[4];
        comp->features = table[5];
    } else {
        comp->model = peek_b( 0xFFFFEUL );
        comp->features = comp->model == 0xFC || comp->model == 0xF8 ? 0x40 : 0;
    }
    comp->cascade = (comp->features & 0x40) != 0;
    comp->mca = (comp->features & 0x02) != 0;
    if ( comp->features & 0x04 ) {
        comp->ebda_seg = peek_w( 0x40E );
        if ( comp->ebda_seg >= 0x1000 ) {
            comp->ebda_k = peek_b( comp->ebda_seg << 4 );
        } else {
            comp->ebda_seg = 0;
        }
    }
    comp->enhanced = (peek_b( 0x496 ) & 0x10) != 0;
    comp->dma = (peek_w( 0x410 ) & 0x0100) == 0;
    comp->pci = sys_pci_here( &last, &count );

    cpu_identify( &comp->cpu );
    fpu = sys_fpu_flags();
    if ( !(fpu & FPUF_X87) ) {
        str_cpy( comp->copro, "None" );
    } else if ( comp->cpu.fpu_internal ) {
        str_cpy( comp->copro, "Internal" );
    } else {
        str_cpy( comp->copro, fpu_is_287() ? "80287" : "80387" );
    }
    return comp;
}

int bus_is_mca( void )
{
    return get_computer()->mca;
}

static const char *bus_name( const COMPUTER *comp )
{
    if ( comp->mca ) {
        return "Micro Channel";
    }
    if ( comp->pci ) {
        return comp->eisa ? "PCI and EISA" : "PCI and ISA";
    }
    return comp->eisa ? "EISA" : "ISA/AT/Classic Bus";
}

void pg_computer( TEXT *text )
{
    COMPUTER *comp = get_computer();
    int index, width = 19;

    tx_field( text, width, "Computer Name: ", comp->name );
    tx_field( text, width, "BIOS Manufacturer: ", comp->maker );
    tx_field( text, width, "BIOS Version: ", comp->versions[0] );
    for ( index = 1; index < comp->nversions; index++ ) {
        tx_field( text, width, "", comp->versions[index] );
    }
    tx_field( text, width, "BIOS Category: ", bios_category( comp ) );
    if ( comp->config_ok ) {
        tx_fieldf( text, width, "BIOS ID Bytes: ", "%02X %02X %02X", comp->model, comp->submodel,
                   comp->revision );
    } else {
        tx_fieldf( text, width, "BIOS ID Bytes: ", "%02X", comp->model );
    }
    tx_field( text, width, "BIOS Date: ", comp->date );
    tx_field( text, width, "Processor: ", comp->cpu.name );
    if ( comp->cpu.cpuid ) {
        tx_fieldf( text, width, "CPUID: ", "%s, family %u model %u stepping %u", comp->cpu.vendor,
                   comp->cpu.family, comp->cpu.model, comp->cpu.stepping );
        if ( comp->cpu.features[0] ) {
            tx_field( text, width, "Features: ", comp->cpu.features );
        }
    }
    tx_field( text, width, "Math Coprocessor: ", comp->copro );
    tx_field( text, width, "Keyboard: ", comp->enhanced ? "Enhanced" : "Non-Enhanced" );
    tx_field( text, width, "Bus Type: ", bus_name( comp ) );
    tx_field( text, width, "DMA Controller: ", comp->dma ? "Yes" : "No" );
    tx_field( text, width, "Cascaded IRQ2: ", comp->cascade ? "Yes" : "No" );
    if ( comp->ebda_seg ) {
        tx_fieldf( text, width, "BIOS Data Segment: ", "%04X %uk", comp->ebda_seg, comp->ebda_k );
    } else {
        tx_field( text, width, "BIOS Data Segment: ", "None" );
    }
}

void sum_computer( char *line1, char *line2 )
{
    COMPUTER *comp = get_computer();
    char both[80];

    sfmt( both, sizeof( both ), "%s/%s", comp->name, comp->maker );
    sum_set( line1, both );
    if ( str_cmp( comp->copro, "None" ) == 0 || str_cmp( comp->copro, "Internal" ) == 0 ) {
        sum_set( line2, comp->cpu.name );
    } else {
        sfmt( both, sizeof( both ), "%s/%s", comp->cpu.name, comp->copro );
        sum_set( line2, both );
    }
}

/* ------------------------------------------------------------------ */
/* Video                                                               */
/* ------------------------------------------------------------------ */

static const char *const video_names[] = {
    "Cirrus", "Tseng", "Trident", "Paradise", "Western Digital", "Oak Tech", "Chips & Tech",
    "Chips and Tech", "Video Seven", "Headland", "Genoa", "Matrox", "Number Nine", "Diamond",
    "Orchid", "Hercules", "NVIDIA", "3dfx", "Rendition", "Realtek", "Avance", "Weitek",
    "Alliance", "ARK Logic", "NeoMagic", "Compaq", "Bochs", "Plex86", "SeaBIOS", "DOSBox",
    "VMware", "Intel", "ATI", "S3", "STB", "SiS", "IBM",
    NULL
};

typedef struct {
    int  have;
    const char *adapter, *display, *second;
    char maker[24], model[40];
    int  mode, cols, rows;
    char versions[3][64];
    int  nversions;
    char date[12];
    int  vesa, vesa_version, memory_64k, modes;
    char oem[60];
} VIDEO;

static VIDEO video;

static void display_code( int code, const char **adapter, const char **display )
{
    static const struct {
        u8          code;
        const char *adapter, *display;
    } codes[] = {
        { 0x01, "MDA", "Monochrome" },      { 0x02, "CGA", "CGA Color" },
        { 0x04, "EGA", "EGA Color" },       { 0x05, "EGA", "EGA Monochrome" },
        { 0x06, "PGA", "PGA Color" },       { 0x07, "VGA", "VGA Monochrome" },
        { 0x08, "VGA", "VGA Color" },       { 0x0A, "MCGA", "MCGA Digital Color" },
        { 0x0B, "MCGA", "MCGA Monochrome" },{ 0x0C, "MCGA", "MCGA Color" }
    };
    int index;

    *adapter = NULL;
    *display = "";
    for ( index = 0; index < (int)(sizeof( codes ) / sizeof( codes[0] )); index++ ) {
        if ( codes[index].code == code ) {
            *adapter = codes[index].adapter;
            *display = codes[index].display;
            return;
        }
    }
}

/* A Hercules card is a monochrome adapter whose status port's top bit
   follows the vertical retrace; an MDA's never moves. */
static int is_hercules( void )
{
    unsigned first = port_in( 0x3BA ) & 0x80;
    int tries;

    for ( tries = 0; tries < 3000; tries++ ) {
        if ( (port_in( 0x3BA ) & 0x80) != first ) {
            return 1;
        }
    }
    return 0;
}

static VIDEO *get_video( void )
{
    VIDEO *vid = &video;
    BREGS regs;
    const char *adapter, *display;
    u8 *rom;
    u32 len;
    int index, ega;

    if ( vid->have ) {
        return vid;
    }
    mem_set( vid, 0, sizeof( *vid ) );
    vid->have = 1;
    vid->second = "None";
    str_cpy( vid->maker, "Unknown" );

    /* INT 10h/1A00h: the display combination, where there is a VGA */
    mem_set( &regs, 0, sizeof( regs ) );
    regs.eax = 0x1A00;
    ega = 0;
    if ( sys_bios( 0x10, &regs ) && (regs.eax & 0xFF) == 0x1A ) {
        display_code( (int)(regs.ebx & 0xFF), &vid->adapter, &vid->display );
        display_code( (int)((regs.ebx >> 8) & 0xFF), &adapter, &display );
        if ( adapter ) {
            vid->second = adapter;
        }
        ega = 1;
    }
    if ( vid->adapter == NULL ) {
        /* 12h, BL=10h: an EGA changes BL, and says colour or mono in BH */
        mem_set( &regs, 0, sizeof( regs ) );
        regs.eax = 0x1200;
        regs.ebx = 0x0010;
        if ( sys_bios( 0x10, &regs ) && (regs.ebx & 0xFF) != 0x10 ) {
            vid->adapter = "EGA";
            vid->display = (regs.ebx & 0xFF00) ? "EGA Monochrome" : "EGA Color";
            ega = 1;
        } else if ( (peek_w( 0x410 ) & 0x30) == 0x30 ) {
            vid->adapter = is_hercules() ? "Hercules" : "MDA";
            vid->display = "Monochrome";
        } else {
            vid->adapter = "CGA";
            vid->display = "CGA Color";
        }
    }
    vid->mode = peek_b( 0x449 );
    vid->cols = peek_w( 0x44A );
    vid->rows = ega ? peek_b( 0x484 ) + 1 : 25;

    /* the video BIOS: 55 AA and its length in 512s at C000:0000 */
    if ( peek_w( 0xC0000UL ) == 0xAA55 ) {
        len = (u32)peek_b( 0xC0002UL ) * 512;
        if ( len < 0x2000 ) {
            len = 0x2000;
        }
        if ( len > 0x10000UL ) {
            len = 0x10000UL;
        }
        rom = rom_load( 0xC0000UL, len );
        if ( rom ) {
            index = rom_find_name( rom, len, video_names );
            if ( index >= 0 ) {
                str_cpy( vid->maker, video_names[index] );
                for ( index = (int)str_len( vid->maker ); index > 0 && vid->maker[index - 1] == ' ';
                      index-- ) {
                    vid->maker[index - 1] = 0;
                }
            }
            vid->nversions = rom_versions( rom, len, vid->versions );
            rom_date( rom, len, vid->date );
            xfree( rom );
        }
    }
    vid->vesa = sys_vbe_info( &vid->vesa_version, vid->oem, sizeof( vid->oem ), &vid->memory_64k,
                              &vid->modes );
    return vid;
}

void pg_video( TEXT *text )
{
    VIDEO *vid = get_video();
    int index, width = 24;

    tx_field( text, width, "Video Adapter Type: ", vid->adapter );
    tx_field( text, width, "Manufacturer: ", vid->maker );
    tx_field( text, width, "Model: ", vid->model );
    tx_field( text, width, "Display Type: ", vid->display );
    tx_fieldf( text, width, "Video Mode: ", "%u", vid->mode );
    tx_fieldf( text, width, "Number of Columns: ", "%u", vid->cols );
    tx_fieldf( text, width, "Number of Rows: ", "%u", vid->rows );
    tx_field( text, width, "Video BIOS Version: ", vid->versions[0] );
    for ( index = 1; index < vid->nversions; index++ ) {
        tx_field( text, width, "", vid->versions[index] );
    }
    tx_field( text, width, "Video BIOS Date: ", vid->date );
    tx_field( text, width, "VESA Support Installed: ", vid->vesa ? "Yes" : "No" );
    if ( vid->vesa ) {
        tx_fieldf( text, width, "VESA Version: ", "%u.%02u", vid->vesa_version >> 8,
                   vid->vesa_version & 0xFF );
        tx_field( text, width, "VESA OEM Name: ", vid->oem );
        tx_fieldf( text, width, "VESA Video Memory: ", "%,uK", (u32)vid->memory_64k * 64 );
        tx_fieldf( text, width, "VESA Video Modes: ", "%u", vid->modes );
    }
    tx_field( text, width, "Secondary Adapter: ", vid->second );
}

void sum_video( char *line1, char *line2 )
{
    VIDEO *vid = get_video();
    char both[80];

    sfmt( both, sizeof( both ), "%s, %s", vid->adapter, vid->maker );
    sum_set( line1, both );
    sum_set( line2, vid->model );
}

/* ------------------------------------------------------------------ */
/* OS Version                                                          */
/* ------------------------------------------------------------------ */

void pg_os( TEXT *text )
{
    int major, minor, dos_major, dos_minor, oem, revision, flags, width = 22;
    u32 serial;
    const char *env;

    sys_version( &major, &minor );
    sys_dos_version( &dos_major, &dos_minor, &oem, &serial );
    tx_fieldf( text, width, "Operating System: ", "PM-DOS version %u.%02u", major, minor );
    tx_fieldf( text, width, "DOS Version Reported: ", "%u.%02u", dos_major, dos_minor );
    sys_true_version( &dos_major, &dos_minor, &revision, &flags );
    tx_fieldf( text, width, "Internal Revision: ", "%02u", revision );
    tx_fieldf( text, width, "OEM Serial Number: ", "%02XH", oem );
    tx_fieldf( text, width, "User Serial Number: ", "%06XH", serial );
    /* 3306h has a flag for ROM and one for the HMA; the kernel is in
       neither, and above both */
    tx_field( text, width, "DOS Located in: ",
              (flags & 0x08) ? "ROM" : (flags & 0x10) ? "HMA" : "Extended Memory" );
    tx_fieldf( text, width, "Boot Drive: ", "%c:", 'A' + sys_boot_drive() );
    tx_field( text, width, "Path to Program: ", sys_program_path() );
    tx_add( text, "" );
    tx_add( text, "    Environment Strings" );
    tx_add( text, "---------------------------" );
    env = sys_env_block();
    while ( env && *env ) {
        tx_add( text, env );
        env += str_len( env ) + 1;
    }
}

void sum_os( char *line1, char *line2 )
{
    int major, minor, oem;
    u32 serial;
    char line[40];

    sys_version( &major, &minor );
    sfmt( line, sizeof( line ), "PM-DOS %u.%02u", major, minor );
    sum_set( line1, line );
    sys_dos_version( &major, &minor, &oem, &serial );
    sfmt( line, sizeof( line ), "as MS-DOS %u.%02u", major, minor );
    sum_set( line2, line );
}

/* ------------------------------------------------------------------ */
/* Network                                                             */
/*                                                                     */
/* PM-DOS has no network of its own; what there can be is the software */
/* a DOS box loads - a packet driver on one of the vectors 60h to 80h, */
/* the protocol manager, the helper Windows for Workgroups wants - and */
/* the cards themselves, where the PCI bus lists them.                 */
/* ------------------------------------------------------------------ */

static int packet_vector( int from )
{
    u8 head[12];
    u32 vec;
    int number;

    for ( number = from; number <= 0x80; number++ ) {
        vec = peek_d( (u32)number * 4 );
        if ( vec == 0 ) {
            continue;
        }
        sys_peek( FAR_FLAT( vec >> 16, vec & 0xFFFF ), head, sizeof( head ) );
        if ( mem_cmp( head + 3, "PKT DRVR", 8 ) == 0 ) {
            return number;
        }
    }
    return 0;
}

static int network_lines( TEXT *text )
{
    int found = 0, vec, index, line, pin, width = 22;
    u32 addr, ident, class_rev;

    for ( vec = packet_vector( 0x60 ); vec; vec = packet_vector( vec + 1 ) ) {
        found++;
        if ( text ) {
            tx_fieldf( text, width, "Packet Driver: ", "INT %02XH", vec );
        }
    }
    if ( dev_present( "PROTMAN$" ) ) {
        found++;
        if ( text ) {
            tx_field( text, width, "Protocol Manager: ", "Loaded" );
        }
    }
    if ( dev_present( "IFS$HLP$" ) ) {
        found++;
        if ( text ) {
            tx_field( text, width, "IFSHLP.SYS: ", "Loaded" );
        }
    }
    for ( index = 0; index < 256 && sys_pci_enum( index, &addr, &ident, &class_rev ); index++ ) {
        if ( (class_rev >> 24) != 0x02 ) {          /* a network controller */
            continue;
        }
        if ( text ) {
            line = pin = 0;
            sys_pci_irq( addr, &line, &pin );
            if ( pin ) {
                tx_fieldf( text, width, "Network Adapter: ", "PCI %04X:%04X, IRQ %u",
                           ident & 0xFFFF, ident >> 16, line );
            } else {
                tx_fieldf( text, width, "Network Adapter: ", "PCI %04X:%04X", ident & 0xFFFF,
                           ident >> 16 );
            }
        }
    }
    return found;
}

void pg_network( TEXT *text )
{
    TEXT found;
    int index;

    tx_init( &found );
    tx_field( text, 22, "Network Detected: ", network_lines( &found ) ? "Yes" : "No" );
    for ( index = 0; index < found.count; index++ ) {
        tx_add( text, found.line[index] );
    }
    tx_free( &found );
}

void sum_network( char *line1, char *line2 )
{
    sum_set( line1, network_lines( NULL ) ? "Network Software" : "No Network" );
    line2[0] = 0;
}

/* ------------------------------------------------------------------ */
/* Mouse                                                               */
/* ------------------------------------------------------------------ */

static const char *mouse_type( int type )
{
    switch ( type ) {
    case 1: return "Bus Mouse";
    case 2: return "Serial Mouse";
    case 3: return "InPort Mouse";
    case 4: return "PS/2 Style Mouse";
    case 5: return "Hewlett-Packard Mouse";
    }
    return "Unknown Mouse Type";
}

/* how far the pointer goes for the mouse's movement, by sensitivity */
static const char *mouse_ratio( int sensitivity )
{
    static const char *const ratios[] = {
        "0.03", "0.06", "0.125", "0.25", "0.375", "0.5", "0.625", "0.75", "0.875", "1",
        "1.25", "1.5", "1.75", "2", "2.25", "2.5", "2.75", "3", "3.25", "3.5"
    };
    int index = (sensitivity - 1) / 5;

    if ( index < 0 ) {
        index = 0;
    }
    if ( index > 19 ) {
        index = 19;
    }
    return ratios[index];
}

/* the kernel looks for a serial mouse on COM1 and COM2: line 4 or 3 */
int mouse_com_port( void )
{
    MOUSEINFO info;

    sys_mouse_info( &info );
    if ( !info.present || info.type != 2 ) {
        return 0;
    }
    return info.irq == 3 ? 2 : 1;
}

void pg_mouse( TEXT *text )
{
    MOUSEINFO info;
    int width = 25, port;

    sys_mouse_info( &info );
    if ( !info.present ) {
        tx_field( text, width, "Mouse Hardware: ", "Not Detected" );
        return;
    }
    tx_field( text, width, "Mouse Hardware: ", mouse_type( info.type ) );
    tx_field( text, width, "Driver: ", "The PM-DOS kernel (INT 33h)" );
    tx_fieldf( text, width, "Driver Version: ", "%u.%02X", info.version >> 8, info.version & 0xFF );
    /* 24h gives a PS/2 mouse's line as 0, as Microsoft's driver does:
       it is always 12 */
    if ( info.irq || info.type == 4 ) {
        tx_fieldf( text, width, "Mouse IRQ: ", "%u", info.irq ? info.irq : 12 );
    }
    port = mouse_com_port();
    if ( port ) {
        tx_fieldf( text, width, "Mouse COM Port: ", "COM%u:", port );
        if ( com_port( port - 1 ) ) {
            tx_fieldf( text, width, "Mouse COM Port Address: ", "%04XH", com_port( port - 1 ) );
        }
    }
    tx_fieldf( text, width, "Number of Mouse Buttons: ", "%u", info.buttons );
    tx_fieldf( text, width, "Horizontal Sensitivity: ", "%u", info.horiz );
    tx_fieldf( text, width, "Mouse to Cursor Ratio: ", "%s : 1", mouse_ratio( info.horiz ) );
    tx_fieldf( text, width, "Vertical Sensitivity: ", "%u", info.vert );
    tx_fieldf( text, width, "Mouse to Cursor Ratio: ", "%s : 1", mouse_ratio( info.vert ) );
    tx_fieldf( text, width, "Threshold Speed: ", "%u", info.threshold );
}

void sum_mouse( char *line1, char *line2 )
{
    MOUSEINFO info;
    char version[16];

    sys_mouse_info( &info );
    line2[0] = 0;
    if ( !info.present ) {
        sum_set( line1, "No Mouse" );
        return;
    }
    sum_set( line1, mouse_type( info.type ) );
    sfmt( version, sizeof( version ), "%u.%02X", info.version >> 8, info.version & 0xFF );
    sum_set( line2, version );
}

/* ------------------------------------------------------------------ */
/* Other Adapters                                                      */
/* ------------------------------------------------------------------ */

/* INT 15h/84h: DX=1 is where the sticks are, DX=0 the switches in AL's
   top four bits, a 0 for one that is pressed.  A BIOS without the call
   sets carry; one with it and nothing plugged in answers zeroes, and
   then the port itself says whether anything decodes it. */
int game_port( int stick[4], int *switches )
{
    BREGS regs;

    mem_set( &regs, 0, sizeof( regs ) );
    regs.eax = 0x8400;
    regs.edx = 1;
    if ( !sys_bios( 0x15, &regs ) || (regs.flags & 1) ) {
        return 0;
    }
    stick[0] = (int)(regs.eax & 0xFFFF);
    stick[1] = (int)(regs.ebx & 0xFFFF);
    stick[2] = (int)(regs.ecx & 0xFFFF);
    stick[3] = (int)(regs.edx & 0xFFFF);
    mem_set( &regs, 0, sizeof( regs ) );
    regs.eax = 0x8400;
    regs.edx = 0;
    sys_bios( 0x15, &regs );
    *switches = (int)(regs.eax & 0xF0);
    if ( stick[0] == 0 && stick[1] == 0 && stick[2] == 0 && stick[3] == 0 ) {
        return port_in( 0x201 ) != 0xFF;
    }
    return 1;
}

static const char *pci_class( u32 class_rev )
{
    static const char *const base[] = {
        "Older device", "Disk controller", "Network controller", "Display controller",
        "Multimedia device", "Memory controller", "Bridge", "Communications port",
        "System device", "Input device", "Docking station", "Processor", "Serial bus"
    };
    u32 code = class_rev >> 16;             /* class and subclass */

    switch ( code ) {
    case 0x0100: return "SCSI controller";
    case 0x0101: return "IDE controller";
    case 0x0102: return "Diskette controller";
    case 0x0200: return "Ethernet controller";
    case 0x0300: return "VGA display";
    case 0x0401: return "Sound card";
    case 0x0600: return "Host bridge";
    case 0x0601: return "ISA bridge";
    case 0x0602: return "EISA bridge";
    case 0x0604: return "PCI bridge";
    case 0x0607: return "CardBus bridge";
    case 0x0700: return "Serial port";
    case 0x0701: return "Parallel port";
    case 0x0C00: return "FireWire controller";
    case 0x0C03: return "USB controller";
    case 0x0C05: return "SMBus controller";
    }
    return (code >> 8) < sizeof( base ) / sizeof( base[0] ) ? base[code >> 8] : "Other device";
}

void pg_other( TEXT *text )
{
    int stick[4], switches, last, count, index, line, pin, port, cards, width = 16;
    u32 addr, ident, class_rev;
    char irq[8];

    if ( game_port( stick, &switches ) ) {
        tx_field( text, width, "Game Adapter: ", "Detected" );
        tx_fieldf( text, width, "Joystick A - X: ", "%u", stick[0] );
        tx_fieldf( text, width, "Y: ", "%u", stick[1] );
        tx_field( text, width, "Button 1: ", (switches & 0x10) ? "Off" : "On" );
        tx_field( text, width, "Button 2: ", (switches & 0x20) ? "Off" : "On" );
        tx_fieldf( text, width, "Joystick B - X: ", "%u", stick[2] );
        tx_fieldf( text, width, "Y: ", "%u", stick[3] );
        tx_field( text, width, "Button 1: ", (switches & 0x40) ? "Off" : "On" );
        tx_field( text, width, "Button 2: ", (switches & 0x80) ? "Off" : "On" );
    } else {
        tx_field( text, width, "Game Adapter: ", "Not Detected" );
    }

    tx_add( text, "" );
    if ( !sys_pci_here( &last, &count ) ) {
        tx_field( text, width, "PCI Bus: ", "Not Detected" );
    } else {
        tx_fieldf( text, width, "PCI Bus: ", "Detected, %u device%s", count, count == 1 ? "" : "s" );
        tx_add( text, "" );
        tx_add( text, "Bus Dev Fn  Vendor Device  Type                    IRQ" );
        tx_add( text, "--- --- --  ------ ------  ----------------------  ---" );
        for ( index = 0; index < 256 && sys_pci_enum( index, &addr, &ident, &class_rev ); index++ ) {
            line = pin = 0;
            sys_pci_irq( addr, &line, &pin );
            if ( pin && line && line < 16 ) {
                sfmt( irq, sizeof( irq ), "%3u", line );
            } else {
                str_cpy( irq, "  -" );
            }
            tx_addf( text, "%3u %3u %2u   %04X   %04X   %-22s  %s", (addr >> 16) & 0xFF,
                     (addr >> 11) & 0x1F, (addr >> 8) & 0x07, ident & 0xFFFF, ident >> 16,
                     pci_class( class_rev ), irq );
        }
    }
    cards = sys_pnp_cards( &port );
    if ( cards ) {
        tx_add( text, "" );
        tx_fieldf( text, 0, "ISA Plug and Play Cards: ", "%u, read at port %04XH", cards, port );
    }
}

void sum_other( char *line1, char *line2 )
{
    int stick[4], switches, last, count;

    line1[0] = line2[0] = 0;
    if ( game_port( stick, &switches ) ) {
        sum_set( line1, "Game Adapter" );
    }
    if ( sys_pci_here( &last, &count ) ) {
        sum_set( line1[0] ? line2 : line1, "PCI Bus" );
    }
}

/* ------------------------------------------------------------------ */
/* which routine builds which page                                     */
/* ------------------------------------------------------------------ */

void page_build( int page, TEXT *text, int report )
{
    switch ( page ) {
    case PG_COMPUTER: pg_computer( text );          break;
    case PG_MEMORY:   pg_memory( text, report );    break;
    case PG_VIDEO:    pg_video( text );             break;
    case PG_NETWORK:  pg_network( text );           break;
    case PG_OS:       pg_os( text );                break;
    case PG_MOUSE:    pg_mouse( text );             break;
    case PG_OTHER:    pg_other( text );             break;
    case PG_DISK:     pg_disk( text );              break;
    case PG_LPT:      pg_lpt( text );               break;
    case PG_COM:      pg_com( text );               break;
    case PG_IRQ:      pg_irq( text );               break;
    case PG_TSR:      pg_tsr( text );               break;
    case PG_DEVICE:   pg_device( text );            break;
    }
}

void page_summary( int page, char *line1, char *line2 )
{
    line1[0] = line2[0] = 0;
    switch ( page ) {
    case PG_COMPUTER: sum_computer( line1, line2 ); break;
    case PG_MEMORY:   sum_memory( line1, line2 );   break;
    case PG_VIDEO:    sum_video( line1, line2 );    break;
    case PG_NETWORK:  sum_network( line1, line2 );  break;
    case PG_OS:       sum_os( line1, line2 );       break;
    case PG_MOUSE:    sum_mouse( line1, line2 );    break;
    case PG_OTHER:    sum_other( line1, line2 );    break;
    case PG_DISK:     sum_disk( line1, line2 );     break;
    case PG_LPT:      sum_lpt( line1, line2 );      break;
    case PG_COM:      sum_com( line1, line2 );      break;
    }
}
