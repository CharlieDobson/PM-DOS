/*
 * INF.C - SETUP.INF, the list of everything Setup installs.
 *
 * TOOLS\MKSETUP.EXE writes the file when the Setup disks are made, from
 * DISK\SETUP.LST, NLS\COUNTRY.DEF and NLS\KEYBOARD.DEF, so that what it
 * says always matches what is on the disks.  It is plain text:
 *
 *   [setup]       product=, version=, dosdir=, label=
 *   [disks]       n=TAGFILE,what the disk is called
 *                 A Setup disk is known by its tag file: DISK1 on disk 1
 *                 and so on.  A CD, a USB stick or a network directory
 *                 holds every disk's files, tags and all, in one place.
 *   [files]       disk,name on the disk,installed name,ROOT|DOS|SETUP,
 *                 flags,bytes
 *                 flags: S read-only + hidden + system, P SZDD-packed
 *                 (the name on the disk ends in '_'), F on a startup
 *                 floppy (F7), U on the Uninstall disk.  SETUP is Setup's
 *                 own: the boot code it writes, never installed.
 *   [countries]   code,name,date order,date separator,time separator,
 *                 12|24,code pages
 *   [keyboards]   code,id,name,code pages
 *   [config] [autoexec]           a new system's CONFIG.SYS and
 *                                 AUTOEXEC.BAT; %DOSDIR% is replaced
 *   [config.add]                  lines an upgrade adds to CONFIG.SYS
 *                                 when they are not there
 *   [config.remove] [autoexec.remove]
 *                                 programs whose lines an upgrade turns
 *                                 into REMs, one name a line
 *   [floppy.config]               a startup floppy's CONFIG.SYS
 *
 * ';' starts a comment line; blank lines are skipped.
 */
#include "setup.h"
#include "text.h"

INF inf;

static char *text;                      /* the whole file, cut into lines */

enum { S_NONE, S_SETUP, S_DISKS, S_FILES, S_COUNTRIES, S_KEYBOARDS,
       S_CONFIG, S_AUTOEXEC, S_CFGADD, S_CFGDROP, S_AUTODROP, S_BOOTCFG,
       S_OTHER };

static const char *section_name[] = {
    "", "setup", "disks", "files", "countries", "keyboards", "config",
    "autoexec", "config.add", "config.remove", "autoexec.remove",
    "floppy.config"
};

/* the fields of a comma-separated line, trimmed, cut in place */
static int split( char *line, char **field, int max )
{
    int n = 0;
    char *end;

    while ( n < max ) {
        line = skip_blanks( line );
        field[n++] = line;
        end = str_chr( line, ',' );
        if ( end ) {
            *end = 0;
        }
        /* trailing blanks off this field */
        {
            int len = str_len( line );
            while ( len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t') ) {
                line[--len] = 0;
            }
        }
        if ( !end ) {
            break;
        }
        line = end + 1;
    }
    return n;
}

static int pages( const char *list, u16 *page )
{
    int n = 0;
    const char *end;

    for ( ;; ) {
        list = skip_blanks( list );
        if ( *list < '0' || *list > '9' || n >= MAX_PAGES ) {
            return n;
        }
        page[n++] = (u16)parse_u32( list, &end );
        list = end;
    }
}

static void keyval( char *line )
{
    char *eq = str_chr( line, '=' ), *val;

    if ( !eq ) {
        return;
    }
    *eq = 0;
    val = skip_blanks( eq + 1 );
    if ( !str_icmp( line, "product" ) ) {
        str_cpyn( inf.product, val, sizeof( inf.product ) );
    } else if ( !str_icmp( line, "version" ) ) {
        str_cpyn( inf.version, val, sizeof( inf.version ) );
    } else if ( !str_icmp( line, "dosdir" ) ) {
        str_cpyn( inf.dosdir, val, sizeof( inf.dosdir ) );
    } else if ( !str_icmp( line, "label" ) ) {
        str_cpyn( inf.label, val, sizeof( inf.label ) );
    }
}

static void add_line( char ***list, int *count, char *line )
{
    if ( *list == NULL ) {
        *list = (char **)xalloc( sizeof( char * ) * MAX_LINES );
    }
    if ( *count < MAX_LINES ) {
        (*list)[(*count)++] = line;
    }
}

