@echo off
rem ===================================================================
rem MKLTEST.CMD - build LTEST.EXE, the test of LAY.C (LTEST.C says what
rem it prints), as a Windows console program.  Open Watcom 1.9.  It is
rem built with TFSTUB.C's made-up widths, so that what it prints does
rem not change when a font does.  Not part of BUILD.CMD.
rem
rem   set LTFMT=DOCX ODT            formats whose own code is built in
rem   set LTSRC=f_docx f_odt zip    ...and the sources that takes
rem ===================================================================
setlocal enabledelayedexpansion
pushd "%0\.."
if not exist HOSTOBJ mkdir HOSTOBJ
if not exist HOSTOBJ\LTEST mkdir HOSTOBJ\LTEST
if exist HOSTOBJ\LTEST\*.obj del /q HOSTOBJ\LTEST\*.obj
set DEFS=
for %%H in (%LTFMT%) do set DEFS=!DEFS! -dHAVE_%%H
wasm -q -fo=HOSTOBJ\LTEST\jmp.obj jmp.asm || goto :fail
for %%F in (rt wdoc out cp cptab dump wedit lay tfstub fmttab f_txt f_stubs %LTSRC% sys_nt ltest) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 %DEFS% -i=. -i=PDF -fo=HOSTOBJ\LTEST\%%~nF.obj %%F.c || goto :fail
wlink option quiet system nt name LTEST.EXE file HOSTOBJ\LTEST\*.obj || goto :fail
echo LTEST.EXE built.
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
