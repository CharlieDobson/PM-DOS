;=====================================================================
; RMDOS.INC - the contract between PM-DOS and RMDOS.SYS
;
; Shared by DOS\RMDOS.ASM (the real-mode DOS itself), DOS\PARK.INC
; (the PM-DOS side that loads it and hands over) and IO\IOPARK.INC
; (the mode switch underneath both).
;
;---------------------------------------------------------------------
; WHAT RMDOS IS, AND WHY IT CANNOT BE A SHIM.
;
; Windows in 386 enhanced mode does not run ON a DOS - it REPLACES the
; machine's supervisor and then calls DOS from inside a virtual machine
; it owns.  WIN386.EXE builds its own page tables, its own GDT and IDT,
; and runs from ring 0.  There is exactly one of those seats and PM-DOS
; is in it, which is why v0.44 answers INT 2Fh AX=1605h with a veto.
;
; The only way to let Windows have the seat is to GET OUT OF IT: drop
; paging, leave protected mode, and let WIN386 take the machine.  And
; the moment PM-DOS does that, every INT 21h in the machine has nothing
; to answer it - PM-DOS's entire API is 32-bit protected-mode code and
; there is no INT 21h anywhere in IO\IORM.ASM.
;
; NOR CAN A STUB FORWARD THE CALLS BACK.  While WIN386 owns CR0, CR3,
; the GDT, the IDT and the TSS, there is no way to re-enter PM-DOS's
; protected mode: a V86 program under Windows reaches protected mode
; only through Windows' DPMI, which hands out Windows' descriptors and
; Windows' page tables.  There is no thunk home.  So the thing left
; behind has to IMPLEMENT DOS rather than forward to it, and that is
; what RMDOS is: a second, smaller, 16-bit DOS whose only job is to be
; underneath Windows.
;
;---------------------------------------------------------------------
; WHAT IT INHERITS, WHICH IS MOST OF WHY THIS IS POSSIBLE AT ALL.
;
; A DOS box's memory arena is ALREADY a real-mode MCB chain in real
; conventional memory - PM-DOS's lm_* layer built it there, in DOS's
; own format, because that is what a 16-bit program has to be able to
; walk.  The PSPs in it are real PSPs.  The programs loaded in it are
; at real segment addresses.  Below 1MB the page map is the identity,
; so when paging goes off, every one of those addresses still means
; what it meant.
;
; So RMDOS does not build an arena, or relocate anything, or copy the
; running program anywhere.  It adopts what is there.  What it has to
; supply is the part that lived above 1MB: the handle table, the file
; system, the directory of each volume, and the INT 21h that reaches
; them.
;
; WHAT IT DELIBERATELY DOES NOT SUPPORT: long filenames (Windows 3.x
; has none), SHARE, code pages beyond the one, and anything PM-DOS has
; that DOS 5 did not.  Every one of those is surface that Windows will
; not call and that would have to be kept in step with the kernel's own
; copy for nothing.  A NETWORK REDIRECTOR IS NOT ON THAT LIST ANY MORE
; (2026-10-07): Windows for Workgroups' 32-bit file access takes its
; drives away from DOS through the redirector interface, INT 2Fh 11xxh,
; and RMDOS hands a redirector's drives and files to it as MS-DOS does
; (DOS\RMNET.INC).
;=====================================================================

RMDOS_SIG       EQU     00444D52h               ; 'RMD' as the dword reads in memory
RMDOS_VER       EQU     0104h                   ; 1.04 - this contract.  1.00 had no
                                        ; extended memory in it, 1.01 no
                                        ; RMH_STUBS, 1.02 no RMH_LOWSEG, 1.03
                                        ; no RMH_WINCACHE.  RMDOS
                                        ; checks it: an image built for another
                                        ; layout would read the header wrong,
                                        ; and says so instead

