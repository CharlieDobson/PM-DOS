@echo off
rem ===================================================================
rem MKHD.CMD - build the bootable FAT16 and FAT32 hard-disk images
rem
rem   BIN\HD16.IMG   32MB FAT16, bootable
rem   BIN\HD32.IMG   40MB FAT32, bootable
rem
rem Both get the MBR, a stage-1 volume boot record, stage 2 in the
rem reserved sectors, and the system files.  Pass FRAG to scatter
rem every file across alternate clusters - a loader that reads
rem straight through instead of walking the FAT will not survive it:
rem
rem   MKHD          normal images
rem   MKHD frag     deliberately fragmented images
rem
rem Kept out of BUILD.CMD because they are 32MB and 40MB of test
rem artifact, not part of the shipping floppy.  Mount them with:
rem
rem   imgmount 2 BIN\HD16.IMG -size 512,63,16,65 -t hdd -fs none
rem   imgmount 3 BIN\HD32.IMG -size 512,63,16,81 -t hdd -fs none
rem
rem then "boot -l c" for the FAT16 disk, or swap the mount indexes to
rem boot the FAT32 one; "boot -l a" with the floppy leaves both as
rem plain C: and D: volumes to chew on.
rem ===================================================================
cd /d %~dp0
set FRAG=
if /i "%1"=="frag" set FRAG=-frag
if not exist TOOLS\MKDISK.EXE ( echo Run BUILD.CMD first & exit /b 1 )
if not exist BIN\BOOT2.BIN    ( echo Run BUILD.CMD first & exit /b 1 )

TOOLS\MKDISK.EXE -hd16 32 BIN\HD16.IMG %FRAG% -mbr BIN\MBRBOOT.BIN ^
    -vbr BIN\BOOT16.BIN -s2 BIN\BOOT2.BIN ^
    BIN\PMIO.SYS BIN\PMDOS.SYS BIN\COMMAND.COM BIN\CHOICE.COM BIN\LFNAPI.COM BIN\MORE.COM BIN\SORT.COM BIN\SYS.COM BIN\FORMAT.COM BIN\MEM.EXE BIN\DRIVES.EXE BIN\HELLO.COM BIN\HELLOX.EXE BIN\ARGS.COM BIN\XMSTEST.COM BIN\A20TEST.COM BIN\EXECTST.COM BIN\TSRTEST.COM BIN\I13TEST.COM BIN\CURTEST.COM BIN\DPMITEST.COM BIN\DEVTEST.COM BIN\APITEST.COM BIN\IDLETEST.COM BIN\CRASH.EXE README.TXT ^
    DISK\CONFIG.SYS DISK\AUTOEXEC.BAT DISK\BATTEST.BAT DISK\SUB.BAT DISK\CHTEST.BAT ^
    DISK\GOTOTEST.BAT DISK\LOOPTEST.BAT DISK\REDTEST.BAT DISK\LFNTEST.BAT ^
    DISK\CMDTEST.CMD DISK\SUBQ.CMD DISK\BATQ.BAT DISK\CTRLC.BAT ^
    DISK\CHAIN1.BAT DISK\CHAIN2.BAT DISK\NOLABEL.BAT DISK\SHIFT12.BAT DISK\VDMTEST.BAT DISK\NESTTEST.BAT DISK\DISKTEST.BAT DISK\NULTEST.BAT DISK\CRASHTST.BAT DISK\APITEST.BAT || exit /b 1
TOOLS\MKDISK.EXE -hd32 40 BIN\HD32.IMG %FRAG% -mbr BIN\MBRBOOT.BIN ^
    -vbr BIN\BOOT32.BIN -s2 BIN\BOOT2.BIN ^
    BIN\PMIO.SYS BIN\PMDOS.SYS BIN\COMMAND.COM BIN\CHOICE.COM BIN\LFNAPI.COM BIN\MORE.COM BIN\SORT.COM BIN\SYS.COM BIN\FORMAT.COM BIN\MEM.EXE BIN\DRIVES.EXE BIN\HELLO.COM BIN\HELLOX.EXE BIN\ARGS.COM BIN\XMSTEST.COM BIN\A20TEST.COM BIN\EXECTST.COM BIN\TSRTEST.COM BIN\I13TEST.COM BIN\CURTEST.COM BIN\DPMITEST.COM BIN\DEVTEST.COM BIN\APITEST.COM BIN\IDLETEST.COM BIN\CRASH.EXE README.TXT ^
    DISK\CONFIG.SYS DISK\AUTOEXEC.BAT DISK\BATTEST.BAT DISK\SUB.BAT DISK\CHTEST.BAT ^
    DISK\GOTOTEST.BAT DISK\LOOPTEST.BAT DISK\REDTEST.BAT DISK\LFNTEST.BAT ^
    DISK\CMDTEST.CMD DISK\SUBQ.CMD DISK\BATQ.BAT DISK\CTRLC.BAT ^
    DISK\CHAIN1.BAT DISK\CHAIN2.BAT DISK\NOLABEL.BAT DISK\SHIFT12.BAT DISK\VDMTEST.BAT DISK\NESTTEST.BAT DISK\DISKTEST.BAT DISK\NULTEST.BAT DISK\CRASHTST.BAT DISK\APITEST.BAT || exit /b 1
echo HD16.IMG and HD32.IMG ready (both bootable). %FRAG%




rem A multi-partition test disk: two primaries, a non-FAT partition,
rem and an extended chain with two logical drives and a hidden one.
rem It is the only image that exercises the extended-partition walk.
wcl386 -q -bt=nt -fe=TOOLS\mkmulti.exe TOOLS\mkmulti.c || exit /b 1
TOOLS\mkmulti.exe BIN\MULTI.IMG -mbr BIN\MBRBOOT.BIN || exit /b 1
