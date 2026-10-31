@echo off
rem BUILD.CMD - build the PM-DOS SDK: its C library for each compiler,
rem the host tools, and the samples.
rem
rem   BUILD              every compiler that is installed
rem   BUILD watcom       Open Watcom 1.9 only (also gcc, clang, djgpp)
rem
rem Run it in the SDK directory.  Open Watcom must be on the PATH.  The
rem others are looked for on the PATH and in the places the SDK's
rem README names (C:\I686ELF\bin, C:\Program Files\LLVM\bin,
rem C:\DJGPP\bin); a compiler that is not found is skipped with a
rem message, not an error.
rem
rem What it makes:
rem   LIB\WATCOM\PMC3R.LIB, PMC3S.LIB   the library for -3r and -3s
rem   LIB\GCC\libpmc.a                  for i686-elf-gcc
rem   LIB\CLANG\libpmc.a                for clang --target=i386-none-elf
rem   LIB\DJGPP\libpmc.a                for i586-pc-msdosdjgpp-gcc
rem   BIN\D32BIND.EXE, BIN\D32STUB.EXE  the host tools
rem   OUT\<compiler>\*.EXE              the samples and tests
rem
rem The Open Watcom part keeps to what NT 3.51's CMD.EXE can run: no
rem ~ modifiers, no CALL :label, no = inside a SET.

setlocal
set PMSDK=.
if exist C:\I686ELF\bin\i686-elf-gcc.exe set PATH=C:\I686ELF\bin;%PATH%
if exist C:\DJGPP\bin\i586-pc-msdosdjgpp-gcc.exe set PATH=C:\DJGPP\bin;%PATH%
if exist "C:\Program Files\LLVM\bin\clang.exe" set PATH=C:\Program Files\LLVM\bin;%PATH%

set WANT=%1 %2 %3 %4
rem the programs: SAMPLES\*.C and TESTS\*.C by name
set PROGS=hello libtest libtest2 libtest3
rem the library: SRC\*.C, and the assembly for each compiler
set MODS=dos start exit heap mem str strx ctype conv misc env io fmt fmtflt stdio printf files
set MODS2=strtod scanf mathbits mathfn mathspec time signal assert locale inttypes dir dos2 conio process
set GNUASM=crt0 divdi x87 setjmp
if "%1"=="" set WANT=watcom gcc clang djgpp

if not exist OBJ\nul md OBJ
if not exist LIB\nul md LIB
if not exist BIN\nul md BIN
if not exist OUT\nul md OUT

rem --- the host tools ------------------------------------------------
rem D32BIND puts a 16-bit MS-DOS program in front of a D32 image; the
rem SDK's default is D32STUB, which says the program needs PM-DOS.
echo === host tools ===
wcl386 -q -bt=nt -fe=BIN\D32BIND.EXE -fo=OBJ\D32BIND.OBJ ..\TOOLS\D32BIND.C || goto :fail
wasm -q -fo=OBJ\D32STUB.OBJ ..\TOOLS\D32STUB.ASM || goto :fail
wlink form raw bin option offset=0x0 option quiet name BIN\D32STUB.EXE file OBJ\D32STUB.OBJ || goto :fail

rem The library and the programs use the SDK's headers and no others.
set INCLUDE=

for %%T in (%WANT%) do if "%%T"=="watcom" goto :watcom
goto :afterwatcom

