/*
 * TOOLS.C - what the File and Utilities menus do, the report apart:
 * the files to view, Find File, Memory Block Display, Memory Browser,
 * Insert Command, Test Printer and About.
 *
 * Each is one of MSD's, to do the same job here:
 *
 *   THE FILES TO VIEW are the ones that say how the machine starts -
 *   AUTOEXEC.BAT and CONFIG.SYS from the boot drive, SYSTEM.INI and
 *   WIN.INI from wherever along PATH Windows keeps them.
 *
 *   FIND FILE looks for a name with wildcards from a directory down,
 *   or over the boot drive, or over every fixed disk, and shows any of
 *   what it finds.  A directory is finished with before the ones in it
 *   are entered: one search open at a time, which is all the classic
 *   find calls allow when the kernel has no long names.
 *
 *   MEMORY BLOCK DISPLAY is the TSR Programs page as a list, with the
 *   first megabyte drawn beside it and the chosen block picked out.
 *
 *   MEMORY BROWSER looks through a ROM for a word and shows the text
 *   round every place it finds it.
 *
 *   INSERT COMMAND puts a line into CONFIG.SYS or AUTOEXEC.BAT, in
 *   place of the line that sets the same thing if there is one and
 *   that is wanted.  The rest of the file is written back as it was
 *   read, byte for byte.
 *
 *   TEST PRINTER sends the printable characters to a port as text, or
 *   as a PostScript page that draws them.
 */
#include "pmd.h"

/* ------------------------------------------------------------------ */
/* the files to view                                                   */
/* ------------------------------------------------------------------ */

static void view_add( const char *name, const char *path )
{
    u16 attr;

    if ( view_count < MAX_VIEWS && sys_get_attr( path, &attr ) == 0 && !(attr & ATTR_DIR)
         && !crit_take() ) {
        str_cpy( view_names[view_count], name );
        str_cpyn( view_paths[view_count], path, PATH_MAX );
        view_count++;
    }
}

/* the first directory of PATH that has "name" in it */
static void view_on_path( const char *name )
{
    const char *path = sys_getenv( "PATH" );
    char trial[PATH_MAX];
    u32 len, end;
    int before = view_count;

    while ( path && *path && view_count == before ) {
        for ( len = 0; path[len] && path[len] != ';'; len++ ) {
        }
        if ( len && len < PATH_MAX - 16 ) {
            mem_cpy( trial, path, len );
            end = len;
            if ( trial[end - 1] != '\\' && trial[end - 1] != ':' ) {
                trial[end++] = '\\';
            }
            str_cpy( trial + end, name );
            view_add( name, trial );
        }
        path += len;
        if ( *path == ';' ) {
            path++;
        }
    }
}

void views_find( void )
{
    char path[20];

    view_count = 0;
    crit_take();
    sfmt( path, sizeof( path ), "%c:\\AUTOEXEC.BAT", 'A' + sys_boot_drive() );
    view_add( "AUTOEXEC.BAT", path );
    sfmt( path, sizeof( path ), "%c:\\CONFIG.SYS", 'A' + sys_boot_drive() );
    view_add( "CONFIG.SYS", path );
    view_on_path( "SYSTEM.INI" );
    view_on_path( "WIN.INI" );
}

/* a file as lines: tabs opened out to eight, the other control
   characters dropped, Ctrl+Z the end as it is to TYPE */
int file_to_text( const char *path, TEXT *text )
{
    static char line[200];
    u32 handle, size, done, pos;
    u8 *data;
    int rc, len = 0, ch;

    rc = sys_open_read( path, &handle );
    if ( rc ) {
        return rc;
    }
    rc = sys_file_size( handle, &size );
    if ( rc ) {
        sys_close( handle );
        return rc;
    }
    if ( size > 0x20000UL ) {
        size = 0x20000UL;               /* the first 128K is plenty to look at */
    }
    data = (u8 *)try_alloc( size + 1 );
    if ( data == NULL ) {
        sys_close( handle );
        return ERR_NOMEM;
    }
    rc = sys_read( handle, data, size, &done );
    sys_close( handle );
    if ( rc ) {
        xfree( data );
        return rc;
    }
    for ( pos = 0; pos < done; pos++ ) {
        ch = data[pos];
        if ( ch == 0x1A ) {
            break;
        }
        if ( ch == '\n' ) {
            line[len] = 0;
            tx_add( text, line );
            len = 0;
        } else if ( ch == '\t' ) {
            do {
                if ( len < (int)sizeof( line ) - 1 ) {
                    line[len++] = ' ';
                }
            } while ( len % 8 && len < (int)sizeof( line ) - 1 );
        } else if ( ch >= ' ' && len < (int)sizeof( line ) - 1 ) {
            line[len++] = (char)ch;
        }
    }
    if ( len ) {
        line[len] = 0;
        tx_add( text, line );
    }
    xfree( data );
    return 0;
}

static void view_file( const char *title, const char *path )
{
    TEXT text;
    int rc;

    tx_init( &text );
    rc = file_to_text( path, &text );
    if ( rc ) {
        msg_box2( err_text( rc ), path, MB_OK );
    } else {
        text_window( title, &text, NULL );
    }
    tx_free( &text );
}

