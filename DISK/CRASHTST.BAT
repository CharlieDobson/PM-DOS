@ECHO OFF
ECHO --- a 32-bit program faults in the middle of a batch file ---
ECHO before the crash
CRASH
IF ERRORLEVEL 255 ECHO OK: errorlevel 255, and this batch file is still running
ECHO --- and again, so the recovery is not a one-shot ---
CRASH /D
IF ERRORLEVEL 255 ECHO OK: caught the second one too
ECHO --- and an invalid opcode ---
CRASH /I
IF ERRORLEVEL 255 ECHO OK: caught the third
ECHO --- and a null pointer, which before v0.31 did not fault at all ---
CRASH /N
IF ERRORLEVEL 255 ECHO OK: the null write was caught
ECHO --- the shell is intact: prove it does real work afterwards ---
DIR CRASH.EXE
VER
ECHO CRASHTST done
