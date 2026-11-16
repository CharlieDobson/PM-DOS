@echo off
rem ===================================================================
rem MKMP.CMD - build MPLAY.EXE on its own, outside BUILD.CMD, into a
rem folder of your choosing.  Open Watcom 1.9.
rem
rem   MKMP.CMD outdir          outdir\MPLAY.EXE and outdir\*.obj
rem
rem It is built the way BUILD.CMD builds every native program - a flat
rem D32 image linked with no C library, DOSINT.OBJ first because the
rem D32 header is in it, D32BIND putting the MS-DOS stub in front - but
rem nothing goes to BIN.  With no folder named it uses %TEMP%\MPLAY.
rem
rem The codecs: set MPHAVE=VOC MP3 OGG MIDI names the ones built in,
rem and MPSRC their sources.  With neither set, every codec whose first
rem source is here is built in, except WMA, which BUILD.CMD leaves out
rem too; WMA needs WMATAB.C, which is not shipped (NOTES.TXT says why).
rem The wave codec is always there.
rem ===================================================================
setlocal enabledelayedexpansion
pushd "%~dp0"
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\MPLAY
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\*.obj" del /q "%OUT%\*.obj"
if not "%MPHAVE%"=="" goto :have
set MPHAVE=
set MPSRC=
if exist voc.c set MPHAVE=!MPHAVE! VOC& set MPSRC=!MPSRC! voc
if exist mp3.c set MPHAVE=!MPHAVE! MP3& set MPSRC=!MPSRC! mp3 mp3l12 mp3l3 mp3syn mp3tab
if exist ogg.c set MPHAVE=!MPHAVE! OGG& set MPSRC=!MPSRC! ogg vbdec vbmdct vbtab
if exist midi.c set MPHAVE=!MPHAVE! MIDI& set MPSRC=!MPSRC! midi mdopl mdsyn mdbank
:have
set DEFS=
for %%H in (%MPHAVE%) do set DEFS=!DEFS! -dHAVE_%%H
set CC=wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 -i=. -i=..\..\INC %DEFS%
set APP=main view plist dlgs player eqtd fft trig
set UI=scr gfx ui fbox sys_d32
set CORE=rt src eq tag dectab wav
wasm -q -i=..\..\INC -fo="%OUT%\dosint.obj" dosint.asm || goto :fail
wasm -q -fo="%OUT%\jmp.obj" jmp.asm || goto :fail
echo name '%OUT%\MPLAY.EXE'> "%OUT%\mplay.lnk"
echo file '%OUT%\dosint.obj'>> "%OUT%\mplay.lnk"
echo file '%OUT%\jmp.obj'>> "%OUT%\mplay.lnk"
for %%F in (%APP% %UI% %CORE% %MPSRC%) do (
    %CC% -fo="%OUT%\%%~nF.obj" %%F.c || goto :fail
    echo file '%OUT%\%%~nF.obj'>> "%OUT%\mplay.lnk"
)
wlink option quiet form raw bin option offset=0x2000 option map='%OUT%\MPLAY.MAP' @%OUT%\mplay.lnk order clname CODE clname DATA clname BSS clname D32END || goto :fail
..\..\TOOLS\D32BIND.EXE "%OUT%\MPLAY.EXE" ..\..\TOOLS\D32STUB.EXE || goto :fail
echo MPLAY.EXE built in %OUT% with:%MPHAVE%
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