static int parse_file( char *line )
{
    char *field[6];
    INFFILE *file;
    const char *flag;

    if ( split( line, field, 6 ) < 6 || inf.nfiles >= MAX_FILES ) {
        return 1;
    }
    file = &inf.file[inf.nfiles];
    mem_set( file, 0, sizeof( *file ) );
    file->disk = (u8)parse_u32( field[0], NULL );
    if ( file->disk < 1 || file->disk > MAX_DISKS ) {
        return 1;
    }
    str_cpyn( file->src, field[1], sizeof( file->src ) );
    str_cpyn( file->dst, field[2], sizeof( file->dst ) );
    str_upper( file->src );
    str_upper( file->dst );
    if ( !str_icmp( field[3], "ROOT" ) ) {
        file->where = WH_ROOT;
    } else if ( !str_icmp( field[3], "DOS" ) ) {
        file->where = WH_DOS;
    } else if ( !str_icmp( field[3], "SETUP" ) ) {
        file->where = WH_SETUP;
    } else {
        return 1;
    }
    for ( flag = field[4]; *flag; flag++ ) {
        switch ( ch_upper( (u8)*flag ) ) {
        case 'S': file->flags |= FF_SYSTEM; break;
        case 'P': file->flags |= FF_PACKED; break;
        case 'F': file->flags |= FF_FLOPPY; break;
        case 'U': file->flags |= FF_UNINST; break;
        }
    }
    file->size = parse_u32( field[5], NULL );
    if ( !file->src[0] || !file->dst[0] ) {
        return 1;
    }
    inf.nfiles++;
    return 0;
}

static int parse_country( char *line )
{
    char *field[7];
    COUNTRYDEF *ctry;

    if ( split( line, field, 7 ) < 7 || inf.ncountries >= MAX_COUNTRIES ) {
        return 1;
    }
    ctry = &inf.country[inf.ncountries];
    mem_set( ctry, 0, sizeof( *ctry ) );
    str_cpyn( ctry->code, field[0], sizeof( ctry->code ) );
    str_cpyn( ctry->name, field[1], sizeof( ctry->name ) );
    ctry->date_order = (u8)parse_u32( field[2], NULL );
    ctry->date_sep = field[3][0] ? field[3][0] : '/';
    ctry->time_sep = field[4][0] ? field[4][0] : ':';
    ctry->clock24 = parse_u32( field[5], NULL ) == 24;
    ctry->npages = (u8)pages( field[6], ctry->page );
    if ( ctry->date_order > 2 || ctry->npages == 0 ) {
        return 1;
    }
    inf.ncountries++;
    return 0;
}

static int parse_keyboard( char *line )
{
    char *field[4];
    KEYBDEF *kb;

    if ( split( line, field, 4 ) < 4 || inf.nkeyboards >= MAX_KEYBOARDS ) {
        return 1;
    }
    kb = &inf.keyboard[inf.nkeyboards];
    mem_set( kb, 0, sizeof( *kb ) );
    str_cpyn( kb->code, field[0], sizeof( kb->code ) );
    str_upper( kb->code );
    kb->id = (u16)parse_u32( field[1], NULL );
    str_cpyn( kb->name, field[2], sizeof( kb->name ) );
    kb->npages = (u8)pages( field[3], kb->page );
    if ( !kb->code[0] || kb->npages == 0 ) {
        return 1;
    }
    inf.nkeyboards++;
    return 0;
}

