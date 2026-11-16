@echo off
rem ===================================================================
rem MKETEST.CMD - build ETEST.EXE, the test of WEDIT.C (ETEST.C says
rem what it checks), as a Windows console program.  Open Watcom 1.9.
rem Not part of BUILD.CMD: nothing here ships.
rem ===================================================================
setlocal
pushd "%0\.."
if not exist HOSTOBJ mkdir HOSTOBJ
if not exist HOSTOBJ\ETEST mkdir HOSTOBJ\ETEST
if exist HOSTOBJ\ETEST\*.obj del /q HOSTOBJ\ETEST\*.obj
wasm -q -fo=HOSTOBJ\ETEST\jmp.obj jmp.asm || goto :fail
for %%F in (rt wdoc out cp cptab dump wedit sys_nt etest) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 -fo=HOSTOBJ\ETEST\%%F.obj %%F.c || goto :fail
wlink option quiet system nt name ETEST.EXE file HOSTOBJ\ETEST\*.obj || goto :fail
echo ETEST.EXE built.
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
