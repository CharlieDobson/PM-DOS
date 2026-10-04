@ECHO OFF
PROMPT $P$G
PATH A:\
REM
REM WHERE EVERYTHING IS.  The root of this disk holds PM-DOS and
REM nothing else: the kernel, CMD.EXE and COMMAND.COM, and the commands
REM that are part of the system - CHOICE, MORE, SORT, SYS, FORMAT, MEM,
REM FDISK, MOUNT, MOVE, ATTRIB, CHKDSK, LABEL, CACHE, UNDELETE, CDEX,
REM FC, FIND, DISKCOPY, DISKCOMP, ISAPNP and SHUTDOWN.
REM Everything else is a TEST, written to prove one thing about the
REM kernel, and lives in \TESTS.
REM
REM TWO SHELLS, since v0.60.  CMD.EXE is PM-DOS's own - 32-bit, long
REM names, DOSKEY built in - and it is the one running this file.
REM COMMAND.COM is 16-bit and is what COMSPEC names, so it is the shell
REM a DOS program gets when it shells out: a 32-bit program cannot run
REM in a DOS box, and under Windows 3.1 nothing 32-bit runs at all.  It
REM has DOS 6.22's commands and behaves as 6.22's COMMAND.COM does, and
REM C16TEST.BAT is what checks that:
REM   COMMAND /C C16TEST.BAT
REM
REM THE PATH IS THE ROOT OF THIS DISK AND NOTHING ELSE, so the commands
REM run from anywhere and a test is run from where it lives:
REM   CD \TESTS
REM or by its whole name, \TESTS\HWPROBE.  The batch files in \TESTS
REM call their programs by bare name and want to be started from
REM there.  A hard disk built by MKHD.CMD has this same file with its
REM own PATH line at the end of it, naming C: and C:\TESTS.
REM
REM On the host it is the same split: BIN\ is what a disk is made of
REM and BIN\TESTS\ is the rest.  MKDISK.EXE grew a "-dir NAME" switch
REM to do it, and BUILD.CMD names what SHIPS rather than what moves -
REM so a new test program lands in TESTS by default, and adding a new
REM COMMAND means saying so.
REM
REM CACHETST is the newest of them and is about the DISK CACHE that
REM arrived in v0.59.  It is sixteen-bit for CDXTEST's reason - the
REM interface it asks, INT 2Fh AX=4A10h, is SMARTDRV's, it is
REM answered out of the V86 monitor's own 2Fh chain, and a 32-bit
REM program cannot reach it.  Ten checks: that a cache is there and
REM says it is version 4.00, what its elements are, that reading
REM the same file twice really moves the hit and miss counters,
REM that AH=0Dh leaves nothing owed, that a file written and closed
REM DOES leave something owed until the next commit, and that C: is
REM read AND write cached while A: is read cached only - which is
REM the rule for every removable thing on the machine and has no
REM switch.  It puts C: back the way it found it before it exits.
REM CACHETST
REM
REM SHARETST is the same shape for SHARE, which is in the kernel since
REM v0.60 (SHARE= in CONFIG.SYS): sixteen checks with SHARE.EXE's own
REM answers - compat opens beside each other, deny modes refused with
REM INT 24h asked first, a create refused before it truncates, locks
REM that overlap, a read through the other handle into a locked
REM range, the lock count SHARE= set, and a close letting them all
REM go.  Sixteen-bit because the first question is INT 2Fh AX=1000h.
REM SHARETST
REM
REM CACHE is the same cache from the command line, and it is SMARTDRV
REM /S with the name changed: the element count, the hits and the
REM misses, the three sizes CACHE= sets, and a row for every letter
REM saying whether its reads and its writes are cached.  The switches
REM are SMARTDRV's that still mean something for a cache that cannot
REM be loaded or unloaded - /C writes what is owed, /R empties it and
REM starts the count again, /X and C+ A- D set write-behind per letter,
REM /Q says nothing.  A native program asks the same things over
REM 21h/F1h AL=16h, which is how CACHE does it.
REM   CACHE /S
REM   CACHE C+ /C
REM
REM UNDELETE gets back what was deleted: with UNDELETE=ON (the default)
REM the kernel keeps a deleted file in a hidden RECYCLED directory on
REM its drive instead of freeing it.  UNDELETE on its own offers each
REM file deleted from the current directory in turn; /LIST lists, /ALL
REM recovers without asking, /S takes in the directories below, and
REM /PURGE empties RECYCLED for good.  UNDTEST.BAT in \TESTS checks it.
REM   UNDELETE /LIST
REM   UNDELETE /STATUS
REM
REM CDXTEST needs a disc in the drive: it
REM asks the MSCDEX API (INT 2Fh AH=15h) the nine questions a CD-ROM
REM installer asks, from inside a DOS box - which is the only place it
REM can be asked from, because that API is answered out of the V86
REM monitor's own 2Fh chain and a 32-bit program cannot reach it.
REM There is no MSCDEX.EXE on this disk and there should not be; see
REM DOS\MSCDEX.INC for why.
REM CDXTEST
REM
REM CDEX is the line MSCDEX printed when it loaded - which letter is
REM which drive - as a command: one row per CD-ROM drive with its
REM letter, the unit number MSCD001 counts it by, where it is on the
REM IDE bus, the label of the disc in it (or "no disc") and the name
REM the drive gave IDENTIFY.  It is a report and takes no switches.
REM   CDEX
REM
REM ISAPNP is for a Plug and Play sound card on a machine whose BIOS
REM is not Plug and Play, which leaves everything on the card off.  It
REM lists the cards; /A switches every device on with resources nothing
REM else is using; 1.0 IO=220,330,388 IRQ=5 DMA=1,5 sets one by hand.
REM It does what CTCM did, and belongs in this file if it is wanted:
REM   ISAPNP /A
REM
REM FC and FIND are MS-DOS 6.22's, output and errorlevels alike.  FC
REM compares two files or two sets of them, as text or (/B, or a .EXE
REM or .COM) byte by byte; FIND prints the lines of its files or its
REM input that hold a string, or (/V) do not.
REM   FC /N README.TXT CONFIG.SYS
REM   FIND /I "device" CONFIG.SYS
REM
REM DISKCOPY and DISKCOMP are 6.22's too, but each does a diskette in
REM ONE PASS: the whole disk is read into extended memory first, so a
REM machine with one floppy drive needs one swap, not four.  DISKCOPY
REM formats the target if it has to, and once the source is in memory
REM writes it to as many targets as it is asked for.
REM   DISKCOPY A: A:
REM   DISKCOMP A: A:
REM
REM SHUTDOWN writes everything still owed to the disks and switches
REM the power off through ACPI.  On a machine without ACPI it says so
REM and leaves everything as it was.
REM   SHUTDOWN
REM
REM PARKTEST runs RMTEST.COM on the real-mode DOS the kernel leaves
REM behind - nine checks now, the ninth being CON opened BY NAME,
REM which RMDOS could not do until v0.52.  PARKEXEC parks and then
REM EXECs, which is what Windows does next.  HNDTEST asks 67h for
REM sixty handles and then opens fifty files, which is the unit test
REM for the handle table v0.55 rewrote.
REM
REM PROBE13 is v0.52's: it writes into the gap between the master
REM boot record and the first partition either side of the park and
REM reads it back, on both drives.  It is the positive control for
REM the disk trace channel and it is what found that the FIRST BIOS
REM disk call after any park used to fail.
REM
REM LPTTEST is v0.55's, and asks whether the PRINTER is a trace
REM channel: the port is there and the strobe is seen, but 86Box's
REM Generic Text Printer turns every printable character into a form
REM feed.  Run it with lpt1_enabled = 1 in 86box.cfg - 86Box drops
REM that line every time it rewrites the file on exit.
REM
REM WIN /S NOW STARTS COMPLETELY, AND THINGS IN IT RUN, AND THEY CAN
REM STOP AGAIN.  Windows 3.1 standard mode takes the handover, runs on
REM RMDOS, loads KRNL286 through its EXEC, enters protected mode, and
REM reaches a Program Manager with its groups in it - with a stock
REM DOSX.EXE and nothing in Windows patched.  Ctrl-Esc opens the Task
REM List and Escape closes it; Clipboard Viewer starts from Main and
REM exits on Alt-F4 and Program Manager comes back.  That is v0.57 -
REM see NOTES.TXT for the two bytes it turned on.
REM
REM AND AS OF v0.58 FILE MANAGER LISTS THE DISK: the drive bar, the
REM tree with C:\ and WINDOWS under it, and 98 files in the root.
REM
REM WHAT IS STILL WRONG: EXITING Windows resets the machine instead of
REM unparking.
REM
REM It is left commented out because it costs two and a half minutes
REM of emulated boot to watch, which is a long time to wait for a
REM test that is not about Windows.
REM WIN /S
REM
REM RMFIND asks RMDOS the same questions File Manager asks - 4Eh and
REM 4Fh, including TWO SEARCHES OPEN AT ONCE, which is what a
REM directory tree is read with and what v0.58 fixed.  It is the
REM cheap way to test that: forty seconds of boot instead of three
REM minutes of Windows.
REM PARKTEST \TESTS\RMFIND.COM
REM
REM THE CD-ROM.  With a disc in the drive the letter after the hard
REM disks is an ISO9660 volume, or whatever CDROM= in CONFIG.SYS says.
REM On this machine that is H: - \TESTS\DRIVES prints the row, and
REM it now knows what a CD-ROM is instead of showing one as a hard
REM disk with no partition table.  DRIVES is a diagnostic and lives
REM in \TESTS with the rest of them; the shipping view of the same
REM disk is FDISK /L.  The transfers go by bus-master DMA; IDETEST
REM says which mode, and BUILD ata says whether a read really used it.
REM
REM Joliet long names come through the LFN layer, so DIR shows both the
REM 8.3 alias and the real name, and either spelling can be typed.
REM TOOLS\MKISO.PS1 builds S:\Software\lfntest.iso, which is the disc
REM with the awkward names on it; TOOLS\SWAPTEST.PS1 changes the disc
REM underneath the running machine, which is what proved that a swap is
REM noticed.  All of these are commented out because they need a disc
REM in the emulated drive to say anything.
REM DIR H:\
REM TYPE H:\README.TXT
REM
REM THE DISC IS A DISC NOW AND NOT ONLY A FILESYSTEM.  A CD-R appended
REM to twice mounts its NEWEST session rather than the first one; a CD
REM Extra, whose first session is music and whose block 16 is therefore
REM not a volume descriptor at all, mounts its data session instead of
REM refusing to mount; and a disc authored on Unix shows its real names
REM through Rock Ridge, including the ones too long to fit in a single
REM directory record.  Typing H: works now as well, which it did not
REM before: the shell had four letters written into it as a number,
REM so the CD was reachable as DIR H: and not by its own name.
REM
REM CD AUDIO goes through MSCD001, which is a real character device in
REM the chain behind NUL rather than a shim - the audio API IS the
REM device interface, so there was no way to have one without the
REM other.  \TESTS\CDATEST asks it what is on the disc, plays a track
REM and watches the Q channel move, which is the one check a driver
REM cannot pass by merely answering politely.  It wants a disc with
REM music on it: TOOLS\MKCD.PL builds four different discs, because
REM nothing on the host could make any of them.
REM CDATEST
REM
REM AND THE LAST OF THE CD-ROM LIST, El Torito excepted.  A file
REM written in several extents lists ONCE and reads all of it; a
REM name with an accent in it is spelled rather than turned into
REM underscores, because there is a real codepage 437 table now and
REM not a range check; a program can ask for the RAW 2352-byte frame
REM and for the disc's catalogue number; and 150Fh answers out of
REM the same reader the drive letter uses, in the shape a real
REM MSCDEX.EXE was disassembled to establish.
REM
REM A DISC WITH NO ISO9660 ON IT AT ALL now mounts too: UDF, which
REM is what a DVD authored without a bridge carries.  It is tried
REM only when there is no CD001 on the disc, so nothing that read
REM before is read any differently.  TOOLS\MKUDF.PS1 builds that
REM disc with Windows' own image writer, and Windows will mount it
REM back to say what the listing ought to look like.
REM
REM Two CD-ROM drives at once work as well, which is how the one bug
REM in all of this was found: the reader is one set of globals, so
REM coming back to the other letter has to mount it again.
REM
REM
REM MOUNT gives a FILE a drive letter of its own, which is new in
REM v0.60 and is how this machine tests disk layouts without a host
REM tool writing images.
REM
REM   MOUNT                   what is mounted now
REM   MOUNT E: C:\TESTS\TESTIMG.IMG    give that file the letter E:
REM   MOUNT C:\CD.ISO         ...or the next free letter
REM   MOUNT E: C:\CD.ISO /R   read-only, which an .ISO is anyway
REM   MOUNT /U E:             and give the letter back
REM
REM WHAT IS IN THE FILE DECIDES HOW IT IS READ, not what it is
REM called: an .ISO is ISO9660 or UDF because block 16 says so, an
REM .IMG is FAT because sector zero does, and a file holding a whole
REM disk becomes its first FAT partition.  While a file is mounted it
REM cannot be deleted, truncated, opened for writing or mounted a
REM second time - the volume is standing on it.  Reading it as a file
REM IS allowed: the image's sectors and the file's bytes are the same
REM blocks in the same cache, so the two views cannot disagree.
REM
REM   IMGTEST.BAT   proves all of that, against C:\TESTS\TESTIMG.IMG
REM   DEEPTEST.BAT  the path depth, which is 32 now and was 8
REM   FDTEST.BAT    FDISK's create, delete and set-active, on the
REM                 second disk - it needs MULTI.IMG attached
REM   MVTEST.BAT    MOVE, on MULTI.IMG's volumes and never on C:
REM   ATTRTEST.BAT  ATTRIB, on the same volumes and for the same
REM                 reason: it makes files that cannot be deleted
REM   HELPTEST.BAT  /? on every command there is
REM
REM EVERY COMMAND ANSWERS /?, internal and external, and that is the
REM quickest way to find out what this shell can actually do:
REM
REM   DIR /?        ...and TYPE, COPY, DEL, REN, CLS, VER, VOL, DATE,
REM                 TIME, PAUSE, REM, EXIT, ECHO, BREAK, VERIFY, SET,
REM                 PATH, PROMPT, MD, CD, RD, GOTO, IF, FOR, SHIFT,
REM                 CALL, CTTY, DOSKEY and LOADHIGH
REM   MEM /?        ...and MORE, SORT, CHOICE, FORMAT, SYS, FDISK,
REM                 MOUNT, MOVE, ATTRIB, CHKDSK, LABEL, CACHE, CDEX,
REM                 ISAPNP, CMD, COMMAND and \TESTS\DRIVES
REM
REM WHAT THOSE SCREENS SAY IS THIS SHELL AND NOT MS-DOS.  A switch that
REM is not on the help screen is a switch this CMD.EXE does not have -
REM and since v0.60 DIR has all of DOS 6's (/P /W /A /O /S /B /L /C and
REM DOS 7's /4), COPY asks before overwriting at the prompt and takes
REM /Y and /-Y and COPYCMD, REN takes wildcards, FOR has NT's /F, and
REM CMD /C runs one command line and exits.
REM
REM DOSKEY IS PART OF THE SHELL AND ALWAYS ON.  Up and Down bring back
REM earlier lines, F7 lists them, F8 finds one by its beginning, F9 by
REM its number; Left, Right, Home, End, Ins, Del and the Ctrl arrows
REM edit; F1-F6 work on the last line as they always did.  The DOSKEY
REM command defines macros and sets the rest up:
REM
REM   DOSKEY LS=DIR $1 /B /ON     LS C:\TESTS  is then that listing
REM   DOSKEY /M                   the macros
REM   DOSKEY /H                   the history
REM   DOSKEY /INSERT              typing inserts rather than overtypes
REM
REM   DIRTEST.BAT   DIR's switches     RENTEST.BAT   REN, and COPY /Y
REM   FORFTEST.BAT  FOR /F, CMD /C     KEYTEST.BAT   DOSKEY: the command,
REM                                    and the editor typed from a file
REM   GLUETEST.BAT  DIR/W, CD.., CD\DOS and MEM/F: a command run straight
REM                 into its argument, and a switch in front of a name
REM
REM WHAT THE MACHINE HAS.  A boot says a line for a mouse and a line for
REM each CD-ROM drive, and says nothing for one it did not find.  When
REM that is the wrong answer,
REM   \TESTS\HWPROBE
REM prints what the probes read while they were looking: the bytes
REM the keyboard controller and the mouse answered with, and what each
REM of the eight IDE positions said for itself.
REM
REM A CD-ROM DRIVE ON A SOUND CARD needs no driver and no CTCM.  The
REM IDE port of an ISA Plug and Play card is switched on while PM-DOS
REM starts, at 1E8h, or 168h if that is taken, and the drive on it
REM gets a letter like any other.  Nothing else on the card is
REM touched.  HWPROBE says which card it was and where the port went.
REM
REM FORMAT MAKES A DISKETTE, since this release: every track is laid
REM down, so a disk that has never been formatted can be, and /F:720
REM makes a 720K disk in a 1.44M drive.  FORMAT /? has the switches.
REM
REM ECHO, REM, SET, PROMPT, IF, FOR, GOTO, PAUSE, CALL and LOADHIGH
REM answer /? only when it is the WHOLE argument, because their
REM argument is text or another command line - so
REM
REM   ECHO Run SETUP /? for help
REM
REM prints its sentence rather than a help screen, which is what DOS
REM has always done and is why the rule exists.
REM
REM A FAT VOLUME WITH 1024-, 2048- OR 4096-BYTE SECTORS MOUNTS TOO,
REM since v0.60 - TOOLS\MKLSEC.PL makes one to try MOUNT on.
REM
REM MOVE moves files, and WITHIN ONE VOLUME IT MOVES THE DIRECTORY
REM ENTRY and not the bytes - the kernel's 56h is a rename that can
REM change the parent - so a huge file moves in the time it takes to
REM write two sectors.  Between volumes it copies and then deletes,
REM and the delete happens only once the copy is written, stamped and
REM closed; anything that goes wrong first takes the half-written
REM destination away and leaves the source alone.
REM
REM   MOVE A.TXT B.TXT        rename it
REM   MOVE *.TXT \DOCS        move them into a directory
REM   MOVE A.TXT B.TXT \DOCS  two sources, one destination
REM   MOVE OLDDIR NEWDIR      rename a directory
REM   MOVE /Y NEW.TXT OLD.TXT replace without being asked
REM
REM A DIRECTORY CAN BE RENAMED BUT NOT MOVED into another directory or
REM onto another drive: its ".." would be left pointing at the old
REM parent, and copying a tree is XCOPY's job.  An explicit name means
REM what it says, so a named directory or hidden file is moved; a
REM wildcard means the ordinary files.
REM
REM ATTRIB shows the four attribute bits a file wears, and changes
REM them.  Read-only, archive, system and hidden are all of them: the
REM directory bit and the volume bit belong to the filesystem and the
REM kernel puts them back, so there is no +D and no -V.
REM
REM   ATTRIB                  every file here, and what it is
REM   ATTRIB \DOS\*.*         ...somewhere else
REM   ATTRIB +R READ.ME       make it read-only
REM   ATTRIB -H -S IO.SYS     take those two bits off
REM   ATTRIB +A *.TXT /S      and every directory below this one
REM
REM A CHANGE THAT DOES NOT NAME THE HIDDEN OR SYSTEM BIT WILL NOT TOUCH
REM A FILE THAT HAS ONE.  "ATTRIB -R *.*" in a root would otherwise
REM strip the read-only bit off the system files on its way past, so it
REM leaves them alone and says which ones it left; name the bit - "-R
REM -H" - and it is taken as meaning it.  Same rule as MOVE for the
REM rest: an explicit name means what it says, so a named directory is
REM changed, and a wildcard means the ordinary files.  /S counts as a
REM wildcard, or "ATTRIB +R SUB /S" would set the bit on every
REM directory called SUB on the disk.
REM
REM   ATTRTEST.BAT  proves all of that, on MULTI.IMG's volumes
REM
REM CHKDSK checks a FAT12, FAT16 or FAT32 volume and LABEL names one.
REM They are the first commands here written in C, and they read the
REM disk itself rather than asking the kernel about files - which is
REM why a CD-ROM, or an .ISO given a letter by MOUNT, is refused the
REM way DOS refused one, as a drive they cannot check.
REM
REM   CHKDSK                  the current drive: a report, no changes
REM   CHKDSK D: /F            ...and fix it: lost chains become
REM                           FILEnnnn.CHK in the root
REM   CHKDSK /V               every file, as it is checked
REM   CHKDSK \TESTS\*.COM     and whether those files are contiguous
REM   LABEL C:WORK            name C:
REM   LABEL                   show the label, and ask for a new one
REM
REM NOTHING IS WRITTEN WITHOUT /F, and CHKDSK says so when it finds
REM something.  CROSS-LINKED FILES ARE REPORTED AND NOT UNTANGLED, as
REM in DOS - and the first thing CHKDSK finds on this very disk is one:
REM the long-named test files all share a cluster, which TOOLS\MKDISK.C
REM does on purpose because LFNTEST.BAT only ever reads them.
REM
REM FDISK, and it is the one program on this disk that can destroy the
REM machine it is running on, so read this before running it.
REM
REM   FDISK           the screen: the disk as a bar, a partition dragged
REM                   along it with the mouse to move it, sizes typed
REM                   in MB, F to format - and nothing written until W
REM   FDISK /STATUS   what every disk's partition table says
REM   FDISK /L        ...and the logical drives in the extended chain
REM   FDISK /P d:p    one partition: its BPB, what is stored in it, and
REM                   how far it can be shrunk, grown or moved
REM   FDISK /M d:p n  move it so that it starts at sector n
REM   FDISK /S d:p n  resize it to n sectors, or to n MB written 40M
REM   FDISK /C d:p    create a partition in the largest free gap
REM   FDISK /C d:p n tt  ...n sectors of it, and tt as the type byte
REM   FDISK /D d:p    delete one
REM   FDISK /A d:p    make one the active, bootable partition
REM   FDISK /MBR d    put the standard boot code back in disk d's
REM                   master boot record, leaving the table alone
REM
REM THE COMMAND LINE WRITES AT ONCE - there is no /W and no dry run
REM any more, so look with /P first.  THE SCREEN WAITS: each change is
REM staged and drawn on the bar, U takes back the last one, and W lists
REM them and writes them in order.  Quitting after a change restarts
REM the machine, because the drive letters were dealt out at the start
REM and no longer describe the disk.
REM
REM d:p NAMES A DISK AND A PARTITION - 1 to 4 are the primary slots
REM of that disk, and 5 AND UP ARE THE LOGICAL DRIVES in its extended
REM container, in the order FDISK /L lists them.  Every verb but /A
REM takes either kind: a logical drive can be created, deleted, moved
REM and resized like anything else, and a logical drive cannot be the
REM active one because booting looks in the master boot record and
REM there is no chain in it.
REM
REM CREATING OR DELETING A LOGICAL DRIVE EDITS THE CHAIN, which is
REM the one operation here that changes a structure rather than an
REM entry: a logical drive is a boot record of its own, a volume one
REM track behind it, and a pointer to it in the record before.  A
REM delete writes one sector and a create writes two, the new record
REM first so that losing power between them leaves the chain exactly
REM as it was.
REM
REM A RESIZE CAN CHANGE THE CLUSTER SIZE when the new size needs it:
REM FAT12 and FAT16 trade places, and clusters split or merge, with the
REM files renumbered or packed to fit.  FAT32 volumes resize and move
REM too, but never become FAT16 or the other way round - that moves the
REM root directory, which is a conversion and not a resize.  A FAT32
REM volume's backup boot sector and the free-space hint in its FSInfo
REM sector are put right afterwards.
REM
REM MULTI.IMG is the disk to practise on and the second disk in this
REM machine: two primaries, a Linux partition, and an extended chain
REM with two logical drives and a hidden one.  MKHD.CMD builds it
REM afresh every time, so anything done to it is undone by a rebuild.
REM
REM   FDISK /P 1:1              what D: is, and how far it can go
REM   FDISK /S 1:1 40000        shrinking it
REM   FDISK /P 1:5              the first LOGICAL drive on that disk
REM   FDISK /D 1:7              and deleting the last one in the chain
REM
REM What it refuses is the point: a resize between FAT16 and FAT32, or
REM one that would leave data above the new end or run into the next
REM partition, or one on a damaged volume (run CHKDSK first) - or, for
REM a logical drive, a move over its own boot record or outside its
REM container - is refused with the reason and not attempted.  See the
REM release notes.
REM
REM   FDTEST.BAT    create, delete and set-active on the four slots
REM   FDLTEST.BAT   creating and deleting LOGICAL drives
REM   FDRTEST.BAT   moving and resizing one
REM   FD32TEST.BAT  the same on FAT32 - it wants HD32.IMG as the
REM                 second disk, not MULTI.IMG, so it is its own boot
REM   FDMTEST.BAT   the screen, typed at by a key file
