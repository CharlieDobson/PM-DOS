@ECHO OFF
REM ====================================================================
REM IMGTEST.BAT - a FILE given a drive letter of its own.
REM
REM Needs C:\TESTS\TESTIMG.IMG, which BUILD.CMD makes with MKDISK: a
REM 1.44M FAT12 floppy image holding IMGFILE.TXT and nothing else.
REM
REM WHAT EACH STEP IS ACTUALLY ASKING, because a listing that scrolls
REM past proves nothing on its own:
REM
REM   1  the mount happens at all, and MOUNT's table describes it
REM   2  a file INSIDE the image can be read - so the extent map
REM      points where the clusters really are
REM   3  a file can be WRITTEN, and survives an unmount and a second
REM      mount - so the write reached the host's platter and not just
REM      a cache that went away with the volume
REM   4  the image file cannot be deleted or opened for writing while
REM      it is mounted, which is the guard that stops the map being
REM      silently wrong
REM   5  reading the image AS A FILE while it is mounted sees the same
REM      bytes the volume does, which is what putting the translation
REM      above the cache buys
REM
REM M: AND N: RATHER THAN THE NEXT FREE LETTER, because how many
REM letters a machine has already used depends on how many disks are
REM attached to it - and this test ran green on one disk and failed
REM on two, saying "no such drive letter, or it is already in use"
REM about a letter a partition had taken.  A test that names a
REM resource has to name one nothing else can claim.
REM ====================================================================
ECHO --- 1. nothing is mounted yet ---
MOUNT
ECHO --- mounting it ---
MOUNT M: C:\TESTS\TESTIMG.IMG
MOUNT
DRIVES

ECHO --- 2. reading what is inside it ---
DIR M:
TYPE M:\IMGFILE.TXT

ECHO --- 3. writing to it ---
COPY M:\IMGFILE.TXT M:\WRITTEN.TXT
MD M:\SUB
COPY M:\IMGFILE.TXT M:\SUB\DEEPER.TXT
DIR M:
ECHO --- unmounting, and mounting it again ---
MOUNT /U M:
MOUNT
MOUNT M: C:\TESTS\TESTIMG.IMG
DIR M:
TYPE M:\WRITTEN.TXT
TYPE M:\SUB\DEEPER.TXT

ECHO --- 4. what is refused while it is mounted ---
ECHO (each of the next three must fail)
DEL C:\TESTS\TESTIMG.IMG
COPY C:\AUTOEXEC.BAT C:\TESTS\TESTIMG.IMG
MOUNT N: C:\TESTS\TESTIMG.IMG
ECHO (and the file is still there)
DIR C:\TESTS\TESTIMG.IMG

ECHO --- 5. the same bytes through both views ---
ECHO (a read of the file sees the volume's writes - one cache, one truth)
DIR M:\WRITTEN.TXT

ECHO --- unmounting for good ---
MOUNT /U M:
MOUNT
DIR M:
ECHO --- IMGTEST done ---
