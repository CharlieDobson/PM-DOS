@echo off
rem ===================================================================
rem MKPW.CMD - build PMWRITE.EXE on its own, outside BUILD.CMD, into a
rem folder of your choosing.  Open Watcom 1.9.
rem
rem   MKPW.CMD outdir          outdir\PMWRITE.EXE and outdir\*.obj
rem
rem It is built the way BUILD.CMD builds every native program - a flat
rem D32 image linked with no C library, DOSINT.OBJ first because the
rem D32 header is in it, D32BIND putting the MS-DOS stub in front - but
rem nothing goes to BIN.  With no folder named it uses %TEMP%\PMWRITE.
rem
rem While some formats are still to be written, F_STUBS.C stands in for
rem them:  set PWHAVE=PDF DOCX ODT  names the ones whose own code is
rem there, and  set PWFSRC=f_pdf f_docx f_odt zip xml  the sources they
rem add.  With neither set, PDF is taken to be there if F_PDF.C is.
rem ===================================================================
setlocal enabledelayedexpansion
pushd "%0\.."
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\PMWRITE
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\*.obj" del /q "%OUT%\*.obj"
if "%PWHAVE%"=="" if exist f_pdf.c set PWHAVE=PDF
if "%PWFSRC%"=="" if exist f_pdf.c set PWFSRC=f_pdf
set DEFS=
for %%H in (%PWHAVE%) do set DEFS=!DEFS! -dHAVE_%%H
set CC=wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 -i=. -i=PDF %DEFS%
set APP=main view bars cmds dlgs files icons
set UI=scr gfx ui fbox sys_d32
set CORE=rt wdoc out cp cptab fmttab wedit lay edit pdfw img flate crc f_txt f_stubs
set PDFS=tf pdfview PDF\fx PDF\stream PDF\lex PDF\doc PDF\xref PDF\filter PDF\inflate PDF\crypt PDF\ccitt PDF\dct PDF\color PDF\raster PDF\path PDF\names PDF\font PDF\ttf PDF\cff PDF\type1 PDF\sfont PDF\glyph PDF\image PDF\shade PDF\interp PDF\text
wasm -q -i=..\..\INC -fo="%OUT%\dosint.obj" dosint.asm || goto :fail
wasm -q -fo="%OUT%\jmp.obj" jmp.asm || goto :fail
echo name '%OUT%\PMWRITE.EXE'> "%OUT%\pmwrite.lnk"
echo file '%OUT%\dosint.obj'>> "%OUT%\pmwrite.lnk"
echo file '%OUT%\jmp.obj'>> "%OUT%\pmwrite.lnk"
for %%F in (%APP% %UI% %CORE% %PDFS% %PWFSRC%) do (
    %CC% -fo="%OUT%\%%~nF.obj" %%F.c || goto :fail
    echo file '%OUT%\%%~nF.obj'>> "%OUT%\pmwrite.lnk"
)
wlink option quiet form raw bin option offset=0x2000 option map='%OUT%\PMWRITE.MAP' @%OUT%\pmwrite.lnk order clname CODE clname DATA clname BSS clname D32END || goto :fail
..\..\TOOLS\D32BIND.EXE "%OUT%\PMWRITE.EXE" ..\..\TOOLS\D32STUB.EXE || goto :fail
echo PMWRITE.EXE built in %OUT%
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
