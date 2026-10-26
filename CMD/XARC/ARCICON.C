/*
 * ARCICON.C - the list's icons on the graphics screen: the DOS build's
 * ARCICON.CPP, art and all.
 *
 * Each icon is sixteen strings of eight characters, one to a pixel: '.'
 * is transparent (the row's own background shows through, so an icon
 * sits right on a highlighted row) and a hex digit is one of the sixteen
 * EGA colours.  They go in the MARK COLUMN, which every row leaves blank
 * for the '*': the icons cost no columns, and the text screen never asks
 * for them.  The archive formats are one parcel in different colours -
 * blue .ZIP, magenta .RAR, green .7Z, cyan .CAB, red for a COMPRESS.EXE
 * file - so the eye sorts the list by hue without reading it.
 */
#include "xarc.h"

/*---- A page with a folded corner: the "some other file" icon -------------- */
static const char *const ART_FILE[16] = {
    "........",
    "........",
    ".8888...",
    ".8FF88..",
    ".8FFF788",
    ".8FFFFF8",
    ".8F888F8",
    ".8FFFFF8",
    ".8F888F8",
    ".8FFFFF8",
    ".8F888F8",
    ".8FFFFF8",
    ".8FFFFF8",
    ".8888888",
    "........",
    "........"
};

/*---- The same page with blue ruling: text ---------------------------------*/
static const char *const ART_TEXT[16] = {
    "........",
    "........",
    ".8888...",
    ".8FF88..",
    ".8FFF788",
    ".8FFFFF8",
    ".8F111F8",
    ".8FFFFF8",
    ".8F111F8",
    ".8FFFFF8",
    ".8F111F8",
    ".8FFFFF8",
    ".8F11FF8",
    ".8888888",
    "........",
    "........"
};

/*---- A window with a title bar: a program ---------------------------------*/
static const char *const ART_PROGRAM[16] = {
    "........",
    "........",
    "........",
    ".8888888",
    ".8CCCCC8",
    ".8888888",
    ".8FFFFF8",
    ".8FF8FF8",
    ".8F888F8",
    ".8FF8FF8",
    ".8FFFFF8",
    ".8FFFFF8",
    ".8888888",
    "........",
    "........",
    "........"
};

/*---- A folder with a tab ---------------------------------------------------*/
static const char *const ART_FOLDER[16] = {
    "........",
    "........",
    "........",
    ".66.....",
    ".6EE666.",
    ".6EEEEE6",
    ".6EEEEE6",
    ".6EEEEE6",
    ".6EEEEE6",
    ".6EEEEE6",
    ".6EEEEE6",
    ".6EEEEE6",
    ".6666666",
    "........",
    "........",
    "........"
};

/*---- The way out of a folder ----------------------------------------------*/
static const char *const ART_UP[16] = {
    "........",
    "........",
    "........",
    "........",
    "...BB...",
    "..BBBB..",
    ".BBBBBB.",
    "BBBBBBBB",
    "...BB...",
    "...BB...",
    "...BB...",
    "...BB...",
    "........",
    "........",
    "........",
    "........"
};

/*---- A drive, with a light on it ------------------------------------------*/
static const char *const ART_DRIVE[16] = {
    "........",
    "........",
    "........",
    "........",
    ".8888888",
    ".8777778",
    ".8700078",
    ".8777778",
    ".877777A",
    ".8888888",
    "........",
    "........",
    "........",
    "........",
    "........",
    "........"
};

/*---- A floppy: a disk image -----------------------------------------------*/
static const char *const ART_IMAGE[16] = {
    "........",
    "........",
    ".8888888",
    ".87FFF78",
    ".87F0F78",
    ".87FFF78",
    ".8777778",
    ".8777778",
    ".87FFFF8",
    ".87FFFF8",
    ".87FFFF8",
    ".8888888",
    "........",
    "........",
    "........",
    "........"
};

/*---- Parcels with a strap, one hue per format ------------------------------
 * The shape is the same for all of them on purpose: they are all archives,
 * and it is the COLOUR that says which - blue for .ZIP, magenta for .RAR,
 * green for .7Z, cyan for .CAB, red for a COMPRESS.EXE file (SETUP.EX_) -
 * so the eye sorts the list by hue without reading it.
 *---------------------------------------------------------------------------*/
static const char *const ART_ZIP[16] = {
    "........",
    "........",
    "........",
    "..1111..",
    ".199991.",
    ".199991.",
    ".111111.",
    ".1EEEE1.",
    ".111111.",
    ".199991.",
    ".199991.",
    ".199991.",
    "..1111..",
    "........",
    "........",
    "........"
};

static const char *const ART_RAR[16] = {
    "........",
    "........",
    "........",
    "..5555..",
    ".5DDDD5.",
    ".5DDDD5.",
    ".555555.",
    ".5EEEE5.",
    ".555555.",
    ".5DDDD5.",
    ".5DDDD5.",
    ".5DDDD5.",
    "..5555..",
    "........",
    "........",
    "........"
};

