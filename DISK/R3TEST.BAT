@ECHO OFF
REM ====================================================================
REM R3TEST.BAT - the ring-3 release, end to end.
REM
REM R3TEST checks what a native program can still do from out at ring 3;
REM the two CRASH lines check the two things it can no longer do at all.
REM MEM at the end is not decoration - it is the one program that had to
REM be rewritten for this release, because it used to read the arena
REM instead of asking about it.
REM ====================================================================
ECHO --- a native program, and where it is standing ---
R3TEST
IF ERRORLEVEL 1 ECHO *** R3TEST FAILED ***
ECHO --- what a program can no longer reach ---
CRASH /K
IF ERRORLEVEL 255 ECHO OK: the kernel is out of reach
CRASH /N
IF ERRORLEVEL 255 ECHO OK: address zero is out of reach
ECHO --- and the shell is still here, and MEM still works ---
MEM
VER
ECHO R3TEST done