void cmd_view( int index )
{
    if ( index < view_count ) {
        view_file( view_paths[index], view_paths[index] );
    }
}

/* ------------------------------------------------------------------ */
/* Find File                                                           */
/* ------------------------------------------------------------------ */

#define FIND_MAX        400     /* files shown: more than that is a spec to narrow */
#define FIND_DEPTH      24

static char find_spec[64];
static char find_from[PATH_MAX];
static int  find_subdirs = 1, find_boot, find_all;
static TEXT find_found;

/* "dir" ends in a backslash.  Its files that match first, then - one
   search finished before the next is begun - each directory in it. */
static void search_dir( const char *dir, int depth )
{
    static FINDREC rec;
    char path[PATH_MAX];
    TEXT subs;
    u32 handle;
    int rc, index;

    if ( find_found.count >= FIND_MAX || str_len( dir ) + 70 > PATH_MAX ) {
        return;
    }
    sfmt( path, sizeof( path ), "%s%s", dir, find_spec );
    for ( rc = sys_find_first( path, 0x27, &rec, &handle ); rc == 0;
          rc = sys_find_next( handle, &rec ) ) {
        if ( !(rec.attr & (ATTR_DIR | ATTR_VOLUME)) && find_found.count < FIND_MAX ) {
            sfmt( path, sizeof( path ), "%s%s", dir, rec.name );
            tx_add( &find_found, path );
        }
    }
    if ( rc != ERR_NOFILE || handle ) {
        sys_find_close( handle );
    }
    crit_take();
    if ( !find_subdirs || depth >= FIND_DEPTH ) {
        return;
    }
    tx_init( &subs );
    sfmt( path, sizeof( path ), "%s*.*", dir );
    handle = 0;
    for ( rc = sys_find_first( path, 0x37, &rec, &handle ); rc == 0;
          rc = sys_find_next( handle, &rec ) ) {
        if ( (rec.attr & ATTR_DIR) && rec.name[0] != '.' ) {
            tx_add( &subs, rec.name );
        }
    }
    if ( rc != ERR_NOFILE || handle ) {
        sys_find_close( handle );
    }
    crit_take();
    for ( index = 0; index < subs.count; index++ ) {
        if ( str_len( dir ) + str_len( subs.line[index] ) + 2 < PATH_MAX ) {
            sfmt( path, sizeof( path ), "%s%s\\", dir, subs.line[index] );
            search_dir( path, depth + 1 );
        }
    }
    tx_free( &subs );
}

static const char *found_item( DLG *dlg, int index )
{
    (void)dlg;
    return index < find_found.count ? find_found.line[index] : "";
}

static void file_info( const char *path )
{
    static FINDREC rec;
    char line[80];
    u32 handle = 0;

    if ( sys_find_first( path, 0x27, &rec, &handle ) ) {
        msg_box2( err_text( ERR_NOFILE ), path, MB_OK );
        return;
    }
    sys_find_close( handle );
    sfmt( line, sizeof( line ), "%,u bytes   %02u-%02u-%02u  %2u:%02u", rec.size,
          (rec.date >> 5) & 0x0F, rec.date & 0x1F, (80 + (rec.date >> 9)) % 100, rec.time >> 11,
          (rec.time >> 5) & 0x3F );
    msg_box2( path, line, MB_OK );
}

#define FF_LIST         1
#define FF_DISPLAY      2
#define FF_INFO         3

static int found_activate( DLG *dlg, int ctl )
{
    int sel = dlg->ctl[FF_LIST].sel;

    if ( find_found.count == 0 || ctl > FF_INFO ) {
        return 0;
    }
    if ( ctl == FF_INFO ) {
        file_info( find_found.line[sel] );
    } else {
        view_file( find_found.line[sel], find_found.line[sel] );
    }
    return 0;
}

static void found_dialog( void )
{
    static CTL ctl[5];
    DLG dlg;

    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl[0].type = CT_LABEL;
    ctl[0].row = 1;
    ctl[0].col = 2;
    ctl[0].text = "&Files:";
    ctl[FF_LIST].type = CT_LIST;
    ctl[FF_LIST].row = 2;
    ctl[FF_LIST].col = 2;
    ctl[FF_LIST].width = 70;
    ctl[FF_LIST].height = 13;
    ctl[FF_LIST].count = find_found.count;
    ctl[FF_LIST].item = found_item;
    ctl[FF_DISPLAY].type = CT_BUTTON;
    ctl[FF_DISPLAY].row = 16;
    ctl[FF_DISPLAY].col = 12;
    ctl[FF_DISPLAY].text = "&Display File";
    ctl[FF_DISPLAY].id = FF_DISPLAY + 10;
    ctl[FF_DISPLAY].is_default = 1;
    ctl[FF_INFO].type = CT_BUTTON;
    ctl[FF_INFO].row = 16;
    ctl[FF_INFO].col = 32;
    ctl[FF_INFO].text = "File &Info";
    ctl[FF_INFO].id = FF_INFO + 10;
    ctl[4].type = CT_BUTTON;
    ctl[4].row = 16;
    ctl[4].col = 49;
    ctl[4].text = "Cancel";
    ctl[4].id = ID_CANCEL;
    dlg.title = "Find File";
    dlg.rows = 19;
    dlg.cols = 74;
    dlg.ctl = ctl;
    dlg.nctl = 5;
    dlg.focus = FF_LIST;
    dlg.activate = found_activate;
    dlg_run( &dlg );
}

