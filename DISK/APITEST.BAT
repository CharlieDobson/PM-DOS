@ECHO OFF
ECHO --- the v0.31 API additions, from a 16-bit program ---
APITEST
IF ERRORLEVEL 1 ECHO *** APITEST REPORTED FAILURES ***
ECHO.
ECHO --- and the same run with stdout redirected to a file ---
ECHO (4401h is skipped when stdout is not a device, which is the point)
APITEST > APITEST.OUT
TYPE APITEST.OUT
DEL APITEST.OUT
ECHO.
ECHO --- VER now reports both numbers ---
VER
ECHO APITEST.BAT done