rem --- Open Watcom ---------------------------------------------------
:watcom
echo === Open Watcom ===
if not exist OBJ\W3R\nul md OBJ\W3R
if not exist OBJ\W3S\nul md OBJ\W3S
if not exist LIB\WATCOM\nul md LIB\WATCOM
if not exist OUT\WATCOM\nul md OUT\WATCOM
wasm -q -fo=OBJ\W3R\c0d32.obj SRC\WATCOM\c0d32.asm || goto :fail
wasm -q -fo=OBJ\W3R\i8.obj SRC\WATCOM\i8.asm || goto :fail
wasm -q -fo=OBJ\W3R\x87.obj SRC\WATCOM\x87.asm || goto :fail
wasm -q -fo=OBJ\W3R\setjmp.obj SRC\WATCOM\setjmp.asm || goto :fail
copy OBJ\W3R\c0d32.obj OBJ\W3S\c0d32.obj > nul
copy OBJ\W3R\i8.obj OBJ\W3S\i8.obj > nul
copy OBJ\W3R\x87.obj OBJ\W3S\x87.obj > nul
copy OBJ\W3R\setjmp.obj OBJ\W3S\setjmp.obj > nul
if exist OBJ\W3R.LBC del OBJ\W3R.LBC
if exist OBJ\W3S.LBC del OBJ\W3S.LBC
echo +OBJ\W3R\c0d32.obj +OBJ\W3R\i8.obj +OBJ\W3R\x87.obj +OBJ\W3R\setjmp.obj > OBJ\W3R.LBC
echo +OBJ\W3S\c0d32.obj +OBJ\W3S\i8.obj +OBJ\W3S\x87.obj +OBJ\W3S\setjmp.obj > OBJ\W3S.LBC
for %%F in (%MODS%) do wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -fpi87 -w4 -we -i=INCLUDE -fo=OBJ\W3R\%%F.obj SRC\%%F.c || goto :fail
for %%F in (%MODS2%) do wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -fpi87 -w4 -we -i=INCLUDE -fo=OBJ\W3R\%%F.obj SRC\%%F.c || goto :fail
for %%F in (%MODS%) do wcc386 -q -bt=dos -mf -3s -s -zl -zld -ox -fpi87 -w4 -we -i=INCLUDE -fo=OBJ\W3S\%%F.obj SRC\%%F.c || goto :fail
for %%F in (%MODS2%) do wcc386 -q -bt=dos -mf -3s -s -zl -zld -ox -fpi87 -w4 -we -i=INCLUDE -fo=OBJ\W3S\%%F.obj SRC\%%F.c || goto :fail
for %%F in (%MODS%) do echo +OBJ\W3R\%%F.obj >> OBJ\W3R.LBC
for %%F in (%MODS2%) do echo +OBJ\W3R\%%F.obj >> OBJ\W3R.LBC
for %%F in (%MODS%) do echo +OBJ\W3S\%%F.obj >> OBJ\W3S.LBC
for %%F in (%MODS2%) do echo +OBJ\W3S\%%F.obj >> OBJ\W3S.LBC
if exist LIB\WATCOM\PMC3R.LIB del LIB\WATCOM\PMC3R.LIB
if exist LIB\WATCOM\PMC3S.LIB del LIB\WATCOM\PMC3S.LIB
wlib -q -n -b LIB\WATCOM\PMC3R.LIB @OBJ\W3R.LBC || goto :fail
wlib -q -n -b LIB\WATCOM\PMC3S.LIB @OBJ\W3S.LBC || goto :fail

for %%D in (SAMPLES TESTS) do for %%P in (%PROGS%) do if exist %%D\%%P.c wcc386 -q -bt=dos -mf -3r -ox -fpi87 -w4 -i=INCLUDE -fo=OBJ\W3R\%%P.obj %%D\%%P.c || goto :fail
for %%P in (%PROGS%) do wlink @LINK\D32.LNK name OUT\WATCOM\%%P.EXE file OBJ\W3R\%%P.obj || goto :fail
for %%P in (%PROGS%) do BIN\D32BIND.EXE OUT\WATCOM\%%P.EXE BIN\D32STUB.EXE || goto :fail
:afterwatcom