void cmd_find_file( void )
{
    static CTL ctl[9];
    DLG dlg;
    DRVREC rec;
    char root[PATH_MAX];
    int drv, letters = 0;
    u32 len;

    if ( find_from[0] == 0 ) {
        drv = sys_get_drive();
        sfmt( find_from, sizeof( find_from ), "%c:\\", 'A' + drv );
        sys_get_cwd( drv, find_from + 3 );
    }
    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl[0].type = CT_LABEL;
    ctl[0].row = 2;
    ctl[0].col = 3;
    ctl[0].text = "Search &for:";
    ctl[1].type = CT_EDIT;
    ctl[1].row = 2;
    ctl[1].col = 16;
    ctl[1].width = 36;
    ctl[1].buf = find_spec;
    ctl[1].max = sizeof( find_spec );
    ctl[2].type = CT_LABEL;
    ctl[2].row = 4;
    ctl[2].col = 3;
    ctl[2].text = "Start f&rom:";
    ctl[3].type = CT_EDIT;
    ctl[3].row = 4;
    ctl[3].col = 16;
    ctl[3].width = 36;
    ctl[3].buf = find_from;
    ctl[3].max = PATH_MAX - 80;
    ctl[4].type = CT_CHECK;
    ctl[4].row = 6;
    ctl[4].col = 3;
    ctl[4].text = "&Include sub-dirs";
    ctl[4].value = &find_subdirs;
    ctl[5].type = CT_CHECK;
    ctl[5].row = 7;
    ctl[5].col = 3;
    ctl[5].text = "Search &boot drive";
    ctl[5].value = &find_boot;
    ctl[6].type = CT_CHECK;
    ctl[6].row = 8;
    ctl[6].col = 3;
    ctl[6].text = "Search &all drives";
    ctl[6].value = &find_all;
    ctl[7].type = CT_BUTTON;
    ctl[7].row = 10;
    ctl[7].col = 15;
    ctl[7].text = "&Search";
    ctl[7].id = ID_OK;
    ctl[7].is_default = 1;
    ctl[8].type = CT_BUTTON;
    ctl[8].row = 10;
    ctl[8].col = 30;
    ctl[8].text = "Cancel";
    ctl[8].id = ID_CANCEL;
    dlg.title = "Find File";
    dlg.rows = 13;
    dlg.cols = 57;
    dlg.ctl = ctl;
    dlg.nctl = 9;
    dlg.focus = 1;
    if ( dlg_run( &dlg ) != ID_OK ) {
        return;
    }
    if ( find_spec[0] == 0 ) {
        str_cpy( find_spec, "*.*" );
    }

    tx_free( &find_found );
    app_status( "Searching ..." );
    ui_redraw();
    scr_flush();
    crit_take();
    if ( find_all ) {
        /* every letter whose disk stays in it */
        for ( drv = 0; sys_drive_rec( drv, &rec, &letters ); drv++ ) {
            if ( (rec.flags & DRVF_PRESENT) && !(rec.flags & (DRVF_FLOPPY | DRVF_CDROM)) ) {
                sfmt( root, sizeof( root ), "%c:\\", 'A' + drv );
                search_dir( root, 0 );
            }
        }
    } else if ( find_boot ) {
        sfmt( root, sizeof( root ), "%c:\\", 'A' + sys_boot_drive() );
        search_dir( root, 0 );
    } else {
        str_cpyn( root, find_from, sizeof( root ) - 2 );
        len = str_len( root );
        if ( len && root[len - 1] != '\\' && root[len - 1] != ':' ) {
            root[len++] = '\\';
            root[len] = 0;
        }
        search_dir( root, 0 );
    }
    app_status( NULL );
    if ( find_found.count == 0 ) {
        msg_box2( "No files were found", find_spec, MB_OK );
        return;
    }
    found_dialog();
}

/* ------------------------------------------------------------------ */
/* Memory Block Display                                                */
/* ------------------------------------------------------------------ */

#define BLOCK_ROWS      14      /* the map's rows on the screen, 16K each */

static MEMITEM *block_items;
static int      block_count;
static u8       block_map[MAP_ROWS][MAP_COLS];

static const char *block_item( DLG *dlg, int index )
{
    static char line[48];

    (void)dlg;
    sfmt( line, sizeof( line ), "%-18s %04X %7u", block_items[index].name, block_items[index].seg,
          block_items[index].size );
    return line;
}

/* the first megabyte, 16K a row from the top down, scrolled to keep
   the chosen block on it */
