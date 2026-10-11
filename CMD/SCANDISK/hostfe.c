/*
 * HOSTFE.C - the front end as lines of text.  THE HOST TEST BUILD ONLY
 * (SCANDKNT.EXE, which MKHOST.CMD makes): SCANDISK.EXE has its screens
 * and nothing else, as MS-DOS ScanDisk has.
 *
 * The checks know nothing of how they are shown.  They call fe_*() - a
 * check has started, here is a problem and what is to be done about it
 * - and in SCANDISK.EXE those are SCREEN.C's.  Here each check is named
 * as it starts, each problem is a paragraph, and each question is a
 * line with the answer read from standard input, where the end of the
 * input answers "no" to everything from then on.  That is what a test
 * can feed and compare.
 */
#include "scandisk.h"

static void put_line( const char *text )
{
    say( text );
    say( "\r\n" );
}

/* a paragraph, wrapped to the screen's width, "indent" spaces in */
static void put_para( const char *text, int indent )
{
    static const char spaces[] = "        ";
    TEXT lines;
    int index;

    tx_init( &lines );
    tx_wrap( &lines, text, 78 - indent );
    for ( index = 0; index < lines.count; index++ ) {
        say( spaces + 8 - indent );
        put_line( lines.line[index] );
    }
    tx_free( &lines );
}

/* a question with one-letter answers; the end of the input is "last" */
static int ask( const char *question, const char *letters, int last )
{
    char line[80];
    int len, ch;

    for ( ;; ) {
        say( question );
        len = sys_read_line( line, sizeof( line ) );
        if ( len < 0 ) {
            return last;
        }
        ch = ch_upper( line[0] );
        if ( len > 0 && str_chr( letters, ch ) != NULL ) {
            return ch;
        }
    }
}

static int said_problem;        /* since the last check was named */

void fe_start( void )
{
}

void fe_finish( void )
{
}

void fe_drive_begin( void )
{
    char line[80];

    said_problem = 0;
    sfmt( line, sizeof( line ), "ScanDisk is checking drive %c:", 'A' + vol.drive );
    put_line( "" );
    put_line( line );
    put_line( "" );
}

void fe_phase( int phase )
{
    if ( phase < PH_COUNT ) {
        if ( said_problem ) {
            put_line( "" );             /* a problem above: set the name off */
            said_problem = 0;
        }
        put_line( phase_name( phase ) );
    }
}

void fe_progress( u32 done, u32 total )
{
    (void)done;
    (void)total;
    if ( sys_break_take() ) {
        scan_quit( Q_USER );
    }
}

int fe_problem( int kind, const char *text )
{
    said_problem = 1;
    put_line( "" );
    put_para( text, 0 );
    switch ( pb_choices( kind ) ) {
    case CH_FIX:
        return ask( "Fix it (Y/N)? ", "YN", 'N' ) == 'Y' ? ACT_FIX : ACT_SKIP;
    case CH_DELETE:
        return ask( "Delete the entry (Y/N)? ", "YN", 'N' ) == 'Y' ? ACT_DELETE : ACT_SKIP;
    case CH_SAVEDEL:
        switch ( ask( "Save the lost clusters as files, Delete them, or do Neither (S/D/N)? ",
                      "SDN", 'N' ) ) {
        case 'S': return ACT_SAVE;
        case 'D': return ACT_DELETE;
        }
        return ACT_SKIP;
    }
    put_line( "   ScanDisk cannot repair this." );
    return ACT_SKIP;
}

void fe_outcome( int news, const char *text )
{
    if ( news < 0 ) {
        said_problem = 1;
        put_line( "" );
        put_para( text, 0 );
    } else {
        put_para( text, 3 );
    }
}

int fe_ask_surface( void )
{
    char line[120];

    put_line( "" );
    sfmt( line, sizeof( line ), "Scan the surface of drive %c: for sectors that cannot be read "
          "(Y/N)? ", 'A' + vol.drive );
    return ask( line, "YN", 'N' ) == 'Y';
}

int fe_ask_undo( int have_a, int have_b )
{
    int ch;

    put_line( "" );
    put_para( "ScanDisk can keep what its repairs change in an Undo file on a diskette, so "
              "that SCANDISK /UNDO can put the drive back as it was.", 0 );
    ch = ask( have_a && have_b ? "Drive for the Undo disk (A or B), or N for none? "
              : have_a ? "Put the Undo file on the disk in drive A (Y/N)? "
              : "Put the Undo file on the disk in drive B (Y/N)? ",
              have_a && have_b ? "ABN" : "YN", 'N' );
    if ( ch == 'A' || (ch == 'Y' && have_a) ) {
        return 0;
    }
    if ( ch == 'B' || ch == 'Y' ) {
        return 1;
    }
    return -1;
}

void fe_message( const char *text )
{
    put_line( "" );
    put_para( text, 0 );
}

int fe_confirm( const char *title, const char *text )
{
    (void)title;
    put_line( "" );
    put_para( text, 0 );
    return ask( "(Y/N)? ", "YN", 'N' ) == 'Y';
}

void fe_busy( const char *text )
{
    put_line( text );
}

void fe_surface_begin( void )
{
}

void fe_surface_at( u32 clus, u32 count, int bad )
{
    (void)clus;
    (void)count;
    (void)bad;
    if ( sys_break_take() ) {
        scan_quit( Q_USER );
    }
}

void fe_drive_end( int more )
{
    TEXT text;
    int index;

    (void)more;
    tx_init( &text );
    summary_text( &text );
    put_line( "" );
    for ( index = 0; index < text.count; index++ ) {
        put_line( text.line[index] );
    }
    tx_free( &text );
}

void fe_all_end( int drives, u32 found, u32 fixed )
{
    char line[160];

    put_line( "" );
    if ( found == 0 ) {
        sfmt( line, sizeof( line ), "ScanDisk checked %u drives and found no problems.", drives );
    } else {
        sfmt( line, sizeof( line ), "ScanDisk checked %u drives and found %,u problem%s; %,u "
              "%s repaired.", drives, found, found == 1 ? "" : "s", fixed,
              fixed == 1 ? "was" : "were" );
    }
    put_line( line );
}
