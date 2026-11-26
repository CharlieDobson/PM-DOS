/*
 * MAIN.C - MPLAY.EXE, the PM-DOS media player: the settings, the
 * menus, the keys, the commands, and the loop (MPLAY.H).
 *
 * THE SOUND NEVER WAITS FOR THE PROGRAM.  key_idle is set to this
 * file's idle(), which DISP.H's key_get calls over and over while it
 * waits - and every menu and dialog waits in key_get - so the engine
 * is pumped, the next file is started when one ends, and the moving
 * parts of the screen are drawn again, whatever is open.  The screen
 * is redrawn a few times a second at most, fewer on a slow machine,
 * so the decoding gets the time it needs first.
 *
 * ONE FILE PLAYS AT A TIME, from the playlist.  When it ends the next
 * one starts, by the repeat and shuffle settings; one that will not
 * play is marked and passed over, and a list where nothing plays stops
 * after one round instead of trying for ever.
 *
 * MPLAY.INI, beside the program, keeps the screen, the volume, the
 * equalizer, the analyzer and the sound settings from one run to the
 * next.  A disk that cannot be written simply remembers nothing.
 */
#include "mplay.h"
#include "fix.h"

SETTINGS set;
DECINFO now_info;
int now_open;
char now_error[80];

/* half decibels, 31 Hz to 16 kHz */
const EQPRESET eq_presets[] = {
    { "Flat",           {   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 } },
    { "Rock",           {  10,   8,   4,  -2,  -6,  -2,   4,   8,  10,  10 } },
    { "Pop",            {  -2,   0,   4,   8,  10,   8,   4,   0,  -2,  -2 } },
    { "Jazz",           {   6,   4,   2,   4,  -4,  -4,   0,   2,   6,   8 } },
    { "Classical",      {   8,   6,   4,   2,  -2,  -2,   0,   4,   6,   8 } },
    { "Dance",          {  12,  10,   4,   0,   0,  -4,  -6,  -6,   0,   0 } },
    { "Bass boost",     {  14,  12,   8,   4,   0,   0,   0,   0,   0,   0 } },
    { "Treble boost",   {   0,   0,   0,   0,   0,   2,   6,  10,  12,  14 } },
    { "Vocal",          {  -4,  -4,  -2,   2,   8,   8,   6,   2,   0,  -2 } },
    { "Loudness",       {  12,   8,   2,   0,  -2,  -2,   0,   2,   8,  10 } },
    { "Small speakers", {  12,  10,   8,   4,   2,   0,  -2,  -4,  -6,  -8 } }
};
const int eq_preset_count = sizeof( eq_presets ) / sizeof( eq_presets[0] );

static int screen_on;

static uint32_t last_frame;

static const char usage[] =
#ifdef HAVE_WMA
    "Plays sound and music files: WAV, VOC, MIDI, MP3, FLAC, WMA and Ogg Vorbis.\r\n"
#else
    "Plays sound and music files: WAV, VOC, MIDI, MP3, FLAC and Ogg Vorbis.\r\n"
#endif
    "\r\n"
    "MPLAY [/T | /G[:width]] [file...]\r\n"
    "\r\n"
    "  /T      Text screen.\r\n"
    "  /G      Graphics screen, 640 x 480.  /G:800, /G:1024 or /G:1280 for a\r\n"
    "          larger one.\r\n"
    "  file    A file, a pattern such as *.MP3, a directory or a playlist\r\n"
    "          (.M3U or .PLS).  The files play one after another.\r\n"
    "\r\n"
    "The sound card needs its driver in CONFIG.SYS: SBLASTER.SYS for a Sound\r\n"
    "Blaster, ES1371.SYS for an Ensoniq AudioPCI.\r\n";

int mem_reclaim( void )
{
    return 0;
}

void out_of_memory( void )
{
    static const char said[] = "MPLAY: out of memory.\r\n";

    pl_done();
    if ( screen_on ) {
        disp_done();
    }
    sys_stdout( said, sizeof( said ) - 1 );
    sys_exit( 8 );
}

/* ------------------------------------------------------------------ */
/* MPLAY.INI                                                           */
/* ------------------------------------------------------------------ */
static void ini_path( char *path )
{
    sfmt( path, PATH_LEN, "%sMPLAY.INI", sys_program_dir() );
}

/* a signed number at *at, which moves past it and a comma after it */
static int take_int( const char **at, int *ok )
{
    const char *p = *at;
    int neg = 0, value = 0, digits = 0;

    while ( *p == ' ' ) {
        p++;
    }
    if ( *p == '-' ) {
        neg = 1;
        p++;
    }
    while ( *p >= '0' && *p <= '9' && digits < 9 ) {
        value = value * 10 + (*p - '0');
        p++;
        digits++;
    }
    if ( *p == ',' ) {
        p++;
    }
    *at = p;
    *ok = digits > 0;
    return neg ? -value : value;
}

static int clamp( int value, int lo, int hi )
{
    return value < lo ? lo : value > hi ? hi : value;
}

