/*
 * LPT.C - MODE LPTn: a printer's columns, lines per inch and RETRY=,
 * LPTn=COMm, and a printer's status.
 *
 * EVERY PRINTER COMMAND FIRST ENDS THE PRINTER'S ROUTE to a serial port
 * and says "LPTn: not rerouted", even for a port that is not there, as
 * 6.22's did.  Then the columns and the lines go to the printer as the
 * codes IBM's and Epson's printers take - DC2 for 80 columns and SI
 * for 132, ESC "2" for 6 lines to the inch and ESC "0" for 8 - and the
 * retry line follows.
 *
 * RETRY= is any of E, B, R and P, and every one of them retries a byte
 * the printer did not take for as long as it is not taken (the Break
 * key ends it): the letter only changes what a program asking for the
 * printer's status is told while it is busy.  That is why 6.22 said
 * "Infinite retry" for all four, and why its status shows P as B.  The
 * kernel does the retrying (md_lpt, DOS\INT21.INC).
 *
 * "Printer error" is a byte the BIOS says timed out, or found the
 * printer out of paper or in error.  6.22 counted "not busy" as an
 * error too, so an idle printer that had taken a byte at once was
 * reported as failed.
 */
#include "mode.h"

#define LPT_ERRBITS 0x29            /* INT 17h: out of paper, I/O error, time-out */

static int retry_word( const char *text, u32 len )
{
    if ( len == 1 ) {
        switch ( ch_upper( text[0] ) ) {
        case 'E': return RETRY_E;
        case 'B': return RETRY_B;
        case 'R': return RETRY_R;
        case 'P': return RETRY_P;
        case 'N': return RETRY_NONE;
        }
    }
    if ( len == 4 && str_nicmp( text, "NONE", 4 ) == 0 ) {
        return RETRY_NONE;
    }
    return -1;
}

static int is_cpword( const ITEM *it )
{
    return is_word( it, "CP" ) || is_word( it, "CODE" ) || is_word( it, "CODEPAGE" );
}

/* the code page words that need CP in front of them */
static int is_cpverb( const ITEM *it )
{
    return is_key( it, "PREP=" ) || is_key( it, "PREPARE=" ) || is_key( it, "SEL=" ) ||
           is_key( it, "SELECT=" ) || is_word( it, "REF" ) || is_word( it, "REFRESH" );
}

/* one keyword: COLS=, COLUMNS=, LINES= or RETRY=.  -1 if it is none of
   them; "seen" stops a spelling being given twice */
static int lpt_key( REQ *rq, const ITEM *it, int *seen )
{
    int bit, val;

    if ( it->vlen == 0 ) {
        return err_format( it );
    }
    if ( is_key( it, "COLS=" ) || is_key( it, "COLUMNS=" ) ) {
        bit = is_key( it, "COLS=" ) ? 1 : 2;
        if ( val_is( it, "80" ) ) {
            val = 80;
        } else if ( val_is( it, "132" ) ) {
            val = 132;
        } else {
            return err_param( it );
        }
        if ( *seen & bit ) {
            return err_param_none();
        }
        rq->cols = val;
    } else if ( is_key( it, "LINES=" ) ) {
        bit = 4;
        if ( val_is( it, "6" ) ) {
            val = 6;
        } else if ( val_is( it, "8" ) ) {
            val = 8;
        } else {
            return err_param( it );
        }
        if ( *seen & bit ) {
            return err_param_none();
        }
        rq->lines = val;
    } else if ( is_key( it, "RETRY=" ) ) {
        bit = 8;
        if ( (val = retry_word( it->value, it->vlen )) < 0 ) {
            return err_param( it );
        }
        if ( *seen & bit ) {
            return err_param_none();
        }
        rq->retry = val;
    } else {
        return err_param( it );
    }
    *seen |= bit;
    return EXIT_OK;
}

/* after LPTn: the status, the code pages, keywords or places.  "slot"
   is the first place still to come - 1 when the columns were glued on
   (LPT1132) or left out with a comma (LPT1,8,P) */