static void block_draw( DLG *dlg, CTL *ctl )
{
    int sel = dlg->ctl[1].sel, row, cell, kind, top_row, map_row;
    u32 from = 0, to = 0, addr, seg;
    char label[8];
    u8 attr;

    if ( sel < block_count ) {
        from = block_items[sel].seg << 4;
        to = from + 16 + block_items[sel].size;
    }
    /* rows count down from 63 (FC00h); show the block's top one near
       the top of the window */
    top_row = to ? (int)((to - 1) >> 14) + 2 : 63;
    if ( top_row > 63 ) {
        top_row = 63;
    }
    if ( top_row < BLOCK_ROWS - 1 ) {
        top_row = BLOCK_ROWS - 1;
    }
    for ( row = 0; row < BLOCK_ROWS; row++ ) {
        map_row = top_row - row;
        seg = (u32)map_row * 0x400;
        sfmt( label, sizeof( label ), "%04X", seg );
        scr_put( dlg->top + ctl->row + row, dlg->left + ctl->col, label, pal.dlg );
        for ( cell = 0; cell < MAP_COLS; cell++ ) {
            addr = (seg << 4) + (u32)cell * 1024;
            kind = map_row >= 40 ? block_map[map_row - 40][cell] : MC_RAM;
            attr = addr + 1024 > from && addr < to ? pal.map_sel : pal.map;
            scr_ch( dlg->top + ctl->row + row, dlg->left + ctl->col + 5 + cell,
                    mem_map_char( kind, 0 ), attr );
        }
    }
}

void cmd_blocks( void )
{
    static CTL ctl[6];
    DLG dlg;

    block_count = mem_items( &block_items );
    if ( block_items == NULL ) {
        out_of_memory();
        return;
    }
    mem_map( block_map );
    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl[0].type = CT_LABEL;
    ctl[0].row = 1;
    ctl[0].col = 2;
    ctl[0].text = "&Allocated memory:";
    ctl[1].type = CT_LIST;
    ctl[1].row = 2;
    ctl[1].col = 2;
    ctl[1].width = 36;
    ctl[1].height = BLOCK_ROWS + 2;
    ctl[1].count = block_count;
    ctl[1].item = block_item;
    ctl[2].type = CT_LABEL;
    ctl[2].row = 1;
    ctl[2].col = 41;
    ctl[2].text = "Memory map:";
    ctl[3].type = CT_CUSTOM;
    ctl[3].row = 3;
    ctl[3].col = 41;
    ctl[3].draw = block_draw;
    ctl[4].type = CT_BUTTON;
    ctl[4].row = 19;
    ctl[4].col = 28;
    ctl[4].text = "Close";
    ctl[4].id = ID_CANCEL;
    ctl[4].is_default = 1;
    dlg.title = "Memory Block Display";
    dlg.rows = 22;
    dlg.cols = 66;
    dlg.ctl = ctl;
    dlg.nctl = 5;
    dlg.focus = 1;
    dlg_run( &dlg );
    xfree( block_items );
    block_items = NULL;
}

/* ------------------------------------------------------------------ */
/* Memory Browser                                                      */
/* ------------------------------------------------------------------ */

#define AREA_MAX        8
#define STRINGS_MAX     120

typedef struct {
    char name[20];
    u32  seg, len;
} AREA;

static AREA areas[AREA_MAX];
static int  area_count;
static char browse_key[40];

/* the system's ROM, the video adapter's, and any other card's that
   begins 55 AA on a 2K line between C800h and EFFFh */
static void areas_find( void )
{
    u32 addr, len;

    area_count = 0;
    str_cpy( areas[area_count].name, "ROM BIOS" );
    areas[area_count].seg = 0xF000;
    areas[area_count].len = 0x10000UL;
    area_count++;
    for ( addr = 0xC0000UL; addr < 0xF0000UL && area_count < AREA_MAX; addr += 0x800 ) {
        if ( peek_w( addr ) != 0xAA55 ) {
            continue;
        }
        len = (u32)peek_b( addr + 2 ) * 512;
        if ( len == 0 || addr + len > 0xF0000UL ) {
            continue;
        }
        str_cpy( areas[area_count].name, addr == 0xC0000UL ? "Video ROM BIOS" : "Option ROM" );
        areas[area_count].seg = addr >> 4;
        areas[area_count].len = len;
        area_count++;
        addr += ((len + 0x7FF) & ~0x7FFUL) - 0x800;
    }
}

/* one string onto the page: "F000:E05B text", and the rest of it under
   the text when it is longer than a line or has line ends in it */
static void browse_emit( TEXT *text, u32 seg, u32 offset, const u8 *str, u32 len )
{
    char line[80];
    u32 pos, out;
    int first = 1;

    for ( pos = 0; pos < len; ) {
        out = (u32)sfmt( line, sizeof( line ), first ? "%04X:%04X " : "          ", seg, offset );
        for ( ; pos < len && out < 72; pos++ ) {
            if ( str[pos] == '\r' || str[pos] == '\n' ) {
                while ( pos < len && (str[pos] == '\r' || str[pos] == '\n') ) {
                    pos++;
                }
                break;
            }
            line[out++] = str[pos] == '\t' ? ' ' : (char)str[pos];
        }
        line[out] = 0;
        if ( out > 10 ) {
            tx_add( text, line );
            first = 0;
        }
    }
}