static void settings_defaults( void )
{
    mem_set( &set, 0, sizeof( set ) );
    set.volume = 80;
    set.analyzer = AN_PEAKS;
    set.show_eq = set.show_list = 1;
    set.preset = 0;
    set.eq.on = 0;
    set.play.quality = PQ_AUTO;
    set.play.synth = -1;
    set.play.eq_pcm = 1;
}

static void settings_load( void )
{
    char path[PATH_LEN], line[PATH_LEN + 16];
    const char *at;
    SRC src;
    unsigned int len;
    int ch, value, ok, index;

    settings_defaults();
    ini_path( path );
    if ( src_open( &src, path ) != 0 ) {
        return;
    }
    for ( ;; ) {
        len = 0;
        while ( (ch = src_byte( &src )) >= 0 && ch != '\n' ) {
            if ( ch != '\r' && len < sizeof( line ) - 1 ) {
                line[len++] = (char)ch;
            }
        }
        line[len] = 0;
        at = str_chr( line, '=' );
        if ( at != NULL ) {
            at++;
            if ( str_nicmp( line, "Equalizer=", 10 ) == 0 ) {
                set.eq.on = (uint8_t)(take_int( &at, &ok ) != 0);
                set.eq.preamp = (int8_t)clamp( take_int( &at, &ok ), -EQ_MAXDB * 2, EQ_MAXDB * 2 );
                for ( index = 0; index < EQ_BANDS; index++ ) {
                    value = take_int( &at, &ok );
                    set.eq.band[index] = (int8_t)(ok ? clamp( value, -EQ_MAXDB * 2, EQ_MAXDB * 2 ) : 0);
                }
            } else if ( str_nicmp( line, "LastDir=", 8 ) == 0 ) {
                str_cpyt( set.last_dir, at, sizeof( set.last_dir ) );
            } else {
                value = take_int( &at, &ok );
                if ( ok ) {
                    if ( str_nicmp( line, "ScreenWidth=", 12 ) == 0 ) {
                        set.mode_w = value;
                    } else if ( str_nicmp( line, "ScreenHeight=", 13 ) == 0 ) {
                        set.mode_h = value;
                    } else if ( str_nicmp( line, "Volume=", 7 ) == 0 ) {
                        set.volume = clamp( value, 0, 100 );
                    } else if ( str_nicmp( line, "Balance=", 8 ) == 0 ) {
                        set.balance = clamp( value, -100, 100 );
                    } else if ( str_nicmp( line, "Analyzer=", 9 ) == 0 ) {
                        set.analyzer = clamp( value, AN_BARS, AN_OFF );
                    } else if ( str_nicmp( line, "ShowEqualizer=", 14 ) == 0 ) {
                        set.show_eq = value != 0;
                    } else if ( str_nicmp( line, "ShowPlaylist=", 13 ) == 0 ) {
                        set.show_list = value != 0;
                    } else if ( str_nicmp( line, "Repeat=", 7 ) == 0 ) {
                        set.repeat = clamp( value, RP_OFF, RP_ONE );
                    } else if ( str_nicmp( line, "Shuffle=", 8 ) == 0 ) {
                        set.shuffle = value != 0;
                    } else if ( str_nicmp( line, "Preset=", 7 ) == 0 ) {
                        set.preset = clamp( value, -1, eq_preset_count - 1 );
                    } else if ( str_nicmp( line, "Quality=", 8 ) == 0 ) {
                        set.play.quality = clamp( value, PQ_AUTO, PQ_QUARTER );
                    } else if ( str_nicmp( line, "Mono=", 5 ) == 0 ) {
                        set.play.mono = value != 0;
                    } else if ( str_nicmp( line, "Synth=", 6 ) == 0 ) {
                        set.play.synth = clamp( value, -1, SYN_MPU );
                    } else if ( str_nicmp( line, "RateLimit=", 10 ) == 0 ) {
                        set.play.rate_limit = value < 8000 ? 0 : clamp( value, 8000, 48000 );
                    } else if ( str_nicmp( line, "PcmEqualizer=", 13 ) == 0 ) {
                        set.play.eq_pcm = value != 0;
                    }
                }
            }
        }
        if ( ch < 0 ) {
            break;
        }
    }
    src_close( &src );
}

void settings_save( void )
{
    char path[PATH_LEN], text[600];
    uint32_t handle, done;
    int index;

    ini_path( path );
    if ( sys_create( path, &handle ) != 0 ) {
        return;
    }
    sfmt( text, sizeof( text ),
          "ScreenWidth=%u\r\nScreenHeight=%u\r\nVolume=%u\r\nBalance=%d\r\nAnalyzer=%u\r\n"
          "ShowEqualizer=%u\r\nShowPlaylist=%u\r\nRepeat=%u\r\nShuffle=%u\r\nPreset=%d\r\n"
          "Quality=%u\r\nMono=%u\r\nSynth=%d\r\nRateLimit=%u\r\nPcmEqualizer=%u\r\nEqualizer=%u,%d",
          (unsigned int)set.mode_w, (unsigned int)set.mode_h, (unsigned int)set.volume, set.balance,
          (unsigned int)set.analyzer, (unsigned int)set.show_eq, (unsigned int)set.show_list,
          (unsigned int)set.repeat, (unsigned int)set.shuffle, set.preset, (unsigned int)set.play.quality,
          (unsigned int)set.play.mono, set.play.synth, (unsigned int)set.play.rate_limit,
          (unsigned int)set.play.eq_pcm, (unsigned int)set.eq.on, (int)set.eq.preamp );
    for ( index = 0; index < EQ_BANDS; index++ ) {
        sfmt( text + str_len( text ), sizeof( text ) - str_len( text ), ",%d", (int)set.eq.band[index] );
    }
    str_catn( text, "\r\n", sizeof( text ) );
    sys_write( handle, text, str_len( text ), &done );
    if ( set.last_dir[0] ) {
        sfmt( text, sizeof( text ), "LastDir=%s\r\n", set.last_dir );
        sys_write( handle, text, str_len( text ), &done );
    }
    sys_close( handle );
}

