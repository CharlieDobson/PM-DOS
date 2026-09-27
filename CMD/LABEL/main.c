/*
 * MAIN.C - LABEL/32: command line, prompts and the label change.
 *
 *   LABEL [drive:][label]
 *
 * The sequence is that of the MS-DOS 4.0 LABEL (LABEL.ASM): find the
 * current label; take the new one from the command line, or, when the
 * command line has none or it holds an invalid character, show the
 * current label and serial number and prompt for one; if ENTER was
 * pressed and a label exists, ask whether to delete it; end the line;
 * then delete the old label and create the new one.
 *
 * DOS does the last step with FCB delete and a create with the volume
 * attribute.  LABEL/32 edits the root directory itself, the same way:
 * every label entry is deleted and the new entry goes in the first
 * free slot, stamped with the current date and time.
 */
#include "label.h"

static int     drive;
static int     locked;
static char    cmd_label[130];
static ROOTDIR root;

/* characters DOS 5+ refuse in a label, besides control characters */
static const char bad_chars[] = "*?/\\|.,;:+=[]()&^<>\"";

static void finish( int code )
{
    out_flush();
    if ( locked ) {
        sys_reset_drive( drive );
        sys_unlock( drive, vol.fattype == 32 );
        locked = 0;
    }
    sys_exit( code );
}

void out_of_memory( void )
{
    out_line( H_ERR, M_NOMEM );
    finish( 1 );
}

static char *drive_letter( char *buf )
{
    buf[0] = (char)('A' + drive);
    buf[1] = 0;
    return buf;
}

void disk_error( int code, int writing )
{
    char drv_buf[4];

    out_msg( H_ERR, writing ? M_WRITING : M_READING, msg_disk_error( code ),
             drive_letter( drv_buf ) );
    finish( 1 );
}

/* ------------------------------------------------------------------ */
/* command line                                                        */
/* ------------------------------------------------------------------ */

/*
 * [drive:][label]: the label is the rest of the line, spaces included.
 * Blanks between the drive and the label are skipped (DOS 4.0 LABEL
 * prompted instead when the character after the colon was a blank).
 */
static void parse_cmdline( void )
{
    const char *cur = sys_cmdline(), *scan;
    int ch, len = 0;

    for ( scan = cur; *scan; scan++ ) {
        if ( scan[0] == '/' && scan[1] == '?' ) {
            out_text( H_OUT, M_HELP );
            finish( 0 );
        }
    }

    drive = sys_get_drive();
    while ( *cur == ' ' ) {
        cur++;
    }
    if ( cur[0] && cur[1] == ':' ) {
        ch = ch_upper( cur[0] );
        if ( ch < 'A' || ch > 'Z' ) {
            out_line( H_ERR, M_BADDRV );
            finish( 1 );
        }
        drive = ch - 'A';
        cur += 2;
        while ( *cur == ' ' ) {
            cur++;
        }
    }
    while ( *cur && *cur != '\r' && *cur != '\n' &&
            len < (int)sizeof( cmd_label ) - 1 ) {
        cmd_label[len++] = *cur++;
    }
    cmd_label[len] = 0;
}

static void check_drive( void )
{
    int type = sys_drive_type( drive ), real;

    if ( type == DRV_INVALID ) {
        out_line( H_ERR, M_BADDRV );
        finish( 1 );
    }
    if ( type == DRV_REMOTE ) {
        out_msg( H_ERR, M_NONET, "LABEL", NULL );
        finish( 1 );
    }
    real = sys_truename_drive( drive );
    if ( type == DRV_SUBST || (real >= 0 && real != drive) ) {
        out_msg( H_ERR, M_SUBST, "LABEL", NULL );
        finish( 1 );
    }
}

/* ------------------------------------------------------------------ */
/* labels                                                              */
/* ------------------------------------------------------------------ */

/*
 * The 11-byte directory form of a typed label: leading blanks skipped,
 * upper case, blank padded.  As in DOS every character is checked, but
 * only the first 11 are kept.  Returns -1 for an invalid character,
 * 0 for an empty label (ENTER for none), 1 otherwise.
 */
static int make_label( const char *src, u8 *out )
{
    const char *bad;
    int i, len = 0, ch;

    while ( *src == ' ' ) {
        src++;
    }
    for ( i = 0; src[i]; i++ ) {
        ch = (u8)src[i];
        if ( ch < ' ' ) {
            return -1;
        }
        for ( bad = bad_chars; *bad; bad++ ) {
            if ( ch == (u8)*bad ) {
                return -1;
            }
        }
    }
    mem_set( out, ' ', LABEL_LEN );
    for ( i = 0; src[i] && len < LABEL_LEN; i++ ) {
        ch = (u8)src[i];
        out[len++] = (u8)(ch >= 0x80 ? sys_upcase( ch ) : ch_upper( ch ));
    }
    return len > 0;
}

