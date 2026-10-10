/*
 * ICONS.C - the pictures a graphics screen puts beside names, and the
 * mouse pointer.
 *
 * AN ICON IS SIXTEEN STRINGS, a character a pixel, as wide as the
 * cells it takes: 8, 16 or 32 pixels.  '.' is the cell's background
 * showing through and '#' the cell's own text colour, so an arrow or a
 * check box follows the colour scheme; a letter is one of the sixteen
 * colours, whatever the scheme (GFX.C's icon_color has the list - k is
 * black, W white, w and d the greys, y yellow, small letters the dark
 * colours and capitals the bright ones).
 *
 * THEY ARE DRAWN FOR A 16-LINE CELL.  A 14-line cell leaves out the
 * first and the last line, and an 8-line cell shows the odd lines
 * only - so every line that matters to a shape is an odd one: the top
 * and bottom of a box, the bar of a plus sign.
 */
#include "shell.h"

static const char *const dir_rows[16] = {
    "................",
    "................",
    "................",
    "..kkkkk.........",
    ".kyyyyyk........",
    ".kyyyyyykkkkkkk.",
    ".kyyyyyyyyyyyyk.",
    ".kyyyyyyyyyyyyk.",
    ".kyyyyyyyyyyyyk.",
    ".kyyyyyyyyyyyyk.",
    ".kyyyyyyyyyyyyk.",
    ".kyyyyyyyyyyyyk.",
    ".kyyyyyyyyyyyyk.",
    ".kkkkkkkkkkkkkk.",
    "................",
    "................"
};

static const char *const dir_plus_rows[16] = {
    "................",
    "................",
    "................",
    "..kkkkk.........",
    ".kyyyyyk........",
    ".kyyyyyykkkkkkk.",
    ".kyyyyyyyyyyyyk.",
    ".kyyyyykkyyyyyk.",
    ".kyyykkkkkkyyyk.",
    ".kyyykkkkkkyyyk.",
    ".kyyyyykkyyyyyk.",
    ".kyyyyykkyyyyyk.",
    ".kyyyyyyyyyyyyk.",
    ".kkkkkkkkkkkkkk.",
    "................",
    "................"
};

static const char *const dir_minus_rows[16] = {
    "................",
    "................",
    "................",
    "..kkkkk.........",
    ".kyyyyyk........",
    ".kyyyyyykkkkkkk.",
    ".kyyyyyyyyyyyyk.",
    ".kyyyyyyyyyyyyk.",
    ".kyyykkkkkkyyyk.",
    ".kyyykkkkkkyyyk.",
    ".kyyyyyyyyyyyyk.",
    ".kyyyyyyyyyyyyk.",
    ".kyyyyyyyyyyyyk.",
    ".kkkkkkkkkkkkkk.",
    "................",
    "................"
};

static const char *const file_rows[16] = {
    "................",
    "..kkkkkkkk......",
    "..kWWWWWWkk.....",
    "..kWWWWWWkWk....",
    "..kWWWWWWkkkk...",
    "..kWddddWWWWk...",
    "..kWWWWWWWWWk...",
    "..kWdddddddWk...",
    "..kWWWWWWWWWk...",
    "..kWdddddddWk...",
    "..kWWWWWWWWWk...",
    "..kWdddddWWWk...",
    "..kWWWWWWWWWk...",
    "..kkkkkkkkkkk...",
    "................",
    "................"
};

static const char *const prog_rows[16] = {
    "................",
    "................",
    "................",
    ".kkkkkkkkkkkkkk.",
    ".kBBBBBBBBBBBBk.",
    ".kkkkkkkkkkkkkk.",
    ".kWWWWWWWWWWWWk.",
    ".kWWWWWWWWWWWWk.",
    ".kWWWWWWWWWWWWk.",
    ".kWWWWWWWWWWWWk.",
    ".kWWWWWWWWWWWWk.",
    ".kWWWWWWWWWWWWk.",
    ".kWWWWWWWWWWWWk.",
    ".kkkkkkkkkkkkkk.",
    "................",
    "................"
};

static const char *const group_rows[16] = {
    "................",
    "................",
    "................",
    ".kkkkkkkkkkkkkk.",
    ".kwwwwwwwwwwwwk.",
    ".kkkkkkkkkkkkkk.",
    ".kWWWWWWWWWWWWk.",
    ".kWrrWWbbWWggWk.",
    ".kWrrWWbbWWggWk.",
    ".kWWWWWWWWWWWWk.",
    ".kWyyWWccWWmmWk.",
    ".kWyyWWccWWmmWk.",
    ".kWWWWWWWWWWWWk.",
    ".kkkkkkkkkkkkkk.",
    "................",
    "................"
};

static const char *const item_rows[16] = {
    "................",
    "................",
    "................",
    ".kkkkkkkkkkkkkk.",
    ".kccccccccccckk.",
    ".kkkkkkkkkkkkkk.",
    ".kWWWWWWWWWWWWk.",
    ".kWkkkWWWWWWWWk.",
    ".kWWWWWWWWWWWWk.",
    ".kWkkkkkkWWWWWk.",
    ".kWWWWWWWWWWWWk.",
    ".kWkkkkWWWWWWWk.",
    ".kWWWWWWWWWWWWk.",
    ".kkkkkkkkkkkkkk.",
    "................",
    "................"
};

