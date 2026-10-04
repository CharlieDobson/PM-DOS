/*
 * MAIN.C - CHKDSK/32: command line, drive validation, the main sequence
 * and the status report.
 *
 *   CHKDSK [drive:][[path]filename] [/F] [/V]
 *
 * The order of work follows the MS-DOS CHKDSK: volume identification,
 * FAT load and media check, the directory tree walk, lost chains, the
 * cross-link pass, the status report, writing the FAT, and finally the
 * fragmentation report for a named file.
 */
#include "chkdsk.h"

OPTIONS opt;
STATS   st;

static int locked;

void chkdsk_abort( void )
{
    out_flush();
    if ( locked ) {
        sys_reset_drive( opt.drive );
        sys_unlock( opt.drive, vol.fattype == 32 );
        locked = 0;
    }
    sys_exit( 255 );
}

void out_of_memory( void )
{
    out_line( H_ERR, M_NOMEM );
    chkdsk_abort();
}

static void finish( int code )
{
    out_flush();
    if ( locked ) {
        sys_reset_drive( opt.drive );
        sys_unlock( opt.drive, vol.fattype == 32 );
        locked = 0;
    }
    sys_exit( code );
}

static char *drive_name( char *buf )
{
    buf[0] = (char)('A' + opt.drive);
    buf[1] = ':';
    buf[2] = 0;
    return buf;
}

/* ------------------------------------------------------------------ */
/* command line                                                        */
/* ------------------------------------------------------------------ */

static int is_delim( char ch )
{
    return ch == ' ' || ch == '\t' || ch == ',' || ch == ';' || ch == '=' ||
           ch == '\r' || ch == '\n';
}

static void parse_cmdline( void )
{
    const char *cur = sys_cmdline(), *scan;
    static char tok[270], pos[270];
    int len, have_pos = 0, ch;
    const char *spec;

    for ( scan = cur; *scan; scan++ ) {
        if ( scan[0] == '/' && scan[1] == '?' ) {
            out_text( H_OUT, M_HELP );
            finish( 0 );
        }
    }

    while ( *cur ) {
        while ( is_delim( *cur ) ) {
            cur++;
        }
        if ( *cur == 0 ) {
            break;
        }
        len = 0;
        if ( *cur == '/' ) {
            tok[len++] = *cur++;
            while ( *cur && !is_delim( *cur ) && *cur != '/' && len < 260 ) {
                tok[len++] = *cur++;
            }
            tok[len] = 0;
            if ( str_icmp( tok, "/F" ) == 0 ) {
                opt.dofix = 1;
            } else if ( str_icmp( tok, "/V" ) == 0 ) {
                opt.noisy = 1;
            } else {
                out_msg( H_ERR, M_BADSW, tok, NULL, NULL );
                finish( 255 );
            }
            continue;
        }
        if ( *cur == '"' ) {
            cur++;
            while ( *cur && *cur != '"' && len < 260 ) {
                tok[len++] = *cur++;
            }
            if ( *cur == '"' ) {
                cur++;
            }
        } else {
            while ( *cur && !is_delim( *cur ) && *cur != '/' && len < 260 ) {
                tok[len++] = *cur++;
            }
        }
        tok[len] = 0;
        if ( have_pos ) {
            out_msg( H_ERR, M_TOOMANY, tok, NULL, NULL );
            finish( 255 );
        }
        have_pos = 1;
        str_cpy( pos, tok );
    }

    opt.drive = sys_get_drive();
    if ( !have_pos ) {
        return;
    }
    spec = pos;
    if ( spec[0] && spec[1] == ':' ) {
        ch = ch_upper( spec[0] );
        if ( ch < 'A' || ch > 'Z' ) {
            out_line( H_ERR, M_BADDRV );
            finish( 255 );
        }
        opt.drive = ch - 'A';
        spec += 2;
    }
    if ( *spec ) {
        opt.filespec = str_dup( spec );
    }
}

static void check_drive( void )
{
    int type = sys_drive_type( opt.drive ), real;

    if ( type == DRV_INVALID ) {
        out_line( H_ERR, M_BADDRV );
        finish( 255 );
    }
    if ( type == DRV_REMOTE ) {
        out_line( H_ERR, M_NONET );
        finish( 255 );
    }
    real = sys_truename_drive( opt.drive );
    if ( type == DRV_SUBST || (real >= 0 && real != opt.drive) ) {
        out_line( H_ERR, M_SUBST );
        finish( 255 );
    }
}

