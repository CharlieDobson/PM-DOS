@ECHO OFF
REM ====================================================================
REM DTTEST.BAT - DELTREE.EXE.
REM
REM IT DOES NOT RUN ON C:.  DELTREE deletes whole trees, and the disk
REM that must never be the one a mistake lands on is the one PM-DOS
REM started from.  It works on a MULTI.IMG volume, found by marker as
REM ATTRTEST.BAT finds it, and everything it makes is under \DTT there.
REM
REM What is left after each step is shown with DIR /A /B, and the line
REM before it says what that must be.  ("IF EXIST X\NUL" is no test for
REM a directory here: CMD.EXE answers no whether X is there or not.)
REM
REM   1  the command line: /? deletes nothing, no path and a bad switch
REM      are refused with errorlevel 1
REM   2  a tree with read-only, hidden and system files and a hidden
REM      directory in it goes whole, with one "Deleting" line
REM   3  X\*.* and X\ empty X and leave it; "." does the same
REM   4  a read-only file named on its own
REM   5  a wildcard takes files and directories, and nothing else
REM   6  nothing matching says nothing, errorlevel 0
REM   7  the questions, answered from a file: Y, N, the last letter
REM      counts, Enter before a letter does nothing, the end of the
REM      file is a no
REM   8  long names, shown as the file system has them; quotes go
REM   9  the current directory is refused by name, errorlevel 1, and
REM      everything else in the tree goes
REM  10  /Y after the path is still /Y
REM  11  forty files in one directory and a tree ten deep
REM  12  UNDELETE brings a deleted tree back
REM ====================================================================
SET DTW=
IF EXIST D:\PRIM1.TXT SET DTW=D:
IF EXIST E:\PRIM1.TXT SET DTW=E:
IF EXIST F:\PRIM1.TXT SET DTW=F:
IF "%DTW%"=="" GOTO NODRIVE
ECHO DTTEST: working on %DTW%.

%DTW%
CD \
MD DTT
CD DTT

ECHO ==================== 1. THE COMMAND LINE ====================
MD KEEP
DELTREE /? KEEP
DELTREE
IF ERRORLEVEL 1 ECHO errorlevel 1
DELTREE /X KEEP
IF ERRORLEVEL 1 ECHO errorlevel 1
DELTREE /Y /YY KEEP
IF ERRORLEVEL 1 ECHO errorlevel 1
DELTREE /Y
IF ERRORLEVEL 1 ECHO errorlevel 1
ECHO --- KEEP must still be here: ---
DIR /A /B
RD KEEP

ECHO ==================== 2. A WHOLE TREE ====================
MD T1
MD T1\SUB
MD T1\SUB\DEEP
MD T1\HID
ECHO a > T1\A.TXT
ECHO b > T1\SUB\B.TXT
ECHO c > T1\SUB\DEEP\C.TXT
ECHO d > T1\HID\D.TXT
ATTRIB +R +H T1\SUB\B.TXT
ATTRIB +S T1\SUB\DEEP\C.TXT
ATTRIB +R T1\A.TXT
ATTRIB +H T1\HID
DELTREE /Y T1
IF ERRORLEVEL 1 ECHO errorlevel 1
ECHO --- nothing must be left: ---
DIR /A /B

ECHO ==================== 3. WHAT IS IN A DIRECTORY ====================
MD T2
MD T2\SUB
ECHO a > T2\A.TXT
ECHO b > T2\SUB\B.TXT
DELTREE /Y T2\*.*
ECHO --- T2 must be here, and empty: ---
DIR /A /B
DIR /A /B T2
MD T2\SUB
ECHO a > T2\A.TXT
DELTREE /Y T2\
ECHO --- the same: ---
DIR /A /B
DIR /A /B T2
MD T2\SUB
ECHO a > T2\A.TXT
CD T2
DELTREE /Y .
CD ..
ECHO --- the same: ---
DIR /A /B
DIR /A /B T2
RD T2

ECHO ==================== 4. A READ-ONLY FILE ====================
ECHO f > F.TXT
ATTRIB +R F.TXT
DELTREE /Y F.TXT
ECHO --- nothing must be left: ---
DIR /A /B

ECHO ==================== 5. WILDCARDS ====================
MD T5A
MD T5B
ECHO x > T5A\Q.TXT
ECHO x > T5C.TXT
ECHO x > U5.TXT
DELTREE /Y T5*
ECHO --- only U5.TXT must be left: ---
DIR /A /B
DEL U5.TXT