/* every string of the area that has one of the keys in it, once each */
static void browse_area( const AREA *area, const char *const *keys, TEXT *text )
{
    static u32 seen[STRINGS_MAX];
    u8 *rom = rom_load( area->seg << 4, area->len );
    u32 at, start, length;
    int count = 0, key, hit, index;

    if ( rom == NULL ) {
        text->failed = 1;
        return;
    }
    for ( key = 0; keys[key]; key++ ) {
        for ( at = 0; at < area->len && count < STRINGS_MAX; at = start + length + 1 ) {
            hit = mem_ifind( rom + at, area->len - at, keys[key] );
            if ( hit < 0 ) {
                break;
            }
            start = rom_string( rom, area->len, at + (u32)hit, &length );
            if ( length < 4 ) {
                start = at + (u32)hit;      /* not text: step past it */
                length = 1;
                continue;
            }
            for ( index = 0; index < count && seen[index] != start; index++ ) {
            }
            if ( index == count ) {
                seen[count++] = start;
                browse_emit( text, area->seg, start, rom + start, length );
            }
        }
    }
    xfree( rom );
}

static const char *const browse_default[] = { "COPYRIGHT", "(C)", "VERSION", "BIOS", NULL };

/* for the report: every ROM, by the words a BIOS says about itself */
void browse_text( TEXT *text )
{
    int index;

    areas_find();
    for ( index = 0; index < area_count; index++ ) {
        if ( index ) {
            tx_add( text, "" );
        }
        tx_addf( text, "%-20s  %04X  %7u", areas[index].name, areas[index].seg, areas[index].len );
        tx_add( text, "" );
        browse_area( &areas[index], browse_default, text );
    }
}

static const char *area_item( DLG *dlg, int index )
{
    static char line[48];

    (void)dlg;
    sfmt( line, sizeof( line ), "%-18s %04X %7u", areas[index].name, areas[index].seg,
          areas[index].len );
    return line;
}

static int browse_activate( DLG *dlg, int ctl )
{
    const char *one[2];
    TEXT text;
    int sel = dlg->ctl[1].sel;

    if ( dlg->ctl[ctl].type == CT_BUTTON && dlg->ctl[ctl].id == ID_CANCEL ) {
        return 0;
    }
    tx_init( &text );
    one[0] = browse_key;
    one[1] = NULL;
    browse_area( &areas[sel], browse_key[0] ? one : browse_default, &text );
    if ( text.count == 0 ) {
        msg_box( "Nothing was found", MB_OK );
    } else {
        text_window( areas[sel].name, &text, NULL );
    }
    tx_free( &text );
    return 0;
}

void cmd_browse( void )
{
    static CTL ctl[6];
    DLG dlg;

    areas_find();
    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl[0].type = CT_LABEL;
    ctl[0].row = 1;
    ctl[0].col = 2;
    ctl[0].text = "&Area to Browse:";
    ctl[1].type = CT_LIST;
    ctl[1].row = 2;
    ctl[1].col = 2;
    ctl[1].width = 38;
    ctl[1].height = AREA_MAX + 2;
    ctl[1].count = area_count;
    ctl[1].item = area_item;
    ctl[2].type = CT_LABEL;
    ctl[2].row = 13;
    ctl[2].col = 2;
    ctl[2].text = "&Search String:";
    ctl[3].type = CT_EDIT;
    ctl[3].row = 13;
    ctl[3].col = 18;
    ctl[3].width = 22;
    ctl[3].buf = browse_key;
    ctl[3].max = sizeof( browse_key );
    ctl[4].type = CT_BUTTON;
    ctl[4].row = 15;
    ctl[4].col = 9;
    ctl[4].text = "OK";
    ctl[4].id = ID_OK;
    ctl[4].is_default = 1;
    ctl[5].type = CT_BUTTON;
    ctl[5].row = 15;
    ctl[5].col = 22;
    ctl[5].text = "Close";
    ctl[5].id = ID_CANCEL;
    dlg.title = "Memory Browser";
    dlg.rows = 18;
    dlg.cols = 42;
    dlg.ctl = ctl;
    dlg.nctl = 6;
    dlg.focus = 1;
    dlg.activate = browse_activate;
    dlg_run( &dlg );
}

/* ------------------------------------------------------------------ */
/* Insert Command                                                      */
/* ------------------------------------------------------------------ */