/* ------------------------------------------------------------------ */
/* volume identification                                               */
/* ------------------------------------------------------------------ */

/* first volume label entry of the root (first root cluster on FAT32) */
static int find_label( char *label, u16 *date, u16 *time )
{
    u32 lba, nsec, sec, i, per = vol.bps / DE_LEN;
    u8 *buf = (u8 *)xalloc( vol.bps ), *ent;
    int found = 0;

    if ( vol.fattype == 32 ) {
        if ( !CLUS_OK( vol.root_clus ) ) {
            xfree( buf );
            return 0;
        }
        lba = clus_lba( vol.root_clus );
        nsec = vol.spc;
    } else {
        lba = vol.root_sec;
        nsec = vol.root_secs;
    }
    for ( sec = 0; sec < nsec && !found; sec++ ) {
        if ( read_secs( lba + sec, 1, buf ) != 0 ) {
            break;
        }
        for ( i = 0; i < per; i++ ) {
            ent = buf + i * DE_LEN;
            if ( ent[0] == 0 ) {
                sec = nsec;
                break;
            }
            if ( ent[0] == DEL_MARK || IS_LFN( ent ) ) {
                continue;
            }
            if ( (ent[DE_ATTR] & (A_VOLID | A_DIR)) == A_VOLID ) {
                mem_cpy( label, ent, 11 );
                label[11] = 0;
                *date = RD16( ent + DE_DATE );
                *time = RD16( ent + DE_TIME );
                found = 1;
                break;
            }
        }
    }
    xfree( buf );
    return found;
}

static void show_volume_id( void )
{
    char label[12], date_buf[16], time_buf[16], hi_buf[8], lo_buf[8];
    u16 date, time;

    out_blank();
    if ( find_label( label, &date, &time ) ) {
        out_msg( H_OUT, M_VOLID, label, fmt_date( date, date_buf ),
                 fmt_time( time, time_buf ) );
    }
    if ( vol.has_serial ) {
        out_msg( H_OUT, M_SERIAL, fmt_hex4( vol.serial >> 16, hi_buf ),
                 fmt_hex4( vol.serial & 0xFFFF, lo_buf ), NULL );
    }
}

/* ------------------------------------------------------------------ */
/* status report                                                       */
/* ------------------------------------------------------------------ */

#define RPT_MAX 12

typedef struct {
    const char *tmpl;
    u64 value;
    u32 count;
    int has_count;
    int gap_before;             /* blank line before this one */
} RPTLINE;

static void report( void )
{
    RPTLINE rows[RPT_MAX];
    int nrows = 0, i, width = 13, len;
    u32 used, avail, total_mem, free_mem;
    char num[40], pad[48], cnt[40];

    used = st.dir_clus + st.fil_clus + st.hid_clus + st.bad_clus +
           st.orph_clus + st.lost_kept + st.root_clus;
    avail = used < vol.nclus ? vol.nclus - used : 0;
    sys_mem_info( &total_mem, &free_mem );

    mem_set( rows, 0, sizeof( rows ) );
    rows[nrows].tmpl = M_DSKSPC;
    rows[nrows++].value = mul32( vol.nclus, vol.bpc );
    if ( st.hid_clus ) {
        rows[nrows].tmpl = M_HIDMES;
        rows[nrows].value = mul32( st.hid_clus, vol.bpc );
        rows[nrows].count = st.hid_cnt;  rows[nrows++].has_count = 1;
    }
    if ( st.dir_clus ) {
        rows[nrows].tmpl = M_DIRMES;
        rows[nrows].value = mul32( st.dir_clus, vol.bpc );
        rows[nrows].count = st.dir_cnt;  rows[nrows++].has_count = 1;
    }
    if ( st.fil_clus ) {
        rows[nrows].tmpl = M_FILEMES;
        rows[nrows].value = mul32( st.fil_clus, vol.bpc );
        rows[nrows].count = st.fil_cnt;  rows[nrows++].has_count = 1;
    }
    if ( st.orph_clus ) {
        rows[nrows].tmpl = opt.dofix ? M_ORPHMES2 : M_ORPHMES3;
        rows[nrows].value = mul32( st.orph_clus, vol.bpc );
        rows[nrows].count = st.orph_cnt;  rows[nrows++].has_count = 1;
    }
    if ( st.bad_clus ) {
        rows[nrows].tmpl = M_BADSPC;
        rows[nrows++].value = mul32( st.bad_clus, vol.bpc );
    }
    rows[nrows].tmpl = M_FRESPC;
    rows[nrows++].value = mul32( avail, vol.bpc );
    rows[nrows].tmpl = M_IDMES2;  rows[nrows].gap_before = 1;
    rows[nrows++].value = vol.bpc;
    rows[nrows].tmpl = M_IDMES1;  rows[nrows++].value = vol.nclus;
    rows[nrows].tmpl = M_IDMES3;  rows[nrows++].value = avail;
    rows[nrows].tmpl = M_TOTMEM;  rows[nrows].gap_before = 1;
    rows[nrows++].value = total_mem;
    rows[nrows].tmpl = M_FREMEM;  rows[nrows++].value = free_mem;

    /* one column for every number; DOS fills 13 places */
    for ( i = 0; i < nrows; i++ ) {
        len = (int)str_len( fmt_num( rows[i].value, num ) );
        if ( len > width ) {
            width = len;
        }
    }
    for ( i = 0; i < nrows; i++ ) {
        if ( rows[i].gap_before ) {
            out_blank();
        }
        fmt_right( fmt_num( rows[i].value, num ), width, pad );
        out_msg( H_OUT, rows[i].tmpl, pad,
                 rows[i].has_count ? fmt_num( rows[i].count, cnt ) : NULL,
                 NULL );
    }
}

