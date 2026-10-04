@echo off
rem ===================================================================
rem BUILD.CMD - build the PM-DOS kernel and a bootable floppy image
rem
rem Needs Open Watcom 1.9 (wasm + wlink) on the PATH.  MASM 6.1x also
rem assembles the sources (ml /c), swap the assembler lines if wanted.
rem
rem     BUILD             release: boot says "Starting PM-DOS...", and
rem                       a line each for a mouse and a CD-ROM drive
rem                       on a machine that has one
rem     BUILD debug       chatty boot (banners, mount and load messages)
rem     BUILD ata         debug plus IDE probe tracing: per-slot stage
rem                       letter, IDENTIFY geometry, word 48 and the
rem                       flags made of it, and an LBA 0 read-back
rem                       self-test
rem     BUILD ataq        the same trace on a release boot, so that it
rem                       is still on the screen at the prompt
rem     BUILD nohost     release, but no IDE host adapter is recognised:
rem                       for testing the transfer-mode rules for a
rem                       chipset PM-DOS does not know
rem     BUILD fdd         debug plus BIOS-disk tracing: what INT 13h/08h
rem                       said about the geometry and what status every
rem                       INT 13h/02h came back with.  This is what
rem                       proved the 86Box floppy was an EMPTY DRIVE and
rem                       not a bug in the thunk - keep it to hand
rem     BUILD scsi6       release, with every SCSI drive taken for one
rem                       that has only the 6-byte MODE SENSE and MODE
rem                       SELECT, to test the translation
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
rem
rem and, in IMAGES\:
rem     PMDOS.IMG    bootable 1.44M floppy image
rem
rem BIN\ holds what a PM-DOS disk is MADE OF; IMAGES\ holds the finished
rem media, which is why the disk images are not in it.  MKHD.CMD turns
rem the same binaries into bootable FAT16/FAT32 disks in IMAGES\ too.
rem ===================================================================
rem Nothing below uses a cmd.exe command extension, so this builds on
rem Windows NT 3.51 as well as on modern Windows.  NT 3.51's cmd.exe has
rem no %~dp0, no IF /I, no FOR /F, no "~z" file-size modifier on a FOR
rem variable, and no EXIT /B - and it fails SILENTLY at most of them,
rem expanding the ones it does not know to literal text rather than
rem saying anything, so the three that used to be here are replaced
rem below instead of merely avoided.  (Do not write a live "~z" into a
rem comment either: modern cmd.exe substitutes batch parameters even on
rem a rem line, and rejects that modifier on a name it cannot resolve.)
setlocal
rem Win32 folds "<file>\.." lexically, so "%0\.." is this script's own
rem directory; pushd - unlike cd - changes the drive as well.
pushd "%0\.."
if not exist BUILD.CMD goto :nohome
set DEFS=
rem The variant names below are compared exactly, because NT 3.51 has no
rem IF /I.  Fold whatever was typed to the documented lower-case spelling
rem first: bracketing both sides anchors the comparison at each end, so
rem "lpt" cannot match "lptfrz".  A plain "BUILD" pays nothing for this.
set TGT=%1
if "%TGT%"=="" goto :gottgt
for %%T in (debug ata ataq nohost iso fdd img move i13 vfy rmbisect rmmark rmexec rmlow rmhigh cap16 ^
  lptcap lpthigh lptpad rmlowm vdm vdmdbg v86 trace watch watchq nomode ^
  tracenm watchnm lpt stamp freeze stampfrz lptfrz lptfz lptq lpt0 lptnm ^
  lptlow kbionm calls kbio rme9 irqtest fpuirq eager32 trust32 hang scsi6) do echo [%TGT%]| find /i "[%%T]" > nul && set TGT=%%T