/* ------------------------------------------------------------------ */
/* the menus                                                           */
/* ------------------------------------------------------------------ */
static const MENUITEM m_file[] = {
    { "&Open...", "Ctrl+O", CM_OPEN, "Play a file, in place of the playlist" },
    { "&Add Files...", "Ins", CM_ADD, "Add a file or a pattern such as *.MP3 to the playlist" },
    { "Add &Directory...", "", CM_ADDDIR, "Add every file MPLAY plays in a directory" },
    { NULL, NULL, 0, NULL },
    { "Open &Playlist...", "", CM_LOADLIST, "Play an .M3U or .PLS playlist" },
    { "&Save Playlist...", "", CM_SAVELIST, "Save the playlist as an .M3U file" },
    { NULL, NULL, 0, NULL },
    { "&Remove from Playlist", "Del", CM_REMOVE, "Take the chosen file off the playlist" },
    { "&Clear Playlist", "", CM_CLEAR, "Take every file off the playlist" },
    { NULL, NULL, 0, NULL },
    { "E&xit", "Alt+X", CM_EXIT, "Leave MPLAY" }
};

static const MENUITEM m_play[] = {
    { "&Play or Pause", "Space", CM_PLAYPAUSE, "Play, or pause what is playing" },
    { "&Stop", "S", CM_STOP, "Stop and go back to the start" },
    { "&Next File", "N", CM_NEXT, "Play the next file in the playlist" },
    { "Pre&vious File", "P", CM_PREV, "Play the file before" },
    { NULL, NULL, 0, NULL },
    { "&Forward 5 Seconds", "Right", CM_FWD, "Skip ahead" },
    { "&Back 5 Seconds", "Left", CM_BACK, "Go back" },
    { "&Go To...", "Ctrl+G", CM_GOTO, "Go to a time in the file" },
    { NULL, NULL, 0, NULL },
    { "&Repeat Playlist", "", CM_REPEAT_ALL, "Start the playlist again after the last file" },
    { "Repeat &One File", "", CM_REPEAT_ONE, "Play the same file again and again" },
    { "S&huffle", "", CM_SHUFFLE, "Play the playlist in a mixed order" }
};

static const MENUITEM m_eq[] = {
    { "&Equalizer On", "E", CM_EQ_ON, "Switch the equalizer on or off" },
    { "Reset to &Flat", "", CM_EQ_FLAT, "Every slider back to 0 dB" },
    { NULL, NULL, 0, NULL },
    { "&Rock", "", CM_EQ_PRESET + 1, "A preset: lows and highs up, the middle down" },
    { "&Pop", "", CM_EQ_PRESET + 2, "A preset: the middle up" },
    { "&Jazz", "", CM_EQ_PRESET + 3, "A preset" },
    { "&Classical", "", CM_EQ_PRESET + 4, "A preset" },
    { "&Dance", "", CM_EQ_PRESET + 5, "A preset: heavy bass" },
    { "&Bass Boost", "", CM_EQ_PRESET + 6, "A preset: the lows up" },
    { "&Treble Boost", "", CM_EQ_PRESET + 7, "A preset: the highs up" },
    { "&Vocal", "", CM_EQ_PRESET + 8, "A preset: voices forward" },
    { "&Loudness", "", CM_EQ_PRESET + 9, "A preset: for listening quietly" },
    { "&Small Speakers", "", CM_EQ_PRESET + 10, "A preset: more bass for small speakers" }
};

static const MENUITEM m_view[] = {
    { "&Screen...", "", CM_SCREEN, "Text, or a graphics screen up to 1280 x 1024" },
    { NULL, NULL, 0, NULL },
    { "&Bars", "", CM_AN_BARS, "The analyzer as bars" },
    { "Bars and &Peaks", "", CM_AN_PEAKS, "The analyzer as bars, with peak marks" },
    { "&Oscilloscope", "", CM_AN_SCOPE, "The sound's wave" },
    { "&No Analyzer", "", CM_AN_OFF, "Hide the analyzer, which saves time on a slow machine" },
    { NULL, NULL, 0, NULL },
    { "&Equalizer", "", CM_SHOW_EQ, "Show or hide the equalizer" },
    { "Play&list", "", CM_SHOW_LIST, "Show or hide the playlist" }
};

static const MENUITEM m_opts[] = {
    { "&Sound...", "", CM_SOUND, "The sound card, and where MIDI music goes" },
    { "&Quality...", "", CM_QUALITY, "How much work the decoding may take" }
};