ECHO ==================== 6. NOTHING THERE ====================
DELTREE /Y NOSUCH
IF ERRORLEVEL 1 ECHO errorlevel 1
DELTREE /Y NOSUCH\*.*
IF ERRORLEVEL 1 ECHO errorlevel 1
ECHO --- nothing must have been said above this line ---

ECHO ==================== 7. THE QUESTIONS ====================
ECHO Y> K1.KEY
ECHO n> K2.KEY
ECHO.> K3.KEY
ECHO yn>> K3.KEY
ECHO ny> K4.KEY
TYPE NUL > K5.KEY
ECHO y> K6.KEY
ECHO n>> K6.KEY
MD Q1
DELTREE Q1 < K1.KEY
MD Q2
DELTREE Q2 < K2.KEY
MD Q3
DELTREE Q3 < K3.KEY
ECHO --- Q2 and Q3 must be here, not Q1: ---
DIR /A /B Q?
DELTREE Q3 < K4.KEY
ECHO x > Q5.TXT
DELTREE Q5.TXT < K5.KEY
MD Q6
DELTREE Q2 Q5.TXT Q6 < K6.KEY
ECHO --- Q5.TXT and Q6 must be left: ---
DIR /A /B Q*
DEL Q5.TXT
RD Q6
DEL K?.KEY

ECHO ==================== 8. LONG NAMES ====================
MD LongDirectoryName
MD LongDirectoryName\SubFolderName
ECHO x > LongDirectoryName\SubFolderName\ALongFileName.text
DIR /A /B /S LongDirectoryName
DELTREE /Y "LongDirectoryName"
MD MixCase
DELTREE /Y mixcase
ECHO --- nothing must be left: ---
DIR /A /B

ECHO ==================== 9. THE CURRENT DIRECTORY ====================
MD CUR
MD CUR\SUB
MD CUR\OTHER
ECHO x > CUR\X.TXT
ECHO y > CUR\SUB\Y.TXT
CD CUR\SUB
DELTREE /Y \DTT\CUR
IF ERRORLEVEL 1 ECHO errorlevel 1
CD \DTT
ECHO --- only CUR\SUB must be left, and empty: ---
DIR /A /B /S CUR
RD CUR\SUB
RD CUR

ECHO ==================== 10. /Y AFTER THE PATH ====================
MD T10
DELTREE T10 /Y
ECHO --- nothing must be left: ---
DIR /A /B

ECHO ==================== 11. MANY FILES, MANY LEVELS ====================
MD MANY
FOR %%N IN (01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20) DO ECHO x > MANY\F%%N.TXT
FOR %%N IN (21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40) DO ECHO x > MANY\F%%N.TXT
MD MANY\L1
MD MANY\L1\L2
MD MANY\L1\L2\L3
MD MANY\L1\L2\L3\L4
MD MANY\L1\L2\L3\L4\L5
MD MANY\L1\L2\L3\L4\L5\L6
MD MANY\L1\L2\L3\L4\L5\L6\L7
MD MANY\L1\L2\L3\L4\L5\L6\L7\L8
MD MANY\L1\L2\L3\L4\L5\L6\L7\L8\L9
MD MANY\L1\L2\L3\L4\L5\L6\L7\L8\L9\L10
ECHO x > MANY\L1\L2\L3\L4\L5\L6\L7\L8\L9\L10\BOTTOM.TXT
ECHO x > MANY\L1\L2\L3\L4\L5\MIDDLE.TXT
DELTREE /Y MANY
IF ERRORLEVEL 1 ECHO errorlevel 1
ECHO --- nothing must be left: ---
DIR /A /B

ECHO ==================== 12. UNDELETE BRINGS IT BACK ====================
MD UDT
MD UDT\SUB
ECHO one > UDT\ONE.TXT
ECHO two > UDT\SUB\TWO.TXT
DELTREE /Y UDT
UNDELETE \DTT\UDT\*.* /S /ALL
ECHO --- the two files must be back: ---
TYPE UDT\ONE.TXT
TYPE UDT\SUB\TWO.TXT
DELTREE /Y UDT

ECHO ==================== CLEAN UP ====================
ECHO --- nothing must be left: ---
DIR /A /B /S
CD \
RD DTT
C:
CD \
GOTO END

:NODRIVE
ECHO DTTEST needs MULTI.IMG attached as a second disk - it deletes
ECHO whole trees, and never does that on the disk PM-DOS started from.

:END
SET DTW=
ECHO ==== DTTEST DONE ====
