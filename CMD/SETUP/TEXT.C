/*
 * TEXT.C - every word SETUP shows.
 *
 * The screens are laid out as MS-DOS 6.22 Setup's are, and the keys in
 * the status line are named as 6.22 named them, because people know
 * those.  The sentences are PM-DOS's own.
 *
 * In a help page a blank line ("\n\n") separates paragraphs and "* "
 * starts a bullet.  In a box '\n' ends a paragraph.
 */
#include "text.h"

/* ------------------------------------------------------------------ */
/* lines printed before the screen opens                               */
/* ------------------------------------------------------------------ */

const char tx_need386[] = "PM-DOS needs an 80386 processor or newer.";
const char tx_inwin[] =
    "Setup can't run while Windows is running. Quit Windows, and then run Setup again.";
const char tx_onrmdos[] =
    "Setup can't run in a DOS window. Quit Windows, and then run Setup at the PM-DOS prompt.";
const char tx_noinf[] =
    "Setup can't find SETUP.INF. Run Setup from the PM-DOS Setup disk.";
const char tx_badinf[] = "SETUP.INF is damaged and can't be used.";
const char tx_nomem[] = "There is not enough memory for Setup.";
const char tx_badswitch[] = "Invalid switch - %s";
const char tx_usage[] =
    "Installs PM-DOS on your computer.\r\n\r\n"
    "SETUP [/F] [/G]\r\n\r\n"
    "  /F  Puts PM-DOS on a floppy disk you can start the computer from.\r\n"
    "  /G  Does not make an Uninstall disk when Setup replaces another DOS.\r\n";

/* ------------------------------------------------------------------ */
/* the frame                                                           */
/* ------------------------------------------------------------------ */

const char tx_title[] = "%s %s Setup";
const char tx_title_floppy[] = "%s %s Setup for Floppy Disk";
const char tx_title_uninst[] = "%s Uninstall";
const char tx_help_title[] = "Setup Help";
const char tx_complete[] = "% complete";

const char tx_keys_welcome[] = "ENTER=Continue  F1=Help  F3=Exit  F7=Install to a Floppy Disk";
const char tx_keys_ehx[] = "ENTER=Continue  F1=Help  F3=Exit";
const char tx_keys_ehxp[] = "ENTER=Continue  F1=Help  F3=Exit  ESC=Previous Screen";
const char tx_keys_ex[] = "ENTER=Continue  F3=Exit";
const char tx_keys_e[] = "ENTER=Continue";
const char tx_keys_help[] = "F3=Exit  ESC=Previous Screen";
const char tx_keys_commit[] = "Y=Install PM-DOS  F3=Exit";

const char tx_list_accept[] = "To accept the selection, press ENTER.";
const char tx_list_change[] =
    "To change the selection, press the UP or DOWN ARROW key, and then press ENTER.";

const char tx_exit_title[] = "Exiting Setup";
const char tx_exit_text[] =
    "PM-DOS is not completely installed. If you leave Setup now, you will have "
    "to run Setup again to install PM-DOS.";
const char tx_exit_f3[] = "To leave Setup, press F3.";
const char tx_exit_esc[] = "To go back to Setup, press ESC.";

const char tx_cannot_title[] = "Setup Cannot Continue";
const char tx_leave_enter[] = "To leave Setup, press ENTER.";
const char tx_checking[] = "Setup is checking your system configuration.";

/* ------------------------------------------------------------------ */
/* welcome                                                             */
/* ------------------------------------------------------------------ */

const char tx_wel_1[] = "Welcome to Setup.";
const char tx_wel_2[] = "Setup installs PM-DOS %s on this computer and gets it ready to run.";
const char tx_wel_enter[] = "To install PM-DOS now, press ENTER.";
const char tx_wel_f1[] = "To read about Setup before you go on, press F1.";
const char tx_wel_f3[] = "To leave Setup without installing PM-DOS, press F3.";
const char tx_note[] = "Note:";
const char tx_wel_note[] =
    "If your files have not been backed up lately, back them up before you "
    "install. To do that, press F3 to leave Setup now, and then run your "
    "backup program.";
const char tx_wel_last[] = "To continue Setup, press ENTER.";

const char tx_h_using[] = "Using Setup";
const char tx_h_using_text[] =
    "Setup does these things:\n\n"
    "* Looks at your computer's disks and settings.\n\n"
    "* Gets the hard disk ready for PM-DOS, if it needs to: sets up the free "
    "space and formats it.\n\n"
    "* Copies the PM-DOS files to the hard disk and writes CONFIG.SYS and "
    "AUTOEXEC.BAT.\n\n"
    "* Makes drive C: start PM-DOS.\n\n"
    "Where Setup shows a list, the highlighted line is the one ENTER takes. "
    "Press the UP or DOWN ARROW key to move the highlight.";