int lpt_parse( SCAN *sc, REQ *rq, int slot )
{
    ITEM it;
    int seen = 0, val;

    rq->form = F_LPT;
    if ( !scan_next( sc, &it, 0 ) ) {
        if ( slot == 0 ) {
            rq->form = F_LPTRESET;
        }
        return EXIT_OK;
    }
    if ( slot == 0 ) {
        if ( is_help( &it ) ) {
            rq->form = F_HELP;
            return EXIT_OK;
        }
        if ( is_status( &it ) ) {
            rq->form = F_STATUS;
            rq->kind = DK_LPT;
            if ( scan_more( sc ) ) {
                return err_count();
            }
            return EXIT_OK;
        }
        if ( is_cpword( &it ) ) {
            return cp_parse( sc, rq, DK_LPT, rq->unit );
        }
        if ( is_cpverb( &it ) ) {
            return err_count();
        }
        if ( it.kind == IT_KEY ) {
            /* the keyword form: nothing but keywords */
            rq->keyform = 1;
            for ( ;; ) {
                if ( it.kind == IT_SWITCH ) {
                    return err_switch( &it );
                }
                if ( it.kind != IT_KEY ) {
                    return err_param( &it );
                }
                if ( lpt_key( rq, &it, &seen ) != EXIT_OK ) {
                    return EXIT_ERROR;
                }
                if ( !scan_next( sc, &it, 0 ) ) {
                    return EXIT_OK;
                }
            }
        }
    }

    /* [cols][,lines][,retry] */
    for ( ;; ) {
        if ( it.kind == IT_SWITCH ) {
            if ( is_help( &it ) ) {
                rq->form = F_HELP;
                return EXIT_OK;
            }
            return err_switch( &it );
        }
        if ( slot >= 3 ) {
            return err_param( &it );
        }
        if ( it.kind == IT_KEY ) {
            /* 6.22 took "LPT1:132 LINES=8" as it looks */
            if ( lpt_key( rq, &it, &seen ) != EXIT_OK ) {
                return EXIT_ERROR;
            }
        } else if ( it.kind == IT_POS ) {
            switch ( slot ) {
            case 0:
                if ( is_word( &it, "80" ) ) {
                    rq->cols = 80;
                } else if ( is_word( &it, "132" ) ) {
                    rq->cols = 132;
                } else {
                    return err_param( &it );
                }
                break;
            case 1:
                if ( is_word( &it, "6" ) ) {
                    rq->lines = 6;
                } else if ( is_word( &it, "8" ) ) {
                    rq->lines = 8;
                } else {
                    return err_param( &it );
                }
                break;
            case 2:
                if ( is_word( &it, "OFF" ) ) {
                    val = RETRY_NONE;
                } else if ( (val = retry_word( it.text, it.len )) < 0 ) {
                    return err_param( &it );
                }
                rq->retry = val;
                break;
            }
        }
        slot++;
        if ( !scan_next( sc, &it, 0 ) ) {
            return EXIT_OK;
        }
    }
}