;---------------------------------------------------------------------
; WHERE IT LIVES, AND WHY THAT IS THE HARD PART.
;
; RMDOS has to be in memory a real-mode segment can reach, which means
; the first megabyte - and the first megabyte belongs to the program
; that asked for the handover.  A .COM owns the whole transient
; program area until it says otherwise, and DOSX.EXE does its 4Ah
; AFTER broadcasting 1605h, not before: at the instant it asks
; permission there is not one paragraph free to put a DOS in.  Loading
; RMDOS out of the box's own arena therefore fails on a machine with
; 620K free, which is where v0.49 stopped.
;
; So it goes in the HIGH MEMORY AREA, which is where a real DOS puts
; itself and for the same reason.  100000h to 10FFEFh is the only part
; of extended memory a 16-bit far pointer can address - FFFF:FFFF
; comes out at 10FFEFh and no further - it is real memory the moment
; paging is off, it costs no conventional memory at all, and PM-DOS
; has deliberately left it empty since v0.38 (see PMDOS.INC).
;
; THE PRICE IS THE SIXTEEN BYTES BELOW THE MEGABYTE.  Segment FFFFh
; starts at FFFF0h, which is in the ROM, so the image is assembled at
; ORG 10h and its first byte lands at exactly 100000h.  Everything
; inside it is still addressed as an offset from its own segment, so
; the only thing that changes is that the smallest legal offset is 16
; rather than 0 - and RMH_* below carry RM_ORG, so no field moves
; relative to the segment base and no code that reads them changes.
;
; RM_ORG also makes the LOW path still work, unchanged, from the same
; image: load the image at flat (seg+1)*16 and set the segment to seg,
; and every offset means what it says.  That path is the fallback and
; it is what the PARKWIN and RMTEST tests exercise from a prompt,
; where there IS free memory.
;
; THE ONE RULE THIS CREATES: NOTHING RUNNING FROM UP HERE MAY EVER
; CLOSE THE A20 GATE.  With A20 off, segment FFFFh aliases to zero and
; the instruction after the one that shut it is fetched from the
; interrupt vector table.  RMXMS.INC's gate is chocked off when
; RMHF_HIGH is set, which is the whole of the defence.
;---------------------------------------------------------------------
RM_ORG          EQU     0010h           ; where the image begins inside its
                                        ; own segment: 16, so that segment
                                        ; FFFFh puts byte zero on the megabyte
RM_HMA_SEG      EQU     0FFFFh          ; the segment that reaches it
RM_HMA_TOP      EQU     0FFFEh          ; and the highest word-aligned offset
                                        ; in it - SP starts here

;---------------------------------------------------------------------
; The image.  RMDOS.SYS is a flat binary assembled with ORG RM_ORG,
; and every address inside it is relative to its own segment - so
; PM-DOS may put it at any paragraph whose 16th byte it can write.
;
; The first bytes are a jump over the handoff block, which PM-DOS fills
; IN THE IMAGE before it hands over.  Passing arguments in registers
; would not survive: between the fill and the entry there is a mode
; switch, and the only thing certain to come through it is memory.
;---------------------------------------------------------------------
RMH_JMP         EQU     RM_ORG + 0000h  ; db 0E9h + word - jmp rm_entry
RMH_BASE        EQU     RM_ORG + 0003h  ; the handoff block starts here

