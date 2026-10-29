/*
 * CONS.C - MODE CON and the display words: the screen's columns and
 * lines, the adapter, the keyboard's repeat, and the screen's status.
 *
 * WITH ANSI.SYS LOADED, the columns and lines go through it as they
 * did on MS-DOS: the generic IOCTL on CON, 440Ch with CL = 7Fh to read
 * the display's mode and 5Fh to set it, so that the driver knows the
 * screen it is drawing on.
 *
 * WITHOUT IT, MS-DOS's MODE could change the columns (a BIOS mode set)
 * but not the lines: "ANSI.SYS must be installed to perform requested
 * function".  PM-DOS's console follows whatever mode the screen is in,
 * so here the kernel sets the lines as ANSI.SYS would have - the scan
 * lines first on a VGA, the 8x8 font after the mode for 43 or 50
 * (21h/F0h AL=0Ch) - and MODE CON's status shows the columns and lines
 * whether or not the driver is there.
 *
 * THE DISPLAY WORDS: BW40, BW80, CO40, CO80 and MONO set the equipment
 * word's display bits and that BIOS mode - the way to move between a
 * colour and a monochrome card in one machine - if the machine has
 * such a card.  40 and 80 change the columns and keep the rest.
 *
 * SHIFTING THE SCREEN (,R and ,L) moved a CGA's picture by rewriting
 * the BIOS's video parameters in memory MODE left resident.  An EGA or
 * VGA on a colour screen, or any mode from 7 up, could not be moved,
 * and 6.22 said so; on PM-DOS nothing is left resident to hold the
 * table, so every machine gets that answer, and errorlevel 0 with it.
 */
#include "mode.h"

/* the adapters, as ANSI.SYS tells them apart (CMD\ANSI) */
#define VC_CGA      0x01
#define VC_MDA      0x02
#define VC_EGAC     0x04
#define VC_EGAM     0x08
#define VC_MCGA     0x10
#define VC_VGA      0x20

/* 440Ch's display block, CL = 7Fh and 5Fh */
typedef struct {
    u8  level;                  /* 0                                    */
    u8  res1;
    u16 size;                   /* 14, the bytes after this             */
    u16 flags;                  /* 1 = background intensity, not blink  */
    u8  kind;                   /* 1 text, 2 graphics                   */
    u8  res2;
    u16 colours;                /* 16, or 0 for mono                    */
    u16 width, height;          /* pixels, FFFFh in text                */
    u16 cols, rows;
} DISPINFO;

#define GE_NOTSUPP  29          /* the driver's 0Ah: not on this adapter */
#define GE_NOFONT   31          /* its 0Ch: the font is not loaded       */

/* ------------------------------------------------------------------ */
/* reading the line                                                    */
/* ------------------------------------------------------------------ */

static int is_cpword( const ITEM *it )
{
    return is_word( it, "CP" ) || is_word( it, "CODE" ) || is_word( it, "CODEPAGE" );
}

/* a number keyword: 6.22 called a value that was not a number a format
   error and one out of range an invalid parameter */
static int key_num( const ITEM *it, u32 lo, u32 hi, int *out )
{
    u32 num;

    if ( val_num( it, &num ) ) {
        return err_format( it );
    }
    if ( num < lo || num > hi ) {
        return err_param( it );
    }
    *out = (int)num;
    return EXIT_OK;
}