/* LPTn=COMm[:], and nothing after it */
int route_parse( SCAN *sc, REQ *rq, const ITEM *it )
{
    u32 len = it->vlen;

    rq->form = F_ROUTE;
    if ( len == 5 && it->value[4] == ':' ) {
        len = 4;
    }
    if ( len != 4 || str_nicmp( it->value, "COM", 3 ) != 0 ||
         it->value[3] < '1' || it->value[3] > '4' ) {
        return err_param( it );
    }
    rq->com = it->value[3] - '0';
    if ( scan_more( sc ) ) {
        return err_count();
    }
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* the ports                                                           */
/* ------------------------------------------------------------------ */

/* 6.22's status counted the slots above 2 (1 and 2 were markers of its
   own and of the network's), where its settings looked at the slot */
static int lpt_exists( int unit )
{
    int slot, count = 0;

    for ( slot = 0; slot < 3; slot++ ) {
        if ( sys_low_word( BDA_LPT1 + 2 * slot ) > 2 ) {
            count++;
        }
    }
    return count >= unit;
}

static void say_unrouted( int unit )
{
    char num[2];

    num[0] = (char)('0' + unit);
    num[1] = 0;
    sys_port_route( unit - 1, 0 );
    out_text( "\n" );
    out_line( M_NOTROUTED, num, NULL );
}

static void say_retry( int retry )
{
    out_line( M_RETRYMSG, retry != RETRY_NONE ? M_INFINITE : M_NO, NULL );
}

void lpt_set( const REQ *rq )
{
    char dev[6], num[4];
    u8 codes[4];
    int ncodes = 0, pos, status, retry;
    PORTSET set;

    say_unrouted( rq->unit );
    say_dev( dev, "LPT", rq->unit );
    if ( sys_low_word( BDA_LPT1 + 2 * (rq->unit - 1) ) == 0 ) {
        out_line( M_ILLDEV, dev, NULL );
        failed = 1;
        return;
    }

    if ( rq->cols ) {
        codes[ncodes++] = rq->cols == 132 ? 0x0F : 0x12;
    }
    if ( rq->lines ) {
        codes[ncodes++] = 0x1B;
        codes[ncodes++] = rq->lines == 8 ? '0' : '2';
    }
    if ( ncodes ) {
        for ( pos = 0; pos < ncodes; pos++ ) {
            if ( sys_lpt_out( rq->unit - 1, codes[pos], &status ) != 0 ||
                 (status & LPT_ERRBITS) != 0 ) {
                break;
            }
        }
        if ( pos < ncodes ) {
            out_line( M_PRNERR, NULL, NULL );
        } else {
            if ( rq->cols ) {
                num_str( num, (u32)rq->cols );
                out_line( M_SETFOR, dev + 3, num );
            }
            if ( rq->lines ) {
                out_line( M_LPISET, NULL, NULL );
            }
        }
    }

    sys_port_get( &set );
    retry = set.lretry[rq->unit - 1];
    if ( rq->retry >= 0 ) {
        retry = rq->retry;
    } else if ( !rq->keyform ) {
        retry = RETRY_NONE;
    }
    sys_port_retry( rq->unit - 1, retry );
    say_retry( retry );
}

/* MODE LPTn: no route and no retry */
void lpt_reset( int unit )
{
    say_unrouted( unit );
    if ( lpt_exists( unit ) ) {
        sys_port_retry( unit - 1, RETRY_NONE );
        say_retry( RETRY_NONE );
    }
}

void lpt_route( const REQ *rq )
{
    char dev[6], lnum[2], cnum[2];

    say_dev( dev, "COM", rq->com );
    if ( !com_present( rq->com ) ) {
        out_line( M_ILLDEV, dev, NULL );
        return;
    }
    sys_port_route( rq->unit - 1, rq->com );
    lnum[0] = (char)('0' + rq->unit);
    lnum[1] = 0;
    cnum[0] = (char)('0' + rq->com);
    cnum[1] = 0;
    out_text( "\n" );
    out_line( M_ROUTED, lnum, cnum );
}

void lpt_status( int unit )
{
    char dev[6], num[2], cnum[2];
    PORTSET set;
    int retry;

    say_dev( dev, "LPT", unit );
    num[0] = (char)('0' + unit);
    num[1] = 0;
    out_line( M_STATFOR, dev, NULL );
    out_line( M_DASH5, NULL, NULL );
    sys_port_get( &set );
    if ( set.route[unit - 1] ) {
        cnum[0] = (char)('0' + set.route[unit - 1]);
        cnum[1] = 0;
        out_line( M_ROUTED, num, cnum );
    } else {
        out_line( M_NOTROUTED, num, NULL );
    }
    if ( !lpt_exists( unit ) ) {
        return;
    }
    retry = set.lretry[unit - 1];
    switch ( retry ) {
    case RETRY_E: out_line( M_RETRY, "E", NULL ); break;
    case RETRY_R: out_line( M_RETRY, "R", NULL ); break;
    case RETRY_B:
    case RETRY_P: out_line( M_RETRY, "B", NULL ); break;
    default:      out_line( M_RETRY, M_NONE, NULL ); break;
    }
    cp_status( DK_LPT, unit );
}
