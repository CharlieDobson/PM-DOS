@ECHO OFF
ECHO --- the 52h list of lists, from a 16-bit program ---
LOLTEST
IF ERRORLEVEL 1 ECHO *** LOLTEST REPORTED FAILURES ***
ECHO.
ECHO --- and what the 32-bit side sees of the same memory ---
ECHO (MEM walks the kernel's arena; LOLTEST walked the one a DOS box
ECHO  lives in.  Two arenas, two answers, and 52h has to give a guest
ECHO  the second one - which is the bug that froze Manifest.)
MEM
ECHO LOLTEST.BAT done