const char tx_fl_confirm_title[] = "Install to Floppy Disk";
const char tx_fl_confirm[] =
    "You have chosen to put PM-DOS on a floppy disk. To install PM-DOS on the "
    "hard disk instead, go back to the previous screen.";
const char tx_fl_confirm_enter[] = "To put PM-DOS on a floppy disk, press ENTER.";
const char tx_fl_confirm_esc[] = "To go back to the previous screen, press ESC.";
const char tx_h_flconfirm[] = "Installing to a Floppy Disk";
const char tx_h_flconfirm_text[] =
    "Setup can make a floppy disk that starts the computer and runs PM-DOS. "
    "Use it on a computer you don't want to install PM-DOS on, or to try "
    "PM-DOS out first.\n\n"
    "The disk holds PM-DOS itself and the commands that fit on it. The disk "
    "can be formatted or not; everything on it is erased.";

/* ------------------------------------------------------------------ */
/* the hard disk                                                       */
/* ------------------------------------------------------------------ */

const char tx_unal_1[] =
    "Setup needs to set up the unallocated space on your hard disk for "
    "PM-DOS. None of the files on the disk will be changed.";
const char tx_unal_2[] = "To have Setup do this, choose the recommended option.";
const char tx_unal_item[] = "Set up unallocated disk space (recommended).";
const char tx_item_exit[] = "Exit Setup.";
const char tx_item_fdisk[] = "Exit Setup, to change the partitions with FDISK.";
const char tx_h_unal[] = "Setting Up Unallocated Disk Space";
const char tx_h_unal_text[] =
    "Space on a hard disk has to belong to a partition before DOS can use it. "
    "Setup makes one partition of the largest free area on the first hard disk "
    "and formats it. Partitions that are there already are left alone.\n\n"
    "To divide the disk some other way, choose the second option and run "
    "FDISK. When you are done, restart the computer from the Setup disk.";

const char tx_restart_1[] = "Setup will now restart your computer.";
const char tx_restart_2[] = "Leave Setup Disk 1 in drive A:.";
const char tx_restart_2x[] = "Leave the Setup disk in its drive.";
const char tx_restart_go[] = "To continue, press ENTER.";

const char tx_fmt_head[] = "Formatting the Hard Disk";
const char tx_fmt_text[] =
    "Setup is formatting the space set aside for PM-DOS. This can take a few "
    "minutes on a large disk.";
const char tx_fmt_drive[] = "Formatting drive %c:";
const char tx_fmt_pct[] = "%3d%% of the drive formatted";

const char tx_raw_1[] =
    "Drive %c: is not formatted, so PM-DOS can't use it yet. Setup can format "
    "the drive, or you can leave Setup and format the drive yourself.";
const char tx_raw_item[] = "Format drive %c: (recommended).";
const char tx_h_raw[] = "Formatting the Drive";
const char tx_h_raw_text[] =
    "A drive has to be formatted before files can be put on it. Formatting "
    "erases the drive. Setup formats the drive with the FAT file system that "
    "suits its size.";

const char tx_inst_1[] =
    "Setup will install PM-DOS on drive %c:, which has %s free.";
const char tx_inst_2[] =
    "To make room for PM-DOS another way first, leave Setup and change the "
    "partitions with FDISK, and then start Setup again.";
const char tx_inst_item[] = "Install PM-DOS on drive %c: (recommended).";
const char tx_h_inst[] = "Choosing Where to Install";
const char tx_h_inst_text[] =
    "PM-DOS goes on the first partition of the first hard disk, which is "
    "drive C: when PM-DOS starts.\n\n"
    "To give PM-DOS a partition of its own, or to make the partition bigger "
    "or smaller, choose the second option. At the prompt, run FDISK to change "
    "the partitions, and then restart the computer from the Setup disk.";

const char tx_upg_1[] =
    "Setup has found %s on drive %c:. Setup will replace it with PM-DOS and "
    "keep your files.";
const char tx_upg_2[] =
    "The old DOS files are moved to the %s directory. If you need the old DOS "
    "back, the Uninstall disk Setup makes puts those files back.";
const char tx_upg_2g[] =
    "The old DOS files are moved to the %s directory.";
