/*
 * DLGS.C - MPLAY's dialog boxes: Go To, Sound, Quality, Screen, About
 * and the list of keys (MPLAY.H).
 *
 * THE SOUND GOES ON WHILE A DIALOG IS OPEN: dlg_run waits in key_get,
 * and key_get runs MPLAY's idle.  A change made in a dialog takes
 * effect when OK is pressed; the sound settings that decide how a file
 * is decoded take effect from the next file, or at once when the file
 * playing is opened again at the place it had reached.
 */
#include "mplay.h"

static int plain_activate( DLG *dlg, int index )
{
    return dlg->ctl[index].type == CT_BUTTON ? dlg->ctl[index].id : ID_OK;
}

static int ok_cancel( CTL *ctl, int row, int cols )
{
    ctl_button( &ctl[0], row, cols - 26, "OK", ID_OK, 1 );
    ctl_button( &ctl[1], row, cols - 14, "Cancel", ID_CANCEL, 0 );
    return 2;
}

/* the file playing, opened again where it is, so a changed setting
   is heard now and not at the next file */
static void reopen_here( void )
{
    PLSTAT st;
    int was;

    if ( !now_open || plist_now < 0 ) {
        return;
    }
    pl_status( &st );
    was = st.state;
    play_index( plist_now );
    if ( now_open && st.pos_ms > 0 ) {
        seek_to( st.pos_ms );
    }
    if ( now_open && was == PS_PAUSED ) {
        pl_pause();
    }
}

/* ------------------------------------------------------------------ */
/* Play > Go To                                                        */
/* ------------------------------------------------------------------ */
void dlg_goto( void )
{
    char text[16];
    PLSTAT st;
    const char *at;
    u32 minutes = 0, seconds = 0, part;
    int ok, colon = 0;

    pl_status( &st );
    sfmt( text, sizeof( text ), "%u:%02u", st.pos_ms / 60000, (st.pos_ms / 1000) % 60 );
    for ( ;; ) {
        if ( !input_box( "Go To", "&Time (minutes:seconds):", text, sizeof( text ), 10 ) ) {
            return;
        }
        minutes = seconds = 0;
        colon = 0;
        ok = 1;
        for ( at = text; *at && ok; at++ ) {
            if ( *at >= '0' && *at <= '9' ) {
                part = (u32)(*at - '0');
                if ( seconds > 100000UL ) {
                    ok = 0;
                } else {
                    seconds = seconds * 10 + part;
                }
            } else if ( *at == ':' && !colon ) {
                minutes = seconds;
                seconds = 0;
                colon = 1;
            } else if ( *at != ' ' ) {
                ok = 0;
            }
        }
        if ( ok && minutes < 10000UL && seconds < 100000UL ) {
            break;
        }
        msg_box( "Type the time as minutes:seconds, such as 2:30, or as seconds.", MB_OK );
    }
    seek_to( (minutes * 60 + seconds) * 1000 );
}

/* ------------------------------------------------------------------ */
/* Options > Sound                                                     */
/* ------------------------------------------------------------------ */
void dlg_sound( void )
{
    const PLCARD *card = pl_card();
    CTL ctl[16];
    DLG dlg;
    char line1[80], line2[80];
    int used = 0, synth, mono = set.play.mono, eq_pcm = set.play.eq_pcm, limit22 = set.play.rate_limit == 22050;
    int was_synth, was_mono, was_limit;

    synth = set.play.synth < 0 ? -1 : set.play.synth == SYN_OPL2 ? SYN_OPL3 : set.play.synth;
    sfmt( line1, sizeof( line1 ), "Card: %s", card->name );
    sfmt( line2, sizeof( line2 ), "%s", card->present ? card->line : "Put the card's driver in CONFIG.SYS." );
    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Sound";
    dlg.rows = 19;
    dlg.cols = 56;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_text( &ctl[used++], 1, 2, 52, line1 );
    ctl_text( &ctl[used++], 2, 2, 52, line2 );
    ctl_frame( &ctl[used++], 3, 2, 52, 7, "MIDI music" );
    ctl_radio( &ctl[used++], 4, 4, "The &best the card has", &synth, -1 );
    ctl_radio( &ctl[used++], 5, 4, "The card's &FM synthesizer", &synth, SYN_OPL3 );
    ctl_radio( &ctl[used++], 6, 4, "The card's MIDI &port, to a MIDI synthesizer", &synth, SYN_MPU );
    ctl_radio( &ctl[used++], 7, 4, "MPLAY's own &synthesizer", &synth, SYN_SOFT );
    ctl[used - 3].disabled = !(card->flags & (AUDF_OPL2 | AUDF_OPL3));
    ctl[used - 2].disabled = !(card->flags & AUDF_MPU);
    ctl_check( &ctl[used++], 10, 2, "&Mono: one channel, which is less work", &mono );
    ctl_check( &ctl[used++], 11, 2, "No more than &22 kHz from the card", &limit22 );
    ctl_check( &ctl[used++], 12, 2, "&Equalizer for wave, VOC and MIDI files", &eq_pcm );
    ctl_text( &ctl[used++], 13, 4, 50, "(costs time on a slow machine)" );
    dlg.sep = 15;
    used += ok_cancel( &ctl[used], 16, dlg.cols );
    dlg.nctl = used;
    if ( dlg_run( &dlg ) != ID_OK ) {
        return;
    }
    was_synth = set.play.synth;
    was_mono = set.play.mono;
    was_limit = set.play.rate_limit;
    /* OPL3 asked for on a card with only an OPL2 is its OPL2 */
    if ( synth == SYN_OPL3 && !(card->flags & AUDF_OPL3) ) {
        synth = SYN_OPL2;
    }
    set.play.synth = synth;
    set.play.mono = mono;
    set.play.rate_limit = limit22 ? 22050 : 0;
    set.play.eq_pcm = eq_pcm;
    pl_settings( &set.play );
    if ( was_synth != set.play.synth || was_mono != set.play.mono || was_limit != set.play.rate_limit ) {
        reopen_here();
    }
}