static const char *const floppy_rows[16] = {
    "................................",
    "................................",
    "................................",
    "..kkkkkkkkkkkkkkkkkkkkkkkkkkkk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwkkkkkkkkkkkkkkkkkkkkwwwwk..",
    "..kwwkddddddddddddddddddkwwwwk..",
    "..kwwkkkkkkkkkkkkkkkkkkkkwwwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwggwwwwwwwwwwwwwwkkkkkwwwk..",
    "..kwwggwwwwwwwwwwwwwwkkkkkwwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kkkkkkkkkkkkkkkkkkkkkkkkkkkk..",
    "................................",
    "................................"
};

static const char *const fixed_rows[16] = {
    "................................",
    "................................",
    "................................",
    "..kkkkkkkkkkkkkkkkkkkkkkkkkkkk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwddddddddddddddddddddwwwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwddddddddddddddddddddwwwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwRRwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwRRwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kkkkkkkkkkkkkkkkkkkkkkkkkkkk..",
    "................................",
    "................................"
};

static const char *const cdrom_rows[16] = {
    "................................",
    "................................",
    "................................",
    "..kkkkkkkkkkkkkkkkkkkkkkkkkkkk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwkkkkkkkkkkkkkkkkkkkkkkwwk..",
    "..kwwkWWWWWWWWWWWWWWWWWWWWkwwk..",
    "..kwwkkkkkkkkkkkkkkkkkkkkkkwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwyywwwwwwwwwwwwwwwkkkkwwwk..",
    "..kwwyywwwwwwwwwwwwwwwkkkkwwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kkkkkkkkkkkkkkkkkkkkkkkkkkkk..",
    "................................",
    "................................"
};

static const char *const other_rows[16] = {
    "................................",
    "................................",
    "................................",
    "..kkkkkkkkkkkkkkkkkkkkkkkkkkkk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwkkwwkkwwkkwwkkwwkkwwkkwwk..",
    "..kwwkkwwkkwwkkwwkkwwkkwwkkwwk..",
    "..kwwkkwwkkwwkkwwkkwwkkwwkkwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwGGwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwGGwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "..kkkkkkkkkkkkkkkkkkkkkkkkkkkk..",
    "................................",
    "................................"
};

static const char *const arrow_up_rows[16] = {
    "........",
    "........",
    "........",
    "...##...",
    "..####..",
    "..####..",
    ".######.",
    ".######.",
    "...##...",
    "...##...",
    "...##...",
    "...##...",
    "........",
    "........",
    "........",
    "........"
};

static const char *const arrow_down_rows[16] = {
    "........",
    "........",
    "........",
    "........",
    "...##...",
    "...##...",
    "...##...",
    "...##...",
    ".######.",
    ".######.",
    "..####..",
    "..####..",
    "...##...",
    "...##...",
    "........",
    "........"
};

static const char *const check_off_rows[16] = {
    "................",
    "................",
    "................",
    "..############..",
    "..#..........#..",
    "..#..........#..",
    "..#..........#..",
    "..#..........#..",
    "..#..........#..",
    "..#..........#..",
    "..#..........#..",
    "..#..........#..",
    "..#..........#..",
    "..############..",
    "................",
    "................"
};

static const char *const check_on_rows[16] = {
    "................",
    "................",
    "................",
    "..############..",
    "..#..........#..",
    "..#.##....##.#..",
    "..#..##..##..#..",
    "..#...####...#..",
    "..#....##....#..",
    "..#...####...#..",
    "..#..##..##..#..",
    "..#.##....##.#..",
    "..#..........#..",
    "..############..",
    "................",
    "................"
};

static const char *const radio_off_rows[16] = {
    "................",
    "................",
    "................",
    ".....######.....",
    "...##......##...",
    "..#..........#..",
    "..#..........#..",
    ".#............#.",
    ".#............#.",
    ".#............#.",
    "..#..........#..",
    "..#..........#..",
    "...##......##...",
    ".....######.....",
    "................",
    "................"
};

static const char *const radio_on_rows[16] = {
    "................",
    "................",
    "................",
    ".....######.....",
    "...##......##...",
    "..#..........#..",
    "..#...####...#..",
    ".#...######...#.",
    ".#...######...#.",
    ".#...######...#.",
    "..#...####...#..",
    "..#..........#..",
    "...##......##...",
    ".....######.....",
    "................",
    "................"
};

static const char *const none_rows[16] = {
    "........", "........", "........", "........", "........", "........",
    "........", "........", "........", "........", "........", "........",
    "........", "........", "........", "........"
};

/* in IC_* order */
const ICON icons[IC_COUNT] = {
    {  8, none_rows },
    { 16, dir_rows },
    { 16, dir_plus_rows },
    { 16, dir_minus_rows },
    { 16, file_rows },
    { 16, prog_rows },
    { 16, group_rows },
    { 16, item_rows },
    { 32, floppy_rows },
    { 32, fixed_rows },
    { 32, cdrom_rows },
    { 32, other_rows },
    {  8, arrow_up_rows },
    {  8, arrow_down_rows },
    { 16, check_off_rows },
    { 16, check_on_rows },
    { 16, radio_off_rows },
    { 16, radio_on_rows }
};

/* the pointer: k its black edge, W its white inside */
const char *const pointer_shape[16] = {
    "k...............",
    "kk..............",
    "kWk.............",
    "kWWk............",
    "kWWWk...........",
    "kWWWWk..........",
    "kWWWWWk.........",
    "kWWWWWWk........",
    "kWWWWWWWk.......",
    "kWWWWWWWWk......",
    "kWWWWWkkkkk.....",
    "kWWkWWk.........",
    "kWk.kWWk........",
    "kk..kWWk........",
    "k....kWWk.......",
    ".....kkkk......."
};