const char tx_upg_item[] = "Replace %s with PM-DOS (recommended).";
const char tx_h_upg[] = "Replacing Your DOS";
const char tx_h_upg_text[] =
    "Setup moves the DOS commands that PM-DOS replaces into the OLD_DOS "
    "directory, and copies PM-DOS in their place. Commands PM-DOS has no "
    "version of stay where they are.\n\n"
    "Setup keeps your CONFIG.SYS and AUTOEXEC.BAT. Lines that load programs "
    "PM-DOS does the work of, such as HIMEM.SYS, EMM386.EXE and SMARTDRV, "
    "are turned into REM lines. The files as they were are saved in OLD_DOS.";

const char tx_re_1[] =
    "PM-DOS is already installed on drive %c:. Setup will copy this version's "
    "files over it. Your CONFIG.SYS and AUTOEXEC.BAT stay as they are.";
const char tx_re_item[] = "Reinstall PM-DOS (recommended).";
const char tx_h_re[] = "Reinstalling PM-DOS";
const char tx_h_re_text[] =
    "Setup replaces the PM-DOS files with the ones on the Setup disks and "
    "makes drive C: start PM-DOS again. Nothing else on the disk is changed.";

const char tx_cant_nodisk[] =
    "Setup can't find a hard disk in this computer. To put PM-DOS on a floppy "
    "disk, start Setup again and press F7 on the first screen.";
const char tx_cant_full[] =
    "The first hard disk has no DOS partition and no free space for one. Run "
    "FDISK to make room, and then start Setup again.";
const char tx_cant_win9x[] =
    "Drive %c: has the MS-DOS that Windows 95 or Windows 98 starts from. "
    "Replacing that MS-DOS would stop Windows from starting, so Setup won't.";
const char tx_cant_packed[] =
    "Drive %c: has DoubleSpace or DriveSpace compressed drives on it. PM-DOS "
    "can't read compressed drives, so Setup can't install on this drive.";
const char tx_cant_foreign[] =
    "Drive %c: is not a hard disk Setup can install on. A network drive or a "
    "CD-ROM drive can't be used.";
const char tx_cant_nodos[] =
    "Drive C: isn't ready for PM-DOS. To set up a new hard disk, start the "
    "computer from Setup Disk 1, and Setup will start by itself.";
const char tx_cant_space[] =
    "Drive %c: has %s free, and PM-DOS needs %s. Delete some files to make "
    "room, and then run Setup again.";
const char tx_cant_part[] =
    "Setup could not write the partition table of the hard disk.";
const char tx_cant_format[] =
    "Setup could not format drive %c:. The disk may be damaged.";
const char tx_cant_boot[] =
    "Setup could not make drive %c: start PM-DOS.";

/* ------------------------------------------------------------------ */
/* settings                                                            */
/* ------------------------------------------------------------------ */

const char tx_set_1[] = "Setup will use the following system settings:";
const char tx_set_date[] = "Date/Time:";
const char tx_set_country[] = "Country:";
const char tx_set_keyb[] = "Keyboard Layout:";
const char tx_set_to[] = "Install To:";
const char tx_set_ok[] = "The settings are correct.";
const char tx_set_2[] = "If all the settings are correct, press ENTER.";
const char tx_set_3[] =
    "To change a setting, press the UP or DOWN ARROW key to select it, and "
    "then press ENTER to see the choices.";
const char tx_h_set[] = "Choosing System Settings";
const char tx_h_set_text[] =
    "* Date/Time sets the computer's clock.\n\n"
    "* Country sets how PM-DOS writes dates, times and numbers, and the "
    "character set (code page) it uses.\n\n"
    "* Keyboard Layout sets which character each key types. The layout and the "
    "country have to share a code page.\n\n"
    "Setup writes the country and the keyboard layout into CONFIG.SYS and "
    "AUTOEXEC.BAT, so PM-DOS uses them every time it starts.";

const char tx_ctry_1[] =
    "PM-DOS can use the date, time and number formats of many countries. "
    "Choose the country you want:";
const char tx_h_ctry[] = "Choosing a Country";
const char tx_h_ctry_text[] =
    "The country decides how dates, times and numbers look, and how names "
    "are sorted. It also chooses the code page, the set of characters "
    "PM-DOS uses.\n\n"
    "Setup adds a COUNTRY= line to CONFIG.SYS for any country but the United "
    "States.";
const char tx_kb_1[] =
    "PM-DOS supports the keyboard layouts of many countries. Choose the "
    "keyboard layout you want:";
const char tx_h_kb[] = "Choosing a Keyboard Layout";
const char tx_h_kb_text[] =
    "The layout decides which character each key types. Setup adds a KEYB "
    "line to AUTOEXEC.BAT for any layout but the United States one.\n\n"
    "With a layout loaded, CTRL+ALT+F1 switches to the United States layout "
    "and CTRL+ALT+F2 switches back.";
