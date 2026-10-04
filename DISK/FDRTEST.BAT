@ECHO OFF
REM ====================================================================
REM FDRTEST.BAT - /P, /M and /S on a LOGICAL DRIVE.
REM
REM The same engine that moves and resizes a primary partition, handed
REM its argument the other way round: a logical drive's entry lives in
REM its own boot record and its start is measured from that record and
REM not from the disk.  If the planner and the executor have really
REM been made to work through one pointer and one base, then every
REM number below comes out the same as it would for a primary.
REM
REM Needs MULTI.IMG as the SECOND disk.  IT PUTS EVERYTHING BACK: each
REM change is followed by the change that undoes it, so FDLTEST can
REM run after this one in the same boot and still find the chain that
REM MKMULTI built.  The report either side of each pair is the proof.
REM
REM   5  04h FAT16   record 91791, volume 91854 + 32256
REM   6  06h FAT16B  record 124110, volume 124173 + 32256
REM   7  16h hidden  record 156429, volume 156492 + 8064
REM
REM ONE THING DOES NOT COME BACK: THE TYPE BYTE.  6 starts out 06h,
REM which is what FAT16 is called at 32MB and over, and a volume that
REM has been down to 20000 sectors and back is described by 04h - the
REM spelling for one under 32MB.  That is rl_typebyte doing its job:
REM the type byte is worked out from the shape the volume HAS, not
REM remembered from the shape it used to have, and the two spellings
REM mean the same filesystem.  It is worth knowing before reading the
REM report and thinking something was lost.
REM
REM WHAT EACH ONE IS FOR:
REM
REM   (a) 5 is the HEAD of the chain.  Nothing points at it except the
REM       container's own entry in the master boot record, which is
REM       not this program's to adjust - so a resize of 5 writes one
REM       sector and only one.
REM   (b) 6 is NOT the head, so the record in front of it carries the
REM       length of 6's whole link and that length has to be put right
REM       as well.  Nothing walks a chain by that field, which is why
REM       it is the field that rots quietly if nobody writes it.
REM   (c) a move that would put the volume on top of its OWN boot
REM       record must be refused, and a move outside the container
REM       must be refused for a different reason.
REM
REM SINCE 2026-10-02 THE COMMAND LINE WRITES AT ONCE: no /W, no dry
REM run, and no "proposed:" line before a move or a resize.
REM ====================================================================
ECHO --- the chain as MKMULTI left it ---
FDISK /L

ECHO --- (a) what the head of the chain is, and how far it goes ---
FDISK /P 1:5

ECHO --- shrink it to 20000 sectors ---
FDISK /S 1:5 20000
FDISK /L

ECHO --- and grow it back to exactly what it was ---
FDISK /S 1:5 32256
FDISK /L

ECHO --- (b) the same on 6, which has a record in front of it ---
FDISK /P 1:6
FDISK /S 1:6 20000
FDISK /L
FDISK /M 1:6 130000
FDISK /L
ECHO --- put it back: move first, then the size ---
FDISK /M 1:6 124173
FDISK /S 1:6 32256
FDISK /L

ECHO --- (c) the two refusals a logical drive has and a primary ---
ECHO --- does not ---
ECHO (1) on top of its own boot record
FDISK /M 1:5 91791
ECHO (2) outside the container altogether
FDISK /M 1:5 200000
ECHO (3) and one it shares with a primary: into the neighbour
FDISK /S 1:5 40000

ECHO --- the chain must now be exactly what it was at the top ---
FDISK /L
ECHO --- FDRTEST done ---
