/*
 * COM.C - MODE COMm: a serial port's speed, format and RETRY=, and its
 * status.
 *
 * TWO FORMS, as 6.22 had them.  Positions - "COM1 96,N,8,1,P" - where
 * any field after the speed may be empty, and keywords - "COM1 BAUD=96
 * DATA=8" - in any order, each once.  What is left out is not kept from
 * before: parity is even, 7 data bits and 1 stop bit unless said (2 at
 * 110 baud), as 6.22 set them.  The retry is cleared by the positional
 * form without one, and kept by the keyword form without RETRY=.
 *
 * THE PORT IS WRITTEN DIRECTLY: the divisor latch for the speed and the
 * line control register for the rest, as the BIOS's INT 14h AH=00h does
 * it.  So 19200 works on every machine (6.22 needed a PS/2 BIOS for it,
 * and said "Function not supported on this computer" elsewhere), and so
 * do mark and space parity, 5 and 6 data bits and 1.5 stop bits, which
 * 6.22's positional form showed and then set otherwise.  38400, 57600
 * and 115200 are PM-DOS's own, for the chips that have always had them.
 *
 * WHAT THE LINE SAYS AFTERWARDS is 6.22's: "COM1: 9600,n,8,1,-", the
 * speed whole, the parity in small letters, and a stop of 1.5 shown as
 * 2 above 5 data bits and a stop of 2 shown as 1.5 at 5 - which is what
 * the chip does with them.  1.5 with no data bits given means 5.
 *
 * PAR= is PARITY=.  6.22 took it and then looked only for PARITY=, so
 * PAR=N gave even parity.
 */
#include "mode.h"

typedef struct {
    const char *word;
    u32         baud;
} BAUD;

static const BAUD bauds[] = {
    { "110", 110 },     { "11", 110 },
    { "150", 150 },     { "15", 150 },
    { "300", 300 },     { "30", 300 },
    { "600", 600 },     { "60", 600 },
    { "1200", 1200 },   { "12", 1200 },
    { "2400", 2400 },   { "24", 2400 },
    { "4800", 4800 },   { "48", 4800 },
    { "9600", 9600 },   { "96", 9600 },
    { "19200", 19200 }, { "19", 19200 }, { "19.2", 19200 }, { "19.2K", 19200 },
    { "38400", 38400 }, { "57600", 57600 }, { "115200", 115200UL },
    { NULL, 0 }
};

static int text_is( const char *text, u32 len, const char *word )
{
    return str_len( word ) == len && str_nicmp( text, word, len ) == 0;
}

static u32 baud_of( const char *text, u32 len )
{
    const BAUD *row;

    for ( row = bauds; row->word; row++ ) {
        if ( text_is( text, len, row->word ) ) {
            return row->baud;
        }
    }
    return 0;
}

static int parity_of( const char *text, u32 len )
{
    if ( text_is( text, len, "N" ) || text_is( text, len, "NONE" ) )  return 'n';
    if ( text_is( text, len, "E" ) || text_is( text, len, "EVEN" ) )  return 'e';
    if ( text_is( text, len, "O" ) || text_is( text, len, "ODD" ) )   return 'o';
    if ( text_is( text, len, "M" ) || text_is( text, len, "MARK" ) )  return 'm';
    if ( text_is( text, len, "S" ) || text_is( text, len, "SPACE" ) ) return 's';
    return 0;
}

static int data_of( const char *text, u32 len )
{
    if ( len == 1 && text[0] >= '5' && text[0] <= '8' ) {
        return text[0] - '0';
    }
    return 0;
}

static int stop_of( const char *text, u32 len )
{
    if ( text_is( text, len, "1" ) )   return 1;
    if ( text_is( text, len, "1.5" ) ) return 15;
    if ( text_is( text, len, "2" ) )   return 2;
    return 0;
}

/* RETRY_*, or -1 for a word that is none of them */
static int retry_of( const char *text, u32 len )
{
    if ( text_is( text, len, "E" ) )    return RETRY_E;
    if ( text_is( text, len, "B" ) )    return RETRY_B;
    if ( text_is( text, len, "R" ) )    return RETRY_R;
    if ( text_is( text, len, "P" ) )    return RETRY_P;
    if ( text_is( text, len, "N" ) || text_is( text, len, "NONE" ) ) return RETRY_NONE;
    return -1;
}