static const MENUITEM m_help[] = {
    { "&Keys", "F1", CM_HELP_KEYS, "The keys MPLAY knows" },
    { "&About MPLAY...", "", CM_ABOUT, "The version, the sound card and the processor" }
};

static const MENU menus[] = {
    { "&File", m_file, sizeof( m_file ) / sizeof( m_file[0] ) },
    { "&Play", m_play, sizeof( m_play ) / sizeof( m_play[0] ) },
    { "&Equalizer", m_eq, sizeof( m_eq ) / sizeof( m_eq[0] ) },
    { "&View", m_view, sizeof( m_view ) / sizeof( m_view[0] ) },
    { "&Options", m_opts, sizeof( m_opts ) / sizeof( m_opts[0] ) },
    { "&Help", m_help, sizeof( m_help ) / sizeof( m_help[0] ) }
};

static int menu_row_set = -1;

/* ------------------------------------------------------------------ */
/* the application's side of UI.H                                     */
/* ------------------------------------------------------------------ */
void app_draw( void )
{
    view_layout();
    if ( plan.menu_row != menu_row_set ) {
        menu_row_set = plan.menu_row;
        menu_set( menus, sizeof( menus ) / sizeof( menus[0] ), plan.menu_row );
    }
    menu_draw_bar( -1, gfx_on );
    view_draw();
}

int app_can( int cmd )
{
    switch ( cmd ) {
    case CM_SAVELIST:
    case CM_CLEAR:
        return plist_count > 0;
    case CM_REMOVE:
        return plist_count > 0 && plist_sel < plist_count;
    case CM_STOP:
    case CM_GOTO:
    case CM_FWD:
    case CM_BACK:
        return now_open;
    case CM_NEXT:
    case CM_PREV:
        return plist_count > 1;
    case CM_PLAYPAUSE:
        return now_open || plist_count > 0;
    case CM_SHOW_EQ:
    case CM_SHOW_LIST:
        return 1;
    }
    return 1;
}

int app_checked( int cmd )
{
    if ( cmd > CM_EQ_PRESET && cmd < CM_EQ_PRESET + eq_preset_count ) {
        return set.preset == cmd - CM_EQ_PRESET;
    }
    switch ( cmd ) {
    case CM_EQ_ON:
        return set.eq.on;
    case CM_REPEAT_ALL:
        return set.repeat == RP_ALL;
    case CM_REPEAT_ONE:
        return set.repeat == RP_ONE;
    case CM_SHUFFLE:
        return set.shuffle;
    case CM_AN_BARS:
        return set.analyzer == AN_BARS;
    case CM_AN_PEAKS:
        return set.analyzer == AN_PEAKS;
    case CM_AN_SCOPE:
        return set.analyzer == AN_SCOPE;
    case CM_AN_OFF:
        return set.analyzer == AN_OFF;
    case CM_SHOW_EQ:
        return set.show_eq;
    case CM_SHOW_LIST:
        return set.show_list;
    }
    return 0;
}

void app_status( const char *hint )
{
    view_status( hint );
}

void app_abandon( void )
{
    pl_done();
    disp_done();
    sys_exit( 0 );
}

/* ------------------------------------------------------------------ */
/* playing                                                             */
/* ------------------------------------------------------------------ */
void eq_apply( void )
{
    set.eq.serial++;
    pl_set_eq( &set.eq );
}

void set_volume( int volume )
{
    set.volume = clamp( volume, 0, 100 );
    pl_set_volume( set.volume, set.balance );
}

void seek_to( uint32_t ms )
{
    if ( now_open ) {
        pl_seek( ms );
    }
}

static const char *de_text( int rc )
{
    switch ( rc ) {
    case DE_FORMAT:
        return "not a kind of file MPLAY plays, or damaged";
    case DE_NOMEM:
        return "not enough memory";
    case DE_READ:
        return "the file could not be read";
    case DE_CARD:
        return "the sound card does not play this kind of sound";
    }
    return "the file could not be played";
}

void play_index( int index )
{
    DECINFO info;
    char title[TAG_LEN * 2 + 4];
    int rc;

    if ( index < 0 || index >= plist_count ) {
        return;
    }
    now_error[0] = 0;
    plist_now = index;
    plist_sel = index;
    view_list_show( index );
    mem_set( &info, 0, sizeof( info ) );
    rc = pl_open( plist[index].path, &info );
    if ( rc != DE_OK ) {
        now_open = 0;
        plist[index].failed = 1;
        sfmt( now_error, sizeof( now_error ), "%s: %s", plist_name( index ),
              rc == DE_UNSUPPORTED && info.format[0] ? info.format : de_text( rc ) );
        return;
    }
    now_info = info;
    now_open = 1;
    plist[index].failed = 0;
    plist[index].length_ms = info.length_ms;
    if ( info.title[0] && plist[index].title == NULL ) {
        sfmt( title, sizeof( title ), "%s%s%s", info.artist, info.artist[0] ? " - " : "", info.title );
        plist[index].title = (char *)try_alloc( str_len( title ) + 1 );
        if ( plist[index].title != NULL ) {
            str_cpy( plist[index].title, title );
        }
    }
    pl_set_volume( set.volume, set.balance );
    pl_play();
}

