@echo off
rem ===================================================================
rem MKHOST.CMD - build ACROHOST.EXE, ACROREAD's PDF code as a Windows
rem console program (HOST.C says what it does).  Open Watcom 1.9.
rem
rem Not part of BUILD.CMD: nothing here ships.  It is how a page is
rem checked against a file without booting PM-DOS - the same sources,
rem with SYS_NT.C in place of SYS_D32.C and the C library for HOST.C's
rem own printing.  Objects go to HOSTOBJ\ so that they are never taken
rem for the PM-DOS build's.
rem ===================================================================
setlocal
pushd "%0\.."
if not exist HOSTOBJ mkdir HOSTOBJ
if exist HOSTOBJ\*.obj del /q HOSTOBJ\*.obj
set SRCS=rt fx stream lex doc xref filter inflate crypt ccitt dct color raster path names font ttf cff type1 sfont glyph image shade interp text
wasm -q -fo=HOSTOBJ\jmp.obj jmp.asm || goto :fail
for %%F in (%SRCS% sys_nt host) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 -fo=HOSTOBJ\%%F.obj %%F.c || goto :fail
wlink option quiet system nt name ACROHOST.EXE file HOSTOBJ\*.obj || goto :fail
echo ACROHOST.EXE built.
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
