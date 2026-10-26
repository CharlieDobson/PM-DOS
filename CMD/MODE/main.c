/*
 * MAIN.C - MODE for PM-DOS: the first word, the form it leads to, and
 * the status of everything.
 *
 * WHICH FORM A LINE IS comes from its first word, read twice as MS-DOS's
 * MODE read it (MODEPARS.ASM).  The first reading, with no colon among
 * the delimiters, finds /STATUS, a display word (80, CO80, MONO ...),
 * the glued printer forms LPT1132 and LPT180, LPTn=COMm, and "LPTn,"
 * with the columns left out.  The second, with a colon, finds COMn,
 * LPTn, PRN and CON with or without one after them, and "COM12400",
 * which is COM1 at 2400.  Anything else is an invalid parameter.
 *
 * THE WHOLE LINE IS READ BEFORE ANYTHING IS DONE, so that an error at
 * the end of it leaves the machine as it was.
 *
 * THE EXIT CODE is 1 when anything failed and 0 otherwise - 6.22's
 * rule, with 6.22's exceptions: "MODE" on its own ends with 0 whatever
 * the code page parts of it said, and so do a printer error, a screen
 * that cannot be shifted and LPTn=COMm to a port that is not there.
 */
#include "mode.h"

int failed;

static int parse_line( REQ *rq );
static void run( const REQ *rq );
static void status_all( void );

void mode_main( void )
{
    static REQ rq;

    sys_init();
    line_init( sys_cmdline() );
    rq.dmode = DM_NONE;
    rq.retry = -1;
    if ( parse_line( &rq ) == EXIT_OK ) {
        run( &rq );
    }
    sys_exit( failed ? EXIT_ERROR : EXIT_OK );
}

void say_dev( char *buf, const char *name, int unit )
{
    buf[0] = name[0];
    buf[1] = name[1];
    buf[2] = name[2];
    buf[3] = (char)('0' + unit);
    buf[4] = 0;
}

/* "COM1" to "COM4" (max 4) or "LPT1" to "LPT3": the port, or 0 */
static int dev_unit( const char *text, u32 len, const char *name, int max )
{
    if ( len == 4 && str_nicmp( text, name, 3 ) == 0 && text[3] >= '1' && text[3] <= '0' + max ) {
        return text[3] - '0';
    }
    return 0;
}

static int item_unit( const ITEM *it, const char *name, int max )
{
    if ( it->kind != IT_POS ) {
        return 0;
    }
    return dev_unit( it->text, it->len, name, max );
}

static int display_word( const ITEM *it )
{
    if ( is_word( it, "40" ) )   return DM_40;
    if ( is_word( it, "80" ) )   return DM_80;
    if ( is_word( it, "BW40" ) ) return DM_BW40;
    if ( is_word( it, "BW80" ) ) return DM_BW80;
    if ( is_word( it, "CO40" ) ) return DM_CO40;
    if ( is_word( it, "CO80" ) ) return DM_CO80;
    if ( is_word( it, "MONO" ) ) return DM_MONO;
    return DM_NONE;
}

/* a switch that is not /? or /STATUS: 6.22 called a colon in one a
   format error ("/STA:X") and anything else an invalid switch */
static int bad_switch( const ITEM *it )
{
    u32 pos;

    for ( pos = 0; pos < it->len; pos++ ) {
        if ( it->text[pos] == ':' ) {
            return err_format( it );
        }
    }
    return err_switch( it );
}

/* MODE /STATUS [device] */
static int parse_statsw( SCAN *sc, REQ *rq )
{
    ITEM it;
    int unit;

    rq->form = F_STATUS;
    rq->kind = DK_ALL;
    if ( !scan_next( sc, &it, 0 ) ) {
        return EXIT_OK;
    }
    if ( is_help( &it ) ) {
        rq->form = F_HELP;
        return EXIT_OK;
    }
    if ( it.kind == IT_SWITCH ) {
        return bad_switch( &it );
    }
    if ( (unit = item_unit( &it, "COM", 4 )) != 0 ) {
        rq->kind = DK_COM;
    } else if ( (unit = item_unit( &it, "LPT", 3 )) != 0 ) {
        rq->kind = DK_LPT;
    } else if ( is_word( &it, "PRN" ) ) {
        rq->kind = DK_LPT;
        unit = 1;
    } else if ( is_word( &it, "CON" ) ) {
        rq->kind = DK_CON;
    } else {
        return err_param( &it );
    }
    rq->unit = unit;
    if ( scan_more( sc ) ) {
        scan_next( sc, &it, 0 );
        if ( is_help( &it ) ) {
            rq->form = F_HELP;
            return EXIT_OK;
        }
        if ( it.kind == IT_SWITCH ) {
            return bad_switch( &it );
        }
        return err_count();
    }
    return EXIT_OK;
}