:gottgt
if "%TGT%"=="debug" set DEFS=-dPMDOS_DEBUG
if "%TGT%"=="ata"   set DEFS=-dPMDOS_DEBUG -dATA_DEBUG
rem "ataq" is the probe trace and nothing else, for "iso"'s reason: a
rem debug kernel reads CONFIG.SYS out loud, a line at a time, and that
rem scrolls the one line that was wanted off the top of the screen.
if "%TGT%"=="ataq"  set DEFS=-dATA_DEBUG
rem "nohost" has IDEHOST.INC recognise no IDE controller at all, so an
rem Intel or a VIA board is treated as one whose timing PM-DOS cannot
rem write.  The BIOSes that leave a drive in a mode past Ultra DMA 2 are
rem on exactly those boards, and what the kernel does with such a drive
rem on a host it does not know cannot be tested anywhere else.
if "%TGT%"=="nohost" set DEFS=-dIDEH_BLIND
rem "irqtest" puts two handlers of the kernel's own on the real-time
rem clock's interrupt line, one that never claims anything and one that
rem claims the clock's, so that TESTS\IRQTEST.EXE has a shared line to
rem count on.  Not a kernel for anything else: with it in, no DOS box
rem is ever handed that interrupt, and VPICTEST says so.
if "%TGT%"=="irqtest" set DEFS=-dIRQ_SELFTEST
rem "fpuirq" takes the coprocessor's error from interrupt line 13 on
rem every machine, which is how a 386 has to take it and how nothing
rem later does: a 486 and everything after it is told to raise
rem exception 16.  The line's half of IO\FPU.INC is otherwise run only
rem on a 386 with a 387 in it, and no emulator here will boot as one
rem unattended.  TOOLS\FPU86.PS1 is the rig for both kernels.
if "%TGT%"=="fpuirq" set DEFS=-dFPU_BYIRQ
rem "eager32" and "trust32" are a drive that says it does doubleword
rem I/O, on every machine: IDE32=AUTO takes each disk as a candidate for
rem 32-bit PIO, as it does one whose IDENTIFY word 48 is set.  "eager32"
rem then proves it the way a release kernel does (ata_prove32), so on a
rem host that halves a doubleword read - DOSBox-X's is one - C: has to
rem be there all the same.  "trust32" believes the drive without asking,
rem which is what every kernel up to v0.60 did: on that host there is no
rem C: until CONFIG.SYS says IDE32=OFF, and that is the test for
rem disk_rescan.
if "%TGT%"=="eager32" set DEFS=-dIDE32_EAGER
if "%TGT%"=="trust32" set DEFS=-dIDE32_EAGER -dIDE32_TRUST
rem "iso" mounts the first CD in the machine at boot and reads its root
rem directory out loud - the ISO9660 reader's own proof, the way "ata"
rem is the packet transport's.  No PMDOS_DEBUG: the CONFIG.SYS listing
rem scrolls the answer off the screen before it can be photographed.
if "%TGT%"=="iso"   set DEFS=-dISO_DEBUG
if "%TGT%"=="fdd"   set DEFS=-dPMDOS_DEBUG -dFDD_DEBUG
rem "img" traces MOUNT's side of the kernel: the path AS THE KERNEL
rem RECEIVED IT, what resolve_parent made of it, the eleven-byte
rem pattern it searched for, and what the search found.  It prints to
rem the CONSOLE and not to the redirected output, which is the right
rem way round: the trace is for whoever is watching and the test's
rem answer is for FATGET to read out of the image afterwards.
if "%TGT%"=="img"   set DEFS=-dIMG_DEBUG
rem "move" is the same idea one level up, in the PROGRAM rather than
rem the kernel: MOVE's cross-volume copy prints the count 3Fh gave it,
rem the address it asked to be filled, and the bytes that are actually
rem there afterwards.  That is what showed the copy was moving zeroes -
rem and then, once the buffer proved fine, that the copy path was not
rem running at all because 56h had wrongly said the two volumes were
rem one.  It goes to move.asm alone, not to %DEFS%, because the kernel
rem knows nothing about it.
set MVDEFS=
if "%TGT%"=="move"  set MVDEFS=-dMV_DEBUG
rem "i13" traces the disk BIOS a DOS box sees: the function and drive
rem of every INT 13h a guest makes, and the status of every one that
rem comes back an error.  Release builds say nothing at all.
if "%TGT%"=="i13"   set DEFS=-dI13_DEBUG
rem "vfy" prints the sector and the two bytes every time VERIFY's
rem read-back disagrees.  A verify that fires when it should not is
rem indistinguishable from a disk that is genuinely bad until something
rem says WHICH sector and WHAT it saw.  Silent unless one disagrees, so
rem it costs nothing to leave a machine running with it.
if "%TGT%"=="vfy"   set DEFS=-dVFY_DEBUG
rem "rmbisect" cuts RMDOS in half at its own front door: it checks the
rem handoff signature and goes straight back, running none of the DOS.
rem That separates the two questions a failed handover asks - did the
rem MODE SWITCH work, and does the DOS work once it is there - and it
rem is the only instrument that answers the first one, because a
rem machine that never arrives cannot report anything.
if "%TGT%"=="rmbisect" set DEFS=-dRMDOS_BISECT -dRMDOS_MARK
rem "rmmark" is the same markers with the WHOLE of RMDOS behind them:
rem one character per milestone, onto the text page AND the graphics
rem framebuffer, so it can be counted whichever mode the program that
rem asked for the handover has left the screen in.  It is the only
rem instrument that works on the far side of the switch, where nothing
rem of PM-DOS is running and there is no console to print to.
if "%TGT%"=="rmmark" set DEFS=-dRMDOS_MARK
rem "rmlow" forces RMDOS out of the HMA and back into the DOS box's
rem arena, which is where v0.49 put it.  The low path is the fallback
rem for an image too big for 64K, so nothing exercises it by accident
rem any more - and a path nothing exercises is a path nothing tests.
rem One build a release, run against PARKWIN, keeps it honest.
rem "rmexec" traces RMDOS's EXEC through the BIOS teletype - the name
rem asked for, the header, which branch, where it allocated, and the
rem CS:IP:SS:SP it is about to jump to with the four bytes that are
rem there.  It needs a TEXT MODE screen, which is exactly what
rem PARKEXEC.COM parks from and what Windows does not leave.
if "%TGT%"=="rmexec" set DEFS=-dRMDOS_EDBG
rem "rme9" is RMDOS writing a line per INT 21h to the Bochs debug port,
rem E9h: the registers, the caller, the name for a call that takes one,
rem and the answer on the same line - so a call that never returns is
rem the line with nothing after it.  DOSBox-X logs it with "bochs debug
rem port e9=true" and a [log] logfile; real hardware ignores the port.
rem It is the trace that does not stop what it watches - see RME9.INC
rem for why the disk trace does.
if "%TGT%"=="rme9" set DEFS=-dRMDOS_E9
rem From v0.53 the park goes LOW FIRST and the HMA is the fallback:
rem the Windows kernel finds the SFT by scanning memory for "CON "
rem and the scan does not reach the megabyte.  "rmlow" now means
rem low OR NOTHING - no quiet fall back - and "rmhigh" is what the
rem default was up to v0.52, kept so the two are one build apart.
if "%TGT%"=="rmlow" set DEFS=-dRMDOS_LOW
if "%TGT%"=="rmhigh" set DEFS=-dRMDOS_HIGH
if "%TGT%"=="cap16" set DEFS=-dXMSPOOLCAP
if "%TGT%"=="lptcap" set DEFS=-dDBG_LPT -dXMSPOOLCAP
if "%TGT%"=="lpthigh" set DEFS=-dDBG_LPT -dRMDOS_HIGH
if "%TGT%"=="lptpad" set DEFS=-dDBG_LPT -dRMDOS_PAD
rem ...and the same with the markers on, which is the pairing that
rem matters: the low run is the CONTROL for the high one, and a
rem control read with a different instrument is not a control.
if "%TGT%"=="rmlowm" set DEFS=-dRMDOS_LOW -dRMDOS_MARK
if "%TGT%"=="vdm"   set DEFS=-dPMDOS_DEBUG -dVDM_TEST
rem "vdmdbg" keeps CMD.EXE as the way in and prints what the loader
rem actually left at the guest entry point - the question "did the image
rem get there" is otherwise unanswerable from inside a DOS box.
if "%TGT%"=="vdmdbg" set DEFS=-dVDM_DEBUG
rem "v86" adds the monitor's per-fault trace on top of the self-test.
rem It drowns the guest's own output, which is why it is not in "vdm" -
rem but it is the only way to see where a guest dies on real hardware,
rem because a triple fault reboots the machine and takes the screen.
if "%TGT%"=="v86"   set DEFS=-dPMDOS_DEBUG -dVDM_TEST -dV86_TRACE
rem "trace" is the monitor's per-fault trace WITHOUT the self-test, so
rem the machine boots to a prompt as usual and only says anything once
rem a DOS box is running.  Every INT a guest makes is a fault, so this
rem is a system-call trace - and when a guest is spinning rather than
rem stopping, the few lines that repeat for ever ARE the answer.  It is
rem what found the MEM.EXE lockup.
if "%TGT%"=="trace" set DEFS=-dV86_TRACE
rem "watch" is the other half of that, and the half a HANG needs: the
rem timer tick asks, twice a second, whether the guest has made any
rem call at all lately, and prints where it is if it has not.  A guest
rem that spins causes no faults, so the fault trace goes quiet at
rem exactly the moment there is something to see.
if "%TGT%"=="watch" set DEFS=-dV86_WATCH
rem "hang" is the same question asked of the WHOLE machine, and two more
rem with it, all on port E9h, which DOSBox-X puts in its log
rem (TOOLS\DBXRUN.PS1 -E9):
rem   - once five seconds pass with no INT 21h from anybody and no trap
rem     from any box, where the processor is - kernel, native program or
rem     box - and what the last INT 21h was, once a second, eight times;
rem   - every exception but a #GP and a #NM, as it arrives, with TSS ESP0,
rem     CR2 for a page fault, and the stack it landed on from ring 0;
rem   - the IDT register, the IDT and the exception stubs, summed every
rem     tick, and any change said once with who was running.
rem Nothing goes to the screen or the disk, so it moves the timing as
rem little as anything here can.  It is what found the 64 bytes every
rem failed EXEC left on the kernel stack.  A machine that says nothing
rem at all stopped with interrupts off.
if "%TGT%"=="hang" set DEFS=-dHANG_E9
rem "scsi6" takes every SCSI drive for one that has only the 6-byte MODE
rem SENSE and MODE SELECT, so that the translation SCSI.INC does for such
rem a drive runs on a machine whose drives would never need it.  86Box
rem emulates both forms on every model it has.
if "%TGT%"=="scsi6" set DEFS=-dSCSI_MODE6
rem "watchq" is the same sampler with the console taken out of it: it
rem pokes the guest's CS:IP into the top right of the text page instead
rem of printing, which is nine instructions in the timer interrupt
rem rather than a string write.  THE LOUD ONE CHANGES THE ANSWER - a
rem hang that depends on how many instructions the guest gets between
rem ticks moves or disappears when the tick starts taking milliseconds,
rem and WIN.COM is exactly such a hang.  Use this one to locate a spin
rem and "watch" only when the history matters more than the timing.
if "%TGT%"=="watchq" set DEFS=-dV86_WATCHQ
rem "nomode" is the call trace with the guest's INT 10h AH=00h taken
rem away from it, so the screen stays in text mode and the trace stays
rem on it.  Windows sets a graphics mode for its logo and hangs after
rem it, and every tool in this monitor writes to the text page - which
rem is still correct, still there, and not being displayed.  The guest
rem believes the mode was set; only the display disagrees.
if "%TGT%"=="nomode" set DEFS=-dV86_NOMODE -dV86_CALLS
rem "tracenm" is the same, with the FULL fault trace: every trap the
rem guest takes, including the video and keyboard calls "calls" hides.
rem A guest that is looping through the monitor rather than round a
rem loop of its own shows up here and nowhere else.
if "%TGT%"=="tracenm" set DEFS=-dV86_NOMODE -dV86_TRACE
rem "watchnm" is the quiet sampler with the same screen held: the
rem guest's LAST fault address, in the top right corner, updated
rem eighteen times a second.  An address that stops changing is where
rem the guest stopped.
if "%TGT%"=="watchnm" set DEFS=-dV86_NOMODE -dV86_WATCHQ
rem "kbionm" is the 8042 transcript with the screen held in text mode.
rem "lpt" is the trace that does not touch the screen at all.  Every
rem probe before it changed what it measured: printing through the
rem console from an interrupt slowed the guest enough to move the
rem hang, writing to the text page needs a text page the guest has
rem taken away, and holding the screen in text mode leaves the guest
rem believing in a video state the hardware is not in.
rem
rem This one records each guest trap into a ring buffer - no I/O, a
rem dozen instructions - and prints the ring to the PRINTER PORT only
rem once the guest has been silent for five seconds, when there is
rem nothing left to perturb.  On 86Box set lpt1_enabled = 1 and
rem lpt1_device = text_prt and the bytes land in VMs\<name>\printer
rem as a text file; on a machine with no printer they go nowhere.
if "%TGT%"=="lpt" set DEFS=-dDBG_LPT -dXMSPOOLSTAMP
if "%TGT%"=="stamp" set DEFS=-dXMSPOOLSTAMP
if "%TGT%"=="freeze" set DEFS=-dRMDOS_FREEZE
if "%TGT%"=="stampfrz" set DEFS=-dXMSPOOLSTAMP -dRMDOS_FREEZE
if "%TGT%"=="lptfrz" set DEFS=-dDBG_LPT -dXMSPOOLSTAMP -dRMDOS_FREEZE
if "%TGT%"=="lptfz" set DEFS=-dDBG_LPT -dRMDOS_FREEZE
rem "lptq" is the same trace that never writes during the run - see
rem rdbg_maybe in RMDBG.INC.  It answers one question and its answer is
rem a screenshot: does Windows get further when the instrument stops
rem touching the disk?
if "%TGT%"=="lptq" set DEFS=-dDBG_LPT -dRMDOS_FREEZE -dDBG_QUIET
if "%TGT%"=="lpt0" set DEFS=-dDBG_LPT -dDBG_NOREC
if "%TGT%"=="lptnm" set DEFS=-dDBG_LPT -dV86_NOMODE
rem ...and the trace with RMDOS parked LOW instead of in the HMA.
rem The Windows KERNEL finds the SFT by SCANNING memory for the name
rem "CON " - so where the SFT sits is not a detail, and this pair is
rem the control: same instrument, same build, one variable moved.
if "%TGT%"=="lptlow" set DEFS=-dDBG_LPT -dRMDOS_LOW
if "%TGT%"=="kbionm" set DEFS=-dV86_NOMODE -dV86_KBIO
rem "calls" is the fault trace with the noise taken out: software
rem interrupts only, and not the ones a program uses to PRINT.  The
rem full trace is one line per character a guest writes, so the handful
rem of calls where it asks the machine a question - how much memory,
rem where is the list of lists, what is the country - scroll off the
rem top before they can be read, and those are the ones a compatibility
rem bug is made of.  Run the guest with its own output redirected and
rem what is left on the screen is the whole conversation.
if "%TGT%"=="calls" set DEFS=-dV86_CALLS
rem "kbio" traces the guest's side of the 8042, which since v0.36 is a
rem device the monitor stands in for rather than one it merely watches
rem three ports of.  The keyboard controller is a CONVERSATION - a
rem command, then a byte, then a status bit polled until it changes -
rem so a single register dump says nothing and only the transcript
rem does.  Writes are printed as they come; reads are printed only
rem when the answer CHANGES, because a guest waiting on a status bit
rem reads the same port a thousand times a second and printing that
rem is printing the loop rather than the bug.
if "%TGT%"=="kbio" set DEFS=-dV86_KBIO
if not exist BIN mkdir BIN
rem BIN\ is what a PM-DOS disk is MADE OF - the boot sectors, the
rem kernel, the commands - and BIN\TESTS\ is everything written to prove
rem one thing about it.  The split is by what SHIPS, so a new test
rem program lands in TESTS by default and a new COMMAND has to be
rem named; having to name it is the point.  On the disk images they
rem land the same way, through MKDISK.EXE's -dir switch.
if not exist BIN\TESTS mkdir BIN\TESTS
rem IMAGES\ is the third of those: not an ingredient and not a test
rem program, but the finished disk an emulator mounts.  MKDISK.EXE
rem writes the file it is told to and does not create the directory
rem first, so this has to exist before it runs.
if not exist IMAGES mkdir IMAGES