/* 0 = read; else nonzero and the file was not usable */
int inf_load( const char *path )
{
    u16 handle, done;
    u32 size;
    char *line, *next, *end;
    int section = S_NONE, bad = 0, i, len;

    if ( plat_open( path, 0, &handle ) ) {
        return 1;
    }
    if ( plat_size( handle, &size ) || size == 0 || size > 32000u ) {
        plat_close( handle );
        return 2;
    }
    text = (char *)xalloc( size + 1 );
    if ( plat_read( handle, text, (u16)size, &done ) || done != (u16)size ) {
        plat_close( handle );
        return 2;
    }
    plat_close( handle );
    text[size] = 0;

    mem_set( &inf, 0, sizeof( inf ) );
    str_cpyn( inf.product, "PM-DOS", sizeof( inf.product ) );
    str_cpyn( inf.dosdir, "C:\\DOS", sizeof( inf.dosdir ) );
    str_cpyn( inf.label, "PM-DOS", sizeof( inf.label ) );
    inf.file = (INFFILE *)xalloc( sizeof( INFFILE ) * MAX_FILES );
    inf.country = (COUNTRYDEF *)xalloc( sizeof( COUNTRYDEF ) * MAX_COUNTRIES );
    inf.keyboard = (KEYBDEF *)xalloc( sizeof( KEYBDEF ) * MAX_KEYBOARDS );

    for ( line = text; *line; line = next ) {
        end = line;
        while ( *end && *end != '\n' ) {
            end++;
        }
        next = *end ? end + 1 : end;
        *end = 0;
        len = str_len( line );
        while ( len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' ||
                            line[len - 1] == '\t') ) {
            line[--len] = 0;
        }
        line = skip_blanks( line );
        if ( !*line || *line == ';' ) {
            continue;
        }
        if ( *line == '[' ) {
            end = str_chr( line, ']' );
            if ( end ) {
                *end = 0;
            }
            section = S_OTHER;
            for ( i = 1; i < (int)(sizeof( section_name ) / sizeof( section_name[0] )); i++ ) {
                if ( !str_icmp( line + 1, section_name[i] ) ) {
                    section = i;
                }
            }
            continue;
        }
        switch ( section ) {
        case S_SETUP:
            keyval( line );
            break;
        case S_DISKS: {
            char *field[2];
            int disk = (int)parse_u32( line, (const char **)&end );
            if ( disk < 1 || disk > MAX_DISKS || *end != '=' ) {
                bad = 1;
                break;
            }
            if ( split( end + 1, field, 2 ) < 2 ) {
                bad = 1;
                break;
            }
            str_cpyn( inf.disktag[disk], field[0], sizeof( inf.disktag[0] ) );
            str_cpyn( inf.diskname[disk], field[1], sizeof( inf.diskname[0] ) );
            if ( disk > inf.ndisks ) {
                inf.ndisks = disk;
            }
            break;
        }
        case S_FILES:       bad |= parse_file( line ); break;
        case S_COUNTRIES:   bad |= parse_country( line ); break;
        case S_KEYBOARDS:   bad |= parse_keyboard( line ); break;
        case S_CONFIG:      add_line( &inf.config, &inf.nconfig, line ); break;
        case S_AUTOEXEC:    add_line( &inf.autoexec, &inf.nautoexec, line ); break;
        case S_CFGADD:      add_line( &inf.cfgadd, &inf.ncfgadd, line ); break;
        case S_CFGDROP:     add_line( &inf.cfgdrop, &inf.ncfgdrop, line ); break;
        case S_AUTODROP:    add_line( &inf.autodrop, &inf.nautodrop, line ); break;
        case S_BOOTCFG:     add_line( &inf.bootcfg, &inf.nbootcfg, line ); break;
        default:            break;
        }
    }
    if ( bad || inf.ndisks == 0 || inf.nfiles == 0 || inf.ncountries == 0 ||
         inf.nkeyboards == 0 || !path_valid( inf.dosdir ) ) {
        return 3;
    }
    for ( i = 1; i <= inf.ndisks; i++ ) {
        if ( !inf.disktag[i][0] ) {
            return 3;                   /* a disk with no way to know it */
        }
    }
    for ( i = 0; i < inf.nfiles; i++ ) {
        if ( inf.file[i].disk > inf.ndisks ) {
            return 3;
        }
    }
    return 0;
}

const COUNTRYDEF *inf_country( const char *code )
{
    int i;

    for ( i = 0; i < inf.ncountries; i++ ) {
        if ( parse_u32( inf.country[i].code, NULL ) == parse_u32( code, NULL ) ) {
            return &inf.country[i];
        }
    }
    return NULL;
}

const KEYBDEF *inf_keyboard( const char *code, u16 id )
{
    int i;
    const KEYBDEF *first = NULL;

    for ( i = 0; i < inf.nkeyboards; i++ ) {
        if ( !str_icmp( inf.keyboard[i].code, code ) ) {
            if ( id == 0 || inf.keyboard[i].id == id ) {
                return &inf.keyboard[i];
            }
            if ( first == NULL ) {
                first = &inf.keyboard[i];
            }
        }
    }
    return first;
}

INFFILE *inf_file( const char *dst )
{
    int i;

    for ( i = 0; i < inf.nfiles; i++ ) {
        if ( !str_icmp( inf.file[i].dst, dst ) ) {
            return &inf.file[i];
        }
    }
    return NULL;
}