/* the file that ended (or would not play), and the next one that
   plays: each file tried once at most, and on Repeat One the same
   file is not tried again when it fails */
static void advance( void )
{
    int next, tries;

    for ( tries = 0; tries < plist_count; tries++ ) {
        next = plist_next( plist_now, 0 );
        if ( next < 0 ) {
            break;
        }
        play_index( next );
        if ( now_open || set.repeat == RP_ONE ) {
            return;
        }
    }
    now_open = 0;
    pl_close();
}

/* key_idle: the sound, the next file, and the screen a few times a second */
/* the sound's half of idle: the engine pumped, the next file started
   when one ends.  The loop calls it between keys as well - keys that
   come fast, a held arrow or a key file, would otherwise each be
   handled and drawn with nothing decoded in between */
static void keep_playing( void )
{
    int state;

    state = pl_pump( pl_cpu_class() == 0 ? 25 : 40 );
    if ( key_testing() ) {
        static uint32_t dropouts_said;
        PLSTAT st;
        char line[96];

        pl_status( &st );
        if ( st.underruns != dropouts_said ) {
            dropouts_said = st.underruns;
            sfmt( line, sizeof( line ), "dropout %u at %u ms, %s open, card irqs %u", st.underruns, st.pos_ms,
                  ui_busy() ? "a menu or dialog" : "nothing", st.irqs );
            scr_log( line );
        }
    }
    if ( state == PS_ENDED && now_open ) {
        advance();
    } else if ( state == PS_EMPTY && now_open ) {
        /* the file was opened again (Stop, or a cheaper quality) and
           would not open: said, and left there */
        now_open = 0;
        if ( plist_now >= 0 && plist_now < plist_count ) {
            plist[plist_now].failed = 1;
            sfmt( now_error, sizeof( now_error ), "%s: the file could not be opened again", plist_name( plist_now ) );
        }
    }
}

/* key_idle.  THE SCREEN TAKES ITS TURN AFTER THE SOUND, and pays for
   it: a frame of the moving parts is drawn no sooner than six times
   what the last one cost after it, so the screen has at most about a
   sixth of the processor - on a 16-color screen a frame of the
   analyzer is a lot of bit planes - and none at all while less than
   a quarter of a second of sound is waiting for the card */
static void idle( void )
{
    static int inside;
    static uint32_t frame_ms;
    uint32_t now, cost, least;
    PLSTAT st;

    if ( inside ) {
        return;
    }
    inside = 1;
    keep_playing();
    now = sys_clock_ms();
    least = pl_cpu_class() == 0 ? 200 : pl_cpu_class() == 1 ? 125 : 70;
    if ( frame_ms < least ) {
        frame_ms = least;
    }
    pl_status( &st );
    if ( screen_on && now - last_frame >= frame_ms && (st.state != PS_PLAYING || pl_ahead_ms() >= 250) ) {
        last_frame = now;
        view_animate();
        ui_refresh();
        scr_flush();
        cost = sys_clock_ms() - now;
        frame_ms = cost * 6 > 600 ? 600 : cost * 6;
    }
    inside = 0;
}

/* ------------------------------------------------------------------ */
/* the commands                                                        */
/* ------------------------------------------------------------------ */
static const FBTYPE media_types[] = {
#ifdef HAVE_WMA
    { "Sound and music files", "*.MP3;*.WAV;*.MID;*.OGG;*.FLAC;*.FLA;*.WMA;*.VOC;*.MP2;*.MP1;*.RMI;*.KAR;*.MIDI;*.MPA;*.OGA;*.ASF" },
#else
    { "Sound and music files", "*.MP3;*.WAV;*.MID;*.OGG;*.FLAC;*.FLA;*.VOC;*.MP2;*.MP1;*.RMI;*.KAR;*.MIDI;*.MPA;*.OGA" },
#endif
    { "MP3 and MPEG audio (*.MP3)", "*.MP3;*.MP2;*.MP1;*.MPA" },
    { "Wave files (*.WAV)", "*.WAV" },
    { "FLAC files (*.FLAC)", "*.FLAC;*.FLA" },
    { "MIDI music (*.MID)", "*.MID;*.MIDI;*.RMI;*.KAR" },
    { "Ogg Vorbis (*.OGG)", "*.OGG;*.OGA" },
#ifdef HAVE_WMA
    { "Windows Media Audio (*.WMA)", "*.WMA;*.ASF" },
#endif
    { "Creative Voice files (*.VOC)", "*.VOC" },
    { "Playlists (*.M3U)", "*.M3U;*.PLS" },
    { "All files (*.*)", "*.*" }
};

static const FBTYPE list_types[] = {
    { "Playlists (*.M3U)", "*.M3U;*.PLS" },
    { "All files (*.*)", "*.*" }
};

static void remember_dir( const char *path )
{
    const char *last = NULL, *at;
    unsigned int len;

    for ( at = path; *at; at++ ) {
        if ( *at == '\\' ) {
            last = at;
        }
    }
    if ( last != NULL ) {
        len = (unsigned int)(last - path) + 1;
        if ( len < sizeof( set.last_dir ) ) {
            mem_cpy( set.last_dir, path, len );
            set.last_dir[len] = 0;
        }
    }
}

