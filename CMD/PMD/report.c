/*
 * REPORT.C - the report: what /F, /P and /S write, and File, Print
 * Report.
 *
 * A REPORT IS THE PAGES, ONE AFTER ANOTHER, as the same text the
 * windows show - page_build is asked for each with "report" set, which
 * only changes the memory map's block characters into letters a
 * printer has.  Each is headed by its name in a rule of dashes and
 * centred, as MSD's are, on pages of sixty lines with the program, the
 * date and the page number at the top of each.  A section that would
 * be cut by the end of a page starts the next one instead, when a
 * whole page is enough to hold it.
 *
 * It goes to a file or to a device - LPT1, COM2 - which DOS opens by
 * name like any file.
 */
#include "pmd.h"

#define REPORT_WIDTH    72
#define REPORT_INDENT   4
#define PAGE_LINES      60

const char *const customer_labels[8] = {
    "        Name: ", "Company Name: ", "    Address1: ", "    Address2: ",
    " City/ST/Zip: ", "     Country: ", "       Phone: ", "    Comments: "
};

typedef struct {
    u32  handle;
    int  rc;                    /* the first error, and nothing after it */
    int  line, page;
    int  is_device;
    DATETIME when;
    char buf[1024];
    u32  used;
} REPORT;

static void rpt_flush( REPORT *rpt )
{
    u32 done;
    int rc;

    if ( rpt->used && rpt->rc == 0 ) {
        crit_take();                    /* only what THIS write runs into */
        rc = sys_write( rpt->handle, rpt->buf, rpt->used, &done );
        if ( rc == 0 && crit_take() ) {
            rc = 29;                    /* the device would not take it */
        }
        rpt->rc = rc;
    }
    rpt->used = 0;
}

static void rpt_bytes( REPORT *rpt, const char *text, u32 len )
{
    while ( len-- ) {
        if ( rpt->used == sizeof( rpt->buf ) ) {
            rpt_flush( rpt );
        }
        rpt->buf[rpt->used++] = *text++;
    }
}

static void rpt_header( REPORT *rpt )
{
    char line[100];
    int hour = rpt->when.hour % 12, len;

    if ( rpt->page ) {
        rpt_bytes( rpt, "\f", 1 );
    }
    rpt->page++;
    len = sfmt( line, sizeof( line ),
                "\r\n\r\n%*s   PM-DOS Diagnostics version %s   %2u/%02u/%02u   %2u:%02u%s   Page%3u\r\n",
                REPORT_INDENT, "", PMD_VERSION, rpt->when.month, rpt->when.day,
                rpt->when.year % 100, hour ? hour : 12, rpt->when.minute,
                rpt->when.hour < 12 ? "am" : "pm", rpt->page );
    rpt_bytes( rpt, line, (u32)len );
    mem_set( line, ' ', REPORT_INDENT );
    mem_set( line + REPORT_INDENT, '=', REPORT_WIDTH );
    rpt_bytes( rpt, line, REPORT_INDENT + REPORT_WIDTH );
    rpt_bytes( rpt, "\r\n\r\n", 4 );
    rpt->line = 5;
}

static void rpt_line( REPORT *rpt, int indent, const char *text )
{
    char pad[80];
    u32 len = str_len( text );

    if ( rpt->line >= PAGE_LINES ) {
        rpt_header( rpt );
    }
    if ( len ) {
        if ( indent > (int)sizeof( pad ) ) {
            indent = sizeof( pad );
        }
        mem_set( pad, ' ', (u32)indent );
        rpt_bytes( rpt, pad, (u32)indent );
        rpt_bytes( rpt, text, len );
    }
    rpt_bytes( rpt, "\r\n", 2 );
    rpt->line++;
}

