@echo off
rem ===================================================================
rem MKHOST.CMD - build QBHOST.EXE, QBASIC's interpreter as a Windows
rem console program (HOST.C says what it does).  Open Watcom 1.9.
rem
rem Not part of BUILD.CMD: nothing here ships.  It is how a program's
rem results are checked against real QBasic's without booting anything
rem - the same sources, with SYS_NT.C in place of SYS_D32.C and the C
rem library for HOST.C's own printing.  Objects go to HOSTOBJ\ so that
rem they are never taken for the PM-DOS build's.
rem ===================================================================
setlocal
pushd "%~dp0"
if not exist HOSTOBJ mkdir HOSTOBJ
if exist HOSTOBJ\*.obj del /q HOSTOBJ\*.obj
set SRCS=rt mem num bigdec lex list csym cexpr comp vm qstr rtlib cons using file gfx draw snd mach doc prog
wasm -q -fo=HOSTOBJ\jmp.obj jmp.asm || goto :fail
wasm -q -fpi87 -fo=HOSTOBJ\fpu.obj fpu.asm || goto :fail
for %%F in (%SRCS% sys_nt host) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 -fpi87 -fo=HOSTOBJ\%%F.obj %%F.c || goto :fail
wlink option quiet system nt name QBHOST.EXE file HOSTOBJ\*.obj || goto :fail
echo QBHOST.EXE built.
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
