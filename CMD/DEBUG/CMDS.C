/*
 * CMDS.C - the commands that read and change memory, ports and files,
 * and expanded memory.  The disassembler (U) and assembler (A) are
 * their own files; G, T, P and R are RUN.C and REGS.C.
 *
 * The formats are 6.22's, measured: D's sixteen bytes a line with the
 * hyphen after the eighth and the text to the right; E's prompt that
 * walks the bytes; C and S naming the addresses that differ or match;
 * H's sum and difference; the file count as "Writing NNNN bytes".
 */
#include "debug.h"

/* ------------------------------------------------------------------ */
/* D - dump                                                            */
/* ------------------------------------------------------------------ */

static void dump_line( ADDR addr, const u8 *buf, const u8 *ok, int first, int count )
{
    int i;
    int ascii_col = out_col();          /* set below from the address width */

    out_addr( addr );
    out_str( "  " );
    ascii_col = out_col();              /* the first byte's column */
    for ( i = 0; i < 16; i++ ) {
        if ( i >= first + count ) {
            break;                      /* nothing shown past here */
        }
        if ( i > 0 ) {
            /* the dash follows byte 7, so a line starting past it has none */
            out_ch( i == 8 && first < 8 ? '-' : ' ' );
        }
        if ( i >= first ) {
            out_hex( buf[i], 2 );
        } else {
            out_str( "  " );            /* a leading gap, before `first` */
        }
    }
    out_pad( ascii_col + 16 * 3 - 1 + 3 );
    for ( i = 0; i < first + count; i++ ) {
        if ( i >= first && ok[i] ) {
            int ch = buf[i];
            out_ch( ch >= 0x20 && ch < 0x7F ? ch : '.' );
        } else {
            out_ch( ' ' );
        }
    }
    out_nl();
}

void cmd_dump( void )
{
    ADDR addr = next_dump;
    ADDR next;
    u32  len = 128;
    u32  done = 0;
    u8   buf[16], ok[16];
    int  first;

    get_range( &addr, &len, cpu.ds, 128, 0 );
    need_end();

    next = addr;                        /* a bare D goes on from the byte after */
    addr_add( &next, len );
    first = (int)( addr.off & 0x0F );
    addr.off &= ~0x0FUL;
    while ( done < len ) {
        int count = 16 - first;
        if ( (u32)count > len - done ) {
            count = (int)( len - done );
        }
        mem_get( addr, buf, 16, ok );
        dump_line( addr, buf, ok, first, count );
        done += count;
        addr_add( &addr, 16 );
        first = 0;
    }
    next_dump = next;
}

/* ------------------------------------------------------------------ */
/* E - enter                                                           */
/* ------------------------------------------------------------------ */

static int hex_of( int ch )
{
    if ( ch >= '0' && ch <= '9' ) {
        return ch - '0';
    }
    ch = ch_upper( ch );
    if ( ch >= 'A' && ch <= 'F' ) {
        return ch - 'A' + 10;
    }
    return -1;
}

/* E address alone: a key at a time, as 6.0's ENTERDATA.  Each byte is
   shown as "XX." and up to two hex digits typed over it.  SPACE stores
   them and goes to the next byte (eight to a line, each field padded to
   eight columns), "-" stores them and goes back a byte on a new line,
   ENTER stores them and ends E, BACKSPACE takes a digit back.  Any
   other key is shown and rubbed out.  The keys are echoed here, from
   the keyboard and from a file alike. */
