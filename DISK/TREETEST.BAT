@ECHO OFF
REM ====================================================================
REM TREETEST.BAT - TREE.EXE, step by step, on a small tree it makes in
REM C:\TREETEST and takes away again.  Everything here is an ordinary
REM file or directory, so it works on the disk PM-DOS booted from.
REM
REM The drawing is MS-DOS 6.22's, measured against the real TREE.COM:
REM each level is four columns, a subdirectory is "+---" (or "\---"
REM for the last, with /A), and with /F a directory's files come first,
REM then a line of the columns alone.
REM
REM WHAT IT IS ASKING, step by step:
REM
REM   1  the drawing, from a path: ONE (with TWO and THREE under it,
REM      THREE last), then FOUR, last of all
REM   2  /F: the files above the subdirectories, and the empty line
REM      after them with its bar
REM   3  /A: the same tree in + - \ |
REM   4  no path: the heading line is "C:." and the tree is the
REM      current directory's
REM   5  a relative path, and a backslash on the end, which is dropped
REM   6  a hidden directory is not drawn, but can be given as the path
REM   7  a path that is no directory: "Invalid path - \...", then "No
REM      sub-directories exist", and errorlevel 1
REM   8  long names, and a path with a blank in it, in quotes
REM   9  the switch errors, and /?, which leaves errorlevel 1
REM ====================================================================
C:
CD \
MD TREETEST
CD TREETEST
MD ONE
MD ONE\TWO
MD ONE\THREE
MD FOUR
MD HIDE
MD Long Directory Name
ECHO top > TOP.TXT
ECHO two > ONE\TWO\IN2.TXT
ECHO four > FOUR\IN4.TXT
REM A redirection's name ends at a blank, so into the directory first
CD Long Directory Name
ECHO long > LongFileName.Text
CD ..
ATTRIB +H HIDE

ECHO ==================== 1. THE DRAWING ====================
ECHO --- ONE, TWO and THREE below it, FOUR, Long Directory Name ---
TREE C:\TREETEST

ECHO ==================== 2. /F ====================
ECHO --- TOP.TXT first, with a bar in front; IN2.TXT under TWO ---
TREE C:\TREETEST /F

ECHO ==================== 3. /A ====================
TREE C:\TREETEST /A

ECHO ==================== 4. NO PATH ====================
ECHO --- the heading must end "C:." ---
CD ONE
TREE

ECHO ==================== 5. RELATIVE, AND A BACKSLASH ON THE END ======
ECHO --- "C:..\FOUR" (no backslash on the end) and no subdirectories ---
TREE ..\FOUR\
CD ..

ECHO ==================== 6. A HIDDEN DIRECTORY ====================
ECHO --- HIDE is not in the tree of step 1; by name it is a tree ---
TREE C:\TREETEST\HIDE

ECHO ==================== 7. NOT A DIRECTORY ====================
TREE C:\TREETEST\TOP.TXT
IF ERRORLEVEL 1 ECHO --- errorlevel 1, as it must be ---
TREE C:\TREETEST\NOSUCH
IF ERRORLEVEL 1 ECHO --- errorlevel 1, as it must be ---

ECHO ==================== 8. LONG NAMES ====================
ECHO --- the names in full, the path in capitals ---
TREE "C:\TREETEST\Long Directory Name" /F

ECHO ==================== 9. THE SWITCH ERRORS, AND /? ================
ECHO --- "Invalid switch - /X" ---
TREE /X
ECHO --- "Invalid switch - /F": a switch counts once ---
TREE /F /F
ECHO --- "Too many parameters - C:\" ---
TREE C:\TREETEST C:\
ECHO --- the help, and errorlevel 1 ---
TREE /?
IF ERRORLEVEL 1 ECHO --- errorlevel 1, as it must be ---

ECHO ==================== CLEAN UP ====================
REM Named one at a time rather than swept up with a wildcard: if one
REM of these is already gone, DEL says so and the failure is visible.
ATTRIB -H HIDE
RD HIDE
DEL TOP.TXT
DEL ONE\TWO\IN2.TXT
DEL FOUR\IN4.TXT
CD Long Directory Name
DEL LongFileName.Text
CD ..
RD ONE\TWO
RD ONE\THREE
RD ONE
RD FOUR
RD Long Directory Name
CD \
RD TREETEST
ECHO ==== TREETEST DONE ====
