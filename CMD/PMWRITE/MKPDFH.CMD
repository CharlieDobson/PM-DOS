@echo off
rem ===================================================================
rem MKPDFH.CMD - build PDFHOST.EXE: PMWRITE's PDF code (PDF\), its type
rem faces (TF.C) and its PDF viewer calls (PDFVIEW.C) as a Windows
rem console program.  PDFHOST.C says what it does.  Open Watcom 1.9.
rem
rem Not part of BUILD.CMD: nothing here ships.  The objects go to
rem HOSTOBJ\PDFH\ so that they are never taken for the PM-DOS build's
rem or for PWHOST's.  MKHOST.CMD builds PWHOST, which is where F_PDF.C
rem is tried; this one is for pages as pictures.
rem ===================================================================
setlocal
pushd "%0\.."
set OBJ=HOSTOBJ\PDFH
if not exist HOSTOBJ mkdir HOSTOBJ
if not exist %OBJ% mkdir %OBJ%
if exist %OBJ%\*.obj del /q %OBJ%\*.obj
set ENGINE=fx stream lex doc xref filter inflate crypt ccitt dct color raster path names font ttf cff type1 sfont glyph image shade interp text
wasm -q -fo=%OBJ%\jmp.obj jmp.asm || goto :fail
for %%F in (%ENGINE%) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 -i=. -i=PDF -fo=%OBJ%\%%F.obj PDF\%%F.c || goto :fail
for %%F in (rt tf pdfview sys_nt pdfhost) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 -i=. -i=PDF -fo=%OBJ%\%%F.obj %%F.c || goto :fail
wlink option quiet system nt name PDFHOST.EXE file %OBJ%\*.obj || goto :fail
echo PDFHOST.EXE built.
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