static const char *const ART_7Z[16] = {
    "........",
    "........",
    "........",
    "..2222..",
    ".2AAAA2.",
    ".2AAAA2.",
    ".222222.",
    ".2EEEE2.",
    ".222222.",
    ".2AAAA2.",
    ".2AAAA2.",
    ".2AAAA2.",
    "..2222..",
    "........",
    "........",
    "........"
};

static const char *const ART_CAB[16] = {
    "........",
    "........",
    "........",
    "..3333..",
    ".3BBBB3.",
    ".3BBBB3.",
    ".333333.",
    ".3EEEE3.",
    ".333333.",
    ".3BBBB3.",
    ".3BBBB3.",
    ".3BBBB3.",
    "..3333..",
    "........",
    "........",
    "........"
};

static const char *const ART_MSLZ[16] = {
    "........",
    "........",
    "........",
    "..4444..",
    ".4CCCC4.",
    ".4CCCC4.",
    ".444444.",
    ".4EEEE4.",
    ".444444.",
    ".4CCCC4.",
    ".4CCCC4.",
    ".4CCCC4.",
    "..4444..",
    "........",
    "........",
    "........"
};

/*---- A tick: this row is marked -------------------------------------------*/
static const char *const ART_MARK[16] = {
    "........",
    "........",
    "........",
    "........",
    "......AA",
    ".....AA.",
    "....AA..",
    "A...AA..",
    "AA.AA...",
    ".AAAA...",
    "..AA....",
    "........",
    "........",
    "........",
    "........",
    "........"
};

/*---- Registration ----------------------------------------------------------*/

static int s_up = ICON_NONE, s_drive = ICON_NONE, s_folder = ICON_NONE;
static int s_file = ICON_NONE, s_text = ICON_NONE, s_prog = ICON_NONE;
static int s_mark = ICON_NONE, s_zip = ICON_NONE, s_rar = ICON_NONE;
static int s_7z = ICON_NONE, s_cab = ICON_NONE, s_mslz = ICON_NONE;
static int s_image = ICON_NONE;

void icons_register( void )
{
    s_up     = df_icon_register( ART_UP,      16 );
    s_drive  = df_icon_register( ART_DRIVE,   16 );
    s_folder = df_icon_register( ART_FOLDER,  16 );
    s_file   = df_icon_register( ART_FILE,    16 );
    s_text   = df_icon_register( ART_TEXT,    16 );
    s_prog   = df_icon_register( ART_PROGRAM, 16 );
    s_mark   = df_icon_register( ART_MARK,    16 );
    s_zip    = df_icon_register( ART_ZIP,     16 );
    s_rar    = df_icon_register( ART_RAR,     16 );
    s_7z     = df_icon_register( ART_7Z,      16 );
    s_cab    = df_icon_register( ART_CAB,     16 );
    s_mslz   = df_icon_register( ART_MSLZ,    16 );
    s_image  = df_icon_register( ART_IMAGE,   16 );
}

int icon_up( void )      { return s_up; }
int icon_drive( void )   { return s_drive; }
int icon_folder( void )  { return s_folder; }
int icon_file( void )    { return s_file; }
int icon_mark( void )    { return s_mark; }

/* by extension - the archive ones the same list XARC.C opens as archives,
   so what is drawn as a parcel and what Enter opens are one answer */
int icon_for_name( const char *name )
{
    const char *dot = NULL;
    char up[8];
    int at, len;

    if ( name == NULL ) {
        return s_file;
    }
    for ( at = 0; name[at]; at++ ) {
        if ( name[at] == '.' ) {
            dot = name + at;
        }
    }
    if ( dot == NULL ) {
        return s_file;
    }
    for ( len = 0; len < 7 && dot[len]; len++ ) {
        up[len] = (char)toupper( (u8)dot[len] );
    }
    up[len] = 0;
    if ( !strcmp( up, ".7Z" ) )  return s_7z;
    if ( !strcmp( up, ".ZIP" ) ) return s_zip;
    if ( !strcmp( up, ".RAR" ) ) return s_rar;
    if ( !strcmp( up, ".CAB" ) ) return s_cab;
    if ( len >= 2 && up[len - 1] == '_' ) return s_mslz;
    if ( !strcmp( up, ".IMZ" ) || !strcmp( up, ".IMG" ) || !strcmp( up, ".IMA" )
         || !strcmp( up, ".DSK" ) ) {
        return s_image;
    }
    if ( !strcmp( up, ".EXE" ) || !strcmp( up, ".COM" ) || !strcmp( up, ".BAT" )
         || !strcmp( up, ".DLL" ) ) {
        return s_prog;
    }
    if ( !strcmp( up, ".TXT" ) || !strcmp( up, ".DOC" ) || !strcmp( up, ".MD" )
         || !strcmp( up, ".ME" ) || !strcmp( up, ".DIZ" ) || !strcmp( up, ".NFO" )
         || !strcmp( up, ".INI" ) || !strcmp( up, ".LOG" ) || !strcmp( up, ".C" )
         || !strcmp( up, ".H" ) || !strcmp( up, ".CPP" ) || !strcmp( up, ".HPP" )
         || !strcmp( up, ".ASM" ) || !strcmp( up, ".INC" ) ) {
        return s_text;
    }
    return s_file;
}