const char tx_cp_title[] = "Code Page Incompatibility Error";
const char tx_cp_kb[] =
    "The keyboard layout you chose can't be used with the country's code "
    "page. Choose a different keyboard layout.";
const char tx_cp_ctry[] =
    "The country you chose can't be used with the keyboard layout's code "
    "page. Choose a different country, or change the keyboard layout first.";

const char tx_dt_1[] = "Setup uses this date and time to set your computer's clock.";
const char tx_dt_date[] = "System date:";
const char tx_dt_time[] = "System time:";
const char tx_dt_2[] = "If the date and time are correct, press ESC.";
const char tx_dt_3[] =
    "If one of them is wrong, press the UP or DOWN ARROW key to reach it, type "
    "the correct value, and then press ENTER.";
const char tx_h_date[] = "Setting the Date";
const char tx_h_date_text[] =
    "Type the date in the form the country uses: %s. Use two digits for the "
    "year; 80 to 99 are 1980 to 1999, and 00 to 79 are 2000 to 2079.";
const char tx_h_time[] = "Setting the Time";
const char tx_h_time_text[] =
    "Type the time as hours and minutes on a 24-hour clock, with the "
    "separator the country uses: %s. For 2:30 in the afternoon, type 14%c30.";
const char tx_baddate_title[] = "Invalid Date";
const char tx_baddate[] = "Type the date in the form %s.";
const char tx_badtime_title[] = "Invalid Time";
const char tx_badtime[] = "Type the time in the form %s, on a 24-hour clock.";

const char tx_dir_1[] = "Setup will put the PM-DOS files in the following directory:";
const char tx_dir_2[] = "To put the files in this directory, press ENTER.";
const char tx_dir_3[] =
    "To put them in another directory, type its path, and then press ENTER.";
const char tx_h_dir[] = "Choosing the PM-DOS Directory";
const char tx_h_dir_text[] =
    "The PM-DOS commands go in this directory on drive C:. Only the files "
    "PM-DOS starts from go in the root, so the directory can't be the root "
    "itself.\n\n"
    "Press the LEFT or RIGHT ARROW key to move in the path, BACKSPACE or DEL to "
    "remove a character, and type to add one.";
const char tx_baddir_title[] = "ERROR";
const char tx_baddir_1[] = "That is not a valid directory path.";
const char tx_baddir_2[] = "Type a valid path, such as C:\\DOS.";
const char tx_baddir_3[] = "Press ENTER to continue.";
const char tx_mkdir_title[] = "Directory Creation Error";
const char tx_mkdir[] =
    "Setup could not make the directory %s. Type a different path.";

/* ------------------------------------------------------------------ */
/* the Uninstall disk and the last question                            */
/* ------------------------------------------------------------------ */

const char tx_ud_1[] =
    "Setup will now make an Uninstall disk. If PM-DOS doesn't work out on "
    "this computer, starting the computer from the Uninstall disk puts your "
    "old DOS back.";
const char tx_ud_label[] = "Label a floppy disk as follows:";
const char tx_ud_name[] = "UNINSTALL";
const char tx_ud_2[] =
    "The disk can be formatted or not. Everything on the disk will be erased.";
const char tx_ud_3[] = "When the disk is labelled, press ENTER.";
const char tx_ud_insert[] = "Put the disk labelled %s in drive %c:.";
const char tx_ud_erase[] = "Everything on the disk will be erased.";
const char tx_ud_ready[] = "When you are ready, press ENTER.";
const char tx_ud_write[] = "Setup is making the Uninstall disk.";
const char tx_h_ud[] = "The Uninstall Disk";
const char tx_h_ud_text[] =
    "The Uninstall disk starts your old DOS from the floppy and puts it back "
    "on drive C:: the DOS commands from OLD_DOS, the old CONFIG.SYS and "
    "AUTOEXEC.BAT, and the old boot sector. PM-DOS is removed.\n\n"
    "Keep the disk until you are sure PM-DOS works on this computer. To skip "
    "the disk, run SETUP /G.";
const char tx_bad_setupdisk[] = "This is a Setup disk. Please use another disk.";
const char tx_bad_disk[] = "This disk can't be used. Please use another disk.";
const char tx_bad_room[] = "There isn't enough space on this disk.";
const char tx_bad_wp[] = "The disk in drive %c: is write-protected. Use one that isn't.";
const char tx_error_title[] = "ERROR";