static const struct {
    const char *command, *file;
} inserts[] = {
    { "FILES=40",          "CONFIG.SYS" },
    { "BUFFERS=30",        "CONFIG.SYS" },
    { "LASTDRIVE=Z",       "CONFIG.SYS" },
    { "DOS=HIGH,UMB",      "CONFIG.SYS" },
    { "DEVICE=",           "CONFIG.SYS" },
    { "DEVICEHIGH=",       "CONFIG.SYS" },
    { "EMS=AUTO",          "CONFIG.SYS" },
    { "LFN=ON",            "CONFIG.SYS" },
    { "UNDELETE=ON",       "CONFIG.SYS" },
    { "SET TEMP=C:\\TEMP", "AUTOEXEC.BAT" },
    { "PATH=C:\\",         "AUTOEXEC.BAT" },
    { "PROMPT $P$G",       "AUTOEXEC.BAT" }
};
#define INSERT_COUNT    (int)(sizeof( inserts ) / sizeof( inserts[0] ))

static const char *insert_item( DLG *dlg, int index )
{
    static char line[60];

    (void)dlg;
    sfmt( line, sizeof( line ), "%-26s %s", inserts[index].command, inserts[index].file );
    return line;
}

/* what a line sets: up to its '=' - "FILES=", "SET TEMP=" - or, with
   none, its first word.  DEVICE= and its kind set nothing by themselves:
   there may be many, so their key is the whole line. */
static u32 line_key( const char *line, u32 len )
{
    u32 pos;

    for ( pos = 0; pos < len && line[pos] != '='; pos++ ) {
    }
    if ( pos < len ) {
        if ( str_nicmp( line, "DEVICE", 6 ) == 0 || str_nicmp( line, "INSTALL", 7 ) == 0 ) {
            return len;
        }
        return pos + 1;
    }
    for ( pos = 0; pos < len && line[pos] != ' ' && line[pos] != '\t'; pos++ ) {
    }
    return pos;
}

/* the line in "data" that sets what "command" sets: its start and
   length (without its line end), or 0 */
static int find_line( const char *data, u32 size, const char *command, u32 *start, u32 *length )
{
    u32 key = line_key( command, str_len( command ) ), pos = 0, begin, end;

    while ( pos < size ) {
        begin = pos;
        while ( pos < size && data[pos] != '\r' && data[pos] != '\n' ) {
            pos++;
        }
        end = pos;
        while ( pos < size && (data[pos] == '\r' || data[pos] == '\n') ) {
            pos++;
        }
        while ( begin < end && (data[begin] == ' ' || data[begin] == '\t') ) {
            begin++;
        }
        if ( end - begin >= key && str_nicmp( data + begin, command, key ) == 0
             && line_key( data + begin, end - begin ) == key ) {
            *start = begin;
            *length = end - begin;
            return 1;
        }
    }
    return 0;
}

static void insert_into( const char *path, const char *command )
{
    static char shown[70];
    u32 handle, size = 0, done, start = 0, length = 0;
    char *data = NULL;
    int rc, replace = 0;

    rc = sys_open_read( path, &handle );
    if ( rc == 0 ) {
        rc = sys_file_size( handle, &size );
        if ( rc == 0 ) {
            data = (char *)try_alloc( size + 1 );
            if ( data == NULL ) {
                sys_close( handle );
                out_of_memory();
                return;
            }
            rc = sys_read( handle, data, size, &done );
            size = done;
        }
        sys_close( handle );
        if ( rc ) {
            xfree( data );
            msg_box2( err_text( rc ), path, MB_OK );
            return;
        }
    } else if ( rc != ERR_NOFILE ) {
        msg_box2( err_text( rc ), path, MB_OK );
        return;
    }
    while ( size && data[size - 1] == 0x1A ) {      /* an old editor's Ctrl+Z */
        size--;
    }
    if ( data && find_line( data, size, command, &start, &length ) ) {
        mem_cpy( shown, data + start, length > 60 ? 60 : length );
        shown[length > 60 ? 60 : length] = 0;
        rc = msg_box2( "The file has this line.  Replace the line?", shown, MB_YESNO );
        if ( rc == ID_CANCEL ) {
            xfree( data );
            return;
        }
        replace = rc == ID_YES;
    }

    rc = sys_create( path, &handle );
    if ( rc == 0 ) {
        if ( replace ) {
            rc = sys_write( handle, data, start, &done );
            if ( rc == 0 ) {
                rc = sys_write( handle, command, str_len( command ), &done );
            }
            if ( rc == 0 && start + length < size ) {
                rc = sys_write( handle, data + start + length, size - start - length, &done );
            }
        } else {
            if ( size ) {
                rc = sys_write( handle, data, size, &done );
                if ( rc == 0 && data[size - 1] != '\n' ) {
                    rc = sys_write( handle, "\r\n", 2, &done );
                }
            }
            if ( rc == 0 ) {
                rc = sys_write( handle, command, str_len( command ), &done );
            }
            if ( rc == 0 ) {
                rc = sys_write( handle, "\r\n", 2, &done );
            }
        }
        if ( rc ) {
            sys_close( handle );
        } else {
            rc = sys_close( handle );
        }
    }
    xfree( data );
    if ( rc ) {
        msg_box2( err_text( rc ), path, MB_OK );
    } else {
        msg_box2( replace ? "The line was replaced in" : "The line was added to", path, MB_OK );
    }
}

