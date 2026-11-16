@echo off
rem ===================================================================
rem MKHOST.CMD - build MPHOST.EXE, MPLAY's decoders as a Windows
rem console program (MPHOST.C says what it does).  Open Watcom 1.9.
rem
rem Not part of BUILD.CMD: nothing here ships.  It is how a codec is
rem checked against a file without booting PM-DOS - the same sources,
rem with SYS_NT.C in place of SYS_D32.C and the C library for MPHOST.C's
rem own printing.  Objects go under HOSTOBJ\ so that they are never
rem taken for the PM-DOS build's.
rem
rem Someone working on one codec builds only that codec, into a program
rem and an object directory of its own, so that a half-written file
rem somebody else is working on cannot break the build:
rem
rem   set MPTAG=MP3                  the program is MPH_MP3.EXE
rem   set MPHAVE=MP3                 the codecs whose code is built in
rem                                  (HAVE_MP3 for DECTAB.C); the wave
rem                                  codec is always there
rem   set MPSRC=mp3 mp3tab           the sources that takes, no ".c"
rem
rem With MPTAG unset it builds MPHOST.EXE with whatever MPHAVE and
rem MPSRC say (both may be empty: the wave codec alone).
rem ===================================================================
setlocal enabledelayedexpansion
pushd "%~dp0"
if "%MPTAG%"=="" (
    set EXE=MPHOST.EXE
    set OBJ=HOSTOBJ\ALL
) else (
    set EXE=MPH_%MPTAG%.EXE
    set OBJ=HOSTOBJ\%MPTAG%
)
if not exist HOSTOBJ mkdir HOSTOBJ
if not exist %OBJ% mkdir %OBJ%
if exist %OBJ%\*.obj del /q %OBJ%\*.obj
set DEFS=
for %%H in (%MPHAVE%) do set DEFS=!DEFS! -dHAVE_%%H
wasm -q -fo=%OBJ%\jmp.obj jmp.asm || goto :fail
for %%F in (rt src eq tag dectab wav %MPSRC% sys_nt) do wcc386 -q -bt=nt -mf -3r -s -ox -w4 -we -zp4 %DEFS% -i=. -fo=%OBJ%\%%~nF.obj %%F.c || goto :fail
wcc386 -q -bt=nt -mf -3r -ox -w4 -zp4 %DEFS% -i=. -fo=%OBJ%\mphost.obj mphost.c || goto :fail
wlink option quiet system nt name %EXE% file %OBJ%\*.obj || goto :fail
echo %EXE% built.
popd
exit /b 0
:fail
echo BUILD FAILED
popd
exit /b 1