rem wasm 1.9 exits 0 EVEN ON ERRORS, so "|| goto :fail" never catches
rem a failed assembly - but a failed assembly produces no object file.
rem Deleting the old objects first turns a stale-object link (which
rem silently ships yesterday's code) into a loud wlink failure.
for %%D in (BOOT IO DOS CMD TOOLS) do if exist %%D\*.obj del /q %%D\*.obj
rem Each external command has a directory of its own under CMD\, and
rem each one has to be named here too: a directory missing from this
rem list is one whose failed assembly links yesterday's object.  (A
rem failed COMPILE does not need it - wcc386, unlike wasm, returns an
rem error - but CHKDSK's and LABEL's startup is assembled like the rest.)
for %%D in (ATTRIB CACHE CDEX CHKDSK CHOICE DISKCOMP DISKCOPY DRIVES EDIT FC FDISK FIND FORMAT ISAPNP LABEL LFNAPI MEM MORE MOUNT MOVE SHUTDOWN SORT SYS UNDELETE XCOPY) do if exist CMD\%%D\*.obj del /q CMD\%%D\*.obj

rem FSIZE asserts the sizes the checks below used to test with FOR's
rem "~z" size modifier.  That modifier is a cmd.exe command EXTENSION:
rem NT 3.51 expands it to literal text, so every one of those checks
rem silently compared two strings and failed the build.  TOOLS\FSIZE.C
rem says the rest.
cd TOOLS
wcl386 -q -bt=nt -fe=fsize.exe fsize.c          || goto :fail
cd ..

rem === WHICH VERSION IS THIS? ===
rem Printed before anything is assembled, and it is here because the
rem equate went stale for two whole releases: v0.58 and v0.59 were
rem built, tested and committed while INC\PMDOS.INC still said 57, so
rem VER told the user the wrong kernel and nothing anywhere disagreed.
rem A stale version number breaks nothing, which is exactly why no test
rem can catch it - the only guard that works is having the number in
rem front of the person doing the build.  FIND is used rather than FOR
rem /F because NT 3.51's cmd.exe has neither FOR /F nor a way to say it
rem does not: it expands the tokens to literal text and carries on.
echo === version ===
find "PMDOS_MINOR     EQU" INC\PMVER.INC

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
for %%F in (BIN\BOOT.BIN BIN\BOOT16.BIN BIN\BOOT32.BIN BIN\MBRBOOT.BIN) do TOOLS\FSIZE.EXE -eq 512 %%F || goto :fail

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
TOOLS\FSIZE.EXE -eq 4096 IO\IORM.BIN "fix the ORG padding" || goto :fail
rem The stub blits exactly IO_PM_MAX bytes up past 1MB.  If the body
rem outgrows that, the tail is silently left behind and the failure
rem happens later, somewhere else, in whatever routine fell off.
TOOLS\FSIZE.EXE -max 65536 IO\IOPM.BIN "the IO_PM_MAX the stub blits" || goto :fail
copy /b IO\IORM.BIN+IO\IOPM.BIN BIN\PMIO.SYS >nul

echo === PMDOS.SYS ===
cd DOS
wasm -q %DEFS% -i=..\INC dosmain.asm            || goto :fail
rem 0x110000, not 0x100000: the first 64K above 1MB is the HMA, left
rem empty so the A20 wrap can be emulated by aliasing it - see
rem PAGING.INC.  Must match DOS_BASE in PMDOS.INC.
rem
rem 0x1C9000 is KBSS_BASE in PMDOS.INC, and has to be: the kernel's big
rem buffers are a segment of their own that is put there, below the
rem arena, and not written to the file.  The kernel compares the two
rem when it starts and stops if they differ.  "order" is what places a
rem class, and with it "option offset" is ignored - hence the first
rem offset here.  NOT "noemit" on the second: without it, something in
rem _KBSS that has a value makes this file 1.4MB and the check below
rem stops the build; with it, the value is dropped and nothing says so.
wlink form raw bin name ..\BIN\PMDOS.SYS file dosmain.obj order clname CODE offset=0x110000 clname KBSS offset=0x1C9000 || goto :fail
cd ..
rem PMDOS.SYS runs from DOS_BASE to SHELL_PSP and the shell's slot is
rem immediately above it.  Nothing complains if it overruns; the shell
rem simply gets loaded over the end of the kernel.
TOOLS\FSIZE.EXE -max 196608 BIN\PMDOS.SYS "the 192K between DOS_BASE and SHELL_PSP" || goto :fail

echo === CMD.EXE ===
cd CMD
wasm -q -i=..\INC cmd.asm                       || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\CMD.EXE file cmd.obj || goto :fail
cd ..

echo === COMMAND.COM (16-bit, what COMSPEC names) ===
rem The interpreter DOS programs get: a real-mode .COM, because a DOS
rem program that shells out runs COMSPEC in its DOS box, and under
rem Windows standard mode the kernel is parked and nothing 32-bit runs
rem at all.  It includes PMVER.INC and nothing else from INC - it is
rem not a PM-DOS program and must not look like one to wasm.
cd CMD\COMMAND
wasm -q -i=..\..\INC command.asm               || goto :fail
wlink form raw bin name ..\..\BIN\COMMAND.COM file command.obj order clname CODE offset=0x100 || goto :fail
cd ..\..

