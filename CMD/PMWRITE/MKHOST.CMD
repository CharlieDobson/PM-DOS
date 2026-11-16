@echo off
rem ===================================================================
rem MKHOST.CMD - build PWHOST.EXE, PMWRITE's document and format code
rem as a Windows console program (HOST.C says what it does).  Open
rem Watcom 1.9.
rem
rem Not part of BUILD.CMD: nothing here ships.  It is how a format is
rem checked against a file without booting PM-DOS - the same sources,
rem with SYS_NT.C in place of SYS_D32.C and the C library for HOST.C's
rem own printing.  Objects go under HOSTOBJ\ so that they are never
rem taken for the PM-DOS build's.
rem
rem With nothing set it builds everything there is (the list at :all).
rem Someone working on one format builds only that format, into a
rem program and an object directory of its own, so that a half-written
rem file somebody else is working on cannot break the build:
rem
rem   set PWTAG=DOCX                 the program is PWH_DOCX.EXE
rem   set PWHAVE=DOCX ODT            the formats whose code is built in
rem                                  (F_STUBS.C stands in for the rest)
rem   set PWSRC=f_docx f_odt zip     the sources that takes, no ".c";
rem                                  one in PDF\ is written PDF\name
rem ===================================================================
setlocal enabledelayedexpansion
pushd "%0\.."
if "%PWTAG%"=="" goto :all
set EXE=PWH_%PWTAG%.EXE
goto :build
:all
set PWTAG=ALL
set EXE=PWHOST.EXE
set PWHAVE=
set PWSRC=
:build
set OBJ=HOSTOBJ\%PWTAG%
if not exist HOSTOBJ mkdir HOSTOBJ
if not exist %OBJ% mkdir %OBJ%
if exist %OBJ%\*.obj del /q %OBJ%\*.obj
set DEFS=
for %%H in (%PWHAVE%) do set DEFS=!DEFS! -dHAVE_%%H
wasm -q -fo=%OBJ%\jmp.obj jmp.asm || goto :fail
for %%F in (rt wdoc out cp cptab dump fmttab f_txt f_stubs %PWSRC% sys_nt host) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 %DEFS% -i=. -i=PDF -fo=%OBJ%\%%~nF.obj %%F.c || goto :fail
wlink option quiet system nt name %EXE% file %OBJ%\*.obj || goto :fail
echo %EXE% built.
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
