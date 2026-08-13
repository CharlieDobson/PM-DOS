@ECHO OFF
REM ====================================================================
REM DISKTEST.BAT - the volume table, exercised rather than listed.
REM
REM Needs the multi-partition test disk attached (TOOLS\MKMULTI.C).
REM DRIVES says what was found; this says whether it works.
REM
REM The interesting drive is the FAT12 one on a HARD DISK.  FAT12 on
REM anything but a floppy is the case every implementation gets least
REM practice at, and the one where a whole-FAT cache and a partition
REM offset have to agree with each other.
REM ====================================================================
ECHO --- what the scan found ---
DRIVES
ECHO --- reading each volume's own file ---
FOR %%D IN (C D E F G) DO IF EXIST %%D:\*.TXT ECHO %%D: has files
TYPE D:\PRIM1.TXT
TYPE E:\LOG1.TXT
TYPE F:\LOG2.TXT
TYPE G:\PRIM2.TXT
ECHO --- writing to the FAT12 partition on a hard disk ---
COPY D:\PRIM1.TXT G:\COPIED.TXT
TYPE G:\COPIED.TXT
ECHO --- and to a logical drive inside the extended partition ---
COPY G:\PRIM2.TXT E:\COPIED.TXT
TYPE E:\COPIED.TXT
ECHO --- a subdirectory on a logical drive ---
MD E:\SUBDIR
COPY E:\COPIED.TXT E:\SUBDIR\AGAIN.TXT
DIR E:\SUBDIR
DEL E:\SUBDIR\AGAIN.TXT
RD E:\SUBDIR
ECHO --- tidy up ---
DEL G:\COPIED.TXT
DEL E:\COPIED.TXT
DIR G:
ECHO DISKTEST done