void cmd_insert( void )
{
    static CTL ctl[6];
    static char command[80], path[80];
    DLG dlg;
    int sel;

    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl[0].type = CT_LABEL;
    ctl[0].row = 1;
    ctl[0].col = 2;
    ctl[0].text = "&Command                    File";
    ctl[1].type = CT_LIST;
    ctl[1].row = 2;
    ctl[1].col = 2;
    ctl[1].width = 46;
    ctl[1].height = INSERT_COUNT + 2;
    ctl[1].count = INSERT_COUNT;
    ctl[1].item = insert_item;
    ctl[2].type = CT_BUTTON;
    ctl[2].row = INSERT_COUNT + 5;
    ctl[2].col = 13;
    ctl[2].text = "OK";
    ctl[2].id = ID_OK;
    ctl[2].is_default = 1;
    ctl[3].type = CT_BUTTON;
    ctl[3].row = INSERT_COUNT + 5;
    ctl[3].col = 26;
    ctl[3].text = "Cancel";
    ctl[3].id = ID_CANCEL;
    dlg.title = "Insert Command";
    dlg.rows = INSERT_COUNT + 8;
    dlg.cols = 50;
    dlg.ctl = ctl;
    dlg.nctl = 4;
    dlg.focus = 1;
    if ( dlg_run( &dlg ) != ID_OK ) {
        return;
    }
    sel = ctl[1].sel;
    str_cpy( command, inserts[sel].command );
    sfmt( path, sizeof( path ), "%c:\\%s", 'A' + sys_boot_drive(), inserts[sel].file );

    /* the line as it will be written, and where: both can be changed */
    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl[0].type = CT_LABEL;
    ctl[0].row = 2;
    ctl[0].col = 3;
    ctl[0].text = "&Command:";
    ctl[1].type = CT_EDIT;
    ctl[1].row = 2;
    ctl[1].col = 13;
    ctl[1].width = 40;
    ctl[1].buf = command;
    ctl[1].max = sizeof( command );
    ctl[2].type = CT_LABEL;
    ctl[2].row = 4;
    ctl[2].col = 3;
    ctl[2].text = "&File:";
    ctl[3].type = CT_EDIT;
    ctl[3].row = 4;
    ctl[3].col = 13;
    ctl[3].width = 40;
    ctl[3].buf = path;
    ctl[3].max = sizeof( path );
    ctl[4].type = CT_BUTTON;
    ctl[4].row = 6;
    ctl[4].col = 17;
    ctl[4].text = "OK";
    ctl[4].id = ID_OK;
    ctl[4].is_default = 1;
    ctl[5].type = CT_BUTTON;
    ctl[5].row = 6;
    ctl[5].col = 30;
    ctl[5].text = "Cancel";
    ctl[5].id = ID_CANCEL;
    dlg.title = "Insert Command";
    dlg.rows = 9;
    dlg.cols = 58;
    dlg.ctl = ctl;
    dlg.nctl = 6;
    dlg.focus = 1;
    if ( dlg_run( &dlg ) != ID_OK || command[0] == 0 || path[0] == 0 ) {
        return;
    }
    insert_into( path, command );
}

/* ------------------------------------------------------------------ */
/* Test Printer                                                        */
/* ------------------------------------------------------------------ */

static int printer_kind, printer_bits, printer_port;
static const char *const printer_ports[7] = { "LPT1", "LPT2", "LPT3", "COM1", "COM2", "COM3", "COM4" };

static int print_bytes( u32 handle, const char *text, u32 len )
{
    u32 done;
    int rc = sys_write( handle, text, len, &done );

    if ( rc == 0 && crit_take() ) {
        rc = 29;
    }
    return rc;
}

static int print_text( u32 handle, const char *text )
{
    return print_bytes( handle, text, str_len( text ) );
}

/* the characters from a space to 126 (7 bits) or to 255, thirty-two to
   a line: as they are, or as PostScript strings to be shown */
static int print_page( u32 handle )
{
    char line[160];
    DATETIME now;
    int rc, ch, last = printer_bits ? 255 : 126, len, row = 0;

    sys_now( &now );
    if ( printer_kind ) {
        rc = print_text( handle, "%!\r\n/Courier findfont 11 scalefont setfont\r\n"
                                 "72 720 moveto (PM-DOS Diagnostics - printer test) show\r\n" );
        sfmt( line, sizeof( line ), "72 704 moveto (%s   %u/%02u/%u   %u:%02u) show\r\n",
              printer_ports[printer_port], now.month, now.day, now.year, now.hour, now.minute );
    } else {
        rc = print_text( handle, "PM-DOS Diagnostics - printer test\r\n" );
        sfmt( line, sizeof( line ), "%s   %u/%02u/%u   %u:%02u\r\n\r\n", printer_ports[printer_port],
              now.month, now.day, now.year, now.hour, now.minute );
    }
    if ( rc == 0 ) {
        rc = print_text( handle, line );
    }
    for ( ch = 32; ch <= last && rc == 0; ) {
        len = 0;
        if ( printer_kind ) {
            len = sfmt( line, sizeof( line ), "72 %u moveto (", 672 - row * 14 );
        }
        do {
            if ( printer_kind && (ch == '(' || ch == ')' || ch == '\\' || ch > 126) ) {
                len += sfmt( line + len, sizeof( line ) - (u32)len, "\\%u%u%u", (ch >> 6) & 7,
                             (ch >> 3) & 7, ch & 7 );
            } else if ( ch != 127 ) {
                line[len++] = (char)ch;
            }
            ch++;
        } while ( ch <= last && (ch & 31) );
        line[len] = 0;
        str_catn( line, printer_kind ? ") show\r\n" : "\r\n", sizeof( line ) );
        rc = print_text( handle, line );
        row++;
    }
    if ( rc == 0 ) {
        rc = print_text( handle, printer_kind ? "showpage\r\n\004" : "\f" );
    }
    return rc;
}

