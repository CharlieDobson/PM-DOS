/*
 * HELP.C - the help viewer and what it says.
 *
 * A topic is lines of text.  A line that starts with '@' and a letter
 * is a link: the letter names the topic it leads to ('A' is the first)
 * and the rest is what it says.  Tab and Shift+Tab move between the
 * links, Enter follows one, Backspace goes back, Esc closes the viewer;
 * a click on a link follows it and a click outside the box closes it.
 */
#include "edit.h"

static const char *const topic_index[] = {
    "The editor's help",
    "",
    "Move to a subject with Tab and read it with Enter, or click it.",
    "Backspace comes back here; Esc closes help.",
    "",
    "@BKeys: moving around and editing",
    "@DSelecting text, and the clipboard",
    "@CThe menus",
    "@EDialog boxes",
    "@FStarting the editor: the command line",
    "@GOpening files",
    "@HSaving files",
    "@IFinding text",
    "@JChanging text",
    "@KPrinting",
    "@LSettings",
    "@MColours",
    "@NThe mouse",
    NULL
};

static const char *const topic_keys[] = {
    "Keys: moving around and editing",
    "",
    "  Arrow keys         a character or a line at a time",
    "  Ctrl+Left, Right   a word at a time",
    "  Home, End          the start or end of the line",
    "  PgUp, PgDn         a window's height up or down",
    "  Ctrl+Home, End     the start or end of the file",
    "  Ctrl+Up, Down      scroll a line without moving the cursor",
    "  Ctrl+PgUp, PgDn    scroll left or right a window's width",
    "",
    "  Ins                insert or type over: the cursor is a line",
    "                     when inserting and a block when typing over",
    "  Del, Backspace     delete the character under or before the cursor",
    "  Enter              start a new line, indented as this one is",
    "  Tab, Shift+Tab     to the next or previous tab stop; with lines",
    "                     selected, indent or unindent all of them",
    "  Ctrl+Y             delete the line (it goes to the clipboard)",
    "  Ctrl+Q then Y      delete to the end of the line",
    "  Ctrl+T             delete to the start of the next word",
    "  Ctrl+N             break the line at the cursor, staying put",
    "  Ctrl+P then a key  type a control character",
    "",
    "  F1                 help         F6       the other window",
    "  F3                 find again   F4       the screen from before",
    "  F10 or Alt         the menus    Ctrl+F6  split the window",
    "",
    "The WordStar keys work too: Ctrl+E, X, S, D move up, down, left and",
    "right; Ctrl+R, C a page; Ctrl+A, F a word; Ctrl+W, Z scroll; Ctrl+Q",
    "then S, D, R, C, E, X for the line's ends, the file's ends and the",
    "window's top and bottom; Ctrl+K then 0 to 3 sets a bookmark, and",
    "Ctrl+Q then 0 to 3 goes back to it; Ctrl+Q then F finds, then A",
    "changes; Ctrl+L finds again; Ctrl+G deletes; Ctrl+V is Ins.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_menus[] = {
    "The menus",
    "",
    "Press Alt, or F10, and the menu bar lights up; press Alt with a",
    "menu's highlighted letter to open that menu at once.  Arrow keys",
    "move, Enter chooses, the highlighted letter chooses, Esc leaves.",
    "Items in grey cannot be chosen just then.",
    "",
    "  File      New, Open, Save, Save As, Close, Print, Exit",
    "  Edit      Cut, Copy, Paste and Clear, for selected text",
    "  Search    Find, Repeat Last Find, and Replace",
    "  View      Split Window, Size Window, Close Window, Output",
    "            Screen, and the files that are open: up to nine,",
    "            each chosen by its number",
    "  Options   Settings (tab stops, the printer) and Colors",
    "  Help      this help, and the editor's version",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_select[] = {
    "Selecting text, and the clipboard",
    "",
    "Hold Shift and move the cursor with any of the moving keys: the",
    "text it passes over is selected.  Moving without Shift lets go.",
    "Typing, Paste or Enter replace what is selected; Del deletes it.",
    "",
    "  Shift+Del    Cut: the selection to the clipboard",
    "  Ctrl+Ins     Copy: a copy of the selection to the clipboard",
    "  Shift+Ins    Paste: the clipboard in at the cursor",
    "",
    "The clipboard holds one piece of text at a time, and keeps it",
    "while you move between files, so text can go from one to another.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_dialogs[] = {
    "Dialog boxes",
    "",
    "  Tab, Shift+Tab    to the next or previous part of the box",
    "  Alt+letter        straight to the part with that letter lit",
    "  Arrow keys        through a list, or between option buttons",
    "  Space             ticks a check box or presses a button",
    "  Enter             the button with the focus, or the main one",
    "  Esc               closes the box, changing nothing",
    "  F1                help about the box",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_cmdline[] = {
    "Starting the editor: the command line",
    "",
    "  EDIT [/B] [/H] [/R] [/S] [/nnn] [file...]",
    "",
    "  /B     black and white, for a monochrome screen",
    "  /H     as many lines as the display can show: 43 or 50",
    "  /R     the files are opened read-only",
    "  /S     files are shown by their short 8.3 names",
    "  /nnn   binary files, shown nnn bytes to a line (10 to 1024); they",
    "         are saved with nothing added between the lines",
    "  file   up to nine files, and wildcards can name several; a name",
    "         that does not exist yet is a new file with that name",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_open[] = {
    "Opening files",
    "",
    "Type a name, or choose one from Files.  A name with wildcards in",
    "it (*.BAT) shows those files instead; a directory's name, or one",
    "from Dirs/Drives, goes there.  Each file opened keeps its place;",
    "the View menu lists them, and nine can be open at once.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_save[] = {
    "Saving files",
    "",
    "Save writes the file under its own name; a new file is asked for",
    "a name first.  Save As writes it under a new name, which then",
    "becomes its name, and asks before it replaces another file.",
    "Lines are saved with CR LF between them, and nothing else is",
    "changed: tabs are kept as tabs.",
    "",
    "When you close a file or leave with changes not saved, the editor",
    "asks whether to save them.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_find[] = {
    "Finding text",
    "",
    "Type what to look for; the word at the cursor is offered.  Match",
    "Upper/Lowercase makes \"Cat\" and \"cat\" different; Whole Word",
    "skips it inside longer words.  The search starts at the cursor and",
    "goes round from the end to the top.  F3 finds the next one.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_replace[] = {
    "Changing text",
    "",
    "Type what to look for and what to put in its place.  Find and",
    "Verify stops at each one and asks: Change it, Skip it, or Cancel",
    "the rest.  Change All changes every one without asking.  Either",
    "way it starts at the cursor and goes all the way round the file.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_print[] = {
    "Printing",
    "",
    "Prints the whole file, or only the selected text, on the printer",
    "port chosen in Options, Settings.  A page ends the printing.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_settings[] = {
    "Settings",
    "",
    "Tab Stops is how many columns apart the tab stops are: where a",
    "tab in a file reaches, and where Tab moves the cursor.  Print Port",
    "is the printer: PRN, one of three parallel ports, or a serial one.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_colors[] = {
    "Colours",
    "",
    "Choose the part of the screen - the text, the menus and dialogs,",
    "or the status bar - and a foreground and background for it.  The",
    "colours last until the editor is left.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_mouse[] = {
    "The mouse",
    "",
    "Click in the text to put the cursor there.  Drag across text to",
    "select it, or click at one end and Shift+click at the other.  A",
    "double click selects a word.",
    "",
    "On a scroll bar, the arrows move a line or a column at a time for",
    "as long as the button is held down; clicking beside the scroll box",
    "moves a page, and the box itself can be dragged.",
    "",
    "Click a menu's name to open it and an item to choose it, or press",
    "the button on the name and let it go on the item.  In a dialog box",
    "a click uses whatever it lands on; a double click in a list is the",
    "same as Enter.",
    "",
    "With two windows, a click in either one makes it active, and the",
    "lower window's title bar can be dragged to move the line between",
    "them.  F1, F10 and F6 on the status bar can be clicked as well.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const *const topics[HELP_TOPICS] = {
    topic_index, topic_keys, topic_menus, topic_select, topic_dialogs,
    topic_cmdline, topic_open, topic_save, topic_find, topic_replace,
    topic_print, topic_settings, topic_colors, topic_mouse
};

static int count_lines( const char *const *topic )
{
    int count = 0;

    while ( topic[count] ) {
        count++;
    }
    return count;
}

static int next_link( const char *const *topic, int count, int from, int step )
{
    int line = from, tries;

    for ( tries = 0; tries < count; tries++ ) {
        line += step;
        if ( line < 0 ) {
            line = count - 1;
        }
        if ( line >= count ) {
            line = 0;
        }
        if ( topic[line][0] == '@' ) {
            return line;
        }
    }
    return -1;
}

void help_show( int topic_no )
{
    int history[16], depth = 0;
    int top = 2, left = 3, bottom = scr_rows - 3, right = scr_cols - 4;
    int rows = bottom - top - 3, cols = right - left - 3;
    int first = 0, link, count, row, key, line;
    const char *const *topic;
    const char *text;

    if ( topic_no < 0 || topic_no >= HELP_TOPICS ) {
        topic_no = HELP_INDEX;
    }
    for ( ;; ) {
        topic = topics[topic_no];
        count = count_lines( topic );
        link = next_link( topic, count, -1, 1 );
        if ( link >= rows ) {
            link = -1;                      /* start at the top; Tab finds it */
        }
        first = 0;
        for ( ;; ) {
            if ( link >= 0 ) {
                if ( link < first ) {
                    first = link;
                }
                if ( link >= first + rows ) {
                    first = link - rows + 1;
                }
            }
            app_status( "Tab=Next Subject   Enter=Read It   Backspace=Back   Esc=Close Help" );
            ui_redraw();
            scr_box( top, left, bottom, right, pal.dlg );
            scr_shadow( top, left, bottom, right );
            scr_put( top, left + (right - left - 6) / 2, " Help ", pal.dlg );
            for ( row = 0; row < rows; row++ ) {
                line = first + row;
                if ( line >= count ) {
                    break;
                }
                text = topic[line];
                if ( text[0] == '@' ) {
                    scr_ch( top + 2 + row, left + 2, 0xAF, pal.dlg );
                    scr_putn( top + 2 + row, left + 4, text + 2,
                              (int)str_len( text + 2 ) < cols - 2 ? (int)str_len( text + 2 ) : cols - 2,
                              line == link ? pal.field_sel : pal.dlg_hot );
                } else {
                    scr_putn( top + 2 + row, left + 2, text,
                              (int)str_len( text ) < cols ? (int)str_len( text ) : cols,
                              line == 0 ? pal.dlg_hot : pal.dlg );
                }
            }
            scr_cursor( 0, 0, CUR_HIDE );
            scr_flush();
            key = key_get();
            if ( key == K_EOF ) {
                app_abandon();
            }
            if ( key == K_DUMP ) {
                scr_dump();
                continue;
            }
            if ( key == K_ESC ) {
                app_status( NULL );
                return;
            }
            if ( key == K_MOUSE ) {
                /* a subject pressed is a subject read; nothing else in
                   the box does anything, and a press outside it leaves */
                if ( mouse.kind != ME_DOWN && mouse.kind != ME_DOUBLE ) {
                    continue;
                }
                if ( mouse.row < top || mouse.row > bottom || mouse.col < left || mouse.col > right ) {
                    app_status( NULL );
                    return;
                }
                line = first + mouse.row - (top + 2);
                if ( mouse.row < top + 2 || mouse.row >= top + 2 + rows || line >= count
                     || topic[line][0] != '@' ) {
                    continue;
                }
                link = line;
                key = K_ENTER;
            }
            if ( key == K_TAB && link >= 0 ) {
                link = next_link( topic, count, link, 1 );
            } else if ( key == K_SHTAB && link >= 0 ) {
                link = next_link( topic, count, link, -1 );
            } else if ( key == K_DOWN ) {
                if ( first + rows < count ) {
                    first++;
                }
                link = -1;
            } else if ( key == K_UP ) {
                if ( first > 0 ) {
                    first--;
                }
                link = -1;
            } else if ( key == K_PGDN ) {
                first += rows - 1;
                if ( first > count - rows ) {
                    first = count > rows ? count - rows : 0;
                }
                link = -1;
            } else if ( key == K_PGUP ) {
                first = first > rows - 1 ? first - (rows - 1) : 0;
                link = -1;
            } else if ( key == K_ENTER && link >= 0 ) {
                if ( depth < 16 ) {
                    history[depth++] = topic_no;
                }
                topic_no = topic[link][1] - 'A';
                if ( topic_no < 0 || topic_no >= HELP_TOPICS ) {
                    topic_no = HELP_INDEX;
                }
                break;
            } else if ( key == K_BS ) {
                topic_no = depth ? history[--depth] : HELP_INDEX;
                break;
            }
        }
    }
}
