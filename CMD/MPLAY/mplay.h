/*
 * MPLAY.H - what the parts of MPLAY's application share: the settings,
 * the playlist, the screen's plan and the commands.  The engine is
 * PLAYER.H; the screen, menus and dialogs are DISP.H and UI.H.
 */
#ifndef MPLAY_H
#define MPLAY_H

#include "rt.h"
#include "sys.h"
#include "dec.h"
#include "disp.h"
#include "ui.h"
#include "player.h"
#include "audio.h"

#define MP_VERSION      "1.0"

/* ------------------------------------------------------------------ */
/* the settings, kept in MPLAY.INI beside the program                  */
/* ------------------------------------------------------------------ */
enum { AN_BARS, AN_PEAKS, AN_SCOPE, AN_OFF };
enum { RP_OFF, RP_ALL, RP_ONE };

typedef struct {
    int  mode_w, mode_h;        /* the screen: 640x480..., or 0 x lines of text */
    int  volume;                /* 0..100 */
    int  balance;               /* -100..100 */
    int  analyzer;              /* AN_* */
    int  show_eq, show_list;
    int  repeat;                /* RP_* */
    int  shuffle;
    int  preset;                /* the preset last chosen, -1 for the user's own */
    EQSET eq;
    PLSET play;
    char last_dir[PATH_LEN];
} SETTINGS;

extern SETTINGS set;

void settings_save( void );

/* the equalizer's presets: name and ten bands, half decibels */
typedef struct {
    const char *name;
    s8 band[EQ_BANDS];
} EQPRESET;

extern const EQPRESET eq_presets[];
extern const int eq_preset_count;

void eq_apply( void );                  /* set.eq to the engine, after any change */

/* ------------------------------------------------------------------ */
/* the playlist (PLIST.C)                                              */
/* ------------------------------------------------------------------ */
#define PL_MAX          2000

typedef struct {
    char *path;                         /* whole: C:\MUSIC\SONG.MP3 */
    char *title;                        /* from the tags once played, else NULL */
    u32   length_ms;                    /* 0 until known */
    u8    failed;                       /* would not play */
} PLENTRY;

extern PLENTRY *plist;
extern int plist_count;
extern int plist_now;                   /* the one playing or last played, -1 for none */
extern int plist_sel;                   /* the one the cursor is on */
extern int plist_top;                   /* the first showing */

int   plist_init( void );
void  plist_free( void );
int   plist_add( const char *path );    /* a file, a pattern (*.MP3), a directory or a playlist
                                           file -> entries added */
void  plist_remove( int index );
void  plist_clear( void );
int   plist_save( const char *path );   /* .M3U: 0, or a DOS error */
const char *plist_name( int index );    /* the title if known, else the file's name */
int   plist_next( int from, int dir );  /* the next to play after "from" (repeat and shuffle
                                           applied), or -1 at the end */
int   plist_is_media( const char *name );       /* by extension */
void  plist_reshuffle( void );

/* ------------------------------------------------------------------ */
/* the screen (VIEW.C)                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    int title_row;                      /* -1 on a text screen */
    int menu_row;
    int info_top, info_rows;
    int an_top, an_rows;                /* the analyzer */
    int seek_row;
    int tr_top, tr_rows;                /* the transport buttons */
    int low_top, low_rows;              /* the equalizer and the playlist */
    int eq_left, eq_cols;               /* eq_cols 0: hidden */
    int pl_left, pl_cols;               /* pl_cols 0: hidden */
    int status_row;
} PLAN;

extern PLAN plan;
extern int focus;                       /* FO_* */
enum { FO_LIST, FO_EQ };
extern int eq_pick;                     /* the equalizer's slider with the keys: 0 preamp, 1..10 */

void  view_init( void );
void  view_layout( void );
void  view_draw( void );                /* everything below the menu bar, into the cells */
void  view_animate( void );             /* the moving parts again: analyzer, time, seek bar */
int   view_mouse( void );               /* a mouse event on the screen -> a command or 0 */
void  view_list_show( int index );      /* scroll the playlist so the entry shows */
void  view_status( const char *text );  /* a message for the status bar; NULL: the usual */

/* ------------------------------------------------------------------ */
/* the commands (MAIN.C)                                               */
/* ------------------------------------------------------------------ */
enum {
    CM_NONE,
    CM_OPEN = 100, CM_ADD, CM_ADDDIR, CM_LOADLIST, CM_SAVELIST, CM_REMOVE, CM_CLEAR, CM_EXIT,
    CM_PLAYPAUSE, CM_STOP, CM_NEXT, CM_PREV, CM_FWD, CM_BACK, CM_FWD30, CM_BACK30, CM_GOTO,
    CM_REPEAT_ALL, CM_REPEAT_ONE, CM_SHUFFLE, CM_PLAYSEL,
    CM_EQ_ON, CM_EQ_FLAT, CM_EQ_PRESET,     /* CM_EQ_PRESET + n for preset n */
    CM_SCREEN = CM_EQ_PRESET + 32, CM_AN_BARS, CM_AN_PEAKS, CM_AN_SCOPE, CM_AN_OFF,
    CM_SHOW_EQ, CM_SHOW_LIST,
    CM_SOUND, CM_QUALITY,
    CM_HELP_KEYS, CM_ABOUT,
    CM_VOL_UP, CM_VOL_DOWN, CM_FOCUS, CM_REPEAT_CYCLE
};

void  cmd_do( int cmd );
void  play_index( int index );          /* open entry "index" and play it */
void  seek_to( u32 ms );
void  set_volume( int volume );

/* what is playing, for the screen */
extern DECINFO now_info;
extern int now_open;                    /* a file is open in the engine */
extern char now_error[80];              /* why the last file would not play, "" if it did */

/* ------------------------------------------------------------------ */
/* the dialogs (DLGS.C)                                                */
/* ------------------------------------------------------------------ */
void  dlg_goto( void );
void  dlg_sound( void );
void  dlg_quality( void );
void  dlg_screen( void );
void  dlg_about( void );
void  dlg_keys( void );
void  screen_apply( int index );

#endif
