@ECHO OFF
ECHO --- a 16-bit program catching its OWN exceptions ---
UDTEST
IF ERRORLEVEL 1 ECHO *** UDTEST reported failures ***
ECHO.
ECHO --- and one that cannot: the DOS box must end, the machine must not ---
UDTEST /HANG
ECHO --- back at the shell, which is the whole point ---
VER
ECHO UDTEST done
