@echo off
rem ===================================================================
rem MKHOST.CMD - build SCANDKNT.EXE, SCANDISK's checks as a Windows
rem console program that works on disk image files (SYS_NT.C says how
rem it is run).  Open Watcom 1.9.
rem
rem Not part of BUILD.CMD: nothing here ships.  It is how a repair is
rem checked against a damaged image without booting PM-DOS - the same
rem sources, with SYS_NT.C in place of SYS_D32.C and HOSTFE.C, lines of
rem text, in place of the screens.  Objects go to HOSTOBJ\ so that they
rem are never taken for the PM-DOS build's.
rem ===================================================================
setlocal
pushd "%0\.."
if not exist HOSTOBJ mkdir HOSTOBJ
if exist HOSTOBJ\*.obj del /q HOSTOBJ\*.obj
wasm -q -fo=HOSTOBJ\jmp.obj jmp.asm || goto :fail
for %%F in (main ini vol lfn check filesys surface undo frag problem text hostfe rt sys_nt) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 -dHOSTBUILD -fo=HOSTOBJ\%%F.obj %%F.c || goto :fail
wlink option quiet system nt name SCANDKNT.EXE file HOSTOBJ\*.obj || goto :fail
echo SCANDKNT.EXE built.
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