static void enter_prompt( ADDR addr )
{
    int newrow = 1;

    for ( ;; ) {
        u8  cur[1], ok[1];
        u32 val = 0;
        int typed = 0;
        int ch;

        if ( newrow ) {
            out_addr( addr );
            out_str( "  " );
            newrow = 0;
        }
        mem_get( addr, cur, 1, ok );
        out_hex( cur[0], 2 );
        out_ch( '.' );
        for ( ;; ) {
            out_flush();
            ch = sys_get_key();
            if ( ch < 0 ) {
                quit( 0 );              /* the commands have run out */
            }
            if ( ch == '\n' ) {
                ch = '\r';              /* a file's line with no CR */
            }
            if ( ch == ' ' || ch == '-' || ch == '\r' ) {
                break;
            }
            if ( ch == '\b' || ch == 0x7F ) {
                if ( typed == 0 ) {
                    out_str( "\b." );   /* back over the dot, and it stays */
                } else {
                    typed--;
                    val >>= 4;
                    out_str( "\b \b" );
                }
            } else if ( typed < 2 && hex_of( ch ) >= 0 ) {
                out_ch( ch );
                val = val * 16 + (u32)hex_of( ch );
                typed++;
            } else {
                out_ch( ch );
                out_str( "\b \b" );
            }
        }
        if ( typed ) {
            u8 b = (u8)val;
            if ( mem_write( addr, &b, 1 ) ) {
                out_nl();
                coded_error( "df" );
            }
        }
        if ( ch == '\r' ) {
            out_nl();
            return;
        }
        out_ch( ch );
        if ( ch == '-' ) {
            addr_add( &addr, 0xFFFFFFFFUL );    /* back one */
            out_nl();
            newrow = 1;
            continue;
        }
        out_spaces( 4 - typed );
        addr_add( &addr, 1 );
        if ( !mem_is_low( addr ) && addr.off >= mem_seg_end( addr ) ) {
            out_nl();
            return;
        }
        if ( ( addr.off & 7 ) == 0 ) {
            out_nl();
            newrow = 1;
        }
    }
}

void cmd_enter( void )
{
    ADDR addr;
    u8   list[256];
    u32  count;

    addr.seg = cpu.ds;
    get_addr( &addr, cpu.ds );
    if ( at_end() ) {
        enter_prompt( addr );
        return;
    }
    count = get_list( list, sizeof( list ) );
    need_end();
    /* in the first megabyte the list goes round the segment, as M's
       destination does */
    if ( !mem_is_low( addr ) && ( addr.off > mem_seg_end( addr ) || count > mem_seg_end( addr ) - addr.off ) ) {
        syntax_error();
    }
    if ( mem_write( addr, list, count ) ) {
        coded_error( "df" );
    }
}

/* ------------------------------------------------------------------ */
/* F - fill                                                            */
/* ------------------------------------------------------------------ */

void cmd_fill( void )
{
    ADDR addr;
    u32  len;
    u8   list[256];
    u32  count, i;

    addr.seg = cpu.ds;
    get_range( &addr, &len, cpu.ds, 0, 1 );
    count = get_list( list, sizeof( list ) );
    need_end();
    for ( i = 0; i < len; i++ ) {
        u8 b = list[i % count];
        if ( mem_write( addr, &b, 1 ) ) {
            coded_error( "df" );
        }
        addr_add( &addr, 1 );
    }
}

/* ------------------------------------------------------------------ */
/* C - compare                                                         */
/* ------------------------------------------------------------------ */

void cmd_compare( void )
{
    ADDR a, b;
    u32  len, i;
    u8   ba[1], bb[1], oka[1], okb[1];

    a.seg = cpu.ds;
    get_range( &a, &len, cpu.ds, 0, 1 );
    b.seg = cpu.ds;
    get_addr( &b, cpu.ds );
    need_end();
    /* in the first megabyte the second address goes round its segment */
    if ( !mem_is_low( b ) && ( b.off > mem_seg_end( b ) || len > mem_seg_end( b ) - b.off ) ) {
        syntax_error();
    }
    for ( i = 0; i < len; i++ ) {
        mem_get( a, ba, 1, oka );
        mem_get( b, bb, 1, okb );
        if ( ba[0] != bb[0] ) {
            out_addr( a );
            out_str( "  " );
            out_hex( ba[0], 2 );
            out_str( "  " );
            out_hex( bb[0], 2 );
            out_str( "  " );
            out_addr( b );
            out_nl();
        }
        addr_add( &a, 1 );
        addr_add( &b, 1 );
    }
}

