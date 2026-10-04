@ECHO OFF
REM C16TEST.BAT - the 16-bit COMMAND.COM, the shell DOS programs get.
REM Run it through that shell, from CMD.EXE:
REM     COMMAND /C C16TEST.BAT >> %LOG%
REM Everything here is DOS 6.22's batch language and internal commands,
REM and its output was checked line for line against MS-DOS 6.22's own
REM COMMAND.COM running the same file (see NOTES.TXT, v0.60).  Nothing
REM external but ARGS.COM, which exits with the first digit of its tail
REM - the one 16-bit program on the disk that can say an errorlevel.
REM ARGS.COM has to be on the PATH, since the test does its work in \C16.
REM Works in \C16 on the current drive, and takes it away afterwards.
ECHO === C16TEST: the 16-bit COMMAND.COM ===
VER
ECHO --- echo forms
ECHO hello world
ECHO.
ECHO   two of these three blanks survive
ECHO
ECHO ON
ECHO echo is on now
@ECHO OFF
ECHO --- %%-expansion
SET C16VAR=abc def
ECHO var is [%C16VAR%]
SET C16VAR=
ECHO var gone [%C16VAR%]
ECHO percent 50% done
ECHO double %% sign
ECHO --- parameters, CALL and SHIFT
CALL C16SUB.BAT one two three four five six seven eight nine ten eleven
ECHO back from C16SUB
ECHO --- IF
IF EXIST %0 ECHO exist ok
IF NOT EXIST NOPE.TXT ECHO not exist ok
IF abc==abc ECHO string eq ok
IF NOT abc==abd ECHO string ne ok
IF "%1"=="" ECHO empty param ok
ARGS.COM 3 > NUL
IF ERRORLEVEL 4 ECHO WRONG - errorlevel 4
IF ERRORLEVEL 3 ECHO errorlevel 3 ok
IF NOT ERRORLEVEL 3 ECHO WRONG - not errorlevel 3
ECHO --- FOR, and FOR with CALL in order
FOR %%F IN (a b c) DO ECHO item %%F
FOR %%F IN (x y) DO CALL C16SUB.BAT %%F
ECHO --- GOTO
GOTO skip
ECHO WRONG - GOTO fell through
:skip
ECHO after skip
GOTO :SKIP2
ECHO WRONG - GOTO fell through again
:skip2
ECHO after skip2, which case did not matter to
ECHO --- directories
MD \C16
CD \C16
CD
MD SUB
RD SUB
IF NOT EXIST SUB\NUL ECHO RD removed SUB
ECHO --- files
ECHO line one> F1.TXT
ECHO line two>> F1.TXT
TYPE F1.TXT
COPY F1.TXT F2.TXT
COPY F1.TXT+F2.TXT F3.TXT
TYPE F3.TXT
REN F3.TXT F4.TXT
TYPE F4.TXT
REN F?.TXT G?.TXT
DIR /B /ON G*.TXT
COPY G*.TXT ALL.TXT
DIR /B ALL.TXT
DEL G?.TXT
DEL ALL.TXT
DIR /B
ECHO --- a pipe: a second COMMAND.COM reading its commands from one
ECHO ECHO this line came through the pipe | %COMSPEC%
ECHO --- output of a 16-bit program sent to a file
ARGS.COM from a file > ARGS.TXT
TYPE ARGS.TXT
DEL ARGS.TXT
CD \
RD \C16
IF NOT EXIST \C16\NUL ECHO \C16 is gone
ECHO === C16TEST done ===