/* the keyword form, from the first keyword on */
static int parse_keys( SCAN *sc, REQ *rq, ITEM *it )
{
    int seen = 0, bit, val;

    for ( ;; ) {
        if ( it->kind == IT_SWITCH ) {
            return err_switch( it );
        }
        if ( it->kind != IT_KEY ) {
            return err_param( it );
        }
        if ( it->vlen == 0 ) {
            return err_format( it );
        }
        if ( is_key( it, "BAUD=" ) ) {
            bit = 1;
            if ( (rq->baud = baud_of( it->value, it->vlen )) == 0 ) {
                return err_param( it );
            }
        } else if ( is_key( it, "PARITY=" ) || is_key( it, "PAR=" ) ) {
            bit = is_key( it, "PAR=" ) ? 32 : 2;
            if ( (rq->parity = parity_of( it->value, it->vlen )) == 0 ) {
                return err_param( it );
            }
        } else if ( is_key( it, "DATA=" ) ) {
            bit = 4;
            if ( (rq->data = data_of( it->value, it->vlen )) == 0 ) {
                return err_param( it );
            }
        } else if ( is_key( it, "STOP=" ) ) {
            bit = 8;
            if ( (rq->stop = stop_of( it->value, it->vlen )) == 0 ) {
                return err_param( it );
            }
        } else if ( is_key( it, "RETRY=" ) ) {
            bit = 16;
            if ( (val = retry_of( it->value, it->vlen )) < 0 ) {
                return err_param( it );
            }
            rq->retry = val;
        } else {
            return err_param( it );
        }
        if ( seen & bit ) {
            return err_param( it );         /* each spelling once */
        }
        seen |= bit;
        if ( !scan_next( sc, it, 0 ) ) {
            break;
        }
    }
    if ( rq->baud == 0 ) {
        return err_line( M_BAUDREQ );
    }
    return EXIT_OK;
}

/* baud[,parity[,data[,stop[,retry]]]]; "first" is the speed when it was
   glued to the device name (COM12400) */
static int parse_places( SCAN *sc, REQ *rq, ITEM *it )
{
    int slot = 0, val;

    for ( ;; ) {
        if ( it->kind == IT_SWITCH ) {
            if ( is_help( it ) ) {
                rq->form = F_HELP;
                return EXIT_OK;
            }
            if ( !is_status( it ) ) {
                return err_switch( it );
            }
            /* 6.22 let a /STATUS after the fields go by */
        } else if ( slot >= 5 ) {
            return err_param( it );
        } else {
            if ( it->kind == IT_EMPTY ) {
                if ( slot == 0 ) {
                    return err_param( it );     /* the speed has to be there */
                }
            } else if ( it->kind == IT_KEY ) {
                /* a keyword among the places takes one and does nothing,
                   as in 6.22: "COM1 9600 DATA=8" is 9600,e,7,1 */
                if ( slot == 0 ) {
                    return err_param( it );
                }
            } else {
                switch ( slot ) {
                case 0:
                    if ( (rq->baud = baud_of( it->text, it->len )) == 0 ) {
                        return err_param( it );
                    }
                    break;
                case 1:
                    if ( (rq->parity = parity_of( it->text, it->len )) == 0 ) {
                        return err_param( it );
                    }
                    break;
                case 2:
                    if ( (rq->data = data_of( it->text, it->len )) == 0 ) {
                        return err_param( it );
                    }
                    break;
                case 3:
                    if ( (rq->stop = stop_of( it->text, it->len )) == 0 ) {
                        return err_param( it );
                    }
                    break;
                case 4:
                    if ( (val = retry_of( it->text, it->len )) < 0 ) {
                        return err_param( it );
                    }
                    rq->retry = val;
                    break;
                }
            }
            slot++;
        }
        if ( !scan_next( sc, it, 0 ) ) {
            break;
        }
    }
    return EXIT_OK;
}

int com_parse( SCAN *sc, REQ *rq, const ITEM *first )
{
    ITEM it;

    rq->form = F_COM;
    if ( first ) {
        it = *first;
        return parse_places( sc, rq, &it );
    }
    if ( !scan_next( sc, &it, 0 ) ) {
        rq->form = F_STATUS;
        rq->kind = DK_COM;
        return EXIT_OK;
    }
    if ( is_help( &it ) ) {
        rq->form = F_HELP;
        return EXIT_OK;
    }
    if ( is_status( &it ) ) {
        rq->form = F_STATUS;
        rq->kind = DK_COM;
        if ( scan_more( sc ) ) {
            return err_count();
        }
        return EXIT_OK;
    }
    if ( it.kind == IT_SWITCH ) {
        return err_switch( &it );
    }
    if ( it.kind == IT_KEY ) {
        rq->keyform = 1;
        return parse_keys( sc, rq, &it );
    }
    return parse_places( sc, rq, &it );
}