const char tx_commit_title[] = "Ready to Install";
const char tx_commit[] =
    "Setup is ready to install PM-DOS. From here on, Setup changes the files "
    "on drive %c:.";
const char tx_commit_y[] = "To install PM-DOS, press Y.";
const char tx_commit_f3[] = "To leave Setup without changing anything, press F3.";

/* ------------------------------------------------------------------ */
/* copying                                                             */
/* ------------------------------------------------------------------ */

const char tx_copying[] = "Setup is copying the PM-DOS files.";
const char tx_copying_fl[] = "Please wait while Setup copies the files.";
const char tx_reading[] = "Reading  ";
const char tx_writing[] = "Writing  ";
const char tx_creating[] = "Creating ";
const char tx_insert_1[] = "Please put the following disk in drive %c:";
const char tx_insert_3[] = "When you are ready to continue, press ENTER.";
const char tx_cperr_title[] = "SETUP ERROR DETECTED";
const char tx_cperr_read[] = "Setup could not read the file %s.";
const char tx_cperr_write[] = "Setup could not write the file %s.";
const char tx_cperr_go[] = "To go on without this file, press ENTER.";
const char tx_wp_title[] = "DISK WRITE ERROR";
const char tx_remove[] = "Remove any disks from your floppy disk drives, and then press ENTER.";
const char tx_formatting_fl[] = "Formatting the disk. Please wait.";
const char tx_cap_1[] = "Please choose the capacity of the disk in drive %c:";
const char tx_cap_720[] = "720 KB";
const char tx_cap_1440[] = "1.44 MB";
const char tx_cap_1200[] = "1.2 MB";
const char tx_cap_2880[] = "2.88 MB";

/* ------------------------------------------------------------------ */
/* the end                                                             */
/* ------------------------------------------------------------------ */

const char tx_done_title[] = "PM-DOS Setup Complete";
const char tx_done_1[] = "PM-DOS %s is installed on your computer.";
const char tx_done_old[] =
    "Your old CONFIG.SYS and AUTOEXEC.BAT are saved in %s as CONFIG.DAT "
    "and AUTOEXEC.DAT.";
const char tx_done_enter[] = "To restart your computer with PM-DOS, press ENTER.";
const char tx_done_help[] = "To read about the PM-DOS commands, type HELP at the prompt.";
const char tx_fl_done[] = "Setup has finished making the PM-DOS startup disk.";
const char tx_fl_drive_1[] = "Choose the floppy disk drive to put PM-DOS on:";
const char tx_fl_drive_item[] = "Drive %c";
const char tx_h_fldrive[] = "Choosing a Floppy Disk Drive";
const char tx_h_fldrive_text[] =
    "To start a computer from the disk, use the drive the computer starts "
    "from, which is nearly always drive A:.";
const char tx_fl_label_1[] =
    "Setup will make a floppy disk you can start the computer from and run "
    "PM-DOS on. The disk can be formatted or not.";
const char tx_fl_label_name[] = "STARTUP";
const char tx_h_flset[] = "Settings for a Floppy Disk";
const char tx_h_flset_text[] =
    "* Date/Time sets the computer's clock.\n\n"
    "* Install To is the drive the startup disk is made in.";
const char tx_h_list[] = "Using the List";

/* ------------------------------------------------------------------ */
/* UNINSTAL                                                            */
/* ------------------------------------------------------------------ */

const char tx_un_1[] =
    "Uninstall puts back the DOS that was on drive %c: before PM-DOS was "
    "installed. PM-DOS is removed from the drive, and your old CONFIG.SYS and "
    "AUTOEXEC.BAT come back.";
const char tx_un_enter[] = "To put back your old DOS, press ENTER.";
const char tx_un_f3[] = "To leave without changing anything, press F3.";
const char tx_un_working[] = "Uninstall is putting back your old DOS.";
const char tx_un_done[] =
    "Your old DOS is back on drive %c:. Remove the Uninstall disk, and then "
    "press ENTER to restart the computer.";
const char tx_un_nodata[] =
    "This Uninstall disk does not belong to the PM-DOS on drive %c:, or "
    "%s is missing, so the old DOS can't be put back.";
const char tx_un_failed[] =
    "Uninstall could not finish. %s";
const char tx_h_un[] = "Uninstalling PM-DOS";
const char tx_h_un_text[] =
    "Uninstall deletes the PM-DOS files from drive C:, moves the old DOS "
    "commands back from OLD_DOS into their directory, puts back the old "
    "CONFIG.SYS and AUTOEXEC.BAT, and writes the old boot sector.\n\n"
    "Files you made or changed since PM-DOS was installed are not touched.";
