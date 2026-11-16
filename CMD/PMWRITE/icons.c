/*
 * ICONS.C - the pictures on the toolbar's buttons.
 *
 * Sixteen rows of sixteen characters, a character a pixel, in the
 * letters DISP.H's gfx_icon reads: '.' nothing, '#' the ink (black on
 * a button that can be pressed), and a letter for a colour - k black,
 * b blue, n brown, w light grey, d dark grey, y yellow, W white, B
 * bright blue, r red.  They were drawn for this program.
 */
#include "pw.h"

static const char *const icon_new[16] = {
    "................",
    "...kkkkkkkk.....",
    "...kWWWWWWkk....",
    "...kWWWWWWkWk...",
    "...kWWWWWWkkkk..",
    "...kWWWWWWWWWk..",
    "...kWWWWWWWWWk..",
    "...kWWWWWWWWWk..",
    "...kWWWWWWWWWk..",
    "...kWWWWWWWWWk..",
    "...kWWWWWWWWWk..",
    "...kWWWWWWWWWk..",
    "...kWWWWWWWWWk..",
    "...kWWWWWWWWWk..",
    "...kkkkkkkkkkk..",
    "................"
};

static const char *const icon_open[16] = {
    "................",
    "................",
    "..........kkk...",
    ".........k...k.k",
    "..............kk",
    ".kkk.........kkk",
    "kyyykkkkkkk.....",
    "kyyyyyyyyyk.....",
    "kyyyyyyyyyk.....",
    "kyyykkkkkkkkkkk.",
    "kyykynynynynynk.",
    "kykynynynynynk..",
    "kkynynynynynk...",
    "kynynynynynk....",
    "kkkkkkkkkkk.....",
    "................"
};

static const char *const icon_save[16] = {
    "................",
    ".kkkkkkkkkkkkk..",
    ".kbkWWWWWWWkbk..",
    ".kbkWWWWWWWkbk..",
    ".kbkWWWWWWWkbk..",
    ".kbkWWWWWWWkbk..",
    ".kbkWWWWWWWkbk..",
    ".kbbkkkkkkkbbk..",
    ".kbbbbbbbbbbbk..",
    ".kbbkkkkkkkbbk..",
    ".kbbkwwwwkkbbk..",
    ".kbbkwwwwkkbbk..",
    ".kbbkwwwwkkbbk..",
    ".kbbkwwwwkkbbk..",
    "..kkkkkkkkkkkk..",
    "................"
};

static const char *const icon_print[16] = {
    "................",
    "....kkkkkkkk....",
    "....kWWWWWWk....",
    "....kWkkkkWk....",
    "....kWWWWWWk....",
    "....kWkkkkWk....",
    ".kkkkWWWWWWkkkk.",
    "kwwwkkkkkkkkwwwk",
    "kwwwwwwwwwwwwywk",
    "kwwwwwwwwwwwwwwk",
    "kddddddddddddddk",
    "kwwkkkkkkkkkkwwk",
    "kwwkWWWWWWWWkwwk",
    ".kkkWWWWWWWWkkk.",
    "...kkkkkkkkkk...",
    "................"
};

static const char *const icon_cut[16] = {
    "................",
    "....k.....k.....",
    "....k.....k.....",
    "....kk...kk.....",
    ".....k...k......",
    ".....kk.kk......",
    "......kkk.......",
    "......kkk.......",
    ".....kk.kk......",
    "...kkk...kkk....",
    "..k..k...k..k...",
    "..k..k...k..k...",
    "..k..k...k..k...",
    "...kk.....kk....",
    "................",
    "................"
};

static const char *const icon_copy[16] = {
    "................",
    ".kkkkkkk........",
    ".kWWWWWkk.......",
    ".kWWWWWkWk......",
    ".kWkkWWkkkkkkk..",
    ".kWWWWkWWWWWkk..",
    ".kWkkkkWWWWWkWk.",
    ".kWWWWkWkkWWkkkk",
    ".kWkkkkWWWWWWWWk",
    ".kWWWWkWkkkkkkWk",
    ".kkkkkkWWWWWWWWk",
    "......kWkkkkkkWk",
    "......kWWWWWWWWk",
    "......kWWWWWWWWk",
    "......kkkkkkkkkk",
    "................"
};

static const char *const icon_paste[16] = {
    "................",
    ".....kkkkk......",
    "..kkkkyyykkkk...",
    ".knnkkkkkkknnk..",
    ".knnnnnnnnnnnk..",
    ".knnnnnnnnnnnk..",
    ".knnnnkkkkkkkkk.",
    ".knnnnkWWWWWWkk.",
    ".knnnnkWWWWWWkWk",
    ".knnnnkWkkkWWkkk",
    ".knnnnkWWWWWWWWk",
    ".knnnnkWkkkkkkWk",
    ".knnnnkWWWWWWWWk",
    ".kkkkkkWkkkkkkWk",
    "......kWWWWWWWWk",
    "......kkkkkkkkkk"
};

static const char *const icon_undo[16] = {
    "................",
    "................",
    "....b...........",
    "...bb...........",
    "..bbbbbbbb......",
    ".bbbbbbbbbbb....",
    "..bbbbbbbbbbb...",
    "...bb.....bbbb..",
    "....b......bbb..",
    "............bbb.",
    "............bbb.",
    "............bbb.",
    "...........bbb..",
    "..........bbb...",
    "................",
    "................"
};

