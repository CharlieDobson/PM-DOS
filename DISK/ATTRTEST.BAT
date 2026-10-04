@ECHO OFF
REM ====================================================================
REM ATTRTEST.BAT - ATTRIB.EXE, and what each step is actually asking.
REM
REM IT DOES NOT RUN ON C:, for MVTEST's reason and one of its own.  The
REM steps below deliberately make files that cannot be deleted and
REM files that do not appear in a listing, and the disk that must not
REM happen on is the one holding the kernel the machine is running
REM from.  It works on MULTI.IMG's volumes, which MKHD rebuilds every
REM time, so whatever a failed run leaves behind is gone at the next
REM build.  It finds the volume BY MARKER and not by letter, for the
REM reason written out at length in MVTEST.BAT: MKMULTI's "expected
REM letters" line is true only when MULTI.IMG is the only disk.
REM
REM WHAT IT IS ACTUALLY ASKING, step by step:
REM
REM   1  the columns.  A bit that ATTRIB sets and ATTRIB reads back is
REM      only proof that ATTRIB agrees with itself, so the display is
REM      checked against a file whose attributes something ELSE set -
REM      a freshly written file, which DOS gives the archive bit
REM   2  +R, and then DEL REFUSING.  This is the step that matters:
REM      an attribute is real when a DIFFERENT part of the system
REM      honours it, and a read-only bit that only ATTRIB can see is
REM      a bit that never reached the directory entry
REM   3  -R, and the same DEL then working
REM   4  +H, and DIR not listing what ATTRIB still shows - the other
REM      half of the same argument
REM   5  the hidden-file rule: "-R" alone must REFUSE to touch a
REM      hidden file and say which one, and "-R -H" must do it.  This
REM      is the rule that keeps "ATTRIB -R *.*" in a root away from
REM      the system files
REM   6  a wildcard leaves a subdirectory alone; an explicit name does
REM      not.  Two commands, one difference, and the difference is
REM      what the user typed
REM   7  /S, which must reach a file two levels down - and which
REM      counts as a wildcard, so the directories on the way are not
REM      themselves changed
REM   8  the two refusals that are easy to merge and must not be: a
REM      name that does not exist is "File not found", a DIRECTORY
REM      that does not exist is "Path not found"
REM   9  /? on its own and /? with a change beside it - neither may
REM      change anything
REM ====================================================================
SET ATW=
IF EXIST D:\PRIM1.TXT SET ATW=D:
IF EXIST E:\PRIM1.TXT SET ATW=E:
IF EXIST F:\PRIM1.TXT SET ATW=F:
IF "%ATW%"=="" GOTO NODRIVE
ECHO ATTRTEST: working on %ATW%.

%ATW%
CD \
MD ATTEST
CD ATTEST
MD SUB
MD SUB\DEEP

ECHO one > ONE.TXT
ECHO two > TWO.TXT
ECHO deep > SUB\DEEP\DEEP.TXT

ECHO ==================== 1. THE COLUMNS ====================
REM A file that has just been written has the archive bit and nothing
REM else, and nothing here set it - the filesystem did.  So "A" in the
REM third column and four spaces after it is the whole check.
ATTRIB ONE.TXT

ECHO ==================== 2. +R, AND DEL REFUSING ====================
ATTRIB +R ONE.TXT
ECHO --- ATTRIB says: ---
ATTRIB ONE.TXT
ECHO --- and DEL must REFUSE (this is the real test) ---
DEL ONE.TXT
ECHO --- so it is still here: ---
DIR ONE.TXT

ECHO ==================== 3. -R, AND DEL WORKING ====================
ATTRIB -R ONE.TXT
ATTRIB ONE.TXT
ECHO --- and now DEL must work ---
DEL ONE.TXT
ECHO --- so it is gone (DIR must say no files): ---
DIR ONE.TXT

ECHO ==================== 4. +H, AND DIR NOT LISTING IT ===============
ATTRIB +H TWO.TXT
ECHO --- DIR must NOT show TWO.TXT ---
DIR
ECHO --- ATTRIB must, and must show H ---
ATTRIB TWO.TXT

ECHO ==================== 5. THE HIDDEN-FILE RULE ====================
ECHO --- "+R" alone must REFUSE and name the file ---
ATTRIB +R TWO.TXT
ECHO --- it must still be A and H, with no R: ---
ATTRIB TWO.TXT
ECHO --- "+R -H" names the hidden bit, so it is allowed ---
ATTRIB +R -H TWO.TXT
ATTRIB TWO.TXT
ATTRIB -R TWO.TXT

ECHO ==================== 6. WILDCARD vs EXPLICIT NAME ================
ECHO --- "*.*" must NOT list SUB, only the files ---
ATTRIB *.*
ECHO --- SUB by name must show the directory itself ---
ATTRIB SUB
ECHO --- and +R on it by name is allowed ---
ATTRIB +R SUB
ATTRIB SUB
ATTRIB -R SUB

ECHO ==================== 7. /S TWO LEVELS DOWN ====================
ECHO --- must reach SUB\DEEP\DEEP.TXT and print its full path ---
ATTRIB *.TXT /S
ECHO --- and must change it ---
ATTRIB +R *.TXT /S
ATTRIB *.TXT /S
ATTRIB -R *.TXT /S

ECHO ==================== 8. THE TWO REFUSALS ====================
ECHO --- a name that is not there: "File not found" ---
ATTRIB NOSUCH.TXT
ECHO --- a DIRECTORY that is not there: "Path not found" ---
ATTRIB \NOSUCHDIR\*.*

ECHO ==================== 9. /? CHANGES NOTHING ====================
ATTRIB /?
ECHO --- /? beside a change: still help, still no change ---
ATTRIB +R TWO.TXT /?
ECHO --- so TWO.TXT must still have no R: ---
ATTRIB TWO.TXT

ECHO ==================== CLEAN UP ====================
REM Named one at a time rather than swept up with a wildcard: if one
REM of these is already gone, DEL says so and the failure is visible.
DEL TWO.TXT
DEL SUB\DEEP\DEEP.TXT
RD SUB\DEEP
RD SUB
CD \
RD ATTEST
C:
CD \
GOTO END

:NODRIVE
ECHO ATTRTEST needs MULTI.IMG attached as a second disk - it makes
ECHO files that cannot be deleted and files that do not show up in a
ECHO listing, and never does that on the disk PM-DOS booted from.
C:

:END
SET ATW=
ECHO ==== ATTRTEST DONE ====