RMH_SIG         EQU     RMH_BASE + 00h  ; dd  RMDOS_SIG, checked by both sides
RMH_VER         EQU     RMH_BASE + 04h  ; dw  RMDOS_VER
RMH_UNPARKSEG   EQU     RMH_BASE + 06h  ; dw  where to FAR JMP to come back
RMH_UNPARKOFF   EQU     RMH_BASE + 08h  ; dw
RMH_MCBHEAD     EQU     RMH_BASE + 0Ah  ; dw  first MCB of the arena to adopt
RMH_PSP         EQU     RMH_BASE + 0Ch  ; dw  the process that is running
RMH_BOOTDRV     EQU     RMH_BASE + 0Eh  ; db  0 = A
RMH_CURDRV      EQU     RMH_BASE + 0Fh  ; db  0 = A
RMH_NDRV        EQU     RMH_BASE + 10h  ; db  how many letters follow
RMH_FLAGS       EQU     RMH_BASE + 11h  ; db  RMHF_*
RMH_RUNSEG      EQU     RMH_BASE + 12h  ; dw  a program to start, or 0000 to
RMH_RUNOFF      EQU     RMH_BASE + 14h  ; dw  IRET to the frame at RMH_IRET
RMH_RUNSP       EQU     RMH_BASE + 16h  ; dw  its initial SS:SP
RMH_RUNSS       EQU     RMH_BASE + 18h  ; dw
RMH_MEMTOP      EQU     RMH_BASE + 1Ah  ; dw  paragraph after conventional mem
RMH_EXITCODE    EQU     RMH_BASE + 1Ch  ; dw  filled in on the way back
RMH_WHY         EQU     RMH_BASE + 1Eh  ; dw  RMW_* - what ended the excursion

;---------------------------------------------------------------------
; The extended memory RMDOS may hand out as XMS.
;
; PM-DOS owns every byte above the megabyte.  Some of it is the kernel
; itself, some of it is arena blocks with live data in them, and the
; excursion has to come back to all of it intact - so RMDOS cannot be
; told "extended memory starts at 100000h" the way a real HIMEM works
; it out.  It is GIVEN a range: PM-DOS allocates the arena's free
; memory to itself before parking and hands over the block, and gets
; it back on the way up.  Nothing outside it is RMDOS's to give away,
; and Windows never sees an address the kernel is standing on.
;
; The base is page-aligned and the length is a whole number of KB,
; because XMS counts in KB and Windows wants pages.  Zero length is
; legal and means the driver reports itself present with nothing to
; hand out, which is what a machine with no free arena looks like.
;---------------------------------------------------------------------
RMH_XMSBASE     EQU     RMH_BASE + 20h  ; dd  flat = physical, page-aligned
RMH_XMSLEN      EQU     RMH_BASE + 24h  ; dd  bytes, a multiple of 1024
;---------------------------------------------------------------------
; THE DOS DATA RMDOS PUBLISHES, AND WHERE WINDOWS INSISTS IT BE.
;
; A DOS has a data segment at the bottom of memory, and Windows in 386
; enhanced mode KNOWS that.  DOSMGR's real-mode initialisation asks
; INT 21h 52h for the list of lists, turns ES:BX into a linear address
; and refuses to load - "Cannot use a device specified in the
; SYSTEM.INI file" - if that address has a bit set above 64K, because
; it is about to pack it into sixteen bits.  RMDOS's list of lists was
; inside its own image, which first-fit puts wherever the parking
; program left room, 124K up on the day this was found.
;
; So PM-DOS hands over a block BELOW 64K - its own DOS data segment at
; DOSDATA_SEG, which is where the boxes already read the kernel's list
; of lists and SDA - and RMDOS builds everything it publishes in there:
; the list of lists, the swappable data area, the current directory
; structures, and the table DOSMGR's callouts are answered with.  The
; block's contents are PM-DOS's and are wanted back: RMDOS copies the
; whole of it into its image on the way in and puts it back on the way
; home, so the kernel finds its data exactly as it left it.
;---------------------------------------------------------------------
RMH_LOWSEG      EQU     RMH_BASE + 28h  ; dw  a block below 64K, as a segment
RMH_LOWLEN      EQU     RMH_BASE + 2Ah  ; dw  its length in bytes: at least
                                        ;     RL_LEN, at most RL_SAVEMAX
RMH_RSVD        EQU     RMH_BASE + 2Ch  ; dd  one spare dword - the DBG_LPT
                                        ;     build's ownership probe