echo === external commands ===
rem One directory per command, CMD\<name>\, so each is two levels down
rem where CMD.EXE is one: hence -i=..\..\INC, ..\..\BIN and the
rem cd ..\.. that closes every block.
cd CMD\CHOICE
wasm -q -i=..\..\INC choice.asm                 || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\CHOICE.COM file choice.obj || goto :fail
cd ..\..
cd CMD\LFNAPI
wasm -q -i=..\..\INC lfnapi.asm                 || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\TESTS\LFNAPI.COM file lfnapi.obj || goto :fail
cd ..\..
cd CMD\MORE
wasm -q -i=..\..\INC more.asm                   || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\MORE.COM file more.obj || goto :fail
cd ..\..
cd CMD\SORT
wasm -q -i=..\..\INC sort.asm                   || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\SORT.COM file sort.obj || goto :fail
cd ..\..
cd CMD\SYS
wasm -q -i=..\..\INC sys.asm                    || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\SYS.COM file sys.obj || goto :fail
cd ..\..
cd CMD\FORMAT
wasm -q -i=..\..\INC format.asm                 || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\FORMAT.COM file format.obj || goto :fail
cd ..\..
cd CMD\MEM
rem %DEFS%, which no other command is given: MEM takes the start of the
rem arena from PMDOS.INC, and a kernel built to trace starts it higher.
wasm -q %DEFS% -i=..\..\INC mem.asm             || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\MEM.EXE file mem.obj || goto :fail
cd ..\..
cd CMD\DRIVES
wasm -q -i=..\..\INC drives.asm                 || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\TESTS\DRIVES.EXE file drives.obj || goto :fail
cd ..\..
rem FDISK CARRIES A MASTER BOOT RECORD INSIDE IT, because /MBR exists
rem to repair the one on the disk and cannot very well read it from
rem there.  wasm has no INCBIN, so the binary is turned into DB
rem statements first - the same trick SYS16.COM uses further down, and
rem the same tool.  BIN2INC is compiled here as well as there: it
rem takes a second, it cannot then be stale, and FDISK is built long
rem before SYS16 is.  MBRCODE.INC is written into CMD\FDISK\, beside
rem the one source that includes it.
cd CMD\FDISK
wcl386 -q -bt=nt -fe=..\..\TOOLS\bin2inc.exe ..\..\TOOLS\bin2inc.c || goto :fail
..\..\TOOLS\bin2inc.exe MBRCODE.INC mbr_code=..\..\BIN\MBRBOOT.BIN || goto :fail
wasm -q -i=..\..\INC fdisk.asm                  || goto :fail
rem The BSS is a segment of its own behind the code, as PMDOS.SYS's is,
rem so the screen's buffers are not zeros in the file: the order
rem clause places it and the raw output stops at the last written byte.
wlink form raw bin name ..\..\BIN\FDISK.EXE file fdisk.obj order clname CODE offset=0x2000 clname BSS || goto :fail
cd ..\..
rem MOUNT ships: giving a file a drive letter is a thing a user does,
rem not a thing a test does, and it is the only way to reach the
rem kernel's F1h AL=0Fh..11h from a command line.
cd CMD\MOUNT
wasm -q -i=..\..\INC mount.asm                  || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\MOUNT.EXE file mount.obj || goto :fail
cd ..\..
rem CACHE is SMARTDRV /S for the cache that lives in the kernel: the
rem report and the switches, asked over 21h/F1h AL=16h.  CACHE and not
rem DISKCACHE because a command's name has to fit 8.3.
cd CMD\CACHE
wasm -q -i=..\..\INC cache.asm                  || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\CACHE.EXE file cache.obj || goto :fail
cd ..\..
rem UNDELETE gets back what the kernel kept when it was deleted
rem (UNDELETE=ON in CONFIG.SYS, DOS\RECYCLE.INC), and empties it for
rem good with /PURGE.
cd CMD\UNDELETE
wasm -q -i=..\..\INC undelete.asm               || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\UNDELETE.EXE file undelete.obj || goto :fail
cd ..\..
rem CDEX is the one line MSCDEX printed that a user still wants - which
rem letter is which CD-ROM drive - plus where the drive is and what it
rem is called, over 21h/F1h AL=17h.
cd CMD\CDEX
wasm -q -i=..\..\INC cdex.asm                   || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\CDEX.EXE file cdex.obj || goto :fail
cd ..\..
rem SHUTDOWN switches the power off through ACPI, over 21h/F1h AL=1Eh,
rem which writes everything the kernel owes the disks first.
cd CMD\SHUTDOWN
wasm -q -i=..\..\INC shutdown.asm               || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\SHUTDOWN.EXE file shutdown.obj || goto :fail
cd ..\..
rem FIND is MS-DOS 6.22's, switch for switch and message for message:
rem batch files test what it prints and the errorlevel it leaves.
cd CMD\FIND
wasm -q -i=..\..\INC find.asm                   || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\FIND.EXE file find.obj || goto :fail
cd ..\..
rem DISKCOPY and DISKCOMP share INC\DISKIMG.INC: the drives, a disk's
rem shape, a track in and out, and the image of a whole diskette that
rem makes each of them one pass - held in extended memory the kernel
rem keeps for the program, 21h/F1h AL=1Ah, because a slot is 512K.
cd CMD\DISKCOPY
wasm -q -i=..\..\INC diskcopy.asm               || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\DISKCOPY.COM file diskcopy.obj || goto :fail
cd ..\..
cd CMD\DISKCOMP
wasm -q -i=..\..\INC diskcomp.asm               || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\DISKCOMP.COM file diskcomp.obj || goto :fail
cd ..\..
rem MOVE ships for the same reason SYS and FORMAT do: it is a command,
rem not a test.  It is EXTERNAL rather than a verb in CMD.EXE
rem because it needs no interpreter state - the same argument CHOICE
rem makes - and because it is where the 56h-moves-between-directories
rem behaviour is actually reachable from a command line.
cd CMD\MOVE
wasm -q %MVDEFS% -i=..\..\INC move.asm          || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\MOVE.EXE file move.obj || goto :fail
cd ..\..
rem ATTRIB is external in every DOS there has ever been, and for once
rem the reason is not size: it is the only command that has to walk a
rem whole tree, and CMD.EXE has no business holding a recursion
rem and a name pool for one verb.  It is an .EXE rather than a .COM
rem for the same reason MEM and FDISK are - it carries ten kilobytes
rem of BSS it never puts in the file.
cd CMD\ATTRIB
wasm -q -i=..\..\INC attrib.asm                 || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\ATTRIB.EXE file attrib.obj || goto :fail
cd ..\..
rem CHKDSK AND LABEL ARE C - Open Watcom, no C library (-zl), and every
rem system call in one file, SYS_D32.C, which is the only part of either
rem that knows it is on PM-DOS.  DOSINT.ASM is their startup: -dD32 puts
rem the D32 header at its front, so it has to be FIRST on the link line.
rem
rem THE LINK IS NOT THE ONE THE ASSEMBLY PROGRAMS USE, and the difference
rem is two things wlink's raw output does that a single-segment program
rem never meets.  It writes one GROUP straight after another, dropping
rem the alignment gap between them, which put every string a few bytes
rem short of where the code looked for it - so DOSINT.ASM puts _TEXT in
rem DGROUP.  And it stops at the last INITIALISED byte, which leaves the
rem BSS out of the file for the kernel to build its heap on - so
rem DOSINT.ASM ends the image with a D32END segment, and "order" puts
rem that class after BSS, which makes wlink write the BSS out as zeroes.
rem SYS_DPMI.C and SYS_NT.C are the same programs for DOS/32A and for a
rem disk image on Windows; neither is built here.
cd CMD\CHKDSK
wasm -q -dD32 -i=..\..\INC dosint.asm           || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 main.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 scan.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 lost.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 frag.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 lfn.c     || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 fat.c     || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 msg.c     || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 rt.c      || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 sys_d32.c || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\CHKDSK.EXE ^
    file dosint.obj,main.obj,scan.obj,lost.obj,frag.obj,lfn.obj,fat.obj,msg.obj,rt.obj,sys_d32.obj ^
    order clname CODE clname DATA clname BSS clname D32END || goto :fail
cd ..\..
cd CMD\LABEL
wasm -q -dD32 -i=..\..\INC dosint.asm           || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 main.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 fat.c     || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 msg.c     || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 rt.c      || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 sys_d32.c || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\LABEL.EXE ^
    file dosint.obj,main.obj,fat.obj,msg.obj,rt.obj,sys_d32.obj ^
    order clname CODE clname DATA clname BSS clname D32END || goto :fail
cd ..\..
rem XCOPY is C the same way, with its own startup: DOSINT.ASM there also
rem carries the ^C handler, which only raises a flag between files -
rem or ends the program at once when it is waiting on a question.
cd CMD\XCOPY
wasm -q -dD32 -i=..\..\INC dosint.asm           || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 main.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 copy.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 path.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 msg.c     || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 rt.c      || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 sys_d32.c || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\XCOPY.EXE ^
    file dosint.obj,main.obj,copy.obj,path.obj,msg.obj,rt.obj,sys_d32.obj ^
    order clname CODE clname DATA clname BSS clname D32END || goto :fail
cd ..\..
rem EDIT is C too.  It draws, reads keys and finds the shift state
rem through 21h/F0h AL=3 to 0Ah, the kernel's console calls for a full-
rem screen program at ring 3, which has no video memory or BIOS of its
rem own.  Its test mode (EDTEST, keys from a redirected file) is
rem TOOLS\MKEDKEYS.PL's other half.
cd CMD\EDIT
wasm -q -dD32 -i=..\..\INC dosint.asm           || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 main.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 doc.c     || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 view.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 scr.c     || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 ui.c      || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 dlgs.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 help.c    || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 rt.c      || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 sys_d32.c || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\EDIT.EXE ^
    file dosint.obj,main.obj,doc.obj,view.obj,scr.obj,ui.obj,dlgs.obj,help.obj,rt.obj,sys_d32.obj ^
    order clname CODE clname DATA clname BSS clname D32END || goto :fail
cd ..\..
rem FC is C, one file and its startup: 6.22's line resync is easier to
rem keep exact in C than in assembler.  Linked as CHKDSK and LABEL are.
cd CMD\FC
wasm -q -i=..\..\INC dosint.asm                 || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 fc.c      || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\FC.EXE ^
    file dosint.obj,fc.obj ^
    order clname CODE clname DATA clname BSS clname D32END || goto :fail
cd ..\..
rem ISAPNP is C as well: a card's resource data is a little language,
rem and choosing from it is easier to keep right in C.  Linked as FC.
cd CMD\ISAPNP
wasm -q -i=..\..\INC dosint.asm                 || goto :fail
wcc386 -q -bt=dos -mf -3r -s -zl -zld -ox -w4 -we -zp4 isapnp.c  || goto :fail
wlink form raw bin option offset=0x2000 name ..\..\BIN\ISAPNP.EXE ^
    file dosint.obj,isapnp.obj ^
    order clname CODE clname DATA clname BSS clname D32END || goto :fail
cd ..\..

echo === HELLO.COM (a real 16-bit .COM, for the VDM) ===
rem An ordinary real-mode DOS program.  It is not part of the kernel;
rem it is the thing the kernel has to be able to run.
cd TOOLS
wasm -q -i=..\INC hello.asm                     || goto :fail
wlink form raw bin name ..\BIN\TESTS\HELLO.COM file hello.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === HELLOX.EXE (a real 16-bit .EXE, for the VDM) ===
rem Three segments and a far call, so the loader has to apply the fixup
rem table for it to run at all.  "system dos" is what makes wlink emit
rem an MZ header instead of a flat image.
cd TOOLS
wasm -q -i=..\INC hellox.asm                    || goto :fail
wlink system dos name ..\BIN\TESTS\HELLOX.EXE file hellox.obj || goto :fail
cd ..

