@ECHO OFF
REM ====================================================================
REM FDTEST.BAT - FDISK's create, delete and set-active.
REM
REM Needs MULTI.IMG as the SECOND disk, which is what MKMULTI builds
REM and MKHD rebuilds every time - so everything this does is undone
REM by the next build.  Its four primary slots start out:
REM
REM   1  06h FAT16B  at 63       PRIMARY-ONE
REM   2  01h FAT12   at 67599    PRIMARY-TWO
REM   3  83h Linux   at 83727    not FAT, and not wanted by anything
REM   4  05h         at 91791    the extended container, with three
REM                              logical drives chained inside it
REM
REM THE INTERESTING NUMBER IS 83727.  It is not a cylinder boundary -
REM 83 cylinders of 1008 sectors is 83664 - so a partition created in
REM the space slot 3 frees cannot start there.  What comes out should
REM start at 84672 (cylinder 84) and end at 91728 (cylinder 91), which
REM is seven whole cylinders inside a gap of eight.  If the report
REM says 83727 the alignment is not happening.
REM
REM SINCE 2026-10-02 THE COMMAND LINE WRITES AT ONCE.  There is no /W
REM and no dry run; /W itself is now an unknown switch, refused with
REM the usage and nothing written, and that refusal is tested below.
REM ====================================================================
ECHO --- the table as MKMULTI left it ---
FDISK /L

ECHO --- refusals, which are the half worth testing ---
ECHO (1) the extended container still has logical drives in it
FDISK /D 1:4
ECHO (2) slot 1 is occupied
FDISK /C 1:1
ECHO (3) a disk that is not there
FDISK /D 5:1

ECHO (4) /W, which is gone: the usage, and slot 1 stays active
FDISK /A 1:2 /W
FDISK /STATUS

ECHO --- set active ---
FDISK /A 1:2
FDISK /STATUS

ECHO --- delete slot 3 ---
FDISK /D 1:3
FDISK /STATUS

ECHO --- and create in the space it freed ---
ECHO (no size given, so it takes the whole aligned gap)
FDISK /C 1:3
FDISK /STATUS

ECHO --- create refused now that the slot is full again ---
FDISK /C 1:3

ECHO --- delete it and create a SMALLER one with an explicit type ---
FDISK /D 1:3
FDISK /C 1:3 2016 04
FDISK /STATUS

ECHO --- and the chain is still intact underneath all that ---
FDISK /L
ECHO --- FDTEST done ---
