@ECHO OFF
ECHO --- the coprocessor, MMX and SSE in a DOS box ---
FPUTEST
IF ERRORLEVEL 1 ECHO FAIL: FPUTEST reported a problem
ECHO --- and in a native program ---
FPU32
IF ERRORLEVEL 1 ECHO FAIL: FPU32 reported a problem
ECHO --- the coprocessor's error in a DPMI client ---
DPMIFPU
IF ERRORLEVEL 1 ECHO FAIL: DPMIFPU reported a problem
ECHO --- and in a 32-bit one ---
DPMIFP32
IF ERRORLEVEL 1 ECHO FAIL: DPMIFP32 reported a problem
ECHO --- an error nobody masked ends the program ---
FPU32 /F
ECHO --- and a handler that clears nothing ends the box ---
FPUTEST /S
ECHO --- and both kinds of program still run ---
FPUTEST
IF ERRORLEVEL 1 ECHO FAIL: FPUTEST reported a problem, the second time
FPU32
IF ERRORLEVEL 1 ECHO FAIL: FPU32 reported a problem, the second time
ECHO FPUTEST.BAT done
