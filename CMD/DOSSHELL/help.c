/*
 * HELP.C - the help viewer and what it says.
 *
 * EDIT's viewer.  A topic is lines of text.  A line that starts with
 * '@' and a letter is a link: the letter names the topic it leads to
 * ('A' is the first) and the rest is what it says.  Tab and Shift+Tab
 * move between the links, Enter follows one, Backspace goes back, Esc
 * closes the viewer; a click on a link follows it and a click outside
 * the box closes it.
 */
#include "shell.h"

static const char *const topic_index[] = {
    "PM-DOS Shell help",
    "",
    "Move to a subject with Tab and read it with Enter, or click it.",
    "Backspace comes back here; Esc closes help.",
    "",
    "@CShell basics: the parts of the screen",
    "@BThe keyboard",
    "@DThe menu commands",
    "@EHow to do the usual things",
    "@GFiles and directories",
    "@HPrograms and groups",
    "@IThe display: text, graphics and colors",
    "@JDialog boxes",
    "@KThe file viewer",
    "@FUsing help",
    NULL
};

static const char *const topic_keys[] = {
    "The keyboard",
    "",
    "  Tab, Shift+Tab     to the next or previous part of the screen",
    "  F10 or Alt         the menu bar; Alt+letter opens a menu",
    "  Arrow keys         through a list",
    "  PgUp, PgDn         a screenful up or down",
    "  Home, End          the top or the bottom of a list",
    "  A letter           the next name that starts with it",
    "  Ctrl+letter        show that drive",
    "",
    "In the directory tree:",
    "  +                  show the directories inside this one",
    "  *                  show everything below this one",
    "  Ctrl+*             show every directory on the drive",
    "  -                  hide what is inside this one",
    "",
    "In a list of files:",
    "  Shift+arrows       select a run of files",
    "  Shift+F8           Add mode on or off: the arrows move without",
    "                     selecting, and Space selects",
    "  Space              select the file, or let it go",
    "  Ctrl+/  Ctrl+\\     select every file; select none",
    "  Enter              open the file",
    "  F7, F8, Del        move, copy, delete",
    "  F9                 look inside the file",
    "",
    "Anywhere:",
    "  F1                 help",
    "  F5                 read the disk again",
    "  Shift+F5           draw the screen again",
    "  Shift+F9           the command prompt",
    "  F3 or Alt+F4       leave the shell",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_basics[] = {
    "Shell basics: the parts of the screen",
    "",
    "From the top: the title, the menu bar, and then one or two file",
    "windows and the program list, depending on the View menu.",
    "",
    "A file window is a line with the directory's path, a line with",
    "the drives, the directory tree on the left and the files of the",
    "selected directory on the right.  Choose a drive and the tree is",
    "that drive's; choose a directory and the files are that",
    "directory's.",
    "",
    "The program list holds programs you start by name, sorted into",
    "groups.  Main is the top group.",
    "",
    "One part of the screen has the keys at a time: its title bar is",
    "lit.  Tab moves on to the next part, and a click goes straight to",
    "the part clicked.  The File menu changes with it: among files it",
    "works on files, and in the program list it works on programs.",
    "",
    "The bottom line shows the time, and says what a menu command",
    "does while the menus are open.",
    "",
    "@BThe keyboard",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_commands[] = {
    "The menu commands",
    "",
    "File, among files:",
    "  Open               runs a program, or opens a file with the",
    "                     program its extension is tied to",
    "  Run                runs a command you type",
    "  Print              sends the selected files to the printer",
    "  Associate          ties an extension to a program",
    "  Search             lists the files that match a name",
    "  View File Contents shows a file as text or in hexadecimal",
    "  Move, Copy         the selected files to another place",
    "  Delete             the selected files, or an empty directory",
    "  Rename             a file or a directory",
    "  Change Attributes  hidden, system, archive, read only",
    "  Create Directory   inside the selected directory",
    "  Select All, Deselect All",
    "",
    "File, in the program list:",
    "  New, Open, Copy, Delete, Properties, Reorder, Run",
    "",
    "Options:",
    "  Confirmation       what is asked before it is done",
    "  File Display Options  which files show, and their order",
    "  Select Across Directories  files stay selected when you",
    "                     look at another directory",
    "  Show Information   the file, the selection, the directory",
    "                     and the disk",
    "  Display, Colors    the screen mode and the color scheme",
    "",
    "View: one file window, two, every file on the drive in one",
    "list, files with programs, or programs alone; Repaint Screen",
    "and Refresh.",
    "",
    "Tree: expand one level, a branch or everything; collapse.",
    "",
    "@GFiles and directories",
    "@HPrograms and groups",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_procs[] = {
    "How to do the usual things",
    "",
    "Copy files to another directory:",
    "  Select the files, press F8, type where they go, press Enter.",
    "  With a mouse: drag them onto the directory in the tree, or",
    "  onto a drive.  Dragging to the same drive moves and to",
    "  another drive copies; hold Ctrl to copy, Alt to move.",
    "",
    "Select several files:",
    "  Hold Shift and press the arrows; or press Shift+F8 and mark",
    "  each one with Space.  With a mouse, Ctrl+click adds a file",
    "  and Shift+click takes everything up to the one clicked.",
    "",
    "Find a file:",
    "  File, Search, and a name; * and ? stand for anything.  The",
    "  answer is a list you can work on like any other.  Esc, or",
    "  choosing a directory, puts the directory's files back.",
    "",
    "Start a program:",
    "  Press Enter on the program's file, or on its entry in the",
    "  program list; or File, Run and type the command.",
    "",
    "Add a program to the list:",
    "  Tab to the program list, File, New, Program Item, and give",
    "  the entry a title and the command that title runs.",
    "",
    "Work at the command prompt:",
    "  Shift+F9.  Type EXIT to come back.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_using[] = {
    "Using help",
    "",
    "F1 opens help anywhere; in a dialog box F1 or the Help button",
    "opens the subject that belongs to that box.",
    "",
    "  Tab, Shift+Tab     to the next or previous subject on the page",
    "  Enter              read the subject",
    "  Up, Down           scroll a line",
    "  PgUp, PgDn         scroll a screenful",
    "  Backspace          the page you came from",
    "  Esc                close help",
    "",
    "With a mouse, click a subject to read it and click outside the",
    "help box to close help.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_files[] = {
    "Files and directories",
    "",
    "Names can be long ones where the disk and the kernel keep them;",
    "a name that does not fit in the list is cut short on the screen",
    "and nowhere else.",
    "",
    "A command works on the selected files.  With none selected a",
    "command works on the file the keys are on; with the keys in the",
    "tree, Delete and Rename work on the directory.",
    "",
    "Move and Copy ask where to.  Type a directory to put the files",
    "there under their own names, or - for one file - a new name.",
    "A copy keeps the file's date, time and attributes.",
    "",
    "Delete removes a directory only when the directory is empty.",
    "",
    "Before deleting, before writing over a file, and before acting",
    "on a drag of the mouse, the shell asks - unless Options,",
    "Confirmation says not to.",
    "",
    "Options, File Display Options chooses which files are listed:",
    "a name with * and ?, whether hidden and system files show, and",
    "what the list is sorted by.",
    "",
    "@EHow to do the usual things",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_programs[] = {
    "Programs and groups",
    "",
    "An entry in the program list is a title and the commands the",
    "title runs.  Several commands go on one line with a semicolon",
    "between them.",
    "",
    "Where the commands say %1, %2 or %3 the shell asks for something",
    "to put there each time the entry is opened.  File, Properties",
    "sets what the question looks like: the box's title, a line of",
    "advice, the words before the field and what the field starts",
    "with.",
    "",
    "  Startup Directory  where the commands run; empty means the",
    "                     directory showing in the file window",
    "  Pause after exit   wait for a key before the shell comes back,",
    "                     so the program's last screen can be read",
    "  Password           asked for before the entry opens or changes",
    "  Help Text          what F1 says when the keys are on the entry",
    "",
    "A group holds entries and other groups.  Enter opens a group;",
    "the first line inside leads back out, and so does Esc.  A group",
    "has to be empty before Delete takes it away.",
    "",
    "File, Reorder moves an entry: the arrows carry the entry up and",
    "down, and Enter leaves it there.",
    "",
    "The list is kept in DOSSHELL.INI, beside DOSSHELL.EXE.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_display[] = {
    "The display: text, graphics and colors",
    "",
    "Options, Display lists the screens this machine can show:",
    "",
    "  Text       25 lines, or 50 (43 on an EGA)",
    "  Graphics   EGA, 640x350 in 16 colors: 25 or 43 lines",
    "  Graphics   VGA, 640x480 in 16 colors: 30, 34 or 60 lines",
    "  Graphics   640x480 in 256 colors: 30, 34 or 60 lines.",
    "             Listed when the video BIOS has VBE 2.0 or later",
    "             and a linear frame buffer for that mode.",
    "",
    "Preview shows a mode without choosing it.  On a graphics screen",
    "the lists have icons and the mouse has an arrow; everything",
    "else works as it does in text.",
    "",
    "Options, Colors lists the color schemes, with a Preview of its",
    "own.",
    "",
    "The shell starts the way it was left.  DOSSHELL /T starts in",
    "text and /G in graphics whatever was saved; DOSSHELL /? lists",
    "the switches.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_dialogs[] = {
    "Dialog boxes",
    "",
    "  Tab, Shift+Tab    to the next or previous part of the box",
    "  Alt+letter        straight to the part with that letter marked",
    "  Arrow keys        through a list, or between option buttons",
    "  Space             marks a check box or presses a button",
    "  Enter             the button with the focus, or the main one",
    "  Esc               closes the box, changing nothing",
    "  F1                help about the box",
    "",
    "In a field, what is there to start with is selected: typing",
    "replaces it, and an arrow key keeps it to be changed.",
    "",
    "With a mouse, click what you want; a button is pressed when the",
    "mouse button is let go over it, and a double click in a list is",
    "the same as Enter.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const topic_viewer[] = {
    "The file viewer",
    "",
    "File, View File Contents - or F9 - shows the file the keys are",
    "on.  Nothing in the file can be changed there.",
    "",
    "  Arrows, PgUp, PgDn   scroll",
    "  Home, End            the start or the end of the file",
    "  F9                   text, or hexadecimal: sixteen bytes a",
    "                       line with their place in the file",
    "  Esc or Enter         close the viewer",
    "",
    "In text, a line too long for the screen goes on to the next",
    "line of the screen.",
    "",
    "@AThe list of subjects",
    NULL
};

static const char *const *const topics[HELP_TOPICS] = {
    topic_index, topic_keys, topic_basics, topic_commands, topic_procs, topic_using,
    topic_files, topic_programs, topic_display, topic_dialogs, topic_viewer
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
            dlg_frame( top, left, bottom - top + 1, right - left + 1, "Help" );
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
                              line == link ? pal.select : pal.dlg_hot );
                } else {
                    scr_putn( top + 2 + row, left + 2, text,
                              (int)str_len( text ) < cols ? (int)str_len( text ) : cols,
                              line == 0 ? pal.dlg_hot : pal.dlg );
                }
            }
            if ( count > rows ) {
                sbar_draw( top + 2, top + 1 + rows, right, first, rows, count );
            }
            scr_cursor( 0, 0, CUR_HIDE );
            scr_flush();
            key = key_get();
            if ( key_service( key ) ) {
                continue;
            }
            if ( key == K_ESC ) {
                app_status( NULL );
                return;
            }
            if ( key == K_MOUSE ) {
                /* a subject pressed is a subject read; a press outside
                   the box leaves */
                if ( mouse.kind != ME_DOWN && mouse.kind != ME_DOUBLE ) {
                    continue;
                }
                if ( mouse.row < top || mouse.row > bottom || mouse.col < left || mouse.col > right ) {
                    app_status( NULL );
                    return;
                }
                if ( mouse.col == right && count > rows ) {
                    switch ( sbar_hit( top + 2, top + 1 + rows, right, first, rows, count, mouse.row ) ) {
                    case SB_UP:   key = K_UP;   break;
                    case SB_DOWN: key = K_DOWN; break;
                    case SB_PGUP: key = K_PGUP; break;
                    case SB_PGDN: key = K_PGDN; break;
                    }
                } else {
                    line = first + mouse.row - (top + 2);
                    if ( mouse.row < top + 2 || mouse.row >= top + 2 + rows || line >= count
                         || topic[line][0] != '@' ) {
                        continue;
                    }
                    link = line;
                    key = K_ENTER;
                }
            }
            key &= ~K_SHIFT;
            if ( key == K_TAB ) {
                link = next_link( topic, count, link >= 0 ? link : first - 1, 1 );
            } else if ( key == K_SHTAB ) {
                link = next_link( topic, count, link >= 0 ? link : first + rows, -1 );
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