/* MODE PRN [/STATUS] [CP ...]: the code pages of LPT1 */
static int parse_prn( SCAN *sc, REQ *rq )
{
    ITEM it;

    rq->form = F_PRNCP;
    rq->unit = 1;
    if ( !scan_next( sc, &it, 0 ) ) {
        return EXIT_OK;
    }
    if ( is_help( &it ) ) {
        rq->form = F_HELP;
        return EXIT_OK;
    }
    if ( is_status( &it ) ) {
        if ( !scan_next( sc, &it, 0 ) ) {
            return EXIT_OK;
        }
        if ( !(is_word( &it, "CP" ) || is_word( &it, "CODE" ) || is_word( &it, "CODEPAGE" )) ) {
            return err_count();
        }
        if ( scan_more( sc ) ) {
            return err_count();
        }
        return EXIT_OK;
    }
    if ( it.kind == IT_SWITCH ) {
        return bad_switch( &it );
    }
    if ( is_word( &it, "CP" ) || is_word( &it, "CODE" ) || is_word( &it, "CODEPAGE" ) ) {
        return cp_parse( sc, rq, DK_LPT, 1 );
    }
    return err_param( &it );
}

static int parse_line( REQ *rq )
{
    SCAN sc;
    ITEM it;
    int unit, mode;
    u32 len;

    /* the first reading: no colon among the delimiters */
    scan_start( &sc );
    if ( !scan_next( &sc, &it, 0 ) ) {
        rq->form = F_STATUS;
        rq->kind = DK_ALL;
        return EXIT_OK;
    }
    if ( is_help( &it ) ) {
        rq->form = F_HELP;
        return EXIT_OK;
    }
    if ( is_status( &it ) ) {
        return parse_statsw( &sc, rq );
    }
    if ( it.kind == IT_SWITCH ) {
        return bad_switch( &it );
    }
    if ( it.kind == IT_EMPTY ) {
        return display_parse( &sc, rq, DM_NONE );
    }
    if ( (mode = display_word( &it )) != DM_NONE ) {
        return display_parse( &sc, rq, mode );
    }
    if ( it.kind == IT_POS && (it.len == 7 || it.len == 6) &&
         (unit = dev_unit( it.text, 4, "LPT", 3 )) != 0 ) {
        if ( it.len == 7 && str_nicmp( it.text + 4, "132", 3 ) == 0 ) {
            rq->unit = unit;
            rq->cols = 132;
            return lpt_parse( &sc, rq, 1 );
        }
        if ( it.len == 6 && str_nicmp( it.text + 4, "80", 2 ) == 0 ) {
            rq->unit = unit;
            rq->cols = 80;
            return lpt_parse( &sc, rq, 1 );
        }
    }
    if ( it.kind == IT_KEY ) {
        len = (u32)(it.value - it.text) - 1;        /* the name, no '=' */
        if ( len == 5 && it.text[4] == ':' ) {
            len = 4;
        }
        if ( (unit = dev_unit( it.text, len, "LPT", 3 )) != 0 ) {
            rq->unit = unit;
            return route_parse( &sc, rq, &it );
        }
    }
    if ( it.comma && (unit = item_unit( &it, "LPT", 3 )) != 0 ) {
        rq->unit = unit;
        return lpt_parse( &sc, rq, 1 );
    }

    /* the second: a colon may follow the device */
    scan_start( &sc );
    scan_next( &sc, &it, 1 );
    if ( (unit = item_unit( &it, "COM", 4 )) != 0 ) {
        rq->unit = unit;
        return com_parse( &sc, rq, NULL );
    }
    if ( (unit = item_unit( &it, "LPT", 3 )) != 0 ) {
        rq->unit = unit;
        return lpt_parse( &sc, rq, 0 );
    }
    if ( is_word( &it, "PRN" ) ) {
        return parse_prn( &sc, rq );
    }
    if ( is_word( &it, "CON" ) ) {
        return con_parse( &sc, rq );
    }

    /* COM glued to a baud rate: COM12400 is COM1 at 2400 */
    if ( it.kind == IT_POS && it.len > 4 && str_nicmp( it.text, "COM", 3 ) == 0 &&
         it.text[3] >= '1' && it.text[3] <= '4' ) {
        ITEM rest = it;

        rq->unit = it.text[3] - '0';
        rest.text = it.text + 4;
        rest.len = it.len - 4;
        return com_parse( &sc, rq, &rest );
    }
    return err_param_cut( &it );
}

static void run( const REQ *rq )
{
    switch ( rq->form ) {
    case F_HELP:
        out_text( M_HELP );
        break;
    case F_STATUS:
        switch ( rq->kind ) {
        case DK_ALL: status_all(); break;
        case DK_COM: com_status( rq->unit ); break;
        case DK_LPT: lpt_status( rq->unit ); break;
        case DK_CON: con_status(); break;
        }
        break;
    case F_COM:      com_set( rq ); break;
    case F_LPT:      lpt_set( rq ); break;
    case F_LPTRESET: lpt_reset( rq->unit ); break;
    case F_ROUTE:    lpt_route( rq ); break;
    case F_CON:      con_set( rq ); break;
    case F_DISPLAY:  display_set( rq ); break;
    case F_CP:       cp_run( rq ); break;
    case F_PRNCP:    cp_status( DK_LPT, 1 ); break;
    }
}

/* every printer, the screen and every serial port the BIOS counted */
static void status_all( void )
{
    int unit, coms;

    for ( unit = 1; unit <= 3; unit++ ) {
        lpt_status( unit );
    }
    con_status();
    failed = 0;
    coms = com_count();
    for ( unit = 1; unit <= coms; unit++ ) {
        com_status( unit );
    }
}
