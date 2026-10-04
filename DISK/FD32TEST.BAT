@ECHO OFF
REM ====================================================================
REM FD32TEST.BAT - MOVING AND RESIZING A FAT32 VOLUME.
REM
REM Needs HD32.IMG as the SECOND disk, not MULTI.IMG - so this is its
REM own boot.  MKHD builds it: one primary partition of type 0Bh at
REM sector 63, 81585 sectors, one sector to a cluster, 32 reserved
REM sectors, two FATs of 628, and 80297 clusters.
REM
REM WHY FAT32 COST SO LITTLE IN THE END.  The same-layout argument is
REM about CLUSTER NUMBERS surviving, and FAT32 keeps its root
REM directory in the data area as an ordinary cluster chain - so
REM BPB_ROOTCLUS is a cluster number, and a cluster number that
REM survives needs no adjusting.  There is no root-directory REGION to
REM relocate, which is the thing the earlier releases said was the
REM obstacle; a region of no sectors is a region that does not move.
REM
REM WHAT IS ACTUALLY DIFFERENT, and each of these has a line in the
REM output below or a byte on the disk afterwards:
REM
REM   (a) the sectors-per-FAT count is zero at 16h and real at 24h, so
REM       BOTH fields have to be written - an operating system reads
REM       the sixteen-bit one first and believes it
REM   (b) four bytes to a FAT entry, of which the top four BITS are
REM       reserved, so the scan for the highest cluster in use has to
REM       mask them
REM   (c) there is a BACKUP boot sector, six sectors in, and a resize
REM       that wrote one of the two would leave a spare copy of the
REM       volume's old size - a backup that will be believed
REM   (d) there is a cached free-cluster count in the FSInfo sector
REM       which is now a lie, and the value that means "go and count"
REM       is all ones
REM
REM SINCE 2026-10-02 THE COMMAND LINE WRITES AT ONCE: no /W, no dry
REM run.  A resize may change the cluster size now, but never between
REM FAT16 and FAT32, so the last refusal below still holds.
REM ====================================================================
ECHO --- the FAT32 disk as MKHD left it ---
FDISK /STATUS
FDISK /P 1:1

ECHO --- shrink it to 70000 sectors ---
FDISK /S 1:1 70000
FDISK /STATUS
FDISK /P 1:1

ECHO --- and grow it back to exactly what it was ---
FDISK /S 1:1 81585
FDISK /STATUS
FDISK /P 1:1

ECHO --- a shrink small enough to stop being FAT32 is refused: ---
ECHO --- under 65525 clusters it would have to become FAT16, and ---
ECHO --- that moves the root directory - a conversion, not a resize ---
FDISK /S 1:1 60000
ECHO --- FD32TEST done ---