/* ------------------------------------------------------------------ */
/* the ports                                                           */
/* ------------------------------------------------------------------ */

/* how many the BIOS lists, counted as 6.22 counted them for its status:
   the slots that are not zero */
int com_count( void )
{
    int unit, count = 0;

    for ( unit = 1; unit <= 4; unit++ ) {
        if ( sys_low_word( BDA_COM1 + 2 * (unit - 1) ) != 0 ) {
            count++;
        }
    }
    return count;
}

int com_present( int unit )
{
    return sys_low_word( BDA_COM1 + 2 * (unit - 1) ) != 0;
}

static char retry_letter( int retry, int shown_p )
{
    switch ( retry ) {
    case RETRY_E: return 'e';
    case RETRY_B: return 'b';
    case RETRY_R: return 'r';
    case RETRY_P: return shown_p ? 'p' : 'b';
    }
    return '-';
}

static void put_num( char **out, u32 num )
{
    *out += num_str( *out, num );
}

void com_set( const REQ *rq )
{
    char dev[6], line[40], *out;
    u32 base, divisor;
    int parity, data, stop, retry, shown_p, lcr;
    PORTSET set;

    say_dev( dev, "COM", rq->unit );
    if ( rq->baud == 0 ) {
        return;                         /* the parser sees that it is there */
    }
    base = sys_low_word( BDA_COM1 + 2 * (rq->unit - 1) );
    if ( base == 0 ) {
        out_line( M_ILLDEV, dev, NULL );
        failed = 1;
        return;
    }

    parity = rq->parity ? rq->parity : 'e';
    data = rq->data;
    stop = rq->stop;
    if ( stop == 0 ) {
        stop = rq->baud == 110 ? 2 : 1;
    }
    if ( data == 0 ) {
        data = stop == 15 ? 5 : 7;
    }

    /* the chip: DLAB on, the divisor, then the format with DLAB off */
    lcr = data - 5;
    if ( stop != 1 ) {
        lcr |= 0x04;                    /* 1.5 at 5 data bits, else 2 */
    }
    switch ( parity ) {
    case 'o': lcr |= 0x08; break;
    case 'e': lcr |= 0x18; break;
    case 'm': lcr |= 0x28; break;
    case 's': lcr |= 0x38; break;
    }
    divisor = 115200UL / rq->baud;
    port_out( base + 3, 0x80 | (u32)lcr );
    port_out( base + 0, divisor & 0xFF );
    port_out( base + 1, (divisor >> 8) & 0xFF );
    port_out( base + 3, (u32)lcr );

    /* RETRY=: given, it is set; left out, the positional form clears it
       and the keyword form keeps it */
    sys_port_get( &set );
    retry = set.cretry[rq->unit - 1];
    shown_p = 0;
    if ( rq->retry >= 0 ) {
        retry = rq->retry;
        shown_p = 1;
    } else if ( !rq->keyform ) {
        retry = RETRY_NONE;
    }
    sys_port_retry( 4 + rq->unit - 1, retry );

    /* "9600,n,8,1,-" */
    out = line;
    put_num( &out, rq->baud );
    *out++ = ',';
    *out++ = (char)parity;
    *out++ = ',';
    *out++ = (char)('0' + data);
    *out++ = ',';
    if ( stop == 1 ) {
        *out++ = '1';
    } else if ( data == 5 ) {
        *out++ = '1';
        *out++ = '.';
        *out++ = '5';
    } else {
        *out++ = '2';
    }
    *out++ = ',';
    *out++ = retry_letter( retry, shown_p );
    *out = 0;
    out_line( M_COMSET, dev + 3, line );
}

void com_status( int unit )
{
    char dev[6], letter[2];
    PORTSET set;
    int retry;

    say_dev( dev, "COM", unit );
    if ( com_count() < unit ) {
        out_line( M_ILLDEV, dev, NULL );
        failed = 1;
        return;
    }
    out_line( M_STATFOR, dev, NULL );
    out_line( M_DASH5, NULL, NULL );
    sys_port_get( &set );
    retry = set.cretry[unit - 1];
    if ( retry == RETRY_NONE ) {
        out_line( M_RETRY, M_NONE, NULL );
    } else {
        letter[0] = (char)ch_upper( retry_letter( retry, 0 ) );
        letter[1] = 0;
        out_line( M_RETRY, letter, NULL );
    }
}
