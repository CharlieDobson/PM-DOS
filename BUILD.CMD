@echo off
rem ===================================================================
rem BUILD.CMD - build the PM-DOS kernel and a bootable floppy image
rem
rem Needs Open Watcom 1.9 (wasm + wlink) on the PATH.  MASM 6.1x also
rem assembles the sources (ml /c), swap the assembler lines if wanted.
rem
rem     BUILD             release: boot says "Starting PM-DOS..." only
rem     BUILD debug       chatty boot (banners, mount and load messages)
rem     BUILD ata         debug plus IDE probe tracing: per-slot stage
rem                       letter, IDENTIFY geometry, and an LBA 0
rem                       read-back self-test
rem     BUILD fdd         debug plus BIOS-disk tracing: what INT 13h/08h
rem                       said about the geometry and what status every
rem                       INT 13h/02h came back with.  This is what
rem                       proved the 86Box floppy was an EMPTY DRIVE and
rem                       not a bug in the thunk - keep it to hand
rem
rem Produces, in BIN\:
rem     BOOT.BIN     512-byte FAT12 floppy boot sector
rem     MBRBOOT.BIN  512-byte master boot record
rem     BOOT16.BIN   FAT16 volume boot record  (hard-disk stage 1)
rem     BOOT32.BIN   FAT32 volume boot record  (hard-disk stage 1)
rem     BOOT2.BIN    hard-disk stage 2 - the FAT-walking loader
rem     PMIO.SYS     real-mode stub (4K) + protected-mode body
rem     PMDOS.SYS    the 32-bit DOS kernel
rem     SYS16.COM    16-bit SYS - builds a PM-DOS disk from real DOS
rem     PMDOS.IMG    bootable 1.44M floppy image
rem
rem MKHD.CMD turns the same binaries into bootable FAT16/FAT32 disks.
rem ===================================================================
setlocal
cd /d %~dp0
set DEFS=
if /i "%1"=="debug" set DEFS=-dPMDOS_DEBUG
if /i "%1"=="ata"   set DEFS=-dPMDOS_DEBUG -dATA_DEBUG
if /i "%1"=="fdd"   set DEFS=-dPMDOS_DEBUG -dFDD_DEBUG
rem "i13" traces the disk BIOS a DOS box sees: the function and drive
rem of every INT 13h a guest makes, and the status of every one that
rem comes back an error.  Release builds say nothing at all.
if /i "%1"=="i13"   set DEFS=-dI13_DEBUG
if /i "%1"=="vdm"   set DEFS=-dPMDOS_DEBUG -dVDM_TEST
rem "vdmdbg" keeps COMMAND.COM as the way in and prints what the loader
rem actually left at the guest entry point - the question "did the image
rem get there" is otherwise unanswerable from inside a DOS box.
if /i "%1"=="vdmdbg" set DEFS=-dVDM_DEBUG
rem "v86" adds the monitor's per-fault trace on top of the self-test.
rem It drowns the guest's own output, which is why it is not in "vdm" -
rem but it is the only way to see where a guest dies on real hardware,
rem because a triple fault reboots the machine and takes the screen.
if /i "%1"=="v86"   set DEFS=-dPMDOS_DEBUG -dVDM_TEST -dV86_TRACE
if not exist BIN mkdir BIN