/* "--------- Name ---------", REPORT_WIDTH wide */
static void rpt_title( REPORT *rpt, const char *name )
{
    char rule[REPORT_WIDTH + 1];
    int len = (int)str_len( name ), left;

    if ( len > REPORT_WIDTH - 6 ) {
        len = REPORT_WIDTH - 6;
    }
    left = (REPORT_WIDTH - len - 2) / 2;
    mem_set( rule, '-', REPORT_WIDTH );
    rule[REPORT_WIDTH] = 0;
    rule[left] = ' ';
    mem_cpy( rule + left + 1, name, (u32)len );
    rule[left + 1 + len] = ' ';
    rpt_line( rpt, REPORT_INDENT, rule );
    rpt_line( rpt, 0, "" );
}

/* a section: its title, its lines as a block in the middle of the
   page, and a blank line.  "as_is" for a file's lines, which start at
   the margin and are cut where the page ends. */
static void rpt_section( REPORT *rpt, const char *name, const TEXT *text, int as_is )
{
    char cut[REPORT_WIDTH + 1];
    int index, width = tx_width( text ), indent, need = text->count + 3;

    if ( rpt->line + need > PAGE_LINES && need <= PAGE_LINES - 5 && rpt->line > 5 ) {
        rpt_header( rpt );
    }
    rpt_title( rpt, name );
    indent = REPORT_INDENT;
    if ( !as_is && width < REPORT_WIDTH ) {
        indent += (REPORT_WIDTH - width) / 2;
    }
    for ( index = 0; index < text->count; index++ ) {
        if ( str_len( text->line[index] ) > REPORT_WIDTH ) {
            mem_cpy( cut, text->line[index], REPORT_WIDTH );
            cut[REPORT_WIDTH] = 0;
            rpt_line( rpt, REPORT_INDENT, cut );
        } else {
            rpt_line( rpt, indent, text->line[index] );
        }
    }
    rpt_line( rpt, 0, "" );
}

/* "Computer: Award/Award, 486DX" - a page's two lines, as one */
void report_summary_text( TEXT *text )
{
    char line1[SUM_WIDTH + 2], line2[SUM_WIDTH + 2];
    int page;

    for ( page = 0; page <= PG_COM; page++ ) {
        page_summary( page, line1, line2 );
        if ( line1[0] && line2[0] ) {
            tx_addf( text, "%14s: %s%s%s", page_names[page], line1,
                     page == PG_DISK ? " " : ", ", line2 );
        } else {
            tx_addf( text, "%14s: %s", page_names[page], line1 );
        }
    }
}

int report_write( const char *path, const u8 *items, const CUSTOMER *who )
{
    static REPORT rpt;
    TEXT text;
    int rc, index;
    char name[PATH_MAX + 8];

    mem_set( &rpt, 0, sizeof( rpt ) );
    sys_now( &rpt.when );
    crit_take();
    /* a device first: PRN, LPT1 and COM1 open, a new file does not */
    rc = sys_open_write( path, &rpt.handle );
    if ( rc == 0 && !sys_is_device( rpt.handle ) ) {
        sys_close( rpt.handle );
        rc = ERR_NOFILE;
    }
    if ( rc ) {
        rc = sys_create( path, &rpt.handle );
    }
    if ( rc || crit_take() ) {
        return rc ? rc : 21;
    }
    rpt_header( &rpt );

    if ( items[RPT_CUSTOMER] ) {
        tx_init( &text );
        for ( index = 0; index < 8; index++ ) {
            tx_addf( &text, "%s%s", customer_labels[index], who->field[index] );
        }
        rpt_section( &rpt, "Customer Information", &text, 0 );
        tx_free( &text );
    }
    if ( items[RPT_SUMMARY] ) {
        tx_init( &text );
        report_summary_text( &text );
        rpt_section( &rpt, "Summary Information", &text, 0 );
        tx_free( &text );
    }
    for ( index = 0; index < PG_COUNT && rpt.rc == 0; index++ ) {
        if ( !items[RPT_PAGE + index] ) {
            continue;
        }
        tx_init( &text );
        page_build( index, &text, 1 );
        rpt_section( &rpt, page_names[index], &text, 0 );
        tx_free( &text );
    }
    if ( items[RPT_BROWSER] && rpt.rc == 0 ) {
        tx_init( &text );
        browse_text( &text );
        rpt_section( &rpt, "Memory Browser", &text, 1 );
        tx_free( &text );
    }
    for ( index = 0; index < view_count && rpt.rc == 0; index++ ) {
        if ( !items[RPT_FILE + index] ) {
            continue;
        }
        tx_init( &text );
        if ( file_to_text( view_paths[index], &text ) == 0 ) {
            str_cpyn( name, view_paths[index], sizeof( name ) );
            rpt_section( &rpt, name, &text, 1 );
        }
        tx_free( &text );
    }
    rpt_bytes( &rpt, "\f", 1 );
    rpt_flush( &rpt );
    rc = sys_close( rpt.handle );
    return rpt.rc ? rpt.rc : rc;
}