int con_parse( SCAN *sc, REQ *rq )
{
    ITEM it;
    int seen = 0, bit, ratedelay = 0;

    rq->form = F_CON;
    if ( !scan_next( sc, &it, 0 ) ) {
        rq->form = F_STATUS;
        rq->kind = DK_CON;
        return EXIT_OK;
    }
    if ( is_help( &it ) ) {
        rq->form = F_HELP;
        return EXIT_OK;
    }
    if ( is_status( &it ) ) {
        rq->form = F_STATUS;
        rq->kind = DK_CON;
        if ( scan_more( sc ) ) {
            return err_count();
        }
        return EXIT_OK;
    }
    if ( is_cpword( &it ) ) {
        return cp_parse( sc, rq, DK_CON, 0 );
    }

    for ( ;; ) {
        if ( it.kind == IT_SWITCH ) {
            return err_switch( &it );
        }
        if ( is_key( &it, "SEL=" ) || is_key( &it, "SELECT=" ) || is_key( &it, "PREP=" ) ||
             is_key( &it, "PREPARE=" ) || is_word( &it, "REF" ) || is_word( &it, "REFRESH" ) ) {
            return err_count();
        }
        if ( it.kind != IT_KEY ) {
            return err_param( &it );
        }
        if ( it.vlen == 0 ) {
            return err_format( &it );
        }
        if ( is_key( &it, "COLS=" ) || is_key( &it, "COLUMNS=" ) ) {
            bit = is_key( &it, "COLS=" ) ? 1 : 2;
            if ( val_is( &it, "40" ) ) {
                rq->ccols = 40;
            } else if ( val_is( &it, "80" ) ) {
                rq->ccols = 80;
            } else {
                return err_param( &it );
            }
        } else if ( is_key( &it, "LINES=" ) ) {
            bit = 4;
            if ( val_is( &it, "25" ) ) {
                rq->crows = 25;
            } else if ( val_is( &it, "43" ) ) {
                rq->crows = 43;
            } else if ( val_is( &it, "50" ) ) {
                rq->crows = 50;
            } else {
                return err_param( &it );
            }
        } else if ( is_key( &it, "RATE=" ) ) {
            bit = 8;
            ratedelay++;
            if ( key_num( &it, 1, 32, &rq->rate ) != EXIT_OK ) {
                return EXIT_ERROR;
            }
        } else if ( is_key( &it, "DELAY=" ) || is_key( &it, "DEL=" ) ) {
            bit = is_key( &it, "DEL=" ) ? 16 : 32;
            ratedelay++;
            if ( key_num( &it, 1, 4, &rq->delay ) != EXIT_OK ) {
                return EXIT_ERROR;
            }
        } else {
            return err_param( &it );
        }
        if ( seen & bit ) {
            return err_param_none();
        }
        seen |= bit;
        if ( !scan_next( sc, &it, 0 ) ) {
            break;
        }
    }
    /* RATE= and DELAY= together or not at all, as 6.22 counted them */
    if ( ratedelay != 0 && ratedelay != 2 ) {
        return err_line( M_RATEDELAY );
    }
    if ( ratedelay == 2 && (rq->rate == 0 || rq->delay == 0) ) {
        return err_line( M_RATEDELAY );
    }
    return EXIT_OK;
}