RMH_STUBS       EQU     RMH_BASE + 30h  ; dd  PM-DOS's DOS vector stubs, far.
RMS_STUBLEN     EQU     5               ; bytes a vector, v at (v - 20h) * 5.
                                        ; PMDOS.INC's DVS_LEN, which RMDOS.ASM
                                        ; cannot see - PARK.INC checks they agree

;---------------------------------------------------------------------
; The sector cache RMDOS keeps for as long as it is the DOS - which is
; for as long as Windows runs, so this is the SMARTDRV of this kernel.
; PM-DOS's own cache gave its memory back before the park (cache_park)
; and is not there to be asked; RMDOS builds one of its own out of the
; XMS pool above, and answers SMARTDRV's INT 2Fh 4A10h for it.  See
; DOS\RMCACHE.INC.  WINCACHE= in CONFIG.SYS sets the size; absent, the
; kernel derives one from the pool, and 0 means none.
;---------------------------------------------------------------------
RMH_WINCACHE    EQU     RMH_BASE + 34h  ; dw  kilobytes of cache, 0 for none
RMH_WCFLAGS     EQU     RMH_BASE + 36h  ; dw  RMWC_*
RMWC_LOG        EQU     0001h           ; write RMCACHE.LOG when Windows exits

; The register frame RMDOS IRETs to when RMH_RUNSEG is zero.  This is
; how Windows is resumed: WIN386 is already loaded and already running
; when it broadcasts 1605h, so there is nothing to start - the handover
; is a return from an interrupt that happens to land in a different
; processor mode from the one it was taken in.
RMH_IRET        EQU     RMH_BASE + 38h
RMI_EAX         EQU     RMH_IRET + 00h  ; dd
RMI_EBX         EQU     RMH_IRET + 04h
RMI_ECX         EQU     RMH_IRET + 08h
RMI_EDX         EQU     RMH_IRET + 0Ch
RMI_ESI         EQU     RMH_IRET + 10h
RMI_EDI         EQU     RMH_IRET + 14h
RMI_EBP         EQU     RMH_IRET + 18h
RMI_ESP         EQU     RMH_IRET + 1Ch
RMI_CS          EQU     RMH_IRET + 20h  ; dw
RMI_IP          EQU     RMH_IRET + 22h
RMI_SS          EQU     RMH_IRET + 24h
RMI_DS          EQU     RMH_IRET + 26h
RMI_ES          EQU     RMH_IRET + 28h
RMI_FS          EQU     RMH_IRET + 2Ah
RMI_GS          EQU     RMH_IRET + 2Ch
RMI_FLAGS       EQU     RMH_IRET + 2Eh
RMI_LEN         EQU     30h

RMH_DRIVES      EQU     RMH_IRET + RMI_LEN      ; RMD_LEN each, RMH_NDRV of them
RMH_DRVROWS     EQU     16                      ; ...and room for this many, the
                                                ; kernel's LASTDRIVE: PARK.INC
                                                ; writes up to that, whatever
                                                ; RMDOS reads.  RMH_LASTDRV is
                                                ; after them, below RMD_LEN

;--- one drive letter, as RMDOS needs it -----------------------------
; PM-DOS scanned the partition tables; RMDOS inherits the answer rather
; than scanning again.  Everything else - cluster size, FAT type, where
; the root is - RMDOS reads out of the volume's own boot sector, so a
; single field is enough to say WHICH volume.
RMD_BIOS        EQU     00h             ; db  BIOS drive number
RMD_FLAGS       EQU     01h             ; db  RMDF_*
RMD_START       EQU     02h             ; dd  absolute LBA of the boot sector
RMD_LEN         EQU     06h

RMH_LASTDRV     EQU     RMH_DRIVES + RMH_DRVROWS * RMD_LEN
                                        ; db  LASTDRIVE= from CONFIG.SYS as a
                                        ; count of letters (Z: is 26), or 0
                                        ; for no line.  The kernel stops at
                                        ; its own sixteen; RMDOS goes to Z:
RMH_CDSEG       EQU     RMH_LASTDRV + 1 ; dw  RMCD.SYS's segment, or 0: the
                                        ; CD-ROM under Windows, loaded by the
                                        ; park after RMDOS's 64K (RMCD.ASM)
IF RMH_CDSEG + 2 GT RM_ORG + 100h
        .ERR    <RMDOS.INC: the handoff block runs into RMDOS's code>
ENDIF

;--- RMCD.SYS's own header: the park fills it in, RMDOS reads it -------
RC_SIG          EQU     'DCMR'          ; "RMCD", as a dword reads it
RCH_SIG         EQU     00h             ; dd  RC_SIG
RCH_VER         EQU     04h             ; dw
RCH_INIT        EQU     06h             ; dw  cd_init's offset: FAR called
                                        ; with ES = the block below 64K
RCH_LETTER      EQU     08h             ; db  the CD's letter, 0 = A
RCH_SEL         EQU     09h             ; db  00h the master, 10h the slave
RCH_BASE        EQU     0Ah             ; dw  the command block's ports
RCH_CTRL        EQU     0Ch             ; dw  the control port

RMDF_PRESENT    EQU     01h
RMDF_FLOPPY     EQU     02h             ; re-read the BPB on every access
RMDF_DRIVER     EQU     04h             ; a unit of a block driver CONFIG.SYS
                                        ; loaded: RMD_START is its header,
                                        ; far, and the unit is in the top
RMDF_UNITSHIFT  EQU     4               ; four bits of the flags.  RMD_BIOS
                                        ; is the kernel's pseudo number for
                                        ; it (DL_UNIT_BASE and up), which no
                                        ; disk has.  Since 2026-10-06

;--- RMH_FLAGS -------------------------------------------------------
RMHF_A20        EQU     01h             ; the gate was open when we parked, and
                                        ; has to be open again on the way back
RMHF_VERBOSE    EQU     02h             ; say what is happening - a test aid,
                                        ; and useless once Windows owns the
                                        ; screen
RMHF_HIGH       EQU     04h             ; THIS IMAGE IS IN THE HMA.  The gate
                                        ; that makes the HMA exist is the gate
                                        ; RMXMS hands out, so when this is set
                                        ; the gate is nailed open and every
                                        ; request to close it is answered
                                        ; "done" without being obeyed.  See
                                        ; the note above RM_ORG.

;--- RMH_WHY: why the excursion ended --------------------------------
RMW_EXIT        EQU     0               ; the program called 4Ch, or Windows
                                        ; came back from its own INT 2Fh 1606h
RMW_FAULT       EQU     1               ; RMDOS could not continue
RMW_NOTOURS     EQU     2               ; never parked - the image was wrong

;---------------------------------------------------------------------
; How big a block PM-DOS has to find for it.  The image is padded to
; RMDOS_CODE and the rest is RMDOS's own working storage - buffers, the
; handle table, the per-drive state - which is not in the file because
; it is all zero and a DOS box has the paragraphs to spare more easily
; than a floppy has the sectors.
;---------------------------------------------------------------------
RMDOS_CODE      EQU     0A000h          ; 40K reserved for code + image data
RMDOS_WORK      EQU     04000h          ; 16K of working storage after it
RMDOS_TOTAL     EQU     RMDOS_CODE + RMDOS_WORK
RMDOS_PARAS     EQU     RMDOS_TOTAL / 16
PARK_RMPARAS    EQU     1000h           ; what park_loadrm actually takes: one
                                        ; whole segment, so that park_seg does
                                        ; not move with the size of the image
                                        ; (PARK.INC says why) - and what
                                        ; vl_winroom keeps free for it
PARK_CDPARAS    EQU     200h            ; ...and 8K more after it when the
                                        ; kernel has a CD-ROM on an IDE
                                        ; channel: RMCD.SYS's room, in the
                                        ; same block, below WIN386 with RMDOS
                                        ; and global to every machine as
                                        ; MSCDEX was (PARK.INC, park_loadcd)