void cmd_printer( void )
{
    static CTL ctl[16];
    DLG dlg;
    u32 handle;
    int index, rc;

    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl[0].type = CT_LABEL;
    ctl[0].row = 2;
    ctl[0].col = 3;
    ctl[0].text = "Printer Type:";
    ctl[1].type = CT_RADIO;
    ctl[1].row = 3;
    ctl[1].col = 5;
    ctl[1].text = "&Generic/TTY Printer";
    ctl[1].id = 0;
    ctl[1].value = &printer_kind;
    ctl[2].type = CT_RADIO;
    ctl[2].row = 4;
    ctl[2].col = 5;
    ctl[2].text = "&PostScript Printer";
    ctl[2].id = 1;
    ctl[2].value = &printer_kind;
    ctl[3].type = CT_LABEL;
    ctl[3].row = 6;
    ctl[3].col = 3;
    ctl[3].text = "Test Type:";
    ctl[4].type = CT_RADIO;
    ctl[4].row = 7;
    ctl[4].col = 5;
    ctl[4].text = "&7-bit ASCII (32-127)";
    ctl[4].id = 0;
    ctl[4].value = &printer_bits;
    ctl[5].type = CT_RADIO;
    ctl[5].row = 8;
    ctl[5].col = 5;
    ctl[5].text = "&8-bit ASCII (32-255)";
    ctl[5].id = 1;
    ctl[5].value = &printer_bits;
    ctl[6].type = CT_LABEL;
    ctl[6].row = 2;
    ctl[6].col = 34;
    ctl[6].text = "Printer Port:";
    for ( index = 0; index < 7; index++ ) {
        ctl[7 + index].type = CT_RADIO;
        ctl[7 + index].row = 3 + index;
        ctl[7 + index].col = 36;
        ctl[7 + index].text = printer_ports[index];
        ctl[7 + index].id = index;
        ctl[7 + index].value = &printer_port;
    }
    ctl[14].type = CT_BUTTON;
    ctl[14].row = 11;
    ctl[14].col = 14;
    ctl[14].text = "OK";
    ctl[14].id = ID_OK;
    ctl[14].is_default = 1;
    ctl[15].type = CT_BUTTON;
    ctl[15].row = 11;
    ctl[15].col = 27;
    ctl[15].text = "Cancel";
    ctl[15].id = ID_CANCEL;
    dlg.title = "Test Printer";
    dlg.rows = 14;
    dlg.cols = 52;
    dlg.ctl = ctl;
    dlg.nctl = 16;
    dlg.focus = 1;
    if ( dlg_run( &dlg ) != ID_OK ) {
        return;
    }
    crit_take();
    rc = sys_open_write( printer_ports[printer_port], &handle );
    if ( rc == 0 ) {
        app_status( "Printing ..." );
        ui_redraw();
        scr_flush();
        rc = print_page( handle );
        sys_close( handle );
        app_status( NULL );
    }
    if ( rc ) {
        msg_box2( err_text( rc ), printer_ports[printer_port], MB_OK );
    } else {
        msg_box2( "The test page was sent to", printer_ports[printer_port], MB_OK );
    }
}

/* ------------------------------------------------------------------ */
/* About                                                               */
/* ------------------------------------------------------------------ */

void cmd_about( void )
{
    static char text[120];
    static CTL ctl[2];
    DLG dlg;
    int major, minor;

    sys_version( &major, &minor );
    sfmt( text, sizeof( text ), "PM-DOS Diagnostics\nVersion %s\n\nPM-DOS version %u.%02u",
          PMD_VERSION, major, minor );
    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl[0].type = CT_TEXT;
    ctl[0].row = 2;
    ctl[0].col = 1;
    ctl[0].width = 38;
    ctl[0].text = text;
    ctl[1].type = CT_BUTTON;
    ctl[1].row = 7;
    ctl[1].col = 16;
    ctl[1].text = "OK";
    ctl[1].id = ID_OK;
    ctl[1].is_default = 1;
    dlg.title = "About PMD";
    dlg.rows = 10;
    dlg.cols = 40;
    dlg.ctl = ctl;
    dlg.nctl = 2;
    dlg.focus = 1;
    dlg_run( &dlg );
}
