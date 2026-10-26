/*
 * CP.C - MODE device CP: code page status, SELECT=, REFRESH and
 * PREPARE=, for CON, LPT1-3 and PRN.
 *
 * ALL OF IT IS THE DEVICE DRIVER'S WORK, asked for with the generic
 * IOCTL (440Ch: category 3 for CON, 5 for a printer) as MS-DOS's MODE
 * asked DISPLAY.SYS and PRINTER.SYS - status 6Ah and 6Bh, select 4Ah,
 * and a prepare or a refresh as 4Ch, the font file written with 4403h,
 * and 4Dh.  PM-DOS has neither driver, and its console and printers
 * answer "invalid function", so what MODE says here is what 6.22 said
 * on a machine without them: "Code page operation not supported on
 * this device", "Device error during select".  A driver that does
 * answer will be driven as MS-DOS's were.
 *
 * Breaking is off while a device is in the middle of it, as in 6.22.
 */
#include "mode.h"

#define CAT_CON     0x03
#define CAT_LPT     0x05

#define ERR_REPLACED    19      /* the driver's 0: a prepare replaced one */
#define ERR_BADSTRUCT   22      /* unknown command                       */
#define ERR_NOTPREP     26      /* 7: the code page is not prepared      */
#define ERR_KEYB        27      /* 8: the keyboard cannot take it        */
#define ERR_DEVICE      29      /* 0Ah                                   */
#define ERR_NOREFRESH   31      /* 0Ch                                   */

#define CHUNK       4096

static int not_supported( int rc )
{
    return rc == 1 || rc == ERR_BADSTRUCT;
}

/* a number from 0 to 999, for SEL= and the list in PREP= */
static int cp_num( const char *text, u32 len, u32 *num )
{
    u32 pos, val = 0;

    if ( len == 0 || len > 5 ) {
        return 1;
    }
    for ( pos = 0; pos < len; pos++ ) {
        if ( text[pos] < '0' || text[pos] > '9' ) {
            return 1;
        }
        val = val * 10 + (u32)(text[pos] - '0');
    }
    *num = val;
    return 0;
}

static int skip_blanks( const char **pos )
{
    while ( **pos == ' ' || **pos == '\t' ) {
        (*pos)++;
    }
    return **pos;
}

/* PREP=((cp[,cp...]) [d:path\file]), read from the line itself: the
   blank between the list and the file is part of the value */
static int prep_parse( REQ *rq, const ITEM *it )
{
    const char *pos = it->value, *start;
    char num[8];
    u32 val, len;

    if ( *pos != '(' || pos[1] != '(' ) {
        return err_count();
    }
    pos += 2;
    for ( ;; ) {
        if ( skip_blanks( &pos ) == ')' ) {
            pos++;
            break;
        }
        if ( rq->ncp >= CP_MAX ) {
            return err_count();
        }
        if ( *pos == ',' ) {
            rq->cplist[rq->ncp++] = -1;     /* an empty place */
        } else {
            start = pos;
            while ( *pos >= '0' && *pos <= '9' ) {
                pos++;
            }
            len = (u32)(pos - start);
            if ( len == 0 ) {
                return err_count();
            }
            if ( cp_num( start, len, &val ) || val > 999 ) {
                if ( len > sizeof( num ) - 1 ) {
                    len = sizeof( num ) - 1;
                }
                mem_cpy( num, start, len );
                num[len] = 0;
                return err_param_str( num );
            }
            rq->cplist[rq->ncp++] = (int)val;
        }
        skip_blanks( &pos );
        if ( *pos == ',' ) {
            pos++;
            continue;
        }
        if ( *pos == ')' ) {
            pos++;
            break;
        }
        return err_count();
    }

    /* the file, if there is one, in capitals */
    skip_blanks( &pos );
    len = 0;
    while ( *pos && *pos != ')' && *pos != ' ' && *pos != '\t' ) {
        if ( len + 1 >= CPFILE_MAX ) {
            return err_count();
        }
        rq->cpfile[len++] = (char)ch_upper( *pos++ );
    }
    rq->cpfile[len] = 0;
    if ( skip_blanks( &pos ) != ')' ) {
        return err_count();
    }
    pos++;
    if ( skip_blanks( &pos ) != 0 ) {
        return err_count();
    }
    return EXIT_OK;
}