/* ------------------------------------------------------------------ */
/* File, Print Report                                                  */
/* ------------------------------------------------------------------ */

#define PORT_FILE       8       /* the "File:" option; 1..7 are the ports */

static int  rpt_checks[RPT_ITEMS + 1];      /* [0]: Report All */
static int  rpt_port = 1;
static char rpt_file[80] = "REPORT.PMD";
static const char *const port_names[7] = { "LPT1", "LPT2", "LPT3", "COM1", "COM2", "COM3", "COM4" };

static void report_changed( DLG *dlg, int ctl )
{
    int index;

    if ( dlg->ctl[ctl].value == &rpt_checks[0] ) {      /* Report All */
        for ( index = 1; index <= RPT_ITEMS; index++ ) {
            rpt_checks[index] = rpt_checks[0];
        }
    } else if ( dlg->ctl[ctl].type == CT_CHECK && !*dlg->ctl[ctl].value ) {
        rpt_checks[0] = 0;
    }
}

static int customer_dialog( CUSTOMER *who )
{
    static CTL ctl[18];
    DLG dlg;
    int index;

    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    for ( index = 0; index < 8; index++ ) {
        ctl[index * 2].type = CT_LABEL;
        ctl[index * 2].row = 2 + index;
        ctl[index * 2].col = 2;
        ctl[index * 2].text = customer_labels[index];
        ctl[index * 2 + 1].type = CT_EDIT;
        ctl[index * 2 + 1].row = 2 + index;
        ctl[index * 2 + 1].col = 17;
        ctl[index * 2 + 1].width = 38;
        ctl[index * 2 + 1].buf = who->field[index];
        ctl[index * 2 + 1].max = sizeof( who->field[index] );
    }
    ctl[16].type = CT_BUTTON;
    ctl[16].row = 11;
    ctl[16].col = 18;
    ctl[16].text = "OK";
    ctl[16].id = ID_OK;
    ctl[16].is_default = 1;
    ctl[17].type = CT_BUTTON;
    ctl[17].row = 11;
    ctl[17].col = 30;
    ctl[17].text = "Cancel";
    ctl[17].id = ID_CANCEL;
    dlg.title = "Customer Information";
    dlg.rows = 14;
    dlg.cols = 59;
    dlg.ctl = ctl;
    dlg.nctl = 18;
    dlg.focus = 1;
    return dlg_run( &dlg ) == ID_OK;
}

