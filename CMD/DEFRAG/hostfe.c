/*
 * HOSTFE.C - the front end as lines of text.  THE HOST TEST BUILD ONLY
 * (DEFRAGNT.EXE, which MKHOST.CMD makes): DEFRAG.EXE has its screen and
 * nothing else, as MS-DOS's DEFRAG has.
 *
 * The work knows nothing of how it is shown.  It calls fe_*() - a
 * drive has been read, these clusters are being moved, is anyone asking
 * to stop - and in DEFRAG.EXE those are SCREEN.C's.  Here a drive is
 * described in a line, each question is answered the way a run started
 * with /F or /U would have it answered, and what a run did is counted.
 *
 *   DFSTOP=n    "stop" is asked for once n moves have been made: a run
 *               left in the middle, to be looked at
 *   DFTRACE=1   a line for every move
 */
#include "defrag.h"
#include "text.h"

static u32  moves, moved, stop_after;
static int  tracing;

static void __cdecl put( const char *fmt, ... )
{
    char line[400];

    vfmt( line, sizeof( line ), fmt, VA_START( fmt ) );
    say( line );
    say( "\r\n" );
}

static u32 env_number( const char *name )
{
    const char *text = sys_getenv( name );
    u32 val = 0;

    for ( ; text && *text >= '0' && *text <= '9'; text++ ) {
        val = val * 10 + (u32)(*text - '0');
    }
    return val;
}

void fe_start( void )
{
    stop_after = env_number( "DFSTOP" );
    tracing = env_number( "DFTRACE" ) != 0;
}

void fe_finish( void )
{
}

void fe_suspend( void )
{
}

void fe_resume( void )
{
}

void fe_drive( int loaded )
{
    if ( !loaded ) {
        return;
    }
    put( "Drive %c: FAT%u, %,u clusters of %,u bytes: %,u used, %,u free, %,u bad, %,u not moved",
         'A' + vol.drive, (u32)vol.fattype, vol.nclus, vol.bpc, dst.used, dst.free, dst.bad, dst.fixed );
    put( "%,u files, %,u directories; %,u in pieces, %,u free clusters below the last one used",
         dst.files, dst.folders, dst.frag_items, dst.gaps );
}

void fe_reading( const char *what )
{
    (void)what;
}

void fe_run( int running )
{
    if ( running ) {
        moves = moved = 0;
        put( "%s%s%s%s", tx_method[opt.method], opt.sort ? ", sorted by " : "",
             opt.sort ? tx_sort[opt.sort] : "", opt.sort && opt.descending ? " descending" : "" );
    }
}

void fe_activity( int what, u32 clus, u32 count )
{
    if ( what == ACT_WRITE ) {
        moves++;
        moved += count;
    }
    if ( tracing && (what == ACT_READ || what == ACT_WRITE || what == ACT_SORT) ) {
        put( "  %s %u+%u", what == ACT_READ ? "read " : what == ACT_WRITE ? "write" : "sort ", clus, count );
    }
}

void fe_progress( u32 done, u32 total )
{
    (void)done;
    (void)total;
}

int fe_stop_asked( void )
{
    return stop_after != 0 && moves >= stop_after;
}

int fe_log_drive( void )
{
    return -1;
}

int fe_menu( void )
{
    return GO_EXIT;
}

int fe_pick_drive( int now )
{
    return now;
}

int fe_pick_method( void )
{
    return 0;
}

void fe_pick_sort( void )
{
}

void fe_legend( void )
{
}

void fe_about( void )
{
}

int fe_recommend( int percent, int method )
{
    put( tx_recommend_1, (u32)percent, 'A' + vol.drive );
    put( "%s %s", tx_recommend_2, tx_method[method] );
    return 1;
}

int fe_fs_error( const char *text )
{
    TEXT lines;
    int index;

    tx_init( &lines );
    tx_wrap( &lines, text, 78 );
    for ( index = 0; index < lines.count; index++ ) {
        put( "%s", lines.line[index] );
    }
    tx_free( &lines );
    return 0;
}

void fe_message( const char *text, int error )
{
    (void)error;
    put( "%s", text );
}

void fe_finished( void )
{
    put( "%s: %,u moves, %,u clusters moved", tx_finished, moves, moved );
    put( "%,u in pieces now, %,u free clusters below the last one used", dst.frag_items, dst.gaps );
}

int fe_done( void )
{
    return DONE_EXIT;
}
