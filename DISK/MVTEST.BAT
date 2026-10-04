@ECHO OFF
REM ====================================================================
REM MVTEST.BAT - MOVE.EXE, and what each step is actually asking.
REM
REM IT DOES NOT RUN ON C:.  Every step here creates, moves and deletes
REM files, and the one disk that must not happen on is the one holding
REM the kernel the machine is running from.  It works on MULTI.IMG's
REM volumes, which MKHD rebuilds every time - so whatever this test
REM leaves behind is gone at the next build.  A test that WRITES has to
REM write somewhere it can afford to be wrong about.
REM
REM IT FINDS THOSE VOLUMES BY MARKER AND NOT BY LETTER, and that is the
REM correction this test needed twice.  MKMULTI prints "expected
REM letters: C: PRIMARY-ONE D: LOGICAL-ONE", which is true of a machine
REM where MULTI.IMG is the ONLY disk - attach it second, behind HD16,
REM and everything shifts by one.  So the markers MKMULTI writes into
REM each volume are what is looked for: PRIM1.TXT is PRIMARY-ONE and
REM LOG1.TXT is LOGICAL-ONE wherever they land.
REM
REM And the guard names a FILE, because IF EXIST tests plain files only
REM exactly as MS-DOS does - which is why everyone learned to write
REM "IF EXIST dir\NUL".  "IF EXIST D:\" is false on a machine with a
REM perfectly good D:, and this test silently skipped itself the first
REM time it ran.
REM
REM   1  a rename, which is 56h and touches no file data
REM   2  moving into a directory, and the file being READABLE after -
REM      a directory entry that moved but lost its cluster chain would
REM      pass a listing and fail a TYPE
REM   3  a wildcard, several files at once, and the collect-then-move
REM      path that exists because moving rewrites the directory a find
REM      is walking
REM   4  the refusals: onto itself, two sources into one name, a
REM      directory into another directory
REM   5  replacing an existing file - /Y, and the replaced file's
REM      contents being the NEW ones
REM   6  a directory RENAMED, which is allowed, with its contents
REM      still inside it afterwards
REM   7  across volumes: the copy-and-delete path, where the source
REM      being GONE afterwards is the half that a plain COPY would
REM      also have passed
REM ====================================================================
SET MVW=
SET MVO=
IF EXIST D:\PRIM1.TXT SET MVW=D:
IF EXIST E:\PRIM1.TXT SET MVW=E:
IF EXIST F:\PRIM1.TXT SET MVW=F:
IF EXIST D:\LOG1.TXT SET MVO=D:
IF EXIST E:\LOG1.TXT SET MVO=E:
IF EXIST F:\LOG1.TXT SET MVO=F:
IF "%MVW%"=="" GOTO NODRIVE
ECHO MVTEST: working on %MVW% with %MVO% as the second volume.

%MVW%
CD \
MD MVTEST
CD \MVTEST

ECHO --- 1. a plain rename ---
ECHO one > ONE.TXT
MOVE ONE.TXT TWO.TXT
DIR

ECHO --- 2. into a directory, and readable afterwards ---
MD SUB
MOVE TWO.TXT SUB
DIR SUB
TYPE SUB\TWO.TXT

ECHO --- 3. a wildcard, several at once ---
ECHO a > A.DAT
ECHO b > B.DAT
ECHO c > C.DAT
MOVE *.DAT SUB
ECHO --- the working directory must have no .DAT left: ---
DIR *.DAT
ECHO --- and SUB must have all three: ---
DIR SUB

ECHO --- 4. the refusals ---
ECHO --- onto itself: ---
MOVE SUB\A.DAT SUB\A.DAT
ECHO --- two sources into one NAME: ---
MOVE SUB\A.DAT SUB\B.DAT ONENAME.TXT
ECHO --- a directory into another directory: ---
MD OTHER
MOVE SUB OTHER

ECHO --- 5. replacing, with /Y so nothing waits for a key ---
ECHO fresh > NEW.TXT
MOVE /Y NEW.TXT SUB\A.DAT
ECHO --- SUB\A.DAT must now say "fresh": ---
TYPE SUB\A.DAT

ECHO --- 6. renaming a directory, contents and all ---
MOVE SUB SUBTWO
DIR
DIR SUBTWO

ECHO --- 7. across volumes ---
IF "%MVO%"=="" GOTO NOSECOND
ECHO travelled > CROSS.TXT
MOVE CROSS.TXT %MVO%\CROSS.TXT
ECHO --- it must be on %MVO% now... ---
TYPE %MVO%\CROSS.TXT
ECHO --- ...and GONE from here, which is the half a copy would fail: ---
DIR CROSS.TXT
DEL %MVO%\CROSS.TXT
GOTO CLEAN
:NOSECOND
ECHO (only one spare volume on this machine - step 7 was skipped)

:CLEAN
REM THE FILES ARE NAMED AND NOT STARRED, and that is not fussiness.
REM "DEL *.*" asks "All files in the directory will be deleted!  Are
REM you sure (Y/N)?" and waits - so a wildcard here stops the whole
REM suite dead on a machine with nobody in front of it, which is the
REM same way LOOPTEST's CTTY AUX used to end an unattended run.  It
REM also states what this test expects to be left, which a star does
REM not: if one of these is already gone, DEL says so and the failure
REM is visible instead of being swallowed by a wildcard that matched
REM whatever happened to be there.
ECHO --- clean up ---
DEL SUBTWO\TWO.TXT
DEL SUBTWO\A.DAT
DEL SUBTWO\B.DAT
DEL SUBTWO\C.DAT
RD SUBTWO
RD OTHER
CD \
RD MVTEST
C:
CD \
GOTO END

:NODRIVE
ECHO MVTEST needs MULTI.IMG attached as a second disk - it works on
ECHO that disk's volumes and never on the one PM-DOS booted from.
C:

:END
SET MVW=
SET MVO=
ECHO ==== MVTEST DONE ====