void cmd_report( void )
{
    static CTL ctl[RPT_ITEMS + 16];
    static CUSTOMER who;
    static int primed;
    static u8 items[RPT_ITEMS];
    static char file_labels[MAX_VIEWS][16];
    DLG dlg;
    int count = 0, index, slot, rc, total = RPT_FILE + view_count;
    const char *label;
    const char *path;

    if ( !primed ) {
        /* everything but who it is from, as /P has it */
        for ( index = 0; index <= RPT_ITEMS; index++ ) {
            rpt_checks[index] = 1;
        }
        rpt_checks[0] = 0;
        rpt_checks[1 + RPT_CUSTOMER] = 0;
        primed = 1;
    }
    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );

    ctl[count].type = CT_CHECK;
    ctl[count].row = 2;
    ctl[count].col = 3;
    ctl[count].text = "Report &All";
    ctl[count].value = &rpt_checks[0];
    count++;
    /* three columns of eight: who it is from, the summary, the pages,
       the browser, the files */
    for ( index = 0; index < total; index++ ) {
        if ( index == RPT_CUSTOMER ) {
            label = "Customer Information";
        } else if ( index == RPT_SUMMARY ) {
            label = "System Summary";
        } else if ( index == RPT_BROWSER ) {
            label = "Memory Browser";
        } else if ( index >= RPT_FILE ) {
            str_cpyn( file_labels[index - RPT_FILE], view_names[index - RPT_FILE],
                      sizeof( file_labels[0] ) );
            label = file_labels[index - RPT_FILE];
        } else {
            label = page_names[index - RPT_PAGE];
        }
        slot = index;
        ctl[count].type = CT_CHECK;
        ctl[count].row = 4 + slot % 8;
        ctl[count].col = 3 + (slot / 8) * 26;
        ctl[count].text = label;
        ctl[count].value = &rpt_checks[1 + index];
        count++;
    }
    ctl[count].type = CT_LABEL;
    ctl[count].row = 13;
    ctl[count].col = 3;
    ctl[count].text = "Print to:";
    count++;
    for ( index = 0; index < 7; index++ ) {
        ctl[count].type = CT_RADIO;
        ctl[count].row = 14;
        ctl[count].col = 3 + index * 10;
        ctl[count].text = port_names[index];
        ctl[count].id = index + 1;
        ctl[count].value = &rpt_port;
        count++;
    }
    ctl[count].type = CT_RADIO;
    ctl[count].row = 15;
    ctl[count].col = 3;
    ctl[count].text = "&File:";
    ctl[count].id = PORT_FILE;
    ctl[count].value = &rpt_port;
    count++;
    ctl[count].type = CT_EDIT;
    ctl[count].row = 15;
    ctl[count].col = 14;
    ctl[count].width = 40;
    ctl[count].buf = rpt_file;
    ctl[count].max = sizeof( rpt_file );
    count++;
    ctl[count].type = CT_BUTTON;
    ctl[count].row = 17;
    ctl[count].col = 26;
    ctl[count].text = "OK";
    ctl[count].id = ID_OK;
    ctl[count].is_default = 1;
    count++;
    ctl[count].type = CT_BUTTON;
    ctl[count].row = 17;
    ctl[count].col = 38;
    ctl[count].text = "Cancel";
    ctl[count].id = ID_CANCEL;
    count++;

    dlg.title = "Report Information";
    dlg.rows = 20;
    dlg.cols = 78;
    dlg.ctl = ctl;
    dlg.nctl = count;
    dlg.focus = 0;
    dlg.changed = report_changed;
    if ( dlg_run( &dlg ) != ID_OK ) {
        return;
    }
    for ( index = 0; index < RPT_ITEMS; index++ ) {
        items[index] = (u8)(index < total && rpt_checks[1 + index]);
    }
    if ( items[RPT_CUSTOMER] && !customer_dialog( &who ) ) {
        return;
    }
    path = rpt_port == PORT_FILE ? rpt_file : port_names[rpt_port - 1];
    if ( path[0] == 0 ) {
        msg_box( "The report needs a file name", MB_OK );
        return;
    }
    app_status( "Writing the report ..." );
    ui_redraw();
    scr_flush();
    rc = report_write( path, items, &who );
    app_status( NULL );
    if ( rc ) {
        msg_box2( err_text( rc ), path, MB_OK );
    }
}