echo === ARGS.COM (what CMD.EXE hands a 16-bit program) ===
rem HELLO proves a DOS box runs; ARGS proves it was started properly -
rem tail, FCBs, entry AX and environment, all of which only exist once
rem a shell is the thing doing the starting.
cd TOOLS
wasm -q -i=..\INC args.asm                      || goto :fail
wlink form raw bin name ..\BIN\TESTS\ARGS.COM file args.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === XMSTEST.COM (extended memory, from inside the DOS box) ===
cd TOOLS
wasm -q -i=..\INC xmstest.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\XMSTEST.COM file xmstest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === A20TEST.COM (the same gate, reached through the hardware) ===
rem XMSTEST asks the driver.  This one writes port 92h and the 8042
rem directly, which is what the TSS I/O bitmap is there to catch.
cd TOOLS
wasm -q -i=..\INC a20test.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\A20TEST.COM file a20test.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === MEMPROBE.COM (what the BIOS says about memory, asked three ways) ===
rem Runs under real DOS as well, which is where its answers are the
rem BIOS's own: the 486 that reported 0K extended is explained by it.
cd TOOLS
wasm -q -i=..\INC memprobe.asm                  || goto :fail
wlink form raw bin name ..\BIN\TESTS\MEMPROBE.COM file memprobe.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === EXECTST.COM and TSRTEST.COM (a guest that starts a guest, ===
echo ===   and one that outlives the box that loaded it)           ===
cd TOOLS
wasm -q -i=..\INC exectst.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\EXECTST.COM file exectst.obj order clname CODE offset=0x100 || goto :fail
wasm -q -i=..\INC tsrtest.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\TSRTEST.COM file tsrtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === I13TEST.COM (the disk BIOS, and whether it agrees with the ===
echo ===   filesystem sitting on top of it)                         ===
cd TOOLS
wasm -q -i=..\INC i13test.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\I13TEST.COM file i13test.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === CURTEST.COM (one cursor between the BIOS and the console, ===
echo ===   or two that disagree)                                   ===
cd TOOLS
wasm -q -i=..\INC curtest.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\CURTEST.COM file curtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === DPMITEST.COM (a 16-bit DPMI client, for the host) ===
cd TOOLS
wasm -q -i=..\INC dpmitest.asm                  || goto :fail
wlink form raw bin name ..\BIN\TESTS\DPMITEST.COM file dpmitest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === DEVTEST.COM (devices, InDOS, extended error, file stamps) ===
cd TOOLS
wasm -q -i=..\INC devtest.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\DEVTEST.COM file devtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === APITEST.COM (IOCTL, 5Ah/5Bh, TRUENAME, commit, verify, version) ===
cd TOOLS
wasm -q -i=..\INC apitest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\APITEST.COM file apitest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === LOLTEST.COM (the 52h list of lists, and the flags a guest reads) ===
rem Both of the things it checks are bridge bugs, invisible from the
rem 32-bit side: MEM walks the arena correctly because MEM is a D32
rem program and never makes the call a 16-bit tool makes.
cd TOOLS
wasm -q -i=..\INC loltest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\LOLTEST.COM file loltest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === PARKWIN.COM (the handover, with nothing to blame but us) ===
rem INT 2Fh AX=1605h with DX bit 0 set - standard mode - from a program
rem small enough to print what happened.  PM-DOS parks inside the call, so
rem every line it prints after it comes from RMDOS.  See TOOLS-PARKWIN.ASM.
cd TOOLS
wasm -q -i=..\INC parkwin.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\PARKWIN.COM file parkwin.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === PEKID.COM (a child that says so before it asks DOS) ===
cd TOOLS
wasm -q -i=..\INC pekid.asm                      || goto :fail
wlink form raw bin name ..\BIN\TESTS\PEKID.COM file pekid.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === PARKEXEC.COM (and then EXEC, which is what Windows does next) ===
rem PARKWIN proves the handover happens.  This one parks the same way and
rem then asks RMDOS to load and run a program - a .COM, a real .EXE, and
rem one that is not there - in TEXT MODE, where the answer is a printed
rem line rather than a bar painted into Windows' framebuffer.  It is
rem EXECTST.COM's run against the other DOS, on purpose: a check that
rem passes there and fails here names the difference.
cd TOOLS
wasm -q -i=..\INC parkexec.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\PARKEXEC.COM file parkexec.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === PROBE13.COM (does a write to the trace gap land at all?) ===
rem The positive control RMDBG's disk channel has never had.  It writes
rem a signature into the same gap on drive 81h either side of the park
rem and reads it back, which separates three failures that have been
rem sharing one silence: the write erroring, the write landing
rem elsewhere, and the write never leaving the emulator.
cd TOOLS
wasm -q -i=..\INC probe13.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\PROBE13.COM file probe13.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === LPTTEST.COM (is there a printer port to trace through?) ===
rem DBGSINK.INC settled that the printer channel delivered nothing and
rem that the cause was 86Box dropping lpt1_enabled out of its own
rem configuration file on exit.  That is a line in a file, not a fault
rem in the code, and the run scripts can put it back before every
rem launch - so the question is worth asking once more, with a program
rem that answers it.  A printer channel costs six OUTs a byte; the disk
rem channel costs an 8K write per DOS call and 8K of RMDOS to hold.
cd TOOLS
wasm -q -i=..\INC lpttest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\LPTTEST.COM file lpttest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === RMSPEED.COM (what does one DOS call cost?) ===
rem The instrument the empty Program Manager turned out to need.  The
rem trace build got 114 DOS calls into a Windows start-up in four
rem hundred seconds and every one of them was ordinary, so the shell is
rem not stuck on anything - it is walking its program items at a few
rem seconds a call.  This times a DOS call that touches no disk, an
rem open and close, and the whole of a profile read, and the same
rem binary runs on PM-DOS's own DOS from the prompt and on RMDOS
rem through PARKTEST.  Two numbers, one boot.
cd TOOLS
wasm -q -i=..\INC rmspeed.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\RMSPEED.COM file rmspeed.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === HNDTEST.COM (how many files can one process open?) ===
rem The unit test for 67h, which RMDOS refused until v0.55 and which is
rem why Program Manager came up with no groups: Windows had eighteen
rem files open when the shell started, the table DOS builds in the PSP
rem holds twenty, and every open of PROGMAN.INI came back "too many
rem open files".  Runs on both DOSes - the command line tests PM-DOS's,
rem PARKTEST tests RMDOS's.
cd TOOLS
wasm -q -i=..\INC hndtest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\HNDTEST.COM file hndtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === RMFSTEST.COM (does RMDOS write, name and answer like DOS?) ===
rem The first DOS session Windows started on RMDOS wrote a swap file, and
rem RMDOS turned out never to have written a second FAT16 cluster, freed
rem a chain, or created or deleted a file without damaging the entries
rem beside it.  This makes those calls - the write exactly as WINOLDAP
rem makes it, MD, CD, TRUENAME, REN, RD, 29h, 1Ch, 65h, 66h, 69h - and
rem runs on both DOSes: the command line tests PM-DOS's, PARKTEST
rem RMDOS's.  What it cannot see (the entries it never looks at, lost
rem clusters) is checked from outside the image.
cd TOOLS
wasm -q -i=..\INC rmfstest.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\RMFSTEST.COM file rmfstest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === WINDRV.EXE (a Windows 3.1 session with nobody at the keyboard) ===
rem A Win16 program, so it wants the Windows headers and not the NT ones
rem the environment usually names; INCLUDE is put back straight after.
rem Named in WIN.INI's run= line it starts a DOS program, waits, logs to
rem C:\WINDRV.TXT and exits Windows.
set PMD_INC=%INCLUDE%
set INCLUDE=%WATCOM%\H;%WATCOM%\H\WIN
cd TOOLS
wcl -zq -bt=windows -l=windows -fe=..\BIN\TESTS\WINDRV.EXE windrv.c || goto :winfail
if exist windrv.obj del windrv.obj
cd ..
set INCLUDE=%PMD_INC%
set PMD_INC=

echo === SFTSCAN.COM (does the SFT actually carry the names?) ===
rem The positive control for the v0.53 SFT_NAME fix.  "Windows opened
rem the console five times instead of ten" cannot tell a scan that
rem SUCCEEDED from one that FAULTED part way through the first five,
rem and those are very different states.  This does the same scan by
rem the same rule on the same DOS, with somewhere to print it.
cd TOOLS
wasm -q -i=..\INC sftscan.asm                     || goto :fail
wlink form raw bin name ..\BIN\TESTS\SFTSCAN.COM file sftscan.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === XMSREPT.COM (what the XMS host offers, and WHERE) ===
rem Windows standard mode takes all its extended memory through the XMS
rem entry point, which the INT 21h trace cannot see.  This asks the host
rem the same questions and locks the largest block to learn its linear
rem address - the number that decides whether a 286 extender can use it.
cd TOOLS
wasm -q -i=..\INC xmsrept.asm                     || goto :fail
wlink form raw bin name ..\BIN\TESTS\XMSREPT.COM file xmsrept.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === CDXTEST.COM (the MSCDEX API, from inside a DOS box) ===
rem 16-bit ON PURPOSE and it is the whole point: the MSCDEX API is
rem answered out of the V86 monitor's INT 2Fh chain, so a 32-bit native
rem program cannot reach that path at all and a guest is the only place
rem the shim can be tested from.  Needs a disc in the drive for the two
rem read checks; without one it correctly reports not-ready.
cd TOOLS
wasm -q -i=..\INC cdxtest.asm                     || goto :fail
wlink form raw bin name ..\BIN\TESTS\CDXTEST.COM file cdxtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === CDATEST.COM (the MSCD001 device, and CD audio) ===
rem 16-bit for the same reason CDXTEST is, and it covers the other half
rem of the family: the calls that go TO the device rather than asking
rem about the drives.  Every audio call is one of those.  Wants a disc
rem with an audio track on it - TOOLS\MKCD.PL builds two.
cd TOOLS
wasm -q -i=..\INC cdatest.asm                     || goto :fail
wlink form raw bin name ..\BIN\TESTS\CDATEST.COM file cdatest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === WINTEST.COM (the swappable data area, and the Windows handshake) ===
rem 16-bit on purpose.  The 2Fh half of what it checks does not exist
rem for a 32-bit caller at all, and the 5D06h half is served through
rem the same bridge that got 52h wrong - see LOLTEST above.
cd TOOLS
wasm -q -i=..\INC wintest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\WINTEST.COM file wintest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === RMDOS.SYS (the real-mode DOS PM-DOS leaves behind) ===
rem The one thing in this tree that is not part of PM-DOS and does not
rem run on it.  A separate 16-bit program, loaded into the DOS box's
rem arena just before the kernel drops out of protected mode, whose
rem whole job is to be the DOS underneath something that has taken the
rem machine.  See INC\RMDOS.INC for why it cannot be a shim.
rem %DEFS% reaches this one too, and it is the only file outside the
rem kernel that wants it: DBG_LPT switches on DOS\RMDBG.INC, the trace
rem for the side of the handover where nothing of PM-DOS is running.
cd DOS
wasm -q -i=..\INC %DEFS% rmdos.asm                || goto :fail
rem offset=0x0 EVEN THOUGH THE IMAGE IS ORG'd AT RM_ORG.  wlink pads
rem down to the offset it is given rather than starting the file at
rem the ORG, so RMDOS.SYS is a picture of the whole segment and its
rem first sixteen bytes are filler.  That is left alone deliberately -
rem it keeps file offset and segment offset the same number, which is
rem what every dump of this file assumes - and park_loadrm seeks past
rem them.  It has to: in the HMA those sixteen bytes are the ROM.
wlink form raw bin option offset=0x0 name ..\BIN\RMDOS.SYS file rmdos.obj || goto :fail
cd ..