/* ------------------------------------------------------------------ */
/* M - move                                                            */
/* ------------------------------------------------------------------ */

void cmd_move( void )
{
    ADDR a, b;
    u32  len;
    u8  *tmp;

    a.seg = cpu.ds;
    get_range( &a, &len, cpu.ds, 0, 1 );
    b.seg = cpu.ds;
    get_addr( &b, cpu.ds );
    need_end();
    if ( !mem_is_low( b ) && ( b.off > mem_seg_end( b ) || len > mem_seg_end( b ) - b.off ) ) {
        syntax_error();
    }
    /* through a buffer, so that overlapping moves copy as DOS's did */
    tmp = (u8 *)scratch_get( len );
    if ( tmp == NULL ) {
        out_err( msg_nomem );
        cmd_abort();
    }
    if ( mem_read( a, tmp, len ) ) {
        coded_error( "df" );
    }
    if ( mem_write( b, tmp, len ) ) {
        coded_error( "df" );
    }
}

/* ------------------------------------------------------------------ */
/* S - search                                                          */
/* ------------------------------------------------------------------ */

void cmd_search( void )
{
    ADDR addr, at;
    u32  len, i, j;
    u8   pat[256];
    u32  plen;
    u8   win[256], ok[256];

    addr.seg = cpu.ds;
    get_range( &addr, &len, cpu.ds, 0, 1 );
    plen = get_list( pat, sizeof( pat ) );
    need_end();
    if ( plen > len ) {
        return;
    }
    for ( i = 0; i + plen <= len; i++ ) {
        at = addr;
        addr_add( &at, i );
        mem_get( at, win, plen, ok );
        for ( j = 0; j < plen; j++ ) {
            if ( !ok[j] || win[j] != pat[j] ) {
                break;
            }
        }
        if ( j == plen ) {
            out_addr( at );
            out_ch( ' ' );          /* 6.22 leaves a space after the hit */
            out_nl();
        }
    }
}

/* ------------------------------------------------------------------ */
/* H - hex add and subtract                                            */
/* ------------------------------------------------------------------ */

void cmd_hex( void )
{
    u32 a, b;
    int digits = off_digits();

    a = get_hex( digits );
    b = get_hex( digits );
    need_end();
    if ( digits == 4 ) {
        out_hex( ( a + b ) & 0xFFFF, 4 );
        out_str( "  " );
        out_hex( ( a - b ) & 0xFFFF, 4 );
    } else {
        out_hex( a + b, 8 );
        out_str( "  " );
        out_hex( a - b, 8 );
    }
    out_nl();
}

/* ------------------------------------------------------------------ */
/* I and O - the ports                                                 */
/* ------------------------------------------------------------------ */

void cmd_input( void )
{
    u32 port;

    port = get_hex( 4 );
    need_end();
    out_hex( (u8)port_in( port ), 2 );
    out_nl();
}

void cmd_output( void )
{
    u32 port, val;

    port = get_hex( 4 );
    val = get_hex( 2 );
    need_end();
    port_out( port, val );
}

/* ------------------------------------------------------------------ */
/* N - name                                                            */
/* ------------------------------------------------------------------ */

void cmd_name( void )
{
    skip_blanks();
    set_file_name( lp );
}

/* ------------------------------------------------------------------ */
/* X - expanded memory, as 6.22's XA/XD/XM/XS                          */
/* ------------------------------------------------------------------ */

void cmd_ems( void )
{
    out_str( msg_noems );
    out_nl();
}

/* ------------------------------------------------------------------ */
/* ? - the commands                                                    */
/* ------------------------------------------------------------------ */

void cmd_help( void )
{
    int i;

    need_end();
    for ( i = 0; help_lines[i]; i++ ) {
        out_str( help_lines[i] );
        out_nl();
    }
}