static int ask_file( const char *title, char *path, const FBTYPE *types, int ntypes )
{
    int type = 0;

    str_cpyt( path, set.last_dir, PATH_LEN );
    if ( !fbox_open( title, path, types, ntypes, &type ) ) {
        return 0;
    }
    remember_dir( path );
    return 1;
}

static void stop_all( void )
{
    pl_close();
    now_open = 0;
}

static void quit( void )
{
    char line[64];

    settings_save();
    stop_all();
    pl_done();
    plist_free();
    if ( key_testing() ) {
        /* what is left is the screen's: the same after any session */
        sfmt( line, sizeof( line ), "heap held at exit: %u", heap_in_use() );
        scr_log( line );
    }
    disp_done();
    sys_exit( 0 );
}

void cmd_do( int cmd )
{
    char path[PATH_LEN], dir[PATH_LEN];
    PLSTAT st;
    int index, rc;
    unsigned int len;

    if ( cmd > CM_EQ_PRESET && cmd < CM_EQ_PRESET + eq_preset_count ) {
        mem_cpy( set.eq.band, eq_presets[cmd - CM_EQ_PRESET].band, EQ_BANDS );
        set.preset = cmd - CM_EQ_PRESET;
        set.eq.on = 1;
        eq_apply();
        return;
    }
    switch ( cmd ) {
    case CM_OPEN:
        if ( ask_file( "Open", path, media_types, sizeof( media_types ) / sizeof( media_types[0] ) ) ) {
            stop_all();
            plist_clear();
            if ( plist_add( path ) > 0 ) {
                play_index( 0 );
            } else {
                msg_box2( "Nothing to play in", path, MB_OK );
            }
        }
        break;
    case CM_ADD:
        if ( ask_file( "Add Files", path, media_types, sizeof( media_types ) / sizeof( media_types[0] ) ) ) {
            index = plist_count;
            if ( plist_add( path ) == 0 ) {
                msg_box2( "Nothing to add in", path, MB_OK );
            } else if ( !now_open ) {
                plist_sel = index;
                view_list_show( index );
            }
        }
        break;
    case CM_ADDDIR:
        if ( ask_file( "Add Directory - choose any file in it", path, media_types,
                       sizeof( media_types ) / sizeof( media_types[0] ) ) ) {
            str_cpyt( dir, set.last_dir, sizeof( dir ) );
            len = str_len( dir );
            if ( len > 3 && dir[len - 1] == '\\' ) {
                dir[len - 1] = 0;
            }
            if ( plist_add( dir ) == 0 ) {
                msg_box2( "Nothing MPLAY plays in", dir, MB_OK );
            }
        }
        break;
    case CM_LOADLIST:
        if ( ask_file( "Open Playlist", path, list_types, 2 ) ) {
            stop_all();
            plist_clear();
            if ( plist_add( path ) > 0 ) {
                play_index( 0 );
            } else {
                msg_box2( "No files could be found from", path, MB_OK );
            }
        }
        break;
    case CM_SAVELIST:
        str_cpyt( path, set.last_dir, sizeof( path ) );
        str_catn( path, "PLAYLIST.M3U", sizeof( path ) );
        index = 0;
        if ( fbox_save( "Save Playlist", path, list_types, 1, &index ) ) {
            rc = plist_save( path );
            if ( rc != 0 ) {
                msg_error( "The playlist could not be saved.", rc );
            }
        }
        break;
    case CM_REMOVE:
        if ( plist_sel == plist_now ) {
            stop_all();
        }
        plist_remove( plist_sel );
        view_list_show( plist_sel );    /* no blank rows left at the bottom */
        break;
    case CM_CLEAR:
        stop_all();
        plist_clear();
        break;
    case CM_EXIT:
        quit();
        break;
    case CM_PLAYSEL:
        play_index( plist_sel );
        break;
    case CM_PLAYPAUSE:
        if ( !now_open ) {
            play_index( plist_sel < plist_count ? plist_sel : 0 );
            break;
        }
        pl_status( &st );
        if ( st.state == PS_PLAYING ) {
            pl_pause();
        } else {
            pl_play();
        }
        break;
    case CM_STOP:
        pl_stop();
        break;
    case CM_NEXT:
    case CM_PREV:
        index = plist_next( plist_now, cmd == CM_NEXT ? 1 : -1 );
        if ( index >= 0 ) {
            play_index( index );
        }
        break;
    case CM_FWD:
    case CM_BACK:
    case CM_FWD30:
    case CM_BACK30:
        pl_status( &st );
        len = cmd == CM_FWD || cmd == CM_BACK ? 5000UL : 30000UL;
        if ( cmd == CM_FWD || cmd == CM_FWD30 ) {
            seek_to( st.pos_ms + len );
        } else {
            seek_to( st.pos_ms > len ? st.pos_ms - len : 0 );
        }
        break;
    case CM_GOTO:
        dlg_goto();
        break;
    case CM_REPEAT_ALL:
        set.repeat = set.repeat == RP_ALL ? RP_OFF : RP_ALL;
        break;
    case CM_REPEAT_ONE:
        set.repeat = set.repeat == RP_ONE ? RP_OFF : RP_ONE;
        break;
    case CM_REPEAT_CYCLE:
        set.repeat = set.repeat == RP_OFF ? RP_ALL : set.repeat == RP_ALL ? RP_ONE : RP_OFF;
        view_status( set.repeat == RP_OFF ? " Repeat is off" : set.repeat == RP_ALL
                     ? " Repeat: the whole playlist" : " Repeat: this file" );
        break;
    case CM_SHUFFLE:
        set.shuffle = !set.shuffle;
        plist_reshuffle();
        view_status( set.shuffle ? " Shuffle is on" : " Shuffle is off" );
        break;
    case CM_EQ_ON:
        set.eq.on = (uint8_t)!set.eq.on;
        eq_apply();
        break;
    case CM_EQ_FLAT:
        mem_set( set.eq.band, 0, EQ_BANDS );
        set.eq.preamp = 0;
        set.preset = 0;
        eq_apply();
        break;
    case CM_SCREEN:
        dlg_screen();
        break;
    case CM_AN_BARS:
    case CM_AN_PEAKS:
    case CM_AN_SCOPE:
    case CM_AN_OFF:
        set.analyzer = cmd - CM_AN_BARS + AN_BARS;
        break;
    case CM_SHOW_EQ:
        set.show_eq = !set.show_eq;
        break;
    case CM_SHOW_LIST:
        set.show_list = !set.show_list;
        break;
    case CM_SOUND:
        dlg_sound();
        break;
    case CM_QUALITY:
        dlg_quality();
        break;
    case CM_HELP_KEYS:
        dlg_keys();
        break;
    case CM_ABOUT:
        dlg_about();
        break;
    case CM_VOL_UP:
        set_volume( set.volume + 5 );
        break;
    case CM_VOL_DOWN:
        set_volume( set.volume - 5 );
        break;
    case CM_FOCUS:
        focus = focus == FO_LIST ? FO_EQ : FO_LIST;
        break;
    }
}