echo === RMTEST.COM (a DOS program with no PM-DOS underneath it) ===
rem Runs on RMDOS, in real mode, after the handover.  Every INT 21h it
rem makes is answered by RMDOS.SYS rather than by the kernel.
cd TOOLS
wasm -q -i=..\INC rmtest.asm                       || goto :fail
wlink form raw bin name ..\BIN\TESTS\RMTEST.COM file rmtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === RMFIND.COM (4Eh and 4Fh, asked what File Manager asks) ===
rem The other program that runs with no PM-DOS underneath it.  RMDOS's
rem find-first and find-next went in written and never executed, and
rem the first caller they ever had was Windows' File Manager, which
rem cannot say what it asked for.  Run it with PARKTEST \RMFIND.COM.
cd TOOLS
wasm -q -i=..\INC rmfind.asm                       || goto :fail
wlink form raw bin name ..\BIN\TESTS\RMFIND.COM file rmfind.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === PARKTEST.EXE (asking the kernel to leave) ===
rem Native, 32-bit, ring 3 - and it is still standing after the
rem processor has been in real mode and another kernel has owned the
rem interrupt table.  That is the whole test.
cd TOOLS
wasm -q -i=..\INC parktest.asm                     || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\PARKTEST.EXE file parktest.obj || goto :fail
cd ..

