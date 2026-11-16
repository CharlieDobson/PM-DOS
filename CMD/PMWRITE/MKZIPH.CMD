@echo off
rem ===================================================================
rem MKZIPH.CMD - build ZIPHOST.EXE, the checks of CRC.C, FLATE.C, ZIP.C
rem and XML.C as a Windows console program (ZIPHOST.C says what it
rem does).  Open Watcom 1.9.
rem
rem Not part of BUILD.CMD: nothing here ships.  Objects go under
rem HOSTOBJ\ZIPH so that they are never taken for anybody else's.
rem
rem   MKZIPH            the four libraries only
rem   MKZIPH FORMATS    with F_DOCX.C and F_ODT.C as well, built to
rem                     keep count of the heap (PW_PEAK), for
rem                     ZIPHOST's "load", "save" and "text"
rem   MKZIPH DOCX       the same with F_DOCX.C alone
rem ===================================================================
setlocal enabledelayedexpansion
pushd "%0\.."
set OBJ=HOSTOBJ\ZIPH
set DEFS=
set MORE=
if /i "%1"=="FORMATS" set DEFS=-dPW_FORMATS -dPW_PEAK -dHAVE_DOCX -dHAVE_ODT
if /i "%1"=="FORMATS" set MORE=dump fmttab f_txt f_stubs f_docx f_odt
if /i "%1"=="DOCX" set DEFS=-dPW_FORMATS -dPW_PEAK -dHAVE_DOCX
if /i "%1"=="DOCX" set MORE=dump fmttab f_txt f_stubs f_docx
if not exist HOSTOBJ mkdir HOSTOBJ
if not exist %OBJ% mkdir %OBJ%
if exist %OBJ%\*.obj del /q %OBJ%\*.obj
wasm -q -fo=%OBJ%\jmp.obj jmp.asm || goto :fail
for %%F in (rt wdoc out cp cptab crc flate zip xml %MORE% sys_nt ziphost) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 %DEFS% -i=. -fo=%OBJ%\%%~nF.obj %%F.c || goto :fail
wlink option quiet system nt name ZIPHOST.EXE file %OBJ%\*.obj || goto :fail
echo ZIPHOST.EXE built.
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
