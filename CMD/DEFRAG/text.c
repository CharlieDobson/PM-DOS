/*
 * TEXT.C - every sentence DEFRAG says.
 *
 * The screens are MS-DOS DEFRAG's: what is where, which words are on
 * the buttons and in the menu.  The sentences are this program's own.
 */
#include "defrag.h"
#include "text.h"

const char help_text[] =
    "Defragments the files on a disk and moves them to the start of the disk.\r\n"
    "\r\n"
    "DEFRAG [drive:] [/F] [/S[:]order] [/H] [/B] [/G]\r\n"
    "DEFRAG [drive:] [/U] [/H] [/B] [/G]\r\n"
    "\r\n"
    "  drive:  Drive to optimize.\r\n"
    "  /F      Full optimization.  Puts every file in one piece and leaves\r\n"
    "          no free space between files.\r\n"
    "  /U      Unfragments files only.  Free space may stay between files.\r\n"
    "  /S      Sorts the files in each directory.  order is one of:\r\n"
    "            N  Name                 E  Extension\r\n"
    "            D  Date and time        S  Size\r\n"
    "          Add a minus sign to reverse the order (for example, /SN-).\r\n"
    "  /H      Moves hidden files.  System files are never moved.\r\n"
    "  /B      Restarts the computer when DEFRAG is finished.\r\n"
    "  /G      Uses a graphics screen.\r\n"
    "\r\n";

const char tx_program[] = "PM-DOS Defrag";

const char *const tx_method[3] = {
    "Full Optimization", "Unfragment Files Only", "No optimization necessary"
};
const char *const tx_method_about[2] = {
    "Every file in one piece, and no free space between files.",
    "Every file in one piece. Free space may stay between files."
};
const char *const tx_sort[5] = { "Unsorted", "Name", "Extension", "Date & Time", "Size" };
const char *const tx_order[2] = { "Ascending", "Descending" };

const char *const tx_fs_error[6] = {
    "the copies of the file allocation table differ",
    "a chain of clusters is broken",
    "files are cross-linked",
    "there are lost clusters",
    "a file's size does not match its clusters",
    "a directory is damaged"
};

const char tx_menu_title[] = "&Optimize";
const MENUTEXT tx_menu[MENU_ITEMS] = {
    { "&Begin optimization",     "Alt-B", "Start optimizing the current drive",           GO_BEGIN },
    { NULL,                      NULL,    NULL,                                           0 },
    { "&Drive...",               NULL,    "Choose the drive to optimize",                 GO_DRIVE },
    { "&Optimization Method...", NULL,    "Choose how thoroughly the drive is optimized", GO_METHOD },
    { NULL,                      NULL,    NULL,                                           0 },
    { "&File sort...",           NULL,    "Choose the order of the files in a directory", GO_SORT },
    { "&Map legend...",          NULL,    "Explain the symbols of the map",               GO_LEGEND },
    { NULL,                      NULL,    NULL,                                           0 },
    { "&About Defrag...",        NULL,    "Say what this program is",                     GO_ABOUT },
    { "e&Xit",                   "Alt-X", "Leave DEFRAG",                                 GO_EXIT }
};

const char tx_hint_menu[] = "Press Alt or F10 for the menu";
const char tx_hint_button[] = "Choose a button and press ENTER";
const char tx_hint_drive[] = "Choose a drive letter and then OK";
const char tx_hint_method[] = "Choose a method with the arrow keys and the space bar";
const char tx_hint_sort[] = "Choose with the arrow keys and the space bar";
const char tx_hint_legend[] = "What the symbols of the map stand for";
const char tx_hint_help[] = "Arrow keys and PgUp, PgDn move through the text";
const char tx_f1[] = "F1=Help";
const char tx_esc_stops[] = "Esc=Stop Defrag";

const char tx_status[] = "Status";
const char tx_legend[] = "Legend";
const char tx_cluster[] = "Cluster %,u";
const char tx_elapsed[] = "Elapsed Time: %02u:%02u:%02u";
const char tx_used[] = "Used";
const char tx_unused[] = "Unused";
const char tx_reading[] = "Reading";
const char tx_writing[] = "Writing";
const char tx_bad[] = "Bad";
const char tx_fixed[] = "Unmovable";
const char tx_scale[] = "Drive %c:   1 block = %,u cluster%s";

const char tx_act_read[] = "Reading...";
const char tx_act_write[] = "Writing...";
const char tx_act_sort[] = "Sorting...";
const char tx_act_fat[] = "Updating the file allocation table...";
const char tx_act_done[] = "Finished condensing";

const char tx_reading_drive[] = "Reading disk information...";
const char tx_analyzing[] = "Analyzing disk information...";

const char tx_recommend_title[] = "Recommendation";
const char tx_recommend_1[] = "%u%% of drive %c: is not fragmented.";
const char tx_recommend_2[] = "Recommended optimization method:";
const char tx_btn_optimize[] = "&Optimize";
const char tx_btn_configure[] = "&Configure";
const char tx_btn_ok[] = "OK";
const char tx_btn_cancel[] = "Cancel";
const char tx_btn_resume[] = "&Resume";
const char tx_btn_another[] = "&Another Drive";
const char tx_btn_exit[] = "&Exit DEFRAG";
const char tx_btn_scandisk[] = "&ScanDisk";
const char tx_btn_leave[] = "&Exit";

