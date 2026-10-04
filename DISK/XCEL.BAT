@ECHO OFF
REM XCEL.BAT - says what errorlevel the last program left, for tests that
REM run under a shell with no %ERRORLEVEL%.  CALLed right after it.
IF ERRORLEVEL 6 GOTO BIG
IF ERRORLEVEL 5 GOTO E5
IF ERRORLEVEL 4 GOTO E4
IF ERRORLEVEL 3 GOTO E3
IF ERRORLEVEL 2 GOTO E2
IF ERRORLEVEL 1 GOTO E1
ECHO [errorlevel 0]
GOTO END
:E1
ECHO [errorlevel 1]
GOTO END
:E2
ECHO [errorlevel 2]
GOTO END
:E3
ECHO [errorlevel 3]
GOTO END
:E4
ECHO [errorlevel 4]
GOTO END
:E5
ECHO [errorlevel 5]
GOTO END
:BIG
ECHO [errorlevel 6 or more]
:END