/* "Volume in drive C is ..." and the serial number */
static void show_label( int idx )
{
    char drv_buf[4], name[LABEL_LEN + 1], hi_buf[8], lo_buf[8];

    if ( idx >= 0 ) {
        mem_cpy( name, root.data + (u32)idx * DE_LEN, LABEL_LEN );
        if ( (u8)name[0] == E5_ALIAS ) {
            name[0] = (char)DEL_MARK;
        }
        name[LABEL_LEN] = 0;
        out_msg( H_OUT, M_HASLABEL, drive_letter( drv_buf ), name );
    } else {
        out_msg( H_OUT, M_NOLABEL, drive_letter( drv_buf ), NULL );
    }
    if ( vol.has_serial ) {
        out_msg( H_OUT, M_SERIAL, fmt_hex4( vol.serial >> 16, hi_buf ),
                 fmt_hex4( vol.serial & 0xFFFF, lo_buf ) );
    }
}

/* "Volume label (11 characters, ENTER for none)? " until it is valid */
static int ask_label( u8 *label, int bad )
{
    char buf[130];
    int result;

    for ( ;; ) {
        if ( bad ) {
            out_line( H_ERR, M_BADCHR );
        }
        out_text( H_OUT, M_NEWLABEL );
        out_flush();
        if ( sys_read_line( buf, sizeof( buf ) ) < 0 ) {
            buf[0] = 0;                 /* end of input: ENTER */
        }
        result = make_label( buf, label );
        if ( result >= 0 ) {
            return result;
        }
        bad = 1;
    }
}

/* "Delete current volume label (Y/N)?"; end of input answers N */
static int ask_delete( void )
{
    int key;

    for ( ;; ) {
        out_text( H_OUT, M_DELLABEL );
        out_flush();
        key = sys_read_key();
        if ( key < 0 ) {
            return 0;
        }
        key = ch_upper( key );
        if ( key == 'Y' ) {
            return 1;
        }
        if ( key == 'N' ) {
            return 0;
        }
    }
}

/* delete every label, then create the new one (label NULL: none) */
static void change_label( const u8 *label )
{
    char drv_buf[4];
    u16 date, time;
    u8 *ent;
    int idx, err;

    if ( sys_lock( drive, vol.fattype == 32 ) != 0 ) {
        drv_buf[0] = (char)('A' + drive);
        drv_buf[1] = ':';
        drv_buf[2] = 0;
        out_msg( H_ERR, M_NOLOCK, drv_buf, NULL );
        finish( 1 );
    }
    locked = 1;

    /* the disk may have been changed at a prompt: like DOS, label the
       disk in the drive now, so read its layout and root afresh */
    sys_reset_drive( drive );
    err = vol_open( drive );
    if ( err != 0 ) {
        disk_error( err, 0 );
    }
    root_free( &root );
    root_load( &root );

    for ( idx = root_next_label( &root, 0 ); idx >= 0;
          idx = root_next_label( &root, (u32)idx + 1 ) ) {
        root.data[(u32)idx * DE_LEN] = DEL_MARK;
        root_touch( &root, (u32)idx );
    }
    if ( label ) {
        /* a slot is always free when a label was just deleted, so
           nothing has been written if there is no room */
        idx = root_free_slot( &root );
        if ( idx < 0 && root_grow( &root ) == 0 ) {
            idx = root_free_slot( &root );
        }
        if ( idx < 0 ) {
            out_line( H_ERR, M_NOROOM );
            finish( 1 );
        }
        ent = root.data + (u32)idx * DE_LEN;
        mem_set( ent, 0, DE_LEN );
        mem_cpy( ent, label, LABEL_LEN );
        if ( ent[0] == DEL_MARK ) {
            ent[0] = E5_ALIAS;
        }
        ent[DE_ATTR] = A_VOLID;
        sys_get_datetime( &date, &time );
        WR16( ent + DE_TIME, time );
        WR16( ent + DE_DATE, date );
        root_touch( &root, (u32)idx );
    }
    root_write( &root );
    boot_set_label( label ? label : (const u8 *)"NO NAME    " );
}

/* ------------------------------------------------------------------ */
/* main sequence                                                       */
/* ------------------------------------------------------------------ */

void label_main( void )
{
    u8 label[LABEL_LEN];
    int err, old, have = 0, bad = 0, del = 0;

    if ( sys_init() != 0 ) {
        sys_exit( 1 );
    }
    parse_cmdline();
    check_drive();

    sys_reset_drive( drive );
    err = vol_open( drive );
    if ( err != 0 ) {
        disk_error( err, 0 );
    }
    root_load( &root );
    old = root_next_label( &root, 0 );

    if ( cmd_label[0] ) {
        have = make_label( cmd_label, label );
        if ( have < 0 ) {
            have = 0;
            bad = 1;
        }
    }
    if ( !have ) {
        show_label( old );
        have = ask_label( label, bad );
    }
    if ( old >= 0 && !have ) {
        del = ask_delete();
    }
    out_text( H_OUT, M_CRLF );

    if ( have || del ) {
        change_label( have ? label : NULL );
    }
    finish( 0 );
}