static u32 count_free( void )
{
    u32 clus, nfree = 0;

    for ( clus = 2; clus <= vol.maxclus; clus++ ) {
        if ( fat_get( clus ) == 0 ) {
            nfree++;
        }
    }
    return nfree;
}

/* ------------------------------------------------------------------ */
/* main sequence                                                       */
/* ------------------------------------------------------------------ */

void chkdsk_main( void )
{
    char dn[4];
    u8 media;

    if ( sys_init() != 0 ) {
        sys_exit( 255 );
    }
    msg_init();
    parse_cmdline();
    check_drive();

    sys_reset_drive( opt.drive );
    if ( vol_open( opt.drive ) != 0 ) {
        out_line( H_ERR, M_BADDRV );
        finish( 255 );
    }
    if ( opt.dofix ) {
        if ( sys_lock( opt.drive, vol.fattype == 32 ) != 0 ) {
            out_msg( H_ERR, M_NOLOCK, drive_name( dn ), NULL, NULL );
            finish( 255 );
        }
        locked = 1;
        vol.writes = 1;
    }

    show_volume_id();
    if ( fat_load() != 0 ) {
        out_msg( H_ERR, M_FATBAD, drive_name( dn ), NULL, NULL );
        out_line( H_ERR, M_FATAL );
        finish( 255 );
    }
    media = (u8)fat_get( 0 );           /* the FAT's first byte */
    if ( media < 0xF8 && media != 0xF0 && !prompt_yn( M_BADIDBYT ) ) {
        finish( 0 );
    }

    vol.map = (u8 *)try_alloc( vol.maxclus + 1 );
    if ( vol.map == NULL ) {
        out_of_memory();
    }
    mem_set( vol.map, 0, vol.maxclus + 1 );

    scan_pass1();
    lost_chains();
    if ( st.cross_cnt ) {
        out_blank();
        scan_pass2();
    }
    out_blank();
    report();

    if ( vol.writes ) {
        if ( vol.fat_dirty && fat_write() != 0 ) {
            out_line( H_ERR, M_FATAL );
            finish( 255 );
        }
        fsinfo_update( count_free() );
    }

    /* PM-DOS keeps a volume's clean-shutdown bit off after a crash until
       a check says the volume is sound: a whole /F run, or a look that
       found nothing.  Before the reset below, whose commit sets it. */
    if ( !st.ftrunc && (opt.dofix || (!errors_seen() && !st.cross_cnt)) ) {
        sys_mark_checked( opt.drive );
    }
    if ( locked ) {
        sys_reset_drive( opt.drive );
        sys_unlock( opt.drive, vol.fattype == 32 );
        locked = 0;
    }

    if ( opt.filespec ) {
        frag_check();
    }
    finish( 0 );
}