const char tx_drive_title[] = "Select the drive you wish to optimize.";
const char tx_method_title[] = "Select Optimization Method";
const char tx_sort_title[] = "File Sort";
const char tx_sort_about[] = "Choose how the files in each\ndirectory are to be sorted.";
const char tx_sort_by[] = "Sort Criterion";
const char tx_sort_order[] = "Sort Order";
const char tx_legend_title[] = "Disk Map Legend";
const char *const tx_legend_line[7] = {
    "Space used by files",
    "Space that has been optimized",
    "Unused space",
    "Space used by files that are not moved",
    "Bad space, which DEFRAG leaves alone",
    "Space that is being read",
    "Space that is being written"
};
const char tx_about_title[] = "About Defrag";
const char *const tx_about_line[3] = {
    "DEFRAG for PM-DOS",
    "Puts files in one piece and together,",
    "on FAT12, FAT16 and FAT32 drives"
};

const char tx_help_title[] = "Help";
const char tx_help[] =
    "\1What DEFRAG does\n"
    "Files that are written to often end up in pieces scattered over the "
    "drive, which makes them slower to read. DEFRAG puts each file back in "
    "one piece.\n"
    "\n"
    "\1Optimization methods\n"
    "Full Optimization puts every file in one piece and moves all files to "
    "the start of the drive, directories first, with no free space between "
    "them. This takes the longest.\n"
    "\n"
    "Unfragment Files Only moves just the files that are in pieces. This is "
    "quicker, but free space stays scattered between files.\n"
    "\n"
    "\1File sort\n"
    "Sorts the entries in each directory by name, extension, date and time, "
    "or size. Sorting is done during a Full Optimization only.\n"
    "\n"
    "\1Files that are not moved\n"
    "System files are never moved. Hidden files are moved only when DEFRAG "
    "is started with /H. These files show as X on the map. Bad blocks show "
    "as B.\n"
    "\n"
    "\1Drive errors\n"
    "DEFRAG checks the drive before moving anything. If the drive has "
    "errors, such as lost clusters or cross-linked files, DEFRAG offers to "
    "run ScanDisk and then reads the drive again.\n"
    "\n"
    "\1Stopping\n"
    "Press Esc to stop. DEFRAG finishes the piece being moved and asks "
    "whether to cancel. The drive is safe to use if you cancel.\n"
    "\n"
    "\1Keys\n"
    "Alt or F10 opens the menu. Alt+B begins optimization and Alt+X exits. "
    "In a dialog box, Tab moves between sections, the arrow keys move "
    "within a section, Space selects, Enter is OK and Esc is Cancel.";


const char tx_stop_1[] = "DEFRAG was interrupted";
const char tx_stop_2[] = "Do you want to cancel\nthe optimization?";
const char tx_finished[] = "Finished condensing";
const char tx_done_1[] = "Drive %c: has been optimized.";
const char tx_done_2[] = "You can optimize another drive,\nchange the settings for this drive,\nor leave DEFRAG.";

const char tx_error_title[] = "Error";
const char tx_fs_found[] = "Drive %c: has errors:";
const char tx_fs_offer[] =
    "The drive needs to be repaired before it can be optimized. "
    "ScanDisk can repair them now.";
const char tx_fs_again[] = "Drive %c: still has errors:";
const char tx_no_scandisk[] =
    "SCANDISK.EXE was not found.";
const char tx_scandisk_failed[] = "%s could not be started (error %u).";

const char tx_bad_drive[] = "Invalid drive specification - %c:";
const char tx_not_local[] = "Drive %c: is a network drive, a CD-ROM or a SUBST and cannot be optimized.";
const char tx_exfat[] = "Drive %c: is exFAT, which DEFRAG does not optimize.";
const char tx_cannot_read[] = "Drive %c: cannot be read, or is not formatted for DOS.";
const char tx_in_use[] = "Drive %c: is in use and cannot be locked.";
const char tx_redirected[] =
    "DEFRAG cannot run with its input or output redirected to a file.";
const char tx_test_same_drive[] =
    "Drive %c: holds the test log and cannot be the drive optimized.";
const char tx_no_free[] =
    "Drive %c: has no free clusters. DEFRAG needs at least one to work with.";
const char tx_read_error[] = "Cluster %,u of drive %c: could not be read. DEFRAG has stopped.";
const char tx_write_error[] = "Cluster %,u of drive %c: could not be written. DEFRAG has stopped.";
const char tx_system_error[] =
    "A directory or the file allocation table of drive %c: could not be "
    "written. DEFRAG has stopped.";
const char tx_internal[] = "DEFRAG met something it did not expect on drive %c: and has stopped.";
const char tx_no_memory[] = "Insufficient memory";
const char tx_no_restart[] = "The computer could not be restarted.";

const char tx_bad_switch[] = "Invalid switch";
const char tx_too_many[] = "Too many parameters";
const char tx_bad_order[] = "Invalid sort order";
const char tx_both_methods[] = "/F and /U cannot be used together";