/* [mode][,R|L[,T]] or [mode][,n] - the first word is read already */
int display_parse( SCAN *sc, REQ *rq, int mode )
{
    ITEM it;

    rq->form = F_DISPLAY;
    rq->dmode = mode;
    if ( !scan_next( sc, &it, 0 ) ) {
        if ( mode == DM_NONE ) {
            return err_count();             /* "MODE ," */
        }
        return EXIT_OK;
    }
    if ( it.kind == IT_SWITCH ) {
        return err_switch( &it );
    }
    if ( it.kind == IT_EMPTY ) {
        return err_count();                 /* "80,,T" */
    }
    if ( is_word( &it, "R" ) || is_word( &it, "L" ) ) {
        rq->shift = ch_upper( it.text[0] );
        if ( scan_next( sc, &it, 0 ) ) {
            if ( it.kind == IT_SWITCH ) {
                return err_switch( &it );
            }
            if ( !is_word( &it, "T" ) ) {
                return err_param( &it );
            }
            rq->test = 1;
            if ( scan_more( sc ) ) {
                return err_count();
            }
        }
        return EXIT_OK;
    }
    if ( is_word( &it, "25" ) ) {
        rq->crows = 25;
    } else if ( is_word( &it, "43" ) ) {
        rq->crows = 43;
    } else if ( is_word( &it, "50" ) ) {
        rq->crows = 50;
    } else {
        return err_param( &it );
    }
    if ( scan_more( sc ) ) {
        return err_count();
    }
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* ANSI.SYS                                                            */
/* ------------------------------------------------------------------ */

/* 440Ch on CON: 0, or the error the driver gave */
static int con_ioctl( int func, DISPINFO *info )
{
    int handle, rc;

    if ( sys_open( "CON", 2, &handle ) != 0 ) {
        return 1;
    }
    rc = sys_genioctl( handle, 3, func, info );
    sys_close( handle );
    return rc;
}

/* the display as ANSI.SYS has it: 1 if the driver is there */
static int ansi_get( DISPINFO *info )
{
    mem_set( info, 0, sizeof( *info ) );
    info->size = 14;
    return con_ioctl( 0x7F, info ) == 0;
}

static void say_notsupp( void )
{
    out_line( M_NOTSUPP_BS, NULL, NULL );
    failed = 1;
}

static void ansi_set( DISPINFO *info )
{
    int rc;

    info->level = 0;
    info->size = 14;
    info->kind = 1;
    rc = con_ioctl( 0x5F, info );
    if ( rc == GE_NOTSUPP ) {
        say_notsupp();
    } else if ( rc == GE_NOFONT ) {
        out_line( M_NOFONT, NULL, NULL );
        failed = 1;
    } else if ( rc != 0 ) {
        failed = 1;
    }
}

/* ------------------------------------------------------------------ */
/* the adapter and the BIOS mode                                       */
/* ------------------------------------------------------------------ */

static int bios_mode( void )
{
    return sys_low_byte( BDA_VMODE ) & 0x7F;
}

static int adapter_class( void )
{
    BQ bq;

    mem_set( &bq, 0, sizeof( bq ) );
    bq.eax = 0x1A00;
    if ( sys_bios( 0x10, &bq ) == 0 && (bq.eax & 0xFF) == 0x1A ) {
        switch ( bq.ebx & 0xFF ) {
        case 0x07: case 0x08: return VC_VGA;
        case 0x0B: case 0x0C: return VC_MCGA;
        }
    }
    mem_set( &bq, 0, sizeof( bq ) );
    bq.eax = 0x1200;
    bq.ebx = 0x10;
    if ( sys_bios( 0x10, &bq ) == 0 && (bq.ebx & 0xFF) != 0x10 ) {
        return (bq.ebx & 0xFF00) ? VC_EGAM : VC_EGAC;
    }
    return (sys_low_word( BDA_EQUIP ) & 0x30) == 0x30 ? VC_MDA : VC_CGA;
}

/* may the display word be set?  6.22's rule (MODEVID.ASM): a colour
   mode wants a colour display among the active and the other one, MONO
   a monochrome one.  A VGA BIOS sets every text mode on any screen. */
static int display_allowed( int mode )
{
    BQ bq;
    int active = 0, other = 0, colour, ega;

    mem_set( &bq, 0, sizeof( bq ) );
    bq.eax = 0x1A00;
    if ( sys_bios( 0x10, &bq ) == 0 && (bq.eax & 0xFF) == 0x1A ) {
        active = (int)(bq.ebx & 0xFF);
        other = (int)((bq.ebx >> 8) & 0xFF);
        if ( active == 0x07 || active == 0x08 ) {
            return 1;
        }
    } else {
        mem_set( &bq, 0, sizeof( bq ) );
        bq.eax = 0x1200;
        bq.ebx = 0x10;
        ega = sys_bios( 0x10, &bq ) == 0 && (bq.ebx & 0xFF) != 0x10;
        if ( ega && (bq.ebx & 0xFF00) == 0 ) {
            active = 0x04;
            other = sys_low_ram( 0xB0000UL ) ? 0x01 : 0;
        } else if ( ega ) {
            active = 0x05;
            other = sys_low_ram( 0xB8000UL ) ? 0x02 : 0;
        } else {
            active = sys_low_ram( 0xB0000UL ) ? 0x01 : 0;
            other = sys_low_ram( 0xB8000UL ) ? 0x02 : 0;
        }
    }
    if ( mode == DM_MONO ) {
        return active == 0x01 || active == 0x05 || active == 0x07 ||
               other == 0x01 || other == 0x05 || other == 0x07;
    }
    colour = active == 0x02 || active == 0x04 || active == 0x06 || active == 0x08 ||
             active == 0x0A || active == 0x0B || active == 0x0C;
    return colour || other == 0x02 || other == 0x04 || other == 0x06 || other == 0x08 ||
           other == 0x0A || other == 0x0B || other == 0x0C;
}

/* the equipment word's display bits for 40 and 80 columns, unless they
   say monochrome, which 6.22 left alone */
static void equip_cols( int cols )
{
    if ( (sys_low_word( BDA_EQUIP ) & 0x30) != 0x30 ) {
        sys_equip( cols == 40 ? 0x10 : 0x20 );
    }
}

/* 40 or 80 columns by a BIOS mode set, from the mode the screen is in:
   6.22's table (MODESCRN.ASM), which keeps black-and-white as it was */
static void bios_cols( int cols )
{
    int mode = bios_mode(), want;

    switch ( mode ) {
    case 0: case 2: case 5: case 6: case 7: case 0x11:
        want = cols == 40 ? 0 : 2;
        break;
    case 1: case 3: case 4: case 0x12: case 0x13:
        want = cols == 40 ? 1 : 3;
        break;
    default:
        return;                     /* not a mode MODE knows: as it is */
    }
    sys_text_mode( want, 0, 0, 0 );
    equip_cols( cols );
}

/* the lines without ANSI.SYS, on the kernel's console: 0, or 1 if the
   adapter cannot show them */
static int kernel_lines( int cols, int rows )
{
    int vc = adapter_class(), mode = bios_mode(), scan = 0, font8 = 0;

    if ( mode == 7 ) {
        if ( cols == 40 ) {
            mode = 0;
        }
    } else if ( mode == 0 || mode == 2 ) {
        mode = cols == 40 ? 0 : 2;
    } else {
        mode = cols == 40 ? 1 : 3;
    }
    switch ( rows ) {
    case 25:
        if ( vc == VC_VGA ) {
            scan = 3;               /* 400 lines */
        }
        break;
    case 43:
        if ( vc == VC_VGA ) {
            scan = 2;               /* 350 lines */
        } else if ( !(vc & (VC_EGAC | VC_EGAM)) ) {
            return 1;
        }
        font8 = 1;
        break;
    case 50:
        if ( vc != VC_VGA ) {
            return 1;
        }
        scan = 3;
        font8 = 1;
        break;
    }
    sys_text_mode( mode, scan, font8, 0 );
    return 0;
}

static void screen_size( int *cols, int *rows )
{
    sys_scr_size( rows, cols );
}

/* ------------------------------------------------------------------ */
/* doing it                                                            */
/* ------------------------------------------------------------------ */

static void kb_rate( const REQ *rq )
{
    if ( rq->rate ) {
        sys_kb_rate( ((rq->delay - 1) << 5) | (32 - rq->rate) );
    }
}

void con_set( const REQ *rq )
{
    DISPINFO info;
    int cols, rows;

    if ( rq->ccols || rq->crows ) {
        if ( ansi_get( &info ) ) {
            if ( rq->crows ) {
                info.rows = (u16)rq->crows;
            }
            if ( rq->ccols ) {
                info.cols = (u16)rq->ccols;
                equip_cols( rq->ccols );
            }
            ansi_set( &info );
        } else if ( rq->crows ) {
            screen_size( &cols, &rows );
            if ( rq->ccols ) {
                cols = rq->ccols;
            }
            if ( kernel_lines( cols, rq->crows ) ) {
                say_notsupp();
            } else if ( rq->ccols ) {
                equip_cols( rq->ccols );
            }
        } else {
            bios_cols( rq->ccols );
        }
    }
    kb_rate( rq );
}

void display_set( const REQ *rq )
{
    DISPINFO info;
    int cols, rows, equip;

    switch ( rq->dmode ) {
    case DM_BW40: case DM_CO40: case DM_BW80: case DM_CO80: case DM_MONO:
        if ( !display_allowed( rq->dmode ) ) {
            out_line( M_NOTSUPP, rq->dmode == DM_MONO ? "MONO" :
                      rq->dmode == DM_BW40 ? "BW40" : rq->dmode == DM_BW80 ? "BW80" :
                      rq->dmode == DM_CO40 ? "CO40" : "CO80", NULL );
            failed = 1;
            return;
        }
        equip = rq->dmode == DM_MONO ? 0x30 :
                (rq->dmode == DM_BW40 || rq->dmode == DM_CO40) ? 0x10 : 0x20;
        sys_text_mode( rq->dmode, 0, 0, equip );
        break;
    case DM_40:
    case DM_80:
        if ( ansi_get( &info ) ) {
            info.cols = (u16)rq->dmode;
            ansi_set( &info );
            equip_cols( rq->dmode );
        } else {
            bios_cols( rq->dmode );
        }
        break;
    }

    if ( rq->crows ) {
        if ( ansi_get( &info ) ) {
            info.rows = (u16)rq->crows;
            if ( rq->dmode != DM_NONE ) {
                info.cols = (u16)(rq->dmode == DM_BW40 || rq->dmode == DM_CO40 ||
                                  rq->dmode == DM_40 ? 40 : 80);
                info.colours = (u16)(rq->dmode == DM_MONO ? 0 : 16);
            }
            ansi_set( &info );
        } else {
            screen_size( &cols, &rows );
            if ( kernel_lines( cols, rq->crows ) ) {
                say_notsupp();
            }
        }
    }

    if ( rq->shift ) {
        out_line( M_NOSHIFT, rq->shift == 'R' ? M_RIGHT : M_LEFT, NULL );
    }
}

/* Columns= and Lines=, then the code page part */
void con_status( void )
{
    DISPINFO info;
    char num[12];
    int cols, rows, mode, text;

    out_line( M_STATFOR, "CON", NULL );
    out_line( M_DASH4, NULL, NULL );
    if ( ansi_get( &info ) ) {
        text = info.kind == 1;
        cols = info.cols;
        rows = info.rows;
    } else {
        mode = bios_mode();
        text = mode <= 3 || mode == 7;
        screen_size( &cols, &rows );
    }
    if ( text ) {
        num_str( num, (u32)cols );
        out_line( M_COLUMNS, num, NULL );
        if ( rows < 10 ) {
            num[0] = ' ';
            num_str( num + 1, (u32)rows );
        } else {
            num_str( num, (u32)rows );
        }
        out_line( M_LINES, num, NULL );
    } else {
        out_line( M_COLUMNS, M_NONE, NULL );
        out_line( M_LINES, M_NONE, NULL );
    }
    cp_status( DK_CON, 0 );
}
