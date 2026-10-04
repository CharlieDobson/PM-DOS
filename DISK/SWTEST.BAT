@ECHO OFF
REM ====================================================================
REM SWTEST.BAT - the interpreter's new switches.
REM
REM DIR /S and /O, DIRCMD, DEL /S, COPY /A /B /V and FORMAT /S were the
REM "missing surface" list: switches a DOS user reaches for without
REM thinking, every one of which used to be lifted out of the line by
REM sw_scan and then ignored.  A switch that is parsed and dropped is
REM worse than one that is refused, because the command appears to have
REM done what was asked.
REM
REM This is a BATCH file and not a program because what it is testing
REM is the interpreter, and the only honest way to test an interpreter
REM is to type at it.  Read the output; nothing here is self-checking,
REM which is the difference between this and the .COM tests beside it.
REM FORMAT /S is not exercised here on purpose - it formats a disk.
REM ====================================================================
ECHO.
ECHO === SWTEST: setting up a small tree ===
MD SWT
MD SWT\SUB
MD SWT\SUB\DEEP
ECHO one > SWT\ALPHA.TXT
ECHO twotwo > SWT\BETA.DAT
ECHO three > SWT\SUB\GAMMA.TXT
ECHO fourfour > SWT\SUB\DEEP\DELTA.TXT
ECHO five > SWT\SUB\DEEP\EPSILON.DAT

ECHO.
ECHO === 1. DIR with no switches (the listing that always worked) ===
DIR SWT

ECHO.
ECHO === 2. DIR /S - the same pattern, all the way down ===
ECHO     expect ALPHA, GAMMA and DELTA and a grand total
DIR SWT\*.TXT /S

ECHO.
ECHO === 3. DIR /O:N - by name ===
DIR SWT /O:N

ECHO.
ECHO === 4. DIR /O:-N - and backwards ===
DIR SWT /O:-N

ECHO.
ECHO === 5. DIR /O:S - by size, smallest first ===
DIR SWT /O:S

ECHO.
ECHO === 6. DIR /OGN - directories first, then by name ===
DIR SWT /OGN

ECHO.
ECHO === 7. DIR /O:E - by extension ===
ECHO     expect the .DAT before the .TXT
DIR SWT /O:E

ECHO.
ECHO === 8. DIRCMD - /W without asking for it ===
SET DIRCMD=/W
DIR SWT

ECHO.
ECHO === 9. ...and /-W to say "not this one" ===
DIR SWT /-W
SET DIRCMD=

ECHO.
ECHO === 10. COPY /B - a plain copy, byte for byte ===
COPY SWT\ALPHA.TXT SWT\B1.TXT /B
DIR SWT\B1.TXT

ECHO.
ECHO === 11. COPY /V - the same copy with VERIFY on ===
ECHO     it should be slower and otherwise identical
COPY /V SWT\BETA.DAT SWT\B2.DAT
DIR SWT\B2.DAT

ECHO.
ECHO === 12. COPY /A concatenation - the ASCII default ===
COPY SWT\ALPHA.TXT+SWT\BETA.DAT SWT\JOINED.TXT
TYPE SWT\JOINED.TXT

ECHO.
ECHO === 13. VERIFY, from the command that has always offered it ===
VERIFY ON
VERIFY
VERIFY OFF
VERIFY

ECHO.
ECHO === 14. DEL /S - the same pattern, all the way down ===
ECHO     expect the two .DAT files and nothing else
DEL SWT\*.DAT /S

ECHO.
ECHO === 15. what is left ===
DIR SWT /S

ECHO.
ECHO === SWTEST: clearing up ===
DEL SWT\*.* /S
RD SWT\SUB\DEEP
RD SWT\SUB
RD SWT
ECHO === SWTEST done ===
