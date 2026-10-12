@echo off
rem ===================================================================
rem MKHOST.CMD - build DEFRAGNT.EXE, DEFRAG's reading and moving of a
rem drive as a Windows console program that works on a disk image file
rem (SYS_NT.C says how it is run).  Open Watcom 1.9.
rem
rem Not part of BUILD.CMD: nothing here ships.  It is how a move is
rem checked against an image without booting PM-DOS - the same sources,
rem with SYS_NT.C in place of SYS_D32.C and HOSTFE.C, lines of text, in
rem place of the screen - and how the power is cut in the middle of
rem one (DFCRASH).  Objects go to HOSTOBJ\ so that they are never taken
rem for the PM-DOS build's.
rem ===================================================================
setlocal
pushd "%0\.."
if not exist HOSTOBJ mkdir HOSTOBJ
if exist HOSTOBJ\*.obj del /q HOSTOBJ\*.obj
wasm -q -fo=HOSTOBJ\jmp.obj jmp.asm || goto :fail
for %%F in (main vol tree dirs move optim text hostfe rt sys_nt) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 -dHOSTBUILD -fo=HOSTOBJ\%%F.obj %%F.c || goto :fail
wlink option quiet system nt name DEFRAGNT.EXE file HOSTOBJ\*.obj || goto :fail
echo DEFRAGNT.EXE built.
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
