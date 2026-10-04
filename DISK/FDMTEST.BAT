@ECHO OFF
REM ====================================================================
REM FDMTEST.BAT - FDISK'S SCREEN, DRIVEN FROM A FILE.
REM
REM The screen is the one part of FDISK that normally cannot be tested
REM without somebody sitting at the keyboard and the mouse - so it reads
REM both from standard input when FDTEST names a log, the way EDIT does,
REM and writes a picture of itself into the log wherever the key file
REM says to.  A file that runs out is Esc from everything and then Q, so
REM a short script ends the program instead of hanging it.
REM
REM Needs MULTI.IMG as the SECOND disk, same as FDTEST, and it CHANGES
REM MOST OF IT - moves, a resize, creates, and in the end the whole
REM extended partition deleted and made again - so run it in a boot of
REM its own, after the other FDISK tests; MKHD puts MULTI.IMG back on
REM the next build.
REM
REM WHAT THE KEY FILE DOES is written at the top of TOOLS\MKFDKEYS.PL,
REM which made it: a partition dragged along the bar with nowhere to go,
REM a delete, a drag into the space it left, a create there, a resize
REM typed in megabytes, a drag inside an extended partition, the menus,
REM the help, a move typed in megabytes, and the extended partition
REM emptied, deleted and made again with a logical drive in it.  The
REM screen stages every change, so the script presses W to write them
REM after its moves and resizes and again at the end.  Its last picture
REM is the restart box: under FDTEST the machine restarts only if
REM FDBOOT is set as well, so the batch goes on.  FDISK /L afterwards
REM should list:
REM
REM   1  FAT16B         63 + 67536      2  FAT12        75600 + 16128
REM   3  FAT16       68544 +  7056      4  Extended     91728 + 73584
REM   5  FAT16B      91791 + 73521
REM ====================================================================
SET FDTEST=C:\FDMLOG.TXT
FDISK < FDM1.KEY
SET FDTEST=
ECHO --- the screens the session showed ---
TYPE C:\FDMLOG.TXT
ECHO --- and disk 1 afterwards ---
FDISK /L
ECHO --- FDMTEST done ---
