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
; has none), a network redirector, SHARE, code pages beyond the one,
; and anything PM-DOS has that DOS 5 did not.  Every one of those is
; surface that Windows will not call and that would have to be kept in
; step with the kernel's own copy for nothing.
;=====================================================================

RMDOS_SIG       EQU     00444D52h               ; 'RMD' as the dword reads in memory
RMDOS_VER       EQU     0101h                   ; 1.01 - this contract.  1.00 had no
                                        ; extended memory in it.

;---------------------------------------------------------------------
; The image.  RMDOS.SYS is a flat binary loaded at paragraph RM_SEG,
; assembled with ORG 0, and every address inside it is relative to its
; own segment - so PM-DOS may put it anywhere a paragraph starts.
;
; The first bytes are a jump over the handoff block, which PM-DOS fills
; IN THE IMAGE before it hands over.  Passing arguments in registers
; would not survive: between the fill and the entry there is a mode
; switch, and the only thing certain to come through it is memory.
;---------------------------------------------------------------------
RMH_JMP         EQU     0000h           ; db 0E9h + word - jmp rm_entry
RMH_BASE        EQU     0003h           ; the handoff block starts here

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
RMH_RSVD        EQU     RMH_BASE + 28h  ; dd  two spare dwords, so the next
                                        ; dd  field to be wanted does not move
                                        ;     the frame below it again

; The register frame RMDOS IRETs to when RMH_RUNSEG is zero.  This is
; how Windows is resumed: WIN386 is already loaded and already running
; when it broadcasts 1605h, so there is nothing to start - the handover
; is a return from an interrupt that happens to land in a different
; processor mode from the one it was taken in.
RMH_IRET        EQU     RMH_BASE + 30h
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

;--- one drive letter, as RMDOS needs it -----------------------------
; PM-DOS scanned the partition tables; RMDOS inherits the answer rather
; than scanning again.  Everything else - cluster size, FAT type, where
; the root is - RMDOS reads out of the volume's own boot sector, so a
; single field is enough to say WHICH volume.
RMD_BIOS        EQU     00h             ; db  BIOS drive number
RMD_FLAGS       EQU     01h             ; db  RMDF_*
RMD_START       EQU     02h             ; dd  absolute LBA of the boot sector
RMD_LEN         EQU     06h

RMDF_PRESENT    EQU     01h
RMDF_FLOPPY     EQU     02h             ; re-read the BPB on every access

;--- RMH_FLAGS -------------------------------------------------------
RMHF_A20        EQU     01h             ; the gate was open when we parked, and
                                        ; has to be open again on the way back
RMHF_VERBOSE    EQU     02h             ; say what is happening - a test aid,
                                        ; and useless once Windows owns the
                                        ; screen

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