/* a key that is not a menu's */
static void key_do( int key )
{
    int plain = key & ~K_MODS, rows = plan.low_rows - 1, value;

    switch ( key ) {
    case ' ':
        cmd_do( CM_PLAYPAUSE );
        return;
    case 's':
    case 'S':
        cmd_do( CM_STOP );
        return;
    case 'n':
    case 'N':
        cmd_do( CM_NEXT );
        return;
    case 'p':
    case 'P':
        cmd_do( CM_PREV );
        return;
    case 'e':
    case 'E':
        cmd_do( CM_EQ_ON );
        return;
    case 'r':
    case 'R':
        cmd_do( CM_REPEAT_CYCLE );
        return;
    case 'h':
    case 'H':
        cmd_do( CM_SHUFFLE );
        return;
    case '+':
    case '=':
        cmd_do( CM_VOL_UP );
        return;
    case '-':
        cmd_do( CM_VOL_DOWN );
        return;
    case K_TAB:
    case K_SHTAB:
        cmd_do( CM_FOCUS );
        return;
    case K_F1:
        cmd_do( CM_HELP_KEYS );
        return;
    case K_C( 'O' ):
        cmd_do( CM_OPEN );
        return;
    case K_C( 'G' ):
        cmd_do( CM_GOTO );
        return;
    case K_INS:
        cmd_do( CM_ADD );
        return;
    case K_LEFT | K_CTRL:
        cmd_do( CM_BACK30 );
        return;
    case K_RIGHT | K_CTRL:
        cmd_do( CM_FWD30 );
        return;
    }
    if ( key == K_A( 'X' ) ) {
        cmd_do( CM_EXIT );
        return;
    }
    if ( focus == FO_EQ ) {
        value = eq_pick == 0 ? set.eq.preamp : set.eq.band[eq_pick - 1];
        switch ( plain ) {
        case K_LEFT:
            eq_pick = eq_pick > 0 ? eq_pick - 1 : EQ_BANDS;
            return;
        case K_RIGHT:
            eq_pick = eq_pick < EQ_BANDS ? eq_pick + 1 : 0;
            return;
        case K_UP:
            value += 2;
            break;
        case K_DOWN:
            value -= 2;
            break;
        case K_PGUP:
            value += 12;
            break;
        case K_PGDN:
            value -= 12;
            break;
        case K_HOME:
            value = 0;
            break;
        default:
            return;
        }
        value = clamp( value, -EQ_MAXDB * 2, EQ_MAXDB * 2 );
        if ( eq_pick == 0 ) {
            set.eq.preamp = (int8_t)value;
        } else {
            set.eq.band[eq_pick - 1] = (int8_t)value;
        }
        set.preset = -1;
        eq_apply();
        return;
    }
    switch ( plain ) {
    case K_LEFT:
        cmd_do( CM_BACK );
        return;
    case K_RIGHT:
        cmd_do( CM_FWD );
        return;
    case K_UP:
        plist_sel--;
        break;
    case K_DOWN:
        plist_sel++;
        break;
    case K_PGUP:
        plist_sel -= rows > 1 ? rows - 1 : 1;
        break;
    case K_PGDN:
        plist_sel += rows > 1 ? rows - 1 : 1;
        break;
    case K_HOME:
        plist_sel = 0;
        break;
    case K_END:
        plist_sel = plist_count - 1;
        break;
    case K_ENTER:
        if ( plist_count > 0 ) {
            cmd_do( CM_PLAYSEL );
        }
        return;
    case K_DEL:
        if ( app_can( CM_REMOVE ) ) {
            cmd_do( CM_REMOVE );
        }
        return;
    default:
        return;
    }
    plist_sel = clamp( plist_sel, 0, plist_count > 0 ? plist_count - 1 : 0 );
    view_list_show( plist_sel );
}

