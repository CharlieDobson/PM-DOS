@echo off
rem ===================================================================
rem MKPTEST.CMD - build PTEST.EXE, the test of PDFW.C (PTEST.C says how
rem it is run), as a Windows console program.  Open Watcom 1.9.  Not
rem part of BUILD.CMD.
rem
rem It is built with the real type faces and the PDF code, because the
rem PDF writer's widths are the faces' and its pictures are decoded by
rem IMG.C, which uses the PDF code's JPEG reader.
rem
rem   set PTFMT=DOCX ODT            formats whose own code is built in
rem   set PTSRC=f_docx f_odt zip    ...and the sources that takes
rem ===================================================================
setlocal enabledelayedexpansion
pushd "%0\.."
if not exist HOSTOBJ mkdir HOSTOBJ
if not exist HOSTOBJ\PTEST mkdir HOSTOBJ\PTEST
if exist HOSTOBJ\PTEST\*.obj del /q HOSTOBJ\PTEST\*.obj
set DEFS=-dHAVE_PDF
for %%H in (%PTFMT%) do set DEFS=!DEFS! -dHAVE_%%H
set PDFS=f_pdf tf pdfview img PDF\fx PDF\stream PDF\lex PDF\doc PDF\xref PDF\filter PDF\inflate PDF\crypt PDF\ccitt PDF\dct PDF\color PDF\raster PDF\path PDF\names PDF\font PDF\ttf PDF\cff PDF\type1 PDF\sfont PDF\glyph PDF\image PDF\shade PDF\interp PDF\text
wasm -q -fo=HOSTOBJ\PTEST\jmp.obj jmp.asm || goto :fail
for %%F in (rt wdoc out cp cptab dump wedit lay pdfw flate crc fmttab f_txt f_stubs %PDFS% %PTSRC% sys_nt ptest) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 %DEFS% -i=. -i=PDF -fo=HOSTOBJ\PTEST\%%~nF.obj %%F.c || goto :fail
wlink option quiet system nt name PTEST.EXE file HOSTOBJ\PTEST\*.obj || goto :fail
echo PTEST.EXE built.
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