;=====================================================================
; What RMDOS.ASM needs and PMDOS.INC would otherwise have to supply.
;
; IT DELIBERATELY DOES NOT INCLUDE PMDOS.INC.  RMDOS is a separate
; program that happens to be built in the same tree, and the kernel's
; header describes the kernel's own structures - its SFT, its device
; numbering, its memory map.  Including it brought in a dozen names
; that mean something different on this side of the handover, and
; wasm folds case, so the first sign of that was twenty "label is
; already defined" errors and not a wrong answer at run time.  Which
; was luck: SFT_SIZE means "how big the table is" in one file and
; "how big the file is" in the other, and both are valid offsets.
;
; Guarded, because PM-DOS's own side of the park includes this header
; AND PMDOS.INC, and there these are already defined.
;=====================================================================
IFNDEF LASTDRIVE
LASTDRIVE       EQU     8               ; A: to H:, as the kernel has it
ENDIF

;---------------------------------------------------------------------
; The layout of the block RMH_LOWSEG names, offsets from its start.
;
; IT IS MS-DOS 5'S OWN LAYOUT WHEREVER ANYBODY LOOKS (2026-10-06).  The
; segment INT 21h 52h answers with is the DOS data segment, and some of
; Windows reads that segment at fixed offsets rather than through the
; pointers: Windows for Workgroups 3.11's VSHARE.386 reads the byte at
; offset 4 - DataVersion, 1 in DOS 5 and 6 - and will not load unless it
; is 0 or 1, and then patches SHARE's jump table at 90h, sets the SHARE
; flag at 303h and reads the list of lists at 26h and the FCB table it
; names (DOS6SRC dos/msconst.asm and inc/const2.asm lay all of them out;
; MS-DOS 5's MSDOS.SYS agrees).  Until then the list of lists was at 0
; and the CDS array covered 90h and 303h, so VSHARE read FFh at offset 4
; and refused to load.  RMDOS calls none of the jump table and reads
; neither flag; they are here to be found where they are looked for.
;
; The list of lists starts at 24h so that 52h's ES:BX-2 is the MCB head
; word, as in DOS.  RL_DEVRET is a RETF: the NUL device header's
; strategy and interrupt entries have to be in the header's own
; segment.  The SDA at 320h puts InDOS at 321h, where DOS 5 has it.
;
; AND IT IS THE KERNEL'S LAYOUT TOO (2026-10-06) - PMDOS.INC's DOSDATA
; has the same offsets under its own names, and PARK.INC checks every
; one of them against these.  The reason is the drivers CONFIG.SYS
; loads.  One asks 34h and 52h once, at its INIT, on the kernel, and
; keeps what it was told; it is still resident when this DOS takes
; over the segment, and it still reads InDOS, the SDA and the device
; chain at the addresses the kernel gave it.  So those have to mean
; the same thing on both sides of the handover, and so do the three
; device headers, which a loaded driver's own header links to: RMDOS's
; CON, AUX and PRN are at the kernel's addresses, and the kernel's
; chain - NUL, the drivers, CON - becomes this DOS's chain unchanged
; (rd_sysinit).
;
; What the kernel keeps in the segment and RMDOS leaves alone: the
; tail of the list of lists (5Ah-6Ch, the boot drive and the rest),
; UMB_HEAD at 8Ch, and the country tables at RL_NLS.  The kernel's file
; table's first block is where DOS 5's SDA goes on, and RMDOS clears
; it; the kernel's current directories are where RMDOS builds its own.
; Both are put back with the rest on the way home.
;---------------------------------------------------------------------
RL_DATAVER      EQU     0004h           ; dw  1 - DOS 5's DataVersion
RL_LOL          EQU     0024h           ; the MCB head word, then LL_PTR at +2
RL_LOLTAIL      EQU     005Ah           ; the list's tail, the kernel's -
RL_LOLEND       EQU     006Dh           ; up to here
RL_UMBWORD      EQU     008Ch           ; dw  DOS 5's UMB_HEAD, the kernel's
RL_JSHARE       EQU     0090h           ; SHARE's jump table, 15 far pointers,
RL_JSHARECNT    EQU     15              ; each at RL_DEVRET until VSHARE puts
                                        ; its own there (and back at exit)
RL_DEVRET       EQU     00D0h           ; db  0CBh
                                        ; D4h-D9h: unused.  The FCB table was
                                        ; here, a header with no room for an
                                        ; entry; it is rd_fcbtab (RMFILE.INC)
RL_DEVS         EQU     00DAh           ; CON, AUX and PRN device headers,
RL_DEVLEN       EQU     12h             ; chained behind NUL, 18 bytes each
RL_DEVEND       EQU     RL_DEVS + 3 * RL_DEVLEN
RL_DOSVARS      EQU     0110h           ; version, then six offsets - DOS 5's
                                        ; Win386_DOSVars (DOS6SRC dostab.asm)
RL_SAVEDS       EQU     0120h           ; dw
RL_SAVEBX       EQU     0122h           ; dw
RL_USERID       EQU     0124h           ; dw - the one DOSMGR really uses
RL_CRITPATCH    EQU     0126h           ; dw
RL_UMBHEAD      EQU     0128h           ; dw
RL_NLS          EQU     0130h           ; the kernel's country tables, left
RL_NLSEND       EQU     02D0h           ; alone; 65h's tables point into them
RL_NLSUP        EQU     RL_NLS          ; 6502h and 6504h: the upper case
RL_NLSTERM      EQU     RL_NLS + 82h    ; 6505h: what ends a file name
RL_NLSCOLL      EQU     RL_NLS + 9Ah    ; 6506h: the collating sequence
RL_NLSDBCS      EQU     RL_NLS + 19Ch   ; 6507h: double-byte ranges (none)
RL_FSHARE       EQU     0303h           ; db  SHARE's "installed" flag
RL_SDA          EQU     0320h           ; the swappable data area: DOS 5's,
RL_SDALEN       EQU     02E0h           ; to the CDS array.  The first SD_LEN
                                        ; bytes are what 5D06h says to swap;
                                        ; the rest is what MS-DOS fills in for
                                        ; a network redirector - the name, the
                                        ; file table entry, the CDS - and what
                                        ; IFSHLP.SYS reads at DOS 5's offsets
                                        ; (RMINFO.INC, SD_*; RMNET.INC)
RL_CDS          EQU     0600h           ; LASTDRIVE current directory
RL_CDSLEN       EQU     58h             ; structures, each MS-DOS 4+'s size -
                                        ; DOSMGR instances this many a drive.
                                        ; Past 5F8h, the last SDA offset
                                        ; VSHARE's SHARE entries would read
RL_NULDEV       EQU     RL_LOL + 24h    ; the NUL device header inside the
                                        ; list of lists (RMINFO's LL_NUL)
RL_NDRV         EQU     8               ; the letters RMDOS has VOLUMES for,
                                        ; whoever is reading this: the kernel
                                        ; has more (PMDOS.INC), and RMDOS's
                                        ; parameter blocks are laid out by its
                                        ; own count (RMDOS.ASM checks the two
                                        ; agree)
RL_NCDS         EQU     26              ; ...and the most CURRENT DIRECTORY
                                        ; STRUCTURES, A: to Z:.  How many are in
                                        ; use is LASTDRIVE= (rd_ncds), and a
                                        ; letter past the volumes is there for
                                        ; a redirector to take: Windows for
                                        ; Workgroups maps a network drive by
                                        ; writing a free structure, and with
                                        ; eight of them there was nowhere for
                                        ; I: to Z: to go (2026-10-09)
RL_DPB          EQU     RL_CDS + RL_NCDS * RL_CDSLEN
RL_DPBLEN       EQU     21h             ; RL_NDRV drive parameter blocks,
                                        ; MS-DOS 4's shape, chained from the
                                        ; list of lists (RMDISK.INC, dpb_fill)
                                        ; - where the kernel keeps the rest of
                                        ; its current directories, which come
                                        ; back with everything else
RL_LEN          EQU     RL_DPB + RL_NDRV * RL_DPBLEN
RD_KEEPLEN      EQU     RL_CDS + RL_NDRV * RL_CDSLEN
                                        ; what RMDOS copies out of the block
                                        ; before it builds there (rd_lowsave):
                                        ; the kernel's CDS for the volumes
                                        ; RMDOS has, and NUL's link.  The
                                        ; kernel keeps all of it (park_lowkeep)
                                        ; the end of what RMDOS builds in
RL_SAVEMAX      EQU     1000h           ; the most RMDOS keeps and puts back:
                                        ; past its own twenty-six structures
                                        ; and the blocks after them, and so
                                        ; past the kernel's sixteen and the
                                        ; first of its file table blocks,
                                        ; which RMDOS writes over and owes

IF RL_NULDEV + RL_DEVLEN GT RL_LOLTAIL
        .ERR    <RMDOS.INC: the NUL header runs into the list's tail>
ENDIF
IF RL_JSHARE + RL_JSHARECNT * 4 GT RL_DEVRET
        .ERR    <RMDOS.INC: SHARE's jump table runs into the RETF>
ENDIF
IF RL_DEVEND GT RL_DOSVARS
        .ERR    <RMDOS.INC: the device headers run into the DOSMGR table>
ENDIF
IF RL_UMBHEAD + 2 GT RL_NLS
        .ERR    <RMDOS.INC: the DOSMGR table runs into the country tables>
ENDIF
IF RL_NLSEND GT RL_FSHARE
        .ERR    <RMDOS.INC: the country tables run into SHARE's flag>
ENDIF
IF RL_SDA + RL_SDALEN GT RL_CDS
        .ERR    <RMDOS.INC: the SDA runs into the CDS array>
ENDIF
IF RL_LEN GT RL_SAVEMAX
        .ERR    <RMDOS.INC: RMDOS builds past what it keeps>
ENDIF

IFNDEF DOSVER_MAJOR
DOSVER_MAJOR    EQU     6
DOSVER_MINOR    EQU     22
ENDIF

IFNDEF ATTR_READONLY
ATTR_READONLY   EQU     01h
ATTR_HIDDEN     EQU     02h
ATTR_SYSTEM     EQU     04h
ATTR_VOLUME     EQU     08h
ATTR_DIR        EQU     10h
ATTR_ARCHIVE    EQU     20h
ENDIF

IFNDEF ERR_FUNC
ERR_FUNC        EQU     1
ERR_NOFILE      EQU     2
ERR_NOPATH      EQU     3
ERR_NOHANDLES   EQU     4
ERR_ACCESS      EQU     5
ERR_BADHANDLE   EQU     6
ERR_BADMCB      EQU     7
ERR_NOMEM       EQU     8
ERR_BADBLOCK    EQU     9
ERR_EXEFMT      EQU     11
ERR_BADDRIVE    EQU     15
ERR_EXISTS      EQU     80
ENDIF

; The real-mode memory control block, which is the shape PM-DOS's own
; low-memory layer already wrote into conventional memory and which
; RMDOS adopts rather than rebuilds.
IFNDEF RMCB_SIG
RMCB_SIG        EQU     00h             ; byte 'M', or 'Z' if last
RMCB_OWN        EQU     01h             ; word PSP segment, 0 = free
RMCB_LEN        EQU     03h             ; word paragraphs, excluding this
RMCB_NAME       EQU     08h             ; 8 bytes
RMCB_SIZE       EQU     10h
ENDIF