/* ------------------------------------------------------------------ */
/* the start                                                           */
/* ------------------------------------------------------------------ */
static int next_word( const char **line, char *word, unsigned int max )
{
    const char *at = *line;
    unsigned int len = 0;
    int quoted = 0;

    while ( *at == ' ' || *at == '\t' ) {
        at++;
    }
    if ( *at == 0 ) {
        *line = at;
        return 0;
    }
    if ( *at == '"' ) {
        quoted = 1;
        at++;
    }
    while ( *at && (quoted ? *at != '"' : (*at != ' ' && *at != '\t')) ) {
        if ( len + 1 < max ) {
            word[len++] = *at;
        }
        at++;
    }
    if ( quoted && *at == '"' ) {
        at++;
    }
    word[len] = 0;
    *line = at;
    return 1;
}

void screen_apply( int index )
{
    const DISPMODE *info;

    ui_set_mode( index );
    info = disp_mode_info( disp_mode_now() );
    if ( info != NULL ) {
        set.mode_w = info->width;
        set.mode_h = info->width ? info->height : info->rows;
    }
    menu_row_set = -1;
}

void mp_main( void )
{
    static const int widths[] = { 640, 800, 1024, 1280 };
    static const int heights[] = { 480, 600, 768, 1024 };
    const char *line;
    char word[PATH_LEN];
    int key, cmd, index, mode, want_w = -1, want_h = 0, ok;

    sys_init();
    sys_break_install();
    line = sys_cmdline();
    while ( next_word( &line, word, sizeof( word ) ) ) {
        if ( word[0] != '/' ) {
            continue;
        }
        if ( word[1] == '?' ) {
            sys_stdout( usage, sizeof( usage ) - 1 );
            sys_exit( 0 );
        } else if ( ch_upper( word[1] ) == 'T' && word[2] == 0 ) {
            want_w = 0;
            want_h = 0;
        } else if ( ch_upper( word[1] ) == 'G' && (word[2] == 0 || word[2] == ':') ) {
            want_w = word[2] == ':' ? (int)dec_parse( word + 3, &ok ) : 640;
            want_h = 0;
            for ( index = 0; index < 4; index++ ) {
                if ( widths[index] == want_w ) {
                    want_h = heights[index];
                }
            }
            if ( want_h == 0 ) {
                want_w = 640;
                want_h = 480;
            }
        } else {
            sys_stdout( "Invalid switch - ", 17 );
            sys_stdout( word, str_len( word ) );
            sys_stdout( "\r\n", 2 );
            sys_exit( 1 );
        }
    }
    settings_load();
    if ( !plist_init() || !pl_init() ) {
        sys_stdout( "MPLAY: not enough memory.\r\n", 27 );
        sys_exit( 8 );
    }
    disp_init();
    screen_on = 1;
    if ( want_w >= 0 ) {
        set.mode_w = want_w;
        set.mode_h = want_h;
    }
    ui_name = "MPLAY";
    ui_look( LOOK_AUTO );
    view_init();
    mode = set.mode_w || set.mode_h ? disp_mode_find( set.mode_w, set.mode_h ) : -1;
    if ( mode >= 0 && mode != disp_mode_now() ) {
        screen_apply( mode );
    }
    if ( mouse_present() ) {
        mouse_moves( 0 );
    }
    pl_settings( &set.play );
    set.eq.serial = 1;
    pl_set_eq( &set.eq );
    pl_set_volume( set.volume, set.balance );
    key_idle = idle;
    /* the files on the command line, played from the first */
    line = sys_cmdline();
    while ( next_word( &line, word, sizeof( word ) ) ) {
        if ( word[0] != '/' ) {
            plist_add( word );
        }
    }
    if ( plist_count > 0 ) {
        /* the screen's first drawing is the biggest - all of it, at
           1280x1024 over a megabyte - so it is done before the sound
           starts and not while the first half second waits in the ring */
        app_draw();
        scr_flush();
        play_index( 0 );
        if ( !now_open ) {
            advance();
        }
    }
    for ( ;; ) {
        keep_playing();
        app_draw();
        scr_flush();
        key = key_get();
        view_status( NULL );
        if ( key_service( key ) ) {
            continue;
        }
        if ( menu_wants( key ) ) {
            cmd = menu_run( key );
            if ( cmd ) {
                cmd_do( cmd );
            }
            continue;
        }
        if ( key == K_MOUSE ) {
            cmd = view_mouse();
            if ( cmd ) {
                cmd_do( cmd );
            }
            continue;
        }
        key_do( key );
    }
}