int cp_parse( SCAN *sc, REQ *rq, int kind, int unit )
{
    ITEM it;
    u32 val;

    rq->form = F_CP;
    rq->kind = kind;
    rq->unit = unit;
    rq->cpop = CP_STATUS;
    if ( !scan_next( sc, &it, 0 ) ) {
        return EXIT_OK;
    }
    if ( is_help( &it ) ) {
        rq->form = F_HELP;
        return EXIT_OK;
    }
    if ( is_status( &it ) ) {
        if ( scan_more( sc ) ) {
            return err_count();
        }
        return EXIT_OK;
    }
    if ( it.kind == IT_SWITCH ) {
        return err_switch( &it );
    }
    if ( is_word( &it, "REF" ) || is_word( &it, "REFRESH" ) ) {
        rq->cpop = CP_REFRESH;
    } else if ( is_key( &it, "SEL=" ) || is_key( &it, "SELECT=" ) ) {
        if ( it.vlen == 0 ) {
            return err_format( &it );
        }
        if ( cp_num( it.value, it.vlen, &val ) ) {
            return err_format( &it );
        }
        if ( val > 999 ) {
            return err_param( &it );
        }
        rq->cpop = CP_SELECT;
        rq->cplist[0] = (int)val;
    } else if ( is_key( &it, "PREP=" ) || is_key( &it, "PREPARE=" ) ) {
        rq->cpop = CP_PREPARE;
        return prep_parse( rq, &it );
    } else {
        return err_param( &it );
    }
    if ( scan_more( sc ) ) {
        return err_count();
    }
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* the device                                                          */
/* ------------------------------------------------------------------ */

static void dev_name( char *buf, int kind, int unit )
{
    if ( kind == DK_CON ) {
        str_cpyn( buf, "CON", 6 );
    } else {
        say_dev( buf, "LPT", unit );
    }
}

/* open it and see that it is a device: the handle, or -1 with the
   reason given */
static int dev_open( const char *name )
{
    int handle;
    u16 info;

    if ( sys_open( name, 2, &handle ) != 0 ) {
        out_line( M_CPDEV, name, NULL );
        failed = 1;
        return -1;
    }
    if ( sys_devinfo( handle, &info ) != 0 || !(info & 0x80) ) {
        sys_close( handle );
        out_line( M_CPNOTSUPP, NULL, NULL );
        failed = 1;
        return -1;
    }
    return handle;
}

static void say_cp( const char *tmpl, int cp )
{
    char num[8];
    u32 len;

    if ( cp < 0 || cp == 0xFFFF ) {
        out_line( M_CPUNPREP, NULL, NULL );
        return;
    }
    len = num_str( num + 2, (u32)cp );
    /* three places wide */
    num[0] = ' ';
    num[1] = ' ';
    out_line( tmpl, num + (len >= 3 ? 2 : len == 2 ? 1 : 0), NULL );
}

/* "Active code page", then the hardware and prepared lists */
void cp_status( int kind, int unit )
{
    char name[8], num[8];
    u16 packet[2 + 2 * (CP_MAX + 1)];
    int handle, rc, cat, n, count, used = 0, problem = 0, list;
    int brk;

    dev_name( name, kind, unit );
    brk = sys_break( 1, 0 );
    handle = dev_open( name );
    if ( handle < 0 ) {
        sys_break( 1, brk );
        return;
    }
    cat = kind == DK_CON ? CAT_CON : CAT_LPT;

    mem_set( packet, 0, sizeof( packet ) );
    packet[0] = 2;
    rc = sys_genioctl( handle, cat, 0x6A, packet );
    if ( rc == ERR_NOTPREP ) {
        out_line( M_CPNOSEL, NULL, NULL );
    } else if ( not_supported( rc ) ) {
        out_line( M_CPNOTSUPP, NULL, NULL );
        failed = 1;
        goto done;
    } else if ( rc != 0 ) {
        goto done;                  /* 6.22 said nothing here */
    } else {
        num_str( num, packet[1] );
        out_line( M_CPACTIVE, name, num );
    }

    mem_set( packet, 0, sizeof( packet ) );
    packet[0] = (u16)(sizeof( packet ) - 2);
    rc = sys_genioctl( handle, cat, 0x6B, packet );
    if ( not_supported( rc ) ) {
        out_line( M_CPNOTSUPP, NULL, NULL );
        failed = 1;
        goto done;
    }
    if ( rc != 0 && rc != ERR_NOTPREP ) {
        goto done;
    }
    /* length, then two lists of a count and the pages */
    n = 1;
    for ( list = 0; list < 2; list++ ) {
        count = packet[n++];
        if ( count > CP_MAX ) {
            count = CP_MAX;
        }
        out_line( M_CPLIST, list == 0 ? M_HARDWARE : M_PREPARED, NULL );
        while ( count-- > 0 ) {
            if ( packet[n] != 0xFFFF ) {
                used = 1;
            }
            say_cp( M_CPENTRY, packet[n++] );
        }
    }
    if ( !used ) {
        out_line( M_CPDEVNOTPREP, name, NULL );
        problem = 1;
    }
    if ( !problem ) {
        out_line( M_CPDONE, M_W_STATUS, NULL );
    }
done:
    sys_close( handle );
    sys_break( 1, brk );
}

/* the errors of a 4Ch that begins a prepare or a refresh */
static void start_error( int rc, const char *word )
{
    if ( rc == ERR_DEVICE ) {
        out_line( M_CPDEVERR, word, NULL );
    } else if ( rc == ERR_NOREFRESH && word == M_W_REFRESH ) {
        out_line( M_CPNOREFRESH, NULL, NULL );
    } else if ( rc == ERR_REPLACED ) {
        out_line( M_CPREPLACED, NULL, NULL );
    } else if ( not_supported( rc ) ) {
        out_line( M_CPNOTSUPP, NULL, NULL );
    } else {
        out_line( M_CPKEYB, NULL, NULL );
    }
    failed = 1;
}

/* the font file, through IOCTL writes, in binary */
static int send_file( int handle, int file )
{
    u8 *buf;
    u32 got;
    u16 info;
    int rc;

    buf = (u8 *)sys_mem_alloc( CHUNK );
    if ( buf == NULL ) {
        out_raw( H_ERR, M_NOMEM, str_len( M_NOMEM ) );
        out_raw( H_ERR, "\r\n", 2 );
        failed = 1;
        return 1;
    }
    if ( sys_devinfo( handle, &info ) == 0 ) {
        sys_setinfo( handle, (u16)((info | 0x20) & 0xFF) );
    }
    for ( ;; ) {
        if ( sys_read( file, buf, CHUNK, &got ) != 0 ) {
            out_line( M_CPREAD, NULL, NULL );
            failed = 1;
            return 1;
        }
        if ( got == 0 ) {
            return 0;
        }
        rc = sys_ioctl_write( handle, buf, got );
        if ( rc != 0 ) {
            if ( rc == ERR_KEYB ) {
                out_line( M_CPMISSING, NULL, NULL );
            } else if ( rc == ERR_DEVICE ) {
                out_line( M_CPDEVERR, M_W_WRITE, NULL );
            } else {
                out_line( M_CPBADFILE, NULL, NULL );
            }
            failed = 1;
            return 1;
        }
    }
}

void cp_run( const REQ *rq )
{
    char name[8];
    u16 packet[3 + CP_MAX];
    int handle, file = -1, rc, cat, n, brk;
    const char *word;

    if ( rq->cpop == CP_STATUS ) {
        cp_status( rq->kind, rq->unit );
        return;
    }
    dev_name( name, rq->kind, rq->unit );
    cat = rq->kind == DK_CON ? CAT_CON : CAT_LPT;

    /* the font file first: 6.22 looked for it before the device */
    if ( rq->cpop == CP_PREPARE && rq->cpfile[0] ) {
        if ( sys_open( rq->cpfile, 0, &file ) != 0 ) {
            out_line( M_CPFILE, NULL, NULL );
            failed = 1;
            return;
        }
    }

    brk = sys_break( 1, 0 );
    handle = dev_open( name );
    if ( handle < 0 ) {
        goto out;
    }

    if ( rq->cpop == CP_SELECT ) {
        packet[0] = 2;
        packet[1] = (u16)rq->cplist[0];
        rc = sys_genioctl( handle, cat, 0x4A, packet );
        if ( rc == ERR_NOTPREP ) {
            out_line( M_CPNOTPREP, NULL, NULL );
            failed = 1;
        } else if ( rc == ERR_KEYB ) {
            out_line( M_CPKEYB, NULL, NULL );
            failed = 1;
        } else if ( rc != 0 ) {
            out_line( M_CPDEVERR, M_W_SELECT, NULL );
            failed = 1;
        } else {
            out_line( M_CPDONE, M_W_SELECT, NULL );
        }
        goto close;
    }

    /* a prepare, or a refresh, which is a prepare of nothing */
    word = rq->cpop == CP_REFRESH ? M_W_REFRESH : M_W_PREPARE;
    mem_set( packet, 0, sizeof( packet ) );
    if ( rq->cpop == CP_PREPARE ) {
        packet[0] = (u16)(file < 0 ? 1 : 0);        /* 1: a cartridge */
        packet[1] = (u16)(2 + 2 * rq->ncp);
        packet[2] = (u16)rq->ncp;
        for ( n = 0; n < rq->ncp; n++ ) {
            packet[3 + n] = (u16)rq->cplist[n];
        }
    } else {
        packet[1] = 2;
    }
    rc = sys_genioctl( handle, cat, 0x4C, packet );
    if ( rc != 0 ) {
        start_error( rc, word );
        goto close;
    }
    if ( file >= 0 && send_file( handle, file ) != 0 ) {
        goto close;
    }
    rc = sys_genioctl( handle, cat, 0x4D, packet );
    if ( rc != 0 ) {
        out_line( M_CPDEVERR, word, NULL );
        failed = 1;
    } else {
        out_line( M_CPDONE, word, NULL );
    }
close:
    sys_close( handle );
out:
    sys_break( 1, brk );
    if ( file >= 0 ) {
        sys_close( file );
    }
}
