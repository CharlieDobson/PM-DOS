@echo off
rem ===================================================================
rem MKDEMO.CMD - build PWDEMO.EXE, the test program for PMWRITE's
rem display and user-interface toolkit (PWDEMO.C says what it shows).
rem Open Watcom 1.9.
rem
rem Not part of BUILD.CMD: nothing here ships.  It is built the way
rem BUILD.CMD builds DOSSHELL - a flat D32 image linked with no C
rem library, DOSINT.OBJ first because the D32 header is in it, and
rem D32BIND putting the MS-DOS stub in front - but the objects and the
rem program go to a folder of their own, never to BIN:
rem
rem   MKDEMO.CMD outdir          outdir\PWDEMO.EXE and outdir\*.obj
rem
rem With no folder named they go to %TEMP%\PWDEMO.  The folder's name
rem must have no blanks in it: wlink takes the names as they stand.
rem ===================================================================
setlocal
pushd "%0\.."
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\PWDEMO
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\*.obj" del /q "%OUT%\*.obj"
set CC=wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4
wasm -q -i=..\..\INC -fo=%OUT%\dosint.obj dosint.asm || goto :fail
for %%F in (pwdemo scr gfx ui fbox rt sys_d32) do %CC% -fo=%OUT%\%%F.obj %%F.c || goto :fail
wlink option quiet form raw bin option offset=0x2000 name %OUT%\PWDEMO.EXE ^
    file %OUT%\dosint.obj,%OUT%\pwdemo.obj,%OUT%\scr.obj,%OUT%\gfx.obj,%OUT%\ui.obj,%OUT%\fbox.obj,%OUT%\rt.obj,%OUT%\sys_d32.obj ^
    order clname CODE clname DATA clname BSS clname D32END || goto :fail
..\..\TOOLS\D32BIND.EXE %OUT%\PWDEMO.EXE ..\..\TOOLS\D32STUB.EXE || goto :fail
echo PWDEMO.EXE built in %OUT%
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
