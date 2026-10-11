/*
 * PROBLEM.C - a problem found: what is to be done about it, the log,
 * the counts the exit code is worked out from, and the way out of a
 * check that is not to go on.
 *
 * A check that finds something wrong calls pb_found() with the sentence
 * that says what, and gets back what to do:
 *
 *   /CHECKONLY     nothing, ever
 *   /AUTOFIX       the repair; lost clusters saved, or freed with /NOSAVE
 *   /CUSTOM        what SCANDISK.INI's [CUSTOM] section says for that
 *                  kind of problem - repair, skip, ask, or stop
 *   otherwise      whatever the user answers
 *
 * and then says how the repair went with pb_fixed() or pb_failed().
 * Every sentence goes into the log on the way.
 */
#include "scandisk.h"

TEXT  log_text;
STATS st;

extern JMPBUF quit_jmp;         /* MAIN.C: where a check is left for */

static char cur_text[700];

static const struct {
    int cat, choices;
} kinds[PB_COUNT] = {
    { CAT_MEDIA,    CH_FIX     },       /* PB_MEDIA       */
    { CAT_MISMATCH, CH_FIX     },       /* PB_FATREAD     */
    { CAT_MISMATCH, CH_FIX     },       /* PB_FATMISMATCH */
    { CAT_OKAY,     CH_FIX     },       /* PB_NAME        */
    { CAT_OKAY,     CH_FIX     },       /* PB_ATTR        */
    { CAT_OKAY,     CH_FIX     },       /* PB_SIZE        */
    { CAT_OKAY,     CH_FIX     },       /* PB_DOTS        */
    { CAT_OKAY,     CH_FIX     },       /* PB_LFN         */
    { CAT_OKAY,     CH_FIX     },       /* PB_LABEL       */
    { CAT_CHAIN,    CH_FIX     },       /* PB_CHAIN       */
    { CAT_CHAIN,    CH_FIX     },       /* PB_FIRST       */
    { CAT_BADENT,   CH_DELETE  },       /* PB_BADDIR      */
    { CAT_CROSS,    CH_FIX     },       /* PB_CROSS       */
    { CAT_LOST,     CH_SAVEDEL },       /* PB_LOST        */
    { CAT_BADCLUS,  CH_FIX     },       /* PB_BADCLUS     */
    { CAT_NONE,     CH_NONE    },       /* PB_SYSAREA     */
    { CAT_NONE,     CH_NONE    }        /* PB_DIRREAD     */
};

int pb_choices( int kind )
{
    return kinds[kind].choices;
}

int pb_category( int kind )
{
    return kinds[kind].cat;
}

/* ------------------------------------------------------------------ */
/* the log                                                             */
/* ------------------------------------------------------------------ */

void __cdecl log_line( const char *fmt, ... )
{
    char line[400];

    vfmt( line, sizeof( line ), fmt, VA_START( fmt ) );
    tx_add( &log_text, line );
}

/* a note under a problem: wrapped, three spaces in */
void log_para( const char *text )
{
    TEXT lines;
    int index;

    tx_init( &lines );
    tx_wrap( &lines, text, LOG_WIDTH - 3 );
    for ( index = 0; index < lines.count; index++ ) {
        log_line( "   %s", lines.line[index] );
    }
    tx_free( &lines );
}

/* SCANDISK.LOG in the root of the drive the system started from */
int log_save( int append, char *where )
{
    u32 handle, done;
    const char *line;
    int index, rc;

    sfmt( where, 24, "%c:\\SCANDISK.LOG", 'A' + sys_boot_drive() );
    if ( append && sys_exists( where ) ) {
        rc = sys_open_rw( where, &handle );
        if ( rc == 0 ) {
            sys_seek_end( handle );
            sys_write( handle, "\r\n", 2, &done );
        }
    } else {
        rc = sys_create( where, &handle );
    }
    if ( rc != 0 ) {
        return rc;
    }
/* a heading's mark is the screen's, not the file's */
    for ( index = 0; index < log_text.count && rc == 0; index++ ) {
        line = log_text.line[index] + (log_text.line[index][0] == '\1');
        rc = sys_write( handle, line, str_len( line ), &done );
        if ( rc == 0 ) {
            rc = sys_write( handle, "\r\n", 2, &done );
        }
    }
    sys_close( handle );
    return rc;
}

/* ------------------------------------------------------------------ */
/* leaving                                                             */
/* ------------------------------------------------------------------ */

void scan_quit( int why )
{
    rt_longjmp( &quit_jmp, why );
}

/* between one piece of work and the next: has ^C been pressed, or Exit
   chosen?  The front end says, and does not come back if so. */
void scan_tick( void )
{
    fe_progress( 0, 0 );
}

/* ------------------------------------------------------------------ */
/* problems                                                            */
/* ------------------------------------------------------------------ */

static int repair_for( int choices )
{
    switch ( choices ) {
    case CH_DELETE:  return ACT_DELETE;
    case CH_SAVEDEL: return opt.nosave ? ACT_DELETE : ACT_SAVE;
    }
    return ACT_FIX;
}

int __cdecl pb_found( int kind, const char *fmt, ... )
{
    int choices = kinds[kind].choices, act = ACT_SKIP, setting;
    TEXT lines;
    int index;

    vfmt( cur_text, sizeof( cur_text ), fmt, VA_START( fmt ) );
    st.found++;
    log_line( "" );
    tx_init( &lines );
    tx_wrap( &lines, cur_text, LOG_WIDTH );
    for ( index = 0; index < lines.count; index++ ) {
        tx_add( &log_text, lines.line[index] );
    }
    tx_free( &lines );

    if ( choices == CH_NONE ) {
        log_line( "   ScanDisk cannot repair this." );
        if ( opt.mode == MODE_ASK ) {
            fe_problem( kind, cur_text );       /* to be read, and OK'd */
        } else {
            fe_outcome( -1, cur_text );
            fe_outcome( 0, "ScanDisk cannot repair this." );
        }
        return ACT_SKIP;
    }
    switch ( opt.mode ) {
    case MODE_CHECKONLY:
        fe_outcome( -1, cur_text );
        break;
    case MODE_AUTOFIX:
        fe_outcome( -1, cur_text );
        act = repair_for( choices );
        break;
    case MODE_CUSTOM:
        setting = opt.action[kinds[kind].cat];
        if ( setting == IA_PROMPT ) {
            act = fe_problem( kind, cur_text );
            break;
        }
        fe_outcome( -1, cur_text );
        if ( setting == IA_QUIT ) {
            log_line( "   SCANDISK.INI says to stop at a problem of this kind." );
            scan_quit( Q_CUSTOM );
        }
        if ( setting == IA_SKIP ) {
            break;
        }
        act = repair_for( choices );
        if ( choices == CH_SAVEDEL ) {
            act = setting == IA_DELETE ? ACT_DELETE : ACT_SAVE;
        }
        break;
    default:
        act = fe_problem( kind, cur_text );
        break;
    }
    if ( act == ACT_SKIP ) {
        log_line( "   Not repaired." );
        fe_outcome( 0, "Not repaired." );
    } else {
        undo_want();
    }
    return act;
}

void pb_fixed( void )
{
    st.fixed++;
    log_line( "   Repaired." );
    fe_outcome( 1, "Repaired." );
}

void __cdecl pb_failed( const char *fmt, ... )
{
    char why[300], line[340];

    vfmt( why, sizeof( why ), fmt, VA_START( fmt ) );
    sfmt( line, sizeof( line ), "This could not be repaired.  %s", why );
    log_para( line );
    fe_outcome( 2, line );
}
