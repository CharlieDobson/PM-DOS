@ECHO OFF
ECHO === BATTEST starts, %%0 is %0 ===
REM this comment must not appear
IF "%1"=="" GOTO NOARG
ECHO arg1 is [%1] and arg2 is [%2]
GOTO ARGDONE
:NOARG
ECHO no arguments were given
:ARGDONE
SET XVAR=expanded
ECHO env XVAR is %XVAR%
ECHO a literal percent: 100%% done
ECHO.
ECHO -- FOR over a plain list --
FOR %%F IN (one two three) DO ECHO   item %%F
ECHO -- FOR over a wildcard --
FOR %%F IN (\*.BAT) DO ECHO   found %%F
ECHO.
ECHO -- SHIFT --
ECHO before shift %0 %1 %2
SHIFT
ECHO after  shift %0 %1 %2
ECHO.
ECHO -- IF EXIST --
IF EXIST \COMMAND.COM ECHO   COMMAND.COM is there
IF NOT EXIST \NOSUCH.XYZ ECHO   NOSUCH.XYZ is not
IF EXIST \NOSUCH.XYZ ECHO   WRONG - should not print
ECHO.
ECHO -- CALL --
CALL \SUB.BAT passed-in
ECHO back from CALL, still running BATTEST
ECHO.
ECHO -- ECHO ON for one line --
ECHO ON
REM this line should echo with the prompt
@ECHO OFF
ECHO === BATTEST done ===