/* ------------------------------------------------------------------ */
/* Options > Quality                                                   */
/* ------------------------------------------------------------------ */
void dlg_quality( void )
{
    CTL ctl[10];
    DLG dlg;
    char line[80];
    int used = 0, quality = set.play.quality;

    sfmt( line, sizeof( line ), "This machine: %s, speed class %d of 3", pl_cpu_name(), pl_cpu_class() );
    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Quality";
    dlg.rows = 14;
    dlg.cols = 60;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_text( &ctl[used++], 1, 2, 56, line );
#ifdef HAVE_WMA
    ctl_frame( &ctl[used++], 2, 2, 56, 7, "MP3, Ogg Vorbis and WMA" );
#else
    ctl_frame( &ctl[used++], 2, 2, 56, 7, "MP3 and Ogg Vorbis" );
#endif
    ctl_radio( &ctl[used++], 3, 4, "&Automatic: less when the processor cannot keep up", &quality, PQ_AUTO );
    ctl_radio( &ctl[used++], 4, 4, "&Best: the whole sound", &quality, PQ_BEST );
    ctl_radio( &ctl[used++], 5, 4, "&Half rate: about half the work", &quality, PQ_HALF );
    ctl_radio( &ctl[used++], 6, 4, "&Quarter rate: for a 386", &quality, PQ_QUARTER );
    ctl_text( &ctl[used++], 9, 2, 56, "Half rate loses the top octave; quarter rate sounds like AM radio." );
    dlg.sep = 10;
    used += ok_cancel( &ctl[used], 11, dlg.cols );
    dlg.nctl = used;
    if ( dlg_run( &dlg ) != ID_OK || quality == set.play.quality ) {
        return;
    }
    set.play.quality = quality;
    pl_settings( &set.play );
    reopen_here();
}

/* ------------------------------------------------------------------ */
/* View > Screen                                                       */
/* ------------------------------------------------------------------ */
void dlg_screen( void )
{
    const char *names[24];
    CTL ctl[3];
    DLG dlg;
    int count = disp_mode_count(), index, height;

    if ( count > 24 ) {
        count = 24;
    }
    if ( count <= 0 ) {
        return;
    }
    for ( index = 0; index < count; index++ ) {
        names[index] = disp_mode_info( index )->name;
    }
    height = count < 12 ? count + 2 : 14;
    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Screen";
    dlg.rows = height + 5;
    dlg.cols = 40;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_list( &ctl[0], 1, 2, 36, height, NULL, names, count );
    ctl[0].sel = disp_mode_now();
    dlg.nctl = 1 + ok_cancel( &ctl[1], height + 2, dlg.cols );
    if ( dlg_run( &dlg ) == ID_OK && ctl[0].sel != disp_mode_now() ) {
        screen_apply( ctl[0].sel );
    }
}

/* ------------------------------------------------------------------ */
/* Help                                                                */
/* ------------------------------------------------------------------ */
void dlg_about( void )
{
    const PLCARD *card = pl_card();
    char text[400];

    sfmt( text, sizeof( text ),
          "MPLAY version %s\n"
#ifdef HAVE_WMA
          "Plays WAV, VOC, MIDI, MP3, WMA and Ogg Vorbis files.\n"
#else
          "Plays WAV, VOC, MIDI, MP3 and Ogg Vorbis files.\n"
#endif
          "\n"
          "Sound card: %s\n"
          "%s\n"
          "Processor: %s, speed class %d of 3",
          MP_VERSION, card->name, card->present ? card->line : "", pl_cpu_name(), pl_cpu_class() );
    msg_text( "About MPLAY", text, MB_OK );
}

void dlg_keys( void )
{
    static const char *const lines[] = {
        "Space        Play or pause",
        "S            Stop",
        "N, P         Next file, previous file",
        "Left, Right  Back or forward 5 seconds",
        "Ctrl+Left    Back 30 seconds; Ctrl+Right: forward",
        "Ctrl+G       Go to a time",
        "+ and -      Volume up and down",
        "R            Repeat: off, the playlist, one file",
        "H            Shuffle on or off",
        "E            Equalizer on or off",
        "Tab          Between the playlist and the equalizer",
        "",
        "In the playlist:",
        "Up, Down     Choose a file; Enter plays it",
        "Ins, Del     Add files; take the chosen file off",
        "",
        "In the equalizer:",
        "Left, Right  Choose a slider",
        "Up, Down     1 dB up or down; PgUp, PgDn: 6 dB",
        "Home         The slider to 0 dB",
        "",
        "Ctrl+O       Open a file      Alt+X  Exit",
        "F10 or Alt   The menus"
    };
    CTL ctl[3];
    DLG dlg;
    int count = sizeof( lines ) / sizeof( lines[0] ), height = 16;

    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Keys";
    dlg.rows = height + 5;
    dlg.cols = 62;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_list( &ctl[0], 1, 2, 58, height, NULL, lines, count );
    ctl_button( &ctl[1], height + 2, dlg.cols - 14, "OK", ID_OK, 1 );
    dlg.nctl = 2;
    dlg_run( &dlg );
}