static const char *const icon_redo[16] = {
    "................",
    "................",
    "...........b....",
    "...........bb...",
    "......bbbbbbbb..",
    "....bbbbbbbbbbb.",
    "...bbbbbbbbbbb..",
    "..bbbb.....bb...",
    "..bbb......b....",
    ".bbb............",
    ".bbb............",
    ".bbb............",
    "..bbb...........",
    "...bbb..........",
    "................",
    "................"
};

static const char *const icon_bold[16] = {
    "................",
    "................",
    "...#######......",
    "....###..###....",
    "....###...###...",
    "....###...###...",
    "....###..###....",
    "....#######.....",
    "....###..####...",
    "....###...####..",
    "....###...####..",
    "....###...####..",
    "....###..####...",
    "...#########....",
    "................",
    "................"
};

static const char *const icon_italic[16] = {
    "................",
    "................",
    ".......#####....",
    ".........##.....",
    ".........##.....",
    "........##......",
    "........##......",
    ".......##.......",
    ".......##.......",
    "......##........",
    "......##........",
    ".....##.........",
    ".....##.........",
    "...#####........",
    "................",
    "................"
};

static const char *const icon_under[16] = {
    "................",
    "................",
    "...####..####...",
    "....##....##....",
    "....##....##....",
    "....##....##....",
    "....##....##....",
    "....##....##....",
    "....##....##....",
    "....##....##....",
    ".....##..##.....",
    "......####......",
    "................",
    "...##########...",
    "................",
    "................"
};

static const char *const icon_left[16] = {
    "................",
    "................",
    "..############..",
    "................",
    "..########......",
    "................",
    "..############..",
    "................",
    "..########......",
    "................",
    "..############..",
    "................",
    "..########......",
    "................",
    "................",
    "................"
};

static const char *const icon_center[16] = {
    "................",
    "................",
    "..############..",
    "................",
    "....########....",
    "................",
    "..############..",
    "................",
    "....########....",
    "................",
    "..############..",
    "................",
    "....########....",
    "................",
    "................",
    "................"
};

static const char *const icon_right[16] = {
    "................",
    "................",
    "..############..",
    "................",
    "......########..",
    "................",
    "..############..",
    "................",
    "......########..",
    "................",
    "..############..",
    "................",
    "......########..",
    "................",
    "................",
    "................"
};

static const char *const icon_justify[16] = {
    "................",
    "................",
    "..############..",
    "................",
    "..############..",
    "................",
    "..############..",
    "................",
    "..############..",
    "................",
    "..############..",
    "................",
    "..############..",
    "................",
    "................",
    "................"
};

static const char *const icon_number[16] = {
    "................",
    "..b.............",
    ".bb...########..",
    "..b.............",
    "..b...########..",
    "................",
    ".bbb............",
    "...b..########..",
    ".bbb............",
    ".b....########..",
    ".bbb............",
    "................",
    ".bbb..########..",
    "..bb............",
    ".bbb..########..",
    "................"
};

static const char *const icon_bullet[16] = {
    "................",
    "................",
    ".bb...########..",
    ".bb.............",
    "......########..",
    "................",
    "................",
    ".bb...########..",
    ".bb.............",
    "......########..",
    "................",
    "................",
    ".bb...########..",
    ".bb.............",
    "......########..",
    "................"
};

static const char *const icon_outdent[16] = {
    "................",
    "................",
    "..############..",
    "................",
    "........######..",
    "....b...........",
    "...bb...######..",
    "..bbbbb.........",
    "...bb...######..",
    "....b...........",
    "........######..",
    "................",
    "..############..",
    "................",
    "................",
    "................"
};

static const char *const icon_indent[16] = {
    "................",
    "................",
    "..############..",
    "................",
    "........######..",
    "....b...........",
    "....bb..######..",
    "..bbbbb.........",
    "....bb..######..",
    "....b...........",
    "........######..",
    "................",
    "..############..",
    "................",
    "................",
    "................"
};

/* a button: its picture, its command, and whether a gap comes before it */
const TOOLBTN tool_buttons[] = {
    { icon_new, CMD_NEW, 0 },           { icon_open, CMD_OPEN, 0 },
    { icon_save, CMD_SAVE, 0 },         { icon_print, CMD_PRINT, 0 },
    { icon_cut, CMD_CUT, 1 },           { icon_copy, CMD_COPY, 0 },
    { icon_paste, CMD_PASTE, 0 },       { icon_undo, CMD_UNDO, 1 },
    { icon_redo, CMD_REDO, 0 },         { icon_bold, CMD_BOLD, 1 },
    { icon_italic, CMD_ITALIC, 0 },     { icon_under, CMD_UNDERLINE, 0 },
    { icon_left, CMD_LEFT, 1 },         { icon_center, CMD_CENTER, 0 },
    { icon_right, CMD_RIGHT, 0 },       { icon_justify, CMD_JUSTIFY, 0 },
    { icon_number, CMD_NUMBERING, 1 },  { icon_bullet, CMD_BULLETS, 0 },
    { icon_outdent, CMD_OUTDENT, 0 },   { icon_indent, CMD_INDENT, 0 }
};

const int tool_count = sizeof( tool_buttons ) / sizeof( tool_buttons[0] );