echo === KBTEST.COM (a 16-bit program asking the BIOS for a keystroke) ===
rem Interactive, and unavoidably so: the bug it exists for is a HANG,
rem and the only way to prove a key arrives is to press one.  Every
rem wait in it is bounded by the tick count, so it reports the hang
rem instead of becoming it.
cd TOOLS
wasm -q -i=..\INC kbtest.asm                     || goto :fail
wlink form raw bin name ..\BIN\TESTS\KBTEST.COM file kbtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === INT9TEST.COM (a DOS box hooking the keyboard INTERRUPT) ===
rem KBTEST asks whether the BIOS buffer gets filled; this asks whether
rem the box gets IRQ 1 at all, which is the question every full-screen
rem program that hooks INT 09h is really asking.  Interactive, and
rem bounded by the tick count for the reason KBTEST is.
cd TOOLS
wasm -q -i=..\INC int9test.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\INT9TEST.COM file int9test.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === ALLOCTST.COM (does a program's memory come back?) ===
rem The leak this exists for is silent - a largest-free-block that
rem shrinks - so it asks in one run by being both halves: the parent
rem measures, EXECs ITSELF as a child that allocates and exits without
rem freeing, and measures again.
cd TOOLS
wasm -q -i=..\INC alloctst.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\ALLOCTST.COM file alloctst.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === CTRYDMP.COM (the country block as bytes, for diffing) ===
cd TOOLS
wasm -q -i=..\INC ctrydmp.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\CTRYDMP.COM file ctrydmp.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === MCBTEST.COM (the memory arena, walked the way MEM walks it) ===
rem The other half of the LOLTEST lesson: PM-DOS's own MEM is a D32
rem program and asks 21h/F1h, so the chain a 16-bit program follows by
rem hand had nothing looking at it at all.
cd TOOLS
wasm -q -i=..\INC mcbtest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\MCBTEST.COM file mcbtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === FCBTEST.COM (the file control block API, all sixteen calls) ===
rem A 16-bit program on purpose: an FCB is a pointer, so every call in
rem here is also a test of the bridge that flattens one - and 29h is
rem the only DOS call whose answer IS a pointer register, which the
rem bridge had never had to carry outwards before.
cd TOOLS
wasm -q -i=..\INC fcbtest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\FCBTEST.COM file fcbtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === BUFTEST.COM (what a DOS call may be told to write on) ===
rem 16-bit, and it has to be: the hole it measures is made of the
rem arithmetic that turns a segment:offset into a flat address, and a
rem native program's pointers never go through it.
cd TOOLS
wasm -q -i=..\INC buftest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\BUFTEST.COM file buftest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === CFGTEST.COM (what CONFIG.SYS actually did) ===
rem 16-bit, because the statements it measures are all about a
rem DOS box: conventional handles, drive letters, an environment
rem copied below 1MB, and a real-mode TSR.
cd TOOLS
wasm -q -i=..\INC cfgtest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\CFGTEST.COM file cfgtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === CACHETST.COM (the disk cache, through the SMARTDRV API) ===
rem 16-bit for exactly CDXTEST's reason: INT 2Fh AX=4A10h is answered
rem out of the V86 monitor's own 2Fh chain, so a DOS box is the only
rem place the interface can be asked from at all.
cd TOOLS
wasm -q -i=..\INC cachetst.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\CACHETST.COM file cachetst.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === VFTEST.COM (the clean-shutdown flag, kernel and disk) ===
rem 16-bit so that the disk is read the way a DOS program reads it,
rem 21h/7305h through the DOS box.  VFTEST HANG is for a rig: a file
rem left half written, and the machine stopped for the plug to be
rem pulled on.
cd TOOLS
wasm -q -i=..\INC vftest.asm                     || goto :fail
wlink form raw bin name ..\BIN\TESTS\VFTEST.COM file vftest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === SHARETST.COM (file sharing and record locking, SHARE's rules) ===
rem 16-bit for the same reason: the first thing a program asks SHARE
rem is INT 2Fh AX=1000h.
cd TOOLS
wasm -q -i=..\INC sharetst.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\SHARETST.COM file sharetst.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === UMBTEST.COM (upper memory, from inside a DOS box) ===
rem 16-bit, and it has to be: a native program runs in a segment based
rem at its own slot and cannot see a UMB at all, let alone the MCB
rem chain that describes one.
cd TOOLS
wasm -q -i=..\INC umbtest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\UMBTEST.COM file umbtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === EMSTEST.COM (expanded memory, and whether the windows work) ===
rem The checks that matter here write through the page frame and read
rem back: a manager that only remembers what 44h told it passes every
rem test built out of return codes and maps nothing.
cd TOOLS
wasm -q -i=..\INC emstest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\EMSTEST.COM file emstest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === EMS4TEST.COM (the LIM 4.0 functions, 4Fh-5Dh) ===
rem The same rule as EMSTEST: where there is memory to look at, the
rem check looks at it - a handle that grew or moved, the bytes 57h
rem moved, the map a 56h target sees and the one its caller gets back.
cd TOOLS
wasm -q -i=..\INC ems4test.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\EMS4TEST.COM file ems4test.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === I24TEST.COM (a DOS box's OWN INT 24h handler) ===
rem 16-bit, and it has to be: a native program's 25h writes the IDT
rem and a guest's writes the real-mode IVT, and only one of those two
rem tables had ever been read.  R3TEST covers the other one.
cd TOOLS
wasm -q -i=..\INC i24test.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\I24TEST.COM file i24test.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === DEVCRIT.EXE (a character device that says no, and INT 24h) ===
rem Needs TESTDEV.SYS loaded: TESTDEV is the only device in the
rem machine that can be made to fail on purpose, and until v0.41 a
rem character driver's error status had nowhere to go.  NATIVE, and it
rem has to be - a 16-bit guest's 25h writes the real-mode IVT and the
rem kernel raises INT 24h through the IDT.
cd TOOLS
wasm -q -i=..\INC devcrit.asm                    || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\DEVCRIT.EXE file devcrit.obj || goto :fail
cd ..

echo === VFYTEST.EXE (does VERIFY verify?) ===
rem Needs TESTBLK.SYS loaded.  A verified write can only be tested
rem against a sector that reads back differently from what was
rem written, and nothing in this machine could be made to - so the RAM
rem disk has a poisoned-sector hook in it that arms on a signature and
rem lies exactly once.
cd TOOLS
wasm -q -i=..\INC vfytest.asm                    || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\VFYTEST.EXE file vfytest.obj || goto :fail
cd ..

echo === IDETEST.EXE (bus-master DMA and 32-bit PIO) ===
rem Reports what the IDE controller is and how each disk is being
rem talked to, then checks that a run of sectors read in ONE transfer
rem - several descriptors, a 64K boundary in the middle - is the same
rem run read one sector at a time.  Needs an ATA disk, and says so
rem plainly on a machine that has none rather than failing.
cd TOOLS
wasm -q -i=..\INC idetest.asm                    || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\IDETEST.EXE file idetest.obj || goto :fail
cd ..

echo === HWPROBE.EXE (what the mouse and IDE probes read at boot) ===
rem Prints the log PMIO keeps of its two boot-time probes.  It is for
rem the machine where a mouse or a CD-ROM drive was NOT found: that
rem leaves nothing to ask about afterwards, and this is what was read.
cd TOOLS
wasm -q -i=..\INC hwprobe.asm                    || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\HWPROBE.EXE file hwprobe.obj || goto :fail
cd ..

echo === F32TEST.COM (the FAT32 API family: 7302h, 7304h, 7305h) ===
cd TOOLS
wasm -q -i=..\INC f32test.asm                    || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\F32TEST.COM file f32test.obj || goto :fail
cd ..

echo === FNDTEST.EXE (4Eh/714Eh errors, 32 find handles and the 4Ch sweep) ===
cd TOOLS
wasm -q -i=..\INC fndtest.asm                    || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\FNDTEST.EXE file fndtest.obj || goto :fail
cd ..

echo === EXECLEAK.EXE (a failed EXEC gives the kernel stack back) ===
cd TOOLS
wasm -q -i=..\INC execleak.asm                   || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\EXECLEAK.EXE file execleak.obj || goto :fail
cd ..

echo === SLOTTEST.EXE (every EXEC level in a slot of its own) ===
cd TOOLS
wasm -q -i=..\INC slottest.asm                   || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\SLOTTEST.EXE file slottest.obj || goto :fail
cd ..

echo === BIGMEM.EXE (a native program with more than 512K) ===
cd TOOLS
wasm -q -i=..\INC bigmem.asm                     || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\BIGMEM.EXE file bigmem.obj || goto :fail
cd ..

echo === VBETEST.EXE (VBE and the linear frame buffer, for a native program) ===
rem 21h/F1h AL=1Fh and SEL_VBE.  VBETEST /S shows a picture in a VBE
rem mode until a key is pressed, for a look at a real card.
cd TOOLS
wasm -q -i=..\INC vbetest.asm                    || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\VBETEST.EXE file vbetest.obj || goto :fail
cd ..

echo === TESTBLK.SYS (a BLOCK device driver: a 32K RAM disk) ===
rem The block half of DEVICE= is a different job from the character
rem half - a unit is a drive letter, not a name - and this is what it
rem was written against.  No offset= : a .SYS is loaded at paragraph
rem zero of wherever it lands.
cd TOOLS
wasm -q -i=..\INC testblk.asm                  || goto :fail
wlink form raw bin name ..\BIN\TESTS\TESTBLK.SYS file testblk.obj order clname CODE || goto :fail
cd ..

echo === TESTDEV.SYS (a device driver to load with DEVICE=) ===
rem A real 16-bit character driver, and the only way to find out
rem whether DEVICE= works: its INIT prints through INT 21h, which is a
rem DOS call made from inside a driver before there is a shell.
rem Linked at offset 0 - a .SYS is a raw image whose header is byte 0,
rem not a .COM with a PSP in front of it.
cd TOOLS
wasm -q -i=..\INC testdev.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\TESTDEV.SYS file testdev.obj order clname CODE || goto :fail
cd ..

echo === CTRYTEST.COM (the block a number is formatted with) ===
rem MEM asks 21h/38h once per number it prints, and prints only the
rem first group of digits - so what comes back from that call, read by
rem a 16-bit program rather than read in the source, is the question.
cd TOOLS
wasm -q -i=..\INC ctrytest.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\CTRYTEST.COM file ctrytest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === UDTEST.COM (a program catching its OWN exceptions) ===
rem Vectors 0-7 belong to the guest, and the monitor used to eat them.
rem The interesting half is /HANG, which is not run by the batch file
rem because a pass there is the DOS box dying on purpose.
cd TOOLS
wasm -q -i=..\INC udtest.asm                     || goto :fail
wlink form raw bin name ..\BIN\TESTS\UDTEST.COM file udtest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === IDLETEST.COM (INT 28h reaching a real-mode handler) ===
cd TOOLS
wasm -q -i=..\INC idletest.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\IDLETEST.COM file idletest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === HOOKTEST.COM (a box's own hooks on 21h and 2Fh, and Ctrl+C) ===
cd TOOLS
wasm -q -i=..\INC hooktest.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\HOOKTEST.COM file hooktest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === MSTEST.COM (INT 33h from a DOS box: the mouse driver) ===
cd TOOLS
wasm -q -i=..\INC mstest.asm                     || goto :fail
wlink form raw bin name ..\BIN\TESTS\MSTEST.COM file mstest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === FPUTEST.COM (the coprocessor, MMX and SSE, from a DOS box) ===
rem -fpi87 on all four of these: without it wasm writes a coprocessor
rem instruction as a fixup for an emulator library to patch, and a raw
rem binary has nobody to patch it.
cd TOOLS
wasm -q -fpi87 -i=..\INC fputest.asm             || goto :fail
wlink form raw bin name ..\BIN\TESTS\FPUTEST.COM file fputest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === DPMIFPU.COM, DPMIFP32.COM (the coprocessor's error in a client) ===
cd TOOLS
wasm -q -fpi87 -i=..\INC dpmifpu.asm             || goto :fail
wlink form raw bin name ..\BIN\TESTS\DPMIFPU.COM file dpmifpu.obj order clname CODE offset=0x100 || goto :fail
wasm -q -fpi87 -dCLIENT32 -i=..\INC -fo=dpmifp32.obj dpmifpu.asm || goto :fail
wlink form raw bin name ..\BIN\TESTS\DPMIFP32.COM file dpmifp32.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === FPU32.EXE (and from a native program) ===
cd TOOLS
wasm -q -fpi87 -i=..\INC fpu32.asm               || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\FPU32.EXE file fpu32.obj || goto :fail
cd ..

echo === R3TEST.EXE (a native program, and where it is standing) ===
rem The permanent test for the ring-3 release: CRASH proves what a
rem program can no longer do, and this proves everything it still has
rem to be able to do still works from out there.
cd TOOLS
wasm -q -i=..\INC r3test.asm                    || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\R3TEST.EXE file r3test.obj || goto :fail
cd ..

echo === MS32TEST.EXE (INT 33h from a native program: the mouse driver) ===
cd TOOLS
wasm -q -i=..\INC ms32test.asm                   || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\MS32TEST.EXE file ms32test.obj || goto :fail
cd ..

echo === VPICTEST.COM (a card's interrupt, in a DOS box) ===
rem INT9TEST asks whether a box gets the keyboard's interrupt, which
rem the kernel delivers by hand.  This asks it of every other line: the
rem mask register a driver writes, the vector it expects, the EOI it
rem sends - with the real-time clock as the card, because every machine
rem has one and it is behind the cascade.
cd TOOLS
wasm -q -i=..\INC vpictest.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\VPICTEST.COM file vpictest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === TIMERTST.COM (the timer, in a DOS box) ===
rem VPICTEST's question asked of line 0: INT 08h and INT 1Ch reach a
rem program at last, and the kernel's clock keeps 18.2 a second when a
rem program runs the timer faster for itself.
cd TOOLS
wasm -q -i=..\INC timertst.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\TIMERTST.COM file timertst.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === RMIVT.COM (the vector table a BIOS call runs with) ===
rem A program's hooks on INT 1Ah, INT 08h, INT 1Ch and IRQ 8 are not
rem called in real mode when the kernel calls the BIOS, and the clock
rem neither runs fast nor loses ticks while it does.
cd TOOLS
wasm -q -i=..\INC rmivt.asm                      || goto :fail
wlink form raw bin name ..\BIN\TESTS\RMIVT.COM file rmivt.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === IRQXTEST.COM, IRQXWAIT.EXE (a resident driver, with no box running) ===
rem IRQXTEST /I goes resident on INT 1Ch and IRQ 8 and types a Z every
rem two seconds; IRQXWAIT is native, so every Z it reads came from a
rem handler the kernel ran with no DOS box there; IRQXTEST /R counts.
cd TOOLS
wasm -q -i=..\INC irqxtest.asm                   || goto :fail
wlink form raw bin name ..\BIN\TESTS\IRQXTEST.COM file irqxtest.obj order clname CODE offset=0x100 || goto :fail
wasm -q -i=..\INC irqxwait.asm                   || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\IRQXWAIT.EXE file irqxwait.obj || goto :fail
cd ..

echo === DMATEST.COM (the 8237, from a DOS box) ===
rem A driver hands the DMA controller segment times sixteen, which is
rem not where upper memory or the page frame really is.  With a Sound
rem Blaster (DBXRUN.PS1 -Sound) the card records into each kind of
rem memory and every byte is looked at; without one, the registers.
cd TOOLS
wasm -q -i=..\INC dmatest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\DMATEST.COM file dmatest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === DPMIIRQ.COM (the same, and a card's memory, in protected mode) ===
cd TOOLS
wasm -q -i=..\INC dpmiirq.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\DPMIIRQ.COM file dpmiirq.obj order clname CODE offset=0x100 || goto :fail
rem And again as a 32-bit client, which is what every extender is: the
rem same 16-bit code, given frames of dwords.  Nothing in the tree had
rem ever been one.
wasm -q -dCLIENT32 -i=..\INC -fo=dpmii32.obj dpmiirq.asm || goto :fail
wlink form raw bin name ..\BIN\TESTS\DPMII32.COM file dpmii32.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === DPMIVEC.COM, DPMIV32.COM (a client's own handlers, and the defaults) ===
rem A handler on a software interrupt, and what it passes on to: the
rem address 0204h answers with has to be somewhere.  Also a thousand
rem calls to the BIOS, which the host used not to survive a hundred of.
cd TOOLS
wasm -q -i=..\INC dpmivec.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\DPMIVEC.COM file dpmivec.obj order clname CODE offset=0x100 || goto :fail
wasm -q -dCLIENT32 -i=..\INC -fo=dpmiv32.obj dpmivec.asm || goto :fail
wlink form raw bin name ..\BIN\TESTS\DPMIV32.COM file dpmiv32.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === VDSTEST.COM (INT 4Bh: where a driver's buffer really is) ===
rem Its last check reads the address it was given from protected mode,
rem through a descriptor - the one road to it that does not go through
rem the page tables the answer came out of.
cd TOOLS
wasm -q -i=..\INC vdstest.asm                    || goto :fail
wlink form raw bin name ..\BIN\TESTS\VDSTEST.COM file vdstest.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === NICIRQ.COM (a PCI network card's interrupt, level-triggered) ===
rem Needs an 8029, which DOSBox-X has not got and 86Box has:
rem TOOLS\BUS86.PS1 is the rig.  Anywhere else it says there is no such
rem card and tests nothing.
cd TOOLS
wasm -q -i=..\INC nicirq.asm                     || goto :fail
wlink form raw bin name ..\BIN\TESTS\NICIRQ.COM file nicirq.obj order clname CODE offset=0x100 || goto :fail
cd ..

echo === PCITEST.EXE (what is on the PCI bus, and does the kernel agree) ===
cd TOOLS
wasm -q -i=..\INC pcitest.asm                    || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\PCITEST.EXE file pcitest.obj || goto :fail
cd ..

echo === IRQTEST.EXE (two handlers on one line; needs BUILD irqtest) ===
cd TOOLS
wasm -q -i=..\INC irqtest.asm                    || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\IRQTEST.EXE file irqtest.obj || goto :fail
cd ..

echo === CRASH.EXE (a 32-bit program that faults, and a shell that lives) ===
rem A D32 program, so it runs at ring 0 in the kernel's own segment -
rem which is exactly why its fault used to end the machine.
cd TOOLS
wasm -q -i=..\INC crash.asm                     || goto :fail
wlink form raw bin option offset=0x2000 name ..\BIN\TESTS\CRASH.EXE file crash.obj || goto :fail
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
rem Rebuilt unconditionally.  This used to compare timestamps with a
rem "for /f" over "dir /o:d" and only rebuild when MKDISK.C was the
rem newer of the two - but FOR /F is a cmd.exe command extension that
rem NT 3.51 rejects outright, and the thing it was guarding against is
rem an edit to MKDISK.C being silently ignored while the images keep
rem whatever the old EXE wrote into them.  One small C file costs a
rem second to compile; getting the staleness test wrong costs a
rem debugging session against a disk image built by yesterday's tool.
cd TOOLS
wcl386 -q -bt=nt -fe=mkdisk.exe mkdisk.c        || goto :fail
cd ..

rem === TESTIMG.IMG - a disk image to be a FILE on another disk ===
rem A plain 1.44M FAT12 floppy with one text file in it, built by the
rem same tool that builds the real images and then carried ONTO them
rem as ordinary content.  MOUNT gives it a drive letter and IMGTEST
rem reads and writes it from there.
rem
rem It is a fixture and not a product, so it goes to BIN\TESTS: the
rem split is by what SHIPS, and nothing about a PM-DOS disk requires
rem a picture of another one.
echo === TESTIMG.IMG (a disk image, to be mounted as a file) ===
TOOLS\MKDISK.EXE BIN\TESTS\TESTIMG.IMG BIN\BOOT.BIN DISK\IMGFILE.TXT || goto :fail

echo === PMDOS.IMG ===
TOOLS\MKDISK.EXE IMAGES\PMDOS.IMG BIN\BOOT.BIN BIN\PMIO.SYS BIN\PMDOS.SYS BIN\CMD.EXE BIN\COMMAND.COM ^
    BIN\CHOICE.COM BIN\MORE.COM BIN\SORT.COM BIN\SYS.COM BIN\FORMAT.COM BIN\MEM.EXE ^
    BIN\FDISK.EXE BIN\MOUNT.EXE BIN\CACHE.EXE BIN\UNDELETE.EXE BIN\CDEX.EXE BIN\MOVE.EXE BIN\ATTRIB.EXE BIN\CHKDSK.EXE BIN\LABEL.EXE BIN\XCOPY.EXE BIN\EDIT.EXE ^
    BIN\FC.EXE BIN\FIND.EXE BIN\DISKCOPY.COM BIN\DISKCOMP.COM BIN\ISAPNP.EXE BIN\SHUTDOWN.EXE ^
    BIN\RMDOS.SYS README.TXT DISK\CONFIG.SYS DISK\CONFIG.TXT DISK\AUTOEXEC.BAT -dir ^
    TESTS BIN\TESTS\DRIVES.EXE BIN\TESTS\LFNAPI.COM BIN\TESTS\HELLO.COM BIN\TESTS\HELLOX.EXE BIN\TESTS\ARGS.COM ^
    BIN\TESTS\XMSTEST.COM BIN\TESTS\A20TEST.COM BIN\TESTS\MEMPROBE.COM BIN\TESTS\EXECTST.COM BIN\TESTS\TSRTEST.COM ^
    BIN\TESTS\I13TEST.COM BIN\TESTS\CURTEST.COM BIN\TESTS\DPMITEST.COM BIN\TESTS\DEVTEST.COM ^
    BIN\TESTS\APITEST.COM BIN\TESTS\IDLETEST.COM BIN\TESTS\HOOKTEST.COM BIN\TESTS\MSTEST.COM BIN\TESTS\LOLTEST.COM BIN\TESTS\RMTEST.COM ^
    BIN\TESTS\RMFIND.COM BIN\TESTS\PARKTEST.EXE BIN\TESTS\WINTEST.COM BIN\TESTS\PARKWIN.COM ^
    BIN\TESTS\PARKEXEC.COM BIN\TESTS\PEKID.COM BIN\TESTS\PROBE13.COM BIN\TESTS\SFTSCAN.COM ^
    BIN\TESTS\XMSREPT.COM BIN\TESTS\CDXTEST.COM BIN\TESTS\CDATEST.COM ^
    BIN\TESTS\KBTEST.COM BIN\TESTS\INT9TEST.COM ^
    BIN\TESTS\UDTEST.COM BIN\TESTS\ALLOCTST.COM BIN\TESTS\MCBTEST.COM BIN\TESTS\FCBTEST.COM ^
    BIN\TESTS\BUFTEST.COM BIN\TESTS\UMBTEST.COM BIN\TESTS\EMSTEST.COM BIN\TESTS\EMS4TEST.COM BIN\TESTS\DEVCRIT.EXE ^
    BIN\TESTS\I24TEST.COM BIN\TESTS\VFYTEST.EXE BIN\TESTS\IDETEST.EXE BIN\TESTS\HWPROBE.EXE BIN\TESTS\F32TEST.COM BIN\TESTS\FNDTEST.EXE ^
    BIN\TESTS\CFGTEST.COM BIN\TESTS\CACHETST.COM BIN\TESTS\VFTEST.COM BIN\TESTS\SHARETST.COM BIN\TESTS\TESTDEV.SYS BIN\TESTS\TESTBLK.SYS BIN\TESTS\CTRYDMP.COM ^
    BIN\TESTS\CTRYTEST.COM BIN\TESTS\CRASH.EXE BIN\TESTS\R3TEST.EXE BIN\TESTS\MS32TEST.EXE BIN\TESTS\VPICTEST.COM BIN\TESTS\TIMERTST.COM BIN\TESTS\RMIVT.COM BIN\TESTS\IRQXTEST.COM BIN\TESTS\IRQXWAIT.EXE BIN\TESTS\DMATEST.COM BIN\TESTS\DPMIIRQ.COM BIN\TESTS\DPMII32.COM BIN\TESTS\DPMIVEC.COM BIN\TESTS\DPMIV32.COM BIN\TESTS\VDSTEST.COM BIN\TESTS\NICIRQ.COM BIN\TESTS\PCITEST.EXE BIN\TESTS\IRQTEST.EXE BIN\TESTS\FPUTEST.COM BIN\TESTS\FPU32.EXE BIN\TESTS\DPMIFPU.COM BIN\TESTS\DPMIFP32.COM DISK\BATTEST.BAT ^
    DISK\SUB.BAT DISK\CHTEST.BAT DISK\GOTOTEST.BAT DISK\LOOPTEST.BAT DISK\REDTEST.BAT ^
    DISK\LFNTEST.BAT DISK\CMDTEST.CMD DISK\SUBQ.CMD DISK\BATQ.BAT DISK\CTRLC.BAT DISK\CHAIN1.BAT ^
    DISK\CHAIN2.BAT DISK\NOLABEL.BAT DISK\SHIFT12.BAT DISK\VDMTEST.BAT DISK\NESTTEST.BAT ^
    DISK\DISKTEST.BAT DISK\IMGTEST.BAT DISK\DEEPTEST.BAT DISK\FDTEST.BAT DISK\NULTEST.BAT DISK\CRASHTST.BAT DISK\UDTEST.BAT DISK\UNDTEST.BAT DISK\APITEST.BAT ^
    DISK\LOLTEST.BAT DISK\PARK.BAT DISK\PARKXMS.BAT DISK\SWTEST.BAT ^
    DISK\DIRTEST.BAT DISK\GLUETEST.BAT DISK\FMTTEST.BAT DISK\FMT1.KEY DISK\FMT2.KEY DISK\RENTEST.BAT DISK\FORFTEST.BAT DISK\KEYTEST.BAT DISK\KEYTEST.KEY DISK\C16TEST.BAT DISK\C16SUB.BAT DISK\XCTEST.CMD DISK\XCEL.BAT ^
    DISK\BUSTEST.BAT DISK\FPUTEST.BAT DISK\MSTEST.BAT DISK\EDTEST.BAT DISK\FDM1.KEY DISK\ED1.KEY DISK\ED2.KEY DISK\ED3.KEY DISK\ED4.KEY DISK\ED5.KEY DISK\ED6.KEY DISK\ED7.KEY DISK\ED8.KEY DISK\ED9.KEY DISK\ED10.KEY DISK\ED11.KEY DISK\ED12.KEY DISK\ED13.KEY DISK\ED14.KEY DISK\ED15.KEY DISK\ED16.KEY DISK\ED17.KEY DISK\EDBIN.DAT DISK\EDTABS.DAT DISK\EDLF.DAT || goto :fail

echo.
echo Build OK.
dir BIN | find "."
dir IMAGES | find ".IMG"
popd
goto :end

:nohome
echo *** Could not switch to the directory holding BUILD.CMD.
echo *** Run BUILD.CMD from that directory; a path with spaces in it
echo *** cannot be found this way on NT 3.51.
goto :failrc

:winfail
cd ..
set INCLUDE=%PMD_INC%
set PMD_INC=

:fail
echo.
echo *** BUILD FAILED ***
popd

:failrc
rem NT 3.51's cmd.exe has no EXIT /B, and its EXIT ignores an exit code.
rem A FIND that matches nothing is how this script hands ERRORLEVEL 1
rem back to whoever called it, so it must be the LAST thing run.
ver | find "@@" > nul

:end