rem --- GCC (i686-elf) ------------------------------------------------
for %%T in (%WANT%) do if "%%T"=="gcc" goto :gcc
goto :aftergcc
:gcc
echo === GCC ===
i686-elf-gcc --version > nul 2> nul
if errorlevel 1 goto :nogcc
if not exist OBJ\GCC\nul md OBJ\GCC
if not exist LIB\GCC\nul md LIB\GCC
if not exist OUT\GCC\nul md OUT\GCC
for %%F in (%GNUASM%) do i686-elf-gcc -march=i486 -nostdinc -c -o OBJ\GCC\%%F.o SRC\GNU\%%F.S || goto :fail
for %%F in (%MODS%) do i686-elf-gcc -march=i486 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -nostdinc -isystem INCLUDE -Wall -Wextra -c -o OBJ\GCC\%%F.o SRC\%%F.c || goto :fail
for %%F in (%MODS2%) do i686-elf-gcc -march=i486 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -nostdinc -isystem INCLUDE -Wall -Wextra -c -o OBJ\GCC\%%F.o SRC\%%F.c || goto :fail
i686-elf-gcc -march=i486 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -nostdinc -isystem INCLUDE -Wall -Wextra -c -o OBJ\GCC\libgcc.o SRC\GNU\libgcc.c || goto :fail
if exist LIB\GCC\libpmc.a del LIB\GCC\libpmc.a
for %%O in (OBJ\GCC\*.o) do i686-elf-ar rcs LIB\GCC\libpmc.a %%O || goto :fail
for %%D in (SAMPLES TESTS) do for %%P in (%PROGS%) do if exist %%D\%%P.c i686-elf-gcc -march=i486 -O2 -ffreestanding -fno-pic -nostdinc -isystem INCLUDE -Wall -c -o OBJ\GCC\%%P.x %%D\%%P.c || goto :fail
for %%P in (%PROGS%) do i686-elf-gcc -nostdlib -T LINK\d32.ld -o OBJ\GCC\%%P.elf OBJ\GCC\%%P.x -LLIB\GCC -lpmc || goto :fail
for %%P in (%PROGS%) do i686-elf-objcopy -O binary OBJ\GCC\%%P.elf OUT\GCC\%%P.EXE || goto :fail
for %%P in (%PROGS%) do BIN\D32BIND.EXE OUT\GCC\%%P.EXE BIN\D32STUB.EXE || goto :fail
goto :aftergcc
:nogcc
echo i686-elf-gcc was not found: GCC skipped.
:aftergcc

rem --- Clang (i386-none-elf) -----------------------------------------
for %%T in (%WANT%) do if "%%T"=="clang" goto :clang
goto :afterclang
:clang
echo === Clang ===
clang --version > nul 2> nul
if errorlevel 1 goto :noclang
if not exist OBJ\CLANG\nul md OBJ\CLANG
if not exist LIB\CLANG\nul md LIB\CLANG
if not exist OUT\CLANG\nul md OUT\CLANG
for %%F in (%GNUASM%) do clang --target=i386-unknown-none-elf -march=i486 -nostdinc -c -o OBJ\CLANG\%%F.o SRC\GNU\%%F.S || goto :fail
for %%F in (%MODS%) do clang --target=i386-unknown-none-elf -march=i486 -O2 -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -nostdinc -isystem INCLUDE -Wall -Wextra -c -o OBJ\CLANG\%%F.o SRC\%%F.c || goto :fail
for %%F in (%MODS2%) do clang --target=i386-unknown-none-elf -march=i486 -O2 -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -nostdinc -isystem INCLUDE -Wall -Wextra -c -o OBJ\CLANG\%%F.o SRC\%%F.c || goto :fail
clang --target=i386-unknown-none-elf -march=i486 -O2 -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -nostdinc -isystem INCLUDE -Wall -Wextra -c -o OBJ\CLANG\libgcc.o SRC\GNU\libgcc.c || goto :fail
if exist LIB\CLANG\libpmc.a del LIB\CLANG\libpmc.a
for %%O in (OBJ\CLANG\*.o) do llvm-ar rcs LIB\CLANG\libpmc.a %%O || goto :fail
for %%D in (SAMPLES TESTS) do for %%P in (%PROGS%) do if exist %%D\%%P.c clang --target=i386-unknown-none-elf -march=i486 -O2 -ffreestanding -fno-pic -nostdinc -isystem INCLUDE -Wall -c -o OBJ\CLANG\%%P.x %%D\%%P.c || goto :fail
for %%P in (%PROGS%) do ld.lld -T LINK\d32.ld -o OBJ\CLANG\%%P.elf OBJ\CLANG\%%P.x -LLIB\CLANG -lpmc || goto :fail
for %%P in (%PROGS%) do llvm-objcopy -O binary OBJ\CLANG\%%P.elf OUT\CLANG\%%P.EXE || goto :fail
for %%P in (%PROGS%) do BIN\D32BIND.EXE OUT\CLANG\%%P.EXE BIN\D32STUB.EXE || goto :fail
goto :afterclang
:noclang
echo clang was not found: Clang skipped.
:afterclang

