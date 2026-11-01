# PM-DOS
A protected-mode 32bit disk operating system

## Project Goal
A fully backward compatible protected mode DOS with native 32bit
application support.

## Target System Requirements
- 80386 CPU
- 4MB of RAM
- 4MB hard disk space (will vary based on included utils)

## Features
- FAT12, FAT16, FAT32 and exFAT file systems, with long file names
- MBR and GPT partitioned disks; FDISK can move and resize partitions
- IDE disks up to UltraDMA/133, SATA (AHCI) disks, SCSI and CD-ROM drives
- Starts from a hard disk, a diskette or a CD-ROM (El Torito)
- Built-in disk cache, delete protection and ISA Plug and Play
- Disk and CD-ROM images mounted as drives
- USB keyboards, mice and flash drives
- TCP/IP and NetBEUI networking: NET USE drive letters on SMB1/2/3 shares,
  file sharing with SMBSRV, and FTP, TELNET, WGET (with HTTPS), SNTP,
  PING and IPCONFIG
- Runs Windows 3.1 and Windows for Workgroups 3.11 in 386 enhanced mode
- Multiple configurations in CONFIG.SYS
- Native SCANDISK, DEFRAG, DOSSHELL, EDIT, QBASIC, DEBUG, MODE, XARC
  and more
- Online help (HELP) with every command and CONFIG.SYS statement in it

## Building
Written in assembly and C, and built with Open Watcom 1.9 (WASM, WCC386
and WLINK).  Run BUILD.CMD with Open Watcom on the PATH: the programs go
to BIN and the diskette images to IMAGES.  MKHD.CMD then makes the
FAT16 and FAT32 hard disk images in IMAGES.