rem wasm 1.9 exits 0 EVEN ON ERRORS, so "|| goto :fail" never catches
rem a failed assembly - but a failed assembly produces no object file.
rem Deleting the old objects first turns a stale-object link (which
rem silently ships yesterday's code) into a loud wlink failure.
for %%D in (BOOT IO DOS CMD TOOLS) do if exist %%D\*.obj del /q %%D\*.obj

echo === boot sectors ===
cd BOOT
wasm -q -i=..\INC boot.asm                      || goto :fail
wlink form raw bin option offset=0x0 name ..\BIN\BOOT.BIN file boot.obj || goto :fail
wasm -q -i=..\INC mbrboot.asm                   || goto :fail
wlink form raw bin option offset=0x0 name ..\BIN\MBRBOOT.BIN file mbrboot.obj || goto :fail
wasm -q -i=..\INC boothd.asm                    || goto :fail
wlink form raw bin option offset=0x0 name ..\BIN\BOOT16.BIN file boothd.obj || goto :fail
wasm -q -dFAT32BOOT -i=..\INC boothd.asm        || goto :fail
wlink form raw bin option offset=0x0 name ..\BIN\BOOT32.BIN file boothd.obj || goto :fail
wasm -q -i=..\INC boot2.asm                     || goto :fail
wlink form raw bin option offset=0x8000 name ..\BIN\BOOT2.BIN file boot2.obj || goto :fail
cd ..
for %%F in (BIN\BOOT.BIN BIN\BOOT16.BIN BIN\BOOT32.BIN BIN\MBRBOOT.BIN) do if not %%~zF==512 (
    echo %%F is %%~zF bytes, not 512 & goto :fail )

echo === PMIO.SYS ===
cd IO
wasm -q %DEFS% -i=..\INC -i=..\BOOT iorm.asm    || goto :fail
wlink form raw bin option offset=0x0     name iorm.bin file iorm.obj  || goto :fail
wasm -q %DEFS% -i=..\INC iopm.asm               || goto :fail
rem 0x200000, not 0x22000: PMIO's protected-mode half lives above 1MB
rem now, and the stub blits it up there.  See the map in PMDOS.INC.
wlink form raw bin option offset=0x200000 name iopm.bin file iopm.obj || goto :fail
cd ..
rem 4096, not 8192: the stub was halved in 0.24 and the four kilobytes
rem went to the DOS box.  The code is 2076 bytes, the GDT sits at 0C00h
rem and the globals at 0F00h, so this is a real ceiling now and not a
rem round number - if it trips, the stub has outgrown its home.
for %%F in (IO\IORM.BIN) do if not %%~zF==4096 (
    echo IORM.BIN is %%~zF bytes, not 4096 - fix the ORG padding & goto :fail )
rem The stub blits exactly IO_PM_MAX bytes up past 1MB.  If the body
rem outgrows that, the tail is silently left behind and the failure
rem happens later, somewhere else, in whatever routine fell off.
for %%F in (IO\IOPM.BIN) do if %%~zF GTR 65536 (
    echo IOPM.BIN is %%~zF bytes, over the IO_PM_MAX of 65536 & goto :fail )
copy /b IO\IORM.BIN+IO\IOPM.BIN BIN\PMIO.SYS >nul

echo === PMDOS.SYS ===
cd DOS
wasm -q %DEFS% -i=..\INC dosmain.asm            || goto :fail
rem 0x110000, not 0x100000: the first 64K above 1MB is the HMA, left
rem empty so the A20 wrap can be emulated by aliasing it - see
rem PAGING.INC.  Must match DOS_BASE in PMDOS.INC.
wlink form raw bin option offset=0x110000 name ..\BIN\PMDOS.SYS file dosmain.obj || goto :fail
cd ..
rem PMDOS.SYS runs from DOS_BASE to SHELL_PSP and the shell's slot is
rem immediately above it.  Nothing complains if it overruns; the shell
rem simply gets loaded over the end of the kernel.
for %%F in (BIN\PMDOS.SYS) do if %%~zF GTR 196608 (
    echo PMDOS.SYS is %%~zF bytes, past the 192K between DOS_BASE and SHELL_PSP & goto :fail )

echo === COMMAND.COM ===
cd CMD
wasm -q -i=..\INC command.asm                   || goto :fail
wlink form raw bin option offset=0x141000 name ..\BIN\COMMAND.COM file command.obj || goto :fail
cd ..

echo === external commands ===
cd CMD
wasm -q -i=..\INC choice.asm                    || goto :fail
wlink form raw bin option offset=0x181000 name ..\BIN\CHOICE.COM file choice.obj || goto :fail
wasm -q -i=..\INC lfnapi.asm                    || goto :fail
wlink form raw bin option offset=0x181000 name ..\BIN\LFNAPI.COM file lfnapi.obj || goto :fail
wasm -q -i=..\INC more.asm                      || goto :fail
wlink form raw bin option offset=0x181000 name ..\BIN\MORE.COM file more.obj || goto :fail
wasm -q -i=..\INC sort.asm                      || goto :fail
wlink form raw bin option offset=0x181000 name ..\BIN\SORT.COM file sort.obj || goto :fail
wasm -q -i=..\INC sys.asm                       || goto :fail
wlink form raw bin option offset=0x181000 name ..\BIN\SYS.COM file sys.obj || goto :fail
wasm -q -i=..\INC format.asm                    || goto :fail
wlink form raw bin option offset=0x181000 name ..\BIN\FORMAT.COM file format.obj || goto :fail
wasm -q -i=..\INC mem.asm                       || goto :fail
wlink form raw bin option offset=0x181000 name ..\BIN\MEM.EXE file mem.obj || goto :fail
wasm -q -i=..\INC drives.asm                     || goto :fail
wlink form raw bin option offset=0x181000 name ..\BIN\DRIVES.EXE file drives.obj || goto :fail
cd ..

echo === HELLO.COM (a real 16-bit .COM, for the VDM) ===
rem An ordinary real-mode DOS program.  It is not part of the kernel;
rem it is the thing the kernel has to be able to run.
cd TOOLS
wasm -q -i=..\INC hello.asm                     || goto :fail
wlink form raw bin name ..\BIN\HELLO.COM file hello.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === HELLOX.EXE (a real 16-bit .EXE, for the VDM) ===
rem Three segments and a far call, so the loader has to apply the fixup
rem table for it to run at all.  "system dos" is what makes wlink emit
rem an MZ header instead of a flat image.
cd TOOLS
wasm -q -i=..\INC hellox.asm                    || goto :fail
wlink system dos name ..\BIN\HELLOX.EXE file hellox.obj || goto :fail
cd ..

echo === ARGS.COM (what COMMAND.COM hands a 16-bit program) ===
rem HELLO proves a DOS box runs; ARGS proves it was started properly -
rem tail, FCBs, entry AX and environment, all of which only exist once
rem a shell is the thing doing the starting.
cd TOOLS
wasm -q -i=..\INC args.asm                      || goto :fail
wlink form raw bin name ..\BIN\ARGS.COM file args.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === XMSTEST.COM (extended memory, from inside the DOS box) ===
cd TOOLS
wasm -q -i=..\INC xmstest.asm                   || goto :fail
wlink form raw bin name ..\BIN\XMSTEST.COM file xmstest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === A20TEST.COM (the same gate, reached through the hardware) ===
rem XMSTEST asks the driver.  This one writes port 92h and the 8042
rem directly, which is what the TSS I/O bitmap is there to catch.
cd TOOLS
wasm -q -i=..\INC a20test.asm                   || goto :fail
wlink form raw bin name ..\BIN\A20TEST.COM file a20test.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === EXECTST.COM and TSRTEST.COM (a guest that starts a guest, ===
echo ===   and one that outlives the box that loaded it)           ===
cd TOOLS
wasm -q -i=..\INC exectst.asm                   || goto :fail
wlink form raw bin name ..\BIN\EXECTST.COM file exectst.obj order clname CODE offset=0x100 || goto :fail
wasm -q -i=..\INC tsrtest.asm                   || goto :fail
wlink form raw bin name ..\BIN\TSRTEST.COM file tsrtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === I13TEST.COM (the disk BIOS, and whether it agrees with the ===
echo ===   filesystem sitting on top of it)                         ===
cd TOOLS
wasm -q -i=..\INC i13test.asm                   || goto :fail
wlink form raw bin name ..\BIN\I13TEST.COM file i13test.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === CURTEST.COM (one cursor between the BIOS and the console, ===
echo ===   or two that disagree)                                   ===
cd TOOLS
wasm -q -i=..\INC curtest.asm                   || goto :fail
wlink form raw bin name ..\BIN\CURTEST.COM file curtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === DPMITEST.COM (a 16-bit DPMI client, for the host) ===
cd TOOLS
wasm -q -i=..\INC dpmitest.asm                  || goto :fail
wlink form raw bin name ..\BIN\DPMITEST.COM file dpmitest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === DEVTEST.COM (devices, InDOS, extended error, file stamps) ===
cd TOOLS
wasm -q -i=..\INC devtest.asm                   || goto :fail
wlink form raw bin name ..\BIN\DEVTEST.COM file devtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === APITEST.COM (IOCTL, 5Ah/5Bh, TRUENAME, commit, verify, version) ===
cd TOOLS
wasm -q -i=..\INC apitest.asm                    || goto :fail
wlink form raw bin name ..\BIN\APITEST.COM file apitest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === LOLTEST.COM (the 52h list of lists, and the flags a guest reads) ===
rem Both of the things it checks are bridge bugs, invisible from the
rem 32-bit side: MEM walks the arena correctly because MEM is a D32
rem program and never makes the call a 16-bit tool makes.
cd TOOLS
wasm -q -i=..\INC loltest.asm                    || goto :fail
wlink form raw bin name ..\BIN\LOLTEST.COM file loltest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === IDLETEST.COM (INT 28h reaching a real-mode handler) ===
cd TOOLS
wasm -q -i=..\INC idletest.asm                   || goto :fail
wlink form raw bin name ..\BIN\IDLETEST.COM file idletest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === CRASH.EXE (a 32-bit program that faults, and a shell that lives) ===
rem A D32 program, so it runs at ring 0 in the kernel's own segment -
rem which is exactly why its fault used to end the machine.
cd TOOLS
wasm -q -i=..\INC crash.asm                     || goto :fail
wlink form raw bin option offset=0x181000 name ..\BIN\CRASH.EXE file crash.obj || goto :fail
cd ..

echo === SYS16.COM ===
rem The 16-bit SYS, which runs on ordinary DOS and so cannot read the
rem boot code off a PM-DOS disk the way SYS.COM does - it carries all
rem of it instead, which is what BIN2INC is for.  Built here, AFTER
rem the boot sectors, because it is made out of them: a SYS16.COM
rem from an older build would write an older boot sector.
rem
rem BIN2INC is compiled every time rather than when its source looks
rem newer.  It takes a second, and it cannot then go stale.
cd TOOLS
wcl386 -q -bt=nt -fe=bin2inc.exe bin2inc.c      || goto :fail
.\bin2inc.exe BOOTIMG.INC img_fat12=..\BIN\BOOT.BIN img_fat16=..\BIN\BOOT16.BIN ^
    img_fat32=..\BIN\BOOT32.BIN img_s2=..\BIN\BOOT2.BIN || goto :fail
wasm -q -i=..\INC sys16.asm                     || goto :fail
rem ORDER, not "option offset=0x100" - wlink rounds that up to 4K and
rem silently builds the .COM for the wrong address.  See SYS16.ASM.
wlink form raw bin name ..\BIN\SYS16.COM file sys16.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === MKDISK ===
rem Rebuild when the source is newer than the tool: "dir /o:d" lists
rem oldest first, so whatever the loop lands on last is the newest.
rem Without this an edit to MKDISK.C is silently ignored and the images
rem keep whatever the old EXE wrote into them.
set MKNEW=
for /f %%I in ('dir /b /o:d TOOLS\MKDISK.C TOOLS\MKDISK.EXE 2^>nul') do set MKNEW=%%I
if /i not "%MKNEW%"=="MKDISK.EXE" (
    cd TOOLS
    wcl386 -q -bt=nt -fe=mkdisk.exe mkdisk.c    || goto :fail
    cd ..
)

echo === PMDOS.IMG ===
TOOLS\MKDISK.EXE BIN\PMDOS.IMG BIN\BOOT.BIN BIN\PMIO.SYS BIN\PMDOS.SYS BIN\COMMAND.COM BIN\CHOICE.COM BIN\LFNAPI.COM BIN\MORE.COM BIN\SORT.COM BIN\SYS.COM BIN\FORMAT.COM BIN\MEM.EXE BIN\DRIVES.EXE BIN\HELLO.COM BIN\HELLOX.EXE BIN\ARGS.COM BIN\XMSTEST.COM BIN\A20TEST.COM BIN\EXECTST.COM BIN\TSRTEST.COM BIN\I13TEST.COM BIN\CURTEST.COM BIN\DPMITEST.COM BIN\DEVTEST.COM BIN\APITEST.COM BIN\IDLETEST.COM BIN\LOLTEST.COM BIN\CRASH.EXE ^
    README.TXT DISK\CONFIG.SYS DISK\AUTOEXEC.BAT DISK\BATTEST.BAT DISK\SUB.BAT DISK\CHTEST.BAT ^
    DISK\GOTOTEST.BAT DISK\LOOPTEST.BAT DISK\REDTEST.BAT DISK\LFNTEST.BAT ^
    DISK\CMDTEST.CMD DISK\SUBQ.CMD DISK\BATQ.BAT DISK\CTRLC.BAT ^
    DISK\CHAIN1.BAT DISK\CHAIN2.BAT DISK\NOLABEL.BAT DISK\SHIFT12.BAT DISK\VDMTEST.BAT DISK\NESTTEST.BAT DISK\DISKTEST.BAT DISK\NULTEST.BAT DISK\CRASHTST.BAT DISK\APITEST.BAT DISK\LOLTEST.BAT || goto :fail

echo.
echo Build OK.
dir BIN | find "."
exit /b 0

:fail
echo.
echo *** BUILD FAILED ***
exit /b 1