rem --- DJGPP ---------------------------------------------------------
for %%T in (%WANT%) do if "%%T"=="djgpp" goto :djgpp
goto :afterdjgpp
:djgpp
echo === DJGPP ===
i586-pc-msdosdjgpp-gcc --version > nul 2> nul
if errorlevel 1 goto :nodjgpp
if not exist OBJ\DJGPP\nul md OBJ\DJGPP
if not exist LIB\DJGPP\nul md LIB\DJGPP
if not exist OUT\DJGPP\nul md OUT\DJGPP
for %%F in (%GNUASM%) do i586-pc-msdosdjgpp-gcc -march=i486 -nostdinc -c -o OBJ\DJGPP\%%F.o SRC\GNU\%%F.S || goto :fail
for %%F in (%MODS%) do i586-pc-msdosdjgpp-gcc -march=i486 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -nostdinc -isystem INCLUDE -Wall -Wextra -c -o OBJ\DJGPP\%%F.o SRC\%%F.c || goto :fail
for %%F in (%MODS2%) do i586-pc-msdosdjgpp-gcc -march=i486 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -nostdinc -isystem INCLUDE -Wall -Wextra -c -o OBJ\DJGPP\%%F.o SRC\%%F.c || goto :fail
i586-pc-msdosdjgpp-gcc -march=i486 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -nostdinc -isystem INCLUDE -Wall -Wextra -c -o OBJ\DJGPP\libgcc.o SRC\GNU\libgcc.c || goto :fail
if exist LIB\DJGPP\libpmc.a del LIB\DJGPP\libpmc.a
for %%O in (OBJ\DJGPP\*.o) do i586-pc-msdosdjgpp-ar rcs LIB\DJGPP\libpmc.a %%O || goto :fail
for %%D in (SAMPLES TESTS) do for %%P in (%PROGS%) do if exist %%D\%%P.c i586-pc-msdosdjgpp-gcc -march=i486 -O2 -ffreestanding -nostdinc -isystem INCLUDE -Wall -c -o OBJ\DJGPP\%%P.x %%D\%%P.c || goto :fail
for %%P in (%PROGS%) do i586-pc-msdosdjgpp-gcc -nostdlib -T LINK\d32coff.ld -o OBJ\DJGPP\%%P.cof OBJ\DJGPP\%%P.x -LLIB\DJGPP -lpmc || goto :fail
for %%P in (%PROGS%) do i586-pc-msdosdjgpp-objcopy -O binary OBJ\DJGPP\%%P.cof OUT\DJGPP\%%P.EXE || goto :fail
for %%P in (%PROGS%) do BIN\D32BIND.EXE OUT\DJGPP\%%P.EXE BIN\D32STUB.EXE || goto :fail
goto :afterdjgpp
:nodjgpp
echo i586-pc-msdosdjgpp-gcc was not found: DJGPP skipped.
:afterdjgpp

echo === done ===
goto :end
:fail
echo *** BUILD FAILED ***
endlocal
exit 1
:end
endlocal
