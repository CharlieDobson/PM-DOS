@ECHO OFF
REM ====================================================================
REM FDLTEST.BAT - FDISK's EXTENDED CHAIN: creating and deleting logical
REM drives, which is the half FDTEST.BAT does not touch.
REM
REM Needs MULTI.IMG as the SECOND disk, same as FDTEST.  MKHD rebuilds
REM it every time, so everything here is undone by the next build.
REM Its chain starts out:
REM
REM   5  04h FAT16   record 91791, volume 91854 + 32256   LOGICAL-ONE
REM   6  06h FAT16B  record 124110, volume 124173 + 32256 LOGICAL-TWO
REM   7  16h hidden  record 156429, volume 156492 + 8064  HIDDEN-VOL
REM
REM and the container is 91791 + 72765, so it ends at 164556.
REM
REM THE NUMBERS TO WATCH, because each one catches a different mistake:
REM
REM   (a) deleting 7 - the LAST link - must leave 5 and 6 exactly where
REM       they were.  It writes one sector: link 6's slot 1.
REM   (b) creating in the space that frees must put the RECORD on a
REM       cylinder boundary, 157248, and the volume one track behind it
REM       at 157311, ending at 164304.  That is 6993 sectors.  If it
REM       says 156429 the alignment is not happening; if it says 157248
REM       for the VOLUME then the record and the volume have been
REM       confused, which is the mistake that puts a partition on top
REM       of a partition table.
REM   (c) deleting 5 - the HEAD - must not move the chain.  The record
REM       at 91791 stays and takes over LOGICAL-TWO, so 5 becomes 06h
REM       at 124173 and the container's own entry never changes.
REM   (d) with the chain empty, /D on the container must now be allowed
REM       - that refusal was the one this release was meant to answer.
REM
REM SINCE 2026-10-02 THE COMMAND LINE WRITES AT ONCE: no /W, no dry
REM run.  And a shrink that needs FAT12 is no longer refused - FDISK
REM changes the width - so refusal (4) is now a shrink below the
REM smallest size the volume can take, which /P puts at 36 sectors.
REM ====================================================================
ECHO --- the chain as MKMULTI left it ---
FDISK /L

ECHO --- refusals, which are the half worth testing ---
ECHO (1) there is no eighth partition
FDISK /D 1:8
ECHO (2) 5 is already a logical drive
FDISK /C 1:5
ECHO (3) a logical drive cannot be the active one
FDISK /A 1:5
ECHO (4) a shrink below the smallest size the volume can take
FDISK /S 1:5 30
ECHO (5) disk 0 has no extended container at all
FDISK /D 0:5
ECHO (6) and the container still has drives in it
FDISK /D 1:4

ECHO --- (a) delete the LAST logical drive ---
FDISK /D 1:7
FDISK /L

ECHO --- (b) and create in the space it freed ---
ECHO (record at 157248, volume 157311 + 6993)
FDISK /C 1:7
FDISK /L

ECHO --- (c) delete the HEAD of the chain ---
ECHO (5 becomes 06h at 124173; the container entry must not move)
FDISK /D 1:5
FDISK /L
FDISK /STATUS

ECHO --- empty the chain out ---
FDISK /D 1:6
FDISK /D 1:5
FDISK /L

ECHO --- and create into an EMPTY container, where the record has ---
ECHO --- to go exactly at 91791 because the table names that sector ---
FDISK /C 1:5
FDISK /L

ECHO --- one with a size and a type of its own ---
FDISK /D 1:5
FDISK /C 1:5 20M 06
FDISK /L

ECHO --- (d) empty it again and the container itself can go ---
FDISK /D 1:5
FDISK /D 1:4
FDISK /STATUS
ECHO --- /MBR: the boot code, and nothing but the boot code ---
ECHO (MULTI.IMG is built with no loader in front of its table, so the
ECHO  first of these writes one and the second finds it already there)
FDISK /MBR 1
FDISK /MBR 1
ECHO --- and the table it was preserving is still there ---
FDISK /STATUS
ECHO --- FDLTEST done ---
