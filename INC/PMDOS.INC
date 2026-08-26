;=====================================================================
; PMDOS.INC - Global constants for the PM-DOS 32-bit kernel
;
; Shared by BOOT.ASM, IORM.ASM, IOPM.ASM and PMDOS.ASM.
; Assembles with Open Watcom WASM 1.9 and MASM 6.1x.
;=====================================================================

;---------------------------------------------------------------------
; Physical / linear memory map
;
;   00000000-000004FF   Real-mode IVT and BIOS data area
;   000004E8-000004EB   RMSAVE_ESP - the caller's 32-bit ESP
;   000004F0-000004F5   RMRET    - 16:32 far pointer, thunk return address
;   000004F6-000004F7   DOS_ERRFLG / DOS_INDOS - the two DOS status flags
;   00000500-0000052B   RMFRAME  - register frame for the PM->RM thunk
;   00000530-00000FFF   Real-mode stack (SS=0000, SP=1000) - 2.7K
;   00001000-00001FFF   PMIO.SYS real-mode stub (4K, fixed size)
;   00002000-00003FFF   XFER     - transfer buffer for BIOS disk I/O
;   00004000-000040FF   DOS data segment - the list of lists and the
;                       device headers, BELOW the arena as in real DOS
;   00004100-0009FFFF   the DOS box's transient program area, ending
;                       where the BIOS says conventional memory does
;   000A0000-000FFFFF   Video RAM and ROM
;   00100000-0013FFFF   PMDOS.SYS (256K reserved)
;   00140000-0017FFFF   shell slot: PSP at 140000, image at 141000,
;                       stack grows down from 180000 (COMMAND.COM)
;   00180000-001FFFFF   program slot: PSP at 180000, image at 181000,
;                       stack grows down from 200000 (EXEC'd programs)
;   00200000-0020FFFF   PMIO.SYS protected-mode body (64K reserved)
;   00210000-002107FF   IDT (256 gates)
;   00212000-00213FFF   Kernel stack (top = 00214000)
;   00214000-0021606B   TSS and its I/O permission bitmap
;   00220000-....       DOS arena (allocated by INT 21h/48h)
;
; WHY THE FIRST MEGABYTE LOOKS LIKE THIS.  Everything that does not
; have to be below 1MB has been moved above it, because every byte
; down here is a byte a DOS program cannot have.  What is left is what
; genuinely cannot move: the IVT and BIOS data, a real-mode stack, the
; buffer the BIOS transfers disk sectors through (INT 13h cannot write
; above 1MB), and the real-mode half of PMIO that owns the switch into
; protected mode and the thunk back out of it.
;
; That is SIXTEEN KILOBYTES, and a 624K transient program area.  For
; comparison, DOS 6.22 with DOS=HIGH,UMB and everything loaded high
; reached about 620K and a plain one about 560K.
;
; The four items are each at their floor and it is worth writing down
; why, because the obvious next question is where the last 16K went:
;
;   1.25K  IVT and BIOS data area.  Fixed by the hardware.
;   0.05K  the thunk's register frame, in the BDA's user area at 4E8h.
;   2.7K   the real-mode stack.  Every BIOS call the kernel makes runs
;          on it, with IRQs enabled, so the timer and keyboard ISRs
;          nest on it too.  It could be cut to 1K; the 1.7K saved is
;          not worth being the reason a BIOS with deep INT 13h
;          extensions scribbles on the IVT.
;   4K     the stub.  Was 8K, which was a round number and not a
;          measurement: the code is 2076 bytes, the GDT is at 0C00h
;          and the globals at 0F00h, so 4K holds all of it with a
;          kilobyte to grow into.
;   8K     XFER.  16 sectors, which is one full track of a 1.44M
;          floppy less two.  Halving it would buy 4K and cost an extra
;          INT 13h per track on every floppy read; hard disks go
;          through the kernel's own ATA driver and never touch it.
;
; BOOT-ONLY SCRATCH LIVES INSIDE THE TPA, at 4000h-48FFh: the FAT
; window, the directory sector and the E820 table are used while the
; kernel is being loaded and are dead before the first DOS program
; exists.  Putting them below the TPA would have cost 2.25K of real
; memory forever to hold something nothing ever reads again.
;---------------------------------------------------------------------

;---------------------------------------------------------------------
; Where PMIO is loaded, and where it ends up - two different places.
;
; A 512-byte boot sector can do one simple thing: read the file into
; one segment.  So it drops all of PMIO.SYS at IO_LOAD_SEG and jumps
; there, and the stub then puts itself where it actually wants to be -
; it copies its own 4K down to IO_SEG, and once unreal mode is up it
; blits its protected-mode half from IO_PM_LOAD to IO_PM_BASE above
; 1MB.  The boot sector never needs to know any of that.
;---------------------------------------------------------------------
IO_LOAD_SEG     EQU     2000h                   ; where the boot loader puts it
IO_LOAD_BASE    EQU     00020000h
RMSTUB_SIZE     EQU     00001000h               ; IORM.BIN is exactly 4K
IO_PM_LOAD      EQU     IO_LOAD_BASE + RMSTUB_SIZE

IO_SEG          EQU     0100h                   ; where the stub lives for good
IO_BASE         EQU     00001000h
IO_PM_BASE      EQU     00200000h               ; and its 32-bit half, above 1M
IO_PM_MAX       EQU     00010000h               ; how much of it is blitted up
IO_LIMIT        EQU     IO_PM_BASE + IO_PM_MAX  ; PMIO must not grow past here

;---------------------------------------------------------------------
; THE FIRST 64K ABOVE 1MB IS LEFT EMPTY ON PURPOSE.  It is the HMA -
; the only part of extended memory a 16-bit segment:offset can reach,
; since FFFF:FFFF comes out at 10FFEFh and no further - and the whole
; A20 story is about what a program finds there.  The kernel used to
; live at exactly this address, which meant a DOS box writing to
; FFFF:0010 scribbled on PMDOS.SYS, and it meant the A20 wrap could
; never be emulated: aliasing these pages back down to zero would have
; unmapped the kernel from under itself.  So the kernel starts one
; 64K block higher and this range belongs to the guest.
;---------------------------------------------------------------------
HMA_BASE        EQU     00100000h               ; where extended memory starts
HMA_PAGES       EQU     16                      ; 64K: all a 16-bit far
                                                ; pointer can reach past 1MB
DOS_BASE        EQU     00110000h               ; PMDOS.SYS load address
DOS_LIMIT       EQU     00140000h               ; and it must end below this -
                                                ; SHELL_PSP, spelled out because
                                                ; a forward EQU is not a value
                                                ; to wasm, it is an error

;---------------------------------------------------------------------
; Paging, and it is here for ONE reason: to make the A20 gate honest.
;
; The map is the identity everywhere - linear IS physical, as it has
; been since the beginning, and every address in this file still means
; what it says.  The only entries that ever change are the sixteen
; covering the HMA: with A20 on they point at themselves, and with A20
; off they point at 0-FFFFh, which is the 1MB wrap an 8086 had for
; free and every DOS program written before 1985 relied on.  Sixteen
; page table entries is the entire emulation.
;
; PG_MAXTAB tables of 4MB each is the ceiling on memory this kernel
; will map; arena_init is capped to it, because handing out an address
; nothing maps is a page fault with the return address already lost.
;---------------------------------------------------------------------
PG_DIR          EQU     00220000h               ; page directory, 4K
PG_TABS         EQU     00221000h               ; the page tables follow it
PG_MAXTAB       EQU     64                      ; 64 * 4MB = 256MB mappable
PG_MAXLIN       EQU     PG_MAXTAB * 00400000h
PG_TOP          EQU     PG_TABS + PG_MAXTAB * 1000h
; Present, writable, and USER.  The user bit is not optional and its
; absence is not a security setting: V86 code runs at CPL 3, so a
; supervisor-only page is a page a DOS box cannot execute from, and
; the first guest instruction after paging went in took a page fault
; on its own entry point.  Nothing is given away by it either - a
; 16-bit segment:offset cannot reach past 10FFEFh whatever the page
; tables say, so the guest is already confined by arithmetic that no
; permission bit could tighten.  That is true of the POINTER and not
; of the pointer plus a length, which is what buf_ok below is for.
PG_PRESENT      EQU     7                       ; present + writable + user

ARENA_BASE      EQU     PG_TOP                  ; start of the DOS memory arena

;---------------------------------------------------------------------
; WHERE A CALLER'S BUFFER MAY NOT BE.
;
; Every pointer that arrives at INT 21h came from somebody else - a
; DOS box's segment:offset, a DPMI client's selector:offset, a native
; program's slot offset - and each of the three bridges checks that a
; pointer is one its own caller could have meant.  What none of them
; can check is the LENGTH, because a length is not a pointer: a read
; of FFFFh bytes into FFFF:FF00 begins inside the guest's own
; megabyte and ends sixty kilobytes inside PMDOS.SYS.
;
; So the length-carrying calls ask buf_ok, and the rule it enforces is
; deliberately NOT "this belongs to the caller" - it is "this is not
; OURS".  Which program's memory a DOS program corrupts is DOS
; behaving normally and always was; what cannot be allowed is the
; kernel being overwritten while it is running the very call doing the
; overwriting, because the transfer then reports success and the
; machine dies somewhere else entirely with nothing to point at.  That
; is the same argument i13_bufok was written for in v0.27, one API
; further up.
;
; Three ranges, and the gaps between them are deliberate.  The IVT and
; the BIOS data area below 4E8h are the guest's to ruin, as they are
; on a real machine; so is the DOS data segment at 4000h, which holds
; the list of lists and which a program is invited to read.  The slots
; at 140000h-1FFFFFh are a PROGRAM's memory and not the kernel's, and
; their owner passes pointers into them all day - r3_xlat bounds
; those, and it is the only thing that can, since one slot's pointer
; is the other slot's trespass.
;---------------------------------------------------------------------
KBUF_LO1        EQU     000004E8h       ; the thunk's register frame, the
KBUF_HI1        EQU     00004000h       ; real-mode stack, the stub, XFER
KBUF_LO2        EQU     DOS_BASE        ; PMDOS.SYS itself
KBUF_HI2        EQU     DOS_LIMIT
KBUF_LO3        EQU     IO_PM_BASE      ; PMIO, the IDT, the kernel stacks,
KBUF_HI3        EQU     ARENA_BASE      ; the TSS and the page tables

;---------------------------------------------------------------------
; UPPER MEMORY.  The region between the top of conventional memory and
; the system BIOS, and the only part of the first megabyte that was
; never anybody's to begin with.
;
; A0000h-BFFFFh is the display and is left alone: writing a test
; pattern into the frame buffer to find out whether it is there is a
; test that succeeds and is still wrong.  F0000h-FFFFFh is the system
; BIOS, which the V86 monitor reflects every unclaimed interrupt to.
; What is left is C0000h-EFFFFh, and on most machines most of it is
; not decoded by anything at all.
;
; THE PAGE TABLES ARE WHY THIS IS WORTH DOING HERE.  A real-mode DOS
; can only hand out upper memory that some chipset has already been
; persuaded to put RAM behind, which is what made EMM386 and the
; imitators of it necessary; PM-DOS is already paging, so a linear
; address nothing decodes can simply be pointed at a physical page out
; of the arena.  Upper memory stops being a scarce accident of the
; hardware and becomes ordinary memory that happens to live at an
; address a real-mode program can reach.
;
; The scan is in 4K pages because that is the granularity a page table
; entry has.  UMB_MAXRUN caps how many separate runs the chain will
; describe: every run costs a system-owned filler block in front of it
; to keep the chain contiguous, and a machine whose upper memory comes
; in nine pieces has nothing useful in any of them.
;---------------------------------------------------------------------
UMB_SCAN_LO     EQU     000C0000h       ; first page that may be offered
UMB_SCAN_HI     EQU     000F0000h       ; one past the last
UMB_SCAN_PAGES  EQU     (UMB_SCAN_HI - UMB_SCAN_LO) / 1000h
UMB_MAXRUN      EQU     8               ; separate runs the chain describes

; What pg_umbprobe writes in the byte belonging to each page.
UMBP_TAKEN      EQU     0               ; something is there: a ROM, adapter
                                        ; RAM, or a device that answered
UMBP_RAM        EQU     1               ; writable memory already at that
                                        ; address - usable as it stands
UMBP_EMPTY      EQU     2               ; nothing decodes it: usable once a
                                        ; page has been mapped underneath

;---------------------------------------------------------------------
; Program slots.  One shell, one transient program - DOS with the
; serial numbers filed off.
;
; A SLOT IS A SEGMENT NOW.  Native programs run at ring 3 out of a code
; and a data descriptor based at the slot they were loaded into, with a
; limit of its size, so a program can address its own slot and nothing
; else in the machine - not the kernel, not the interrupt vector table,
; not the arena, not the other slot.  Every address a program uses is
; therefore an OFFSET from the base of its slot, and the offsets below
; are the same for both slots, which is what lets one linked image run
; in either.  See RING3.INC.
;
; THE FIRST PAGE OF EVERY SLOT IS NOT PRESENT.  That is the null-
; pointer guard the roadmap wanted and v0.31 could not build: page zero
; of the machine is load-bearing (the IVT, the BIOS data area, InDOS,
; the real-mode frame) and cannot be unmapped, but page zero OF A
; PROGRAM is nothing at all, and a slot is arranged so that offset zero
; lands on it.  A store through a null pointer is now a page fault at a
; known address rather than four debug registers hoping to catch the
; first sixteen bytes of it - and unlike DR7, an emulator implements it.
;
; The environment lives behind the PSP because a program can no longer
; reach the one its parent holds: EXEC copies it in, exactly as the VDM
; already copies one below 1MB for a 16-bit guest, and for the same
; reason.
;
; Image bases are 4K-aligned because wlink silently rounds
; "option offset=" up to a 4K multiple.
;---------------------------------------------------------------------
SLOT_GUARD      EQU     00000000h               ; 4K, deliberately absent
SLOT_PSP        EQU     00001000h               ; the PSP, 100h bytes
SLOT_ENV        EQU     00001100h               ; and the environment behind it
SLOT_ENVMAX     EQU     00000F00h               ; up to the image
SLOT_IMAGE      EQU     00002000h               ; D32 header lands here - and
                                                ; every D32 program is linked
                                                ; for this one offset now

SHELL_SLOT      EQU     00140000h               ; the interpreter's slot
SHELL_SIZE      EQU     00040000h               ; 256K
PROG_SLOT       EQU     00180000h               ; and the transient program's
PROG_SIZE       EQU     00080000h               ; 512K

; The flat addresses the kernel itself uses to reach into them.  The
; kernel is still flat and based at zero, so it adds what a program
; never has to know about.
SHELL_PSP       EQU     SHELL_SLOT + SLOT_PSP
SHELL_TOP       EQU     SHELL_SLOT + SHELL_SIZE ; initial ESP for the shell
PROG_PSP        EQU     PROG_SLOT + SLOT_PSP
PROG_TOP        EQU     PROG_SLOT + PROG_SIZE   ; initial ESP for programs

;---------------------------------------------------------------------
; D32 executable header - 32 bytes at the front of the file.  The file
; is the memory image: it is loaded whole at D32H_BASE (header
; included), so a program's code starts at its base + 32.
;
; THE FIRST FIELD IS A 16-BIT JUMP, and it is there for a machine that
; is not this one.  A D32 file carries no MZ signature, and real
; MS-DOS decides what a program is by the first two bytes and not by
; the extension - so it loads one as a .COM at 100h and executes it
; from byte zero, whether it is called .COM or .EXE.  Byte zero is
; therefore a JMP SHORT over the header, to the stub D32_HEAD plants
; behind it: it says what the file needs and exits with errorlevel 1.
; Under PM-DOS that jump is never executed; the kernel reads the
; header and enters at D32H_ENTRY.
;
; The magic moved to +4 to make room for it, which means a D32 binary
; from before 0.29 is not one now.  That is the intended answer: the
; kernel and its executables are one build, and a stale COMMAND.COM
; that would have been loaded and run is better refused.
;---------------------------------------------------------------------
; THE BASE AND THE ENTRY POINT ARE SLOT OFFSETS as of the ring-3
; release, not absolute addresses: a program is linked for offset
; SLOT_IMAGE and the segment base decides which slot that turns out to
; be, so the same image runs as the shell or as a transient program.
; The base field is kept, and checked, because a file whose idea of the
; layout differs from the kernel's is not one of ours.
D32_MAGIC       EQU     1A323344h               ; 'D','3','2',1Ah
D32H_JMP        EQU     0                       ; db 0EBh,1Eh + 2 pad - the stub
D32H_MAGIC      EQU     4                       ; dd - must be D32_MAGIC
D32H_BASE       EQU     8                       ; dd - load offset: SLOT_IMAGE
D32H_ENTRY      EQU     12                      ; dd - entry point (absolute)
D32H_BSS        EQU     16                      ; dd - bytes to zero after image
D32H_STACK      EQU     20                      ; dd - stack wanted (informational)
D32H_SIZE       EQU     32

;---------------------------------------------------------------------
; D32_HEAD - the header and its 16-bit stub, in one place, so that
; every D32 executable gets both and none of them gets a different one.
;
; THE STUB IS HAND-ENCODED, AND IT HAS TO BE.  It sits inside a USE32
; segment, so any 16-bit form the assembler emitted here would carry a
; 66h operand-size prefix - and in real mode that prefix means the
; other thing.  "mov dx,imm16" would assemble as 66 BA and execute as
; MOV EDX,imm32, swallowing the two bytes after it and running off
; into the message.  So the bytes are written out with what they are
; beside them.
;
; Nothing in the stub depends on the program: the header is a fixed 32
; bytes, so the stub always starts at file offset 20h, which under DOS
; is 120h.  These bytes are identical in every D32 file.
;---------------------------------------------------------------------
D32_HEAD MACRO   d32base, d32entry, d32bss, d32stack
LOCAL   s16, s16msg
        db      0EBh, D32H_SIZE - 2     ; JMP SHORT s16 - where real DOS lands
        db      0, 0                    ; pad, so the magic is aligned
        dd      D32_MAGIC
        dd      d32base
        dd      d32entry
        dd      d32bss
        dd      d32stack
        dd      0, 0                    ; pad the header to D32H_SIZE
s16:
        db      0Eh                     ; push cs
        db      1Fh                     ; pop  ds       - a .COM starts CS = PSP
        db      0BAh                    ; mov  dx,imm16 - the message
        dw      100h + D32H_SIZE + (s16msg - s16)
        db      0B4h, 09h               ; mov  ah,9     - print it
        db      0CDh, 21h               ; int  21h
        db      0B8h                    ; mov  ax,imm16
        dw      4C01h                   ; 4C01h: terminate, errorlevel 1
        db      0CDh, 21h               ; int  21h
s16msg:
        db      13,10,'This program requires 32-bit DOS mode.',13,10,'$'
ENDM

;---------------------------------------------------------------------
; PSP - built by EXEC at the slot's PSP address.
;
; The offsets are the real DOS ones wherever the field survives the
; move to flat mode, which is most of them: DOS kept far pointers at
; 0Ah/0Eh/12h, and a flat pointer is the same four bytes.  The three
; SAVED VECTORS are the point of the layout - EXEC stashes the live
; 22h/23h/24h here and termination puts them back, which is how a
; child that hooks Ctrl-C or the critical-error handler is prevented
; from leaving them hooked over its parent.
;
; What is deliberately absent: the JFT at 18h-2Bh (PM-DOS handles are
; kernel-side, found by owner id) and the two FCBs at 5Ch/6Ch (there
; is no FCB API here).  Those ranges stay zero.
;---------------------------------------------------------------------
PSP_INT20       EQU     00h                     ; dw 20CDh - INT 20h opcode
PSP_MEMTOP      EQU     04h                     ; dd - first byte past the slot
PSP_TERM        EQU     0Ah                     ; dd - saved INT 22h
PSP_CTRLC       EQU     0Eh                     ; dd - saved INT 23h
PSP_CRITERR     EQU     12h                     ; dd - saved INT 24h
PSP_PARENT      EQU     16h                     ; dd - the PSP that EXEC'd us
PSP_ENV         EQU     2Ch                     ; dd - environment, 0 = none
PSP_DOSCALL     EQU     50h                     ; INT 21h / RET thunk
PSP_R3RET       EQU     54h                     ; where a ring-3 handler's IRET
                                                ; comes back to - see below
PSP_TAIL        EQU     80h                     ; command tail: len, text, 0Dh
PSP_SIZE        EQU     100h

;---------------------------------------------------------------------
; The five bytes at PSP+54h, and why a program's PSP has to hold them.
;
; A program's INT 23h and INT 24h handlers now live at ring 3, and an
; interrupt gate cannot reach them: control never goes OUTWARD through
; one, so the kernel cannot `int 24h` its way into a program the way it
; could when programs ran in its own segment.  It has to arrive the
; only way ring 3 is ever reached - by building a frame and IRETDing -
; and then it needs a way BACK, because the specification for every one
; of these handlers is that it ends in an IRET and nothing else.
;
; So the return address pushed for that IRET points here, at an INT 21h
; the kernel recognises by the vector it arrives on.  Trap out, trap
; back, exactly as a DPMI real-mode callback does it, and for the same
; reason: an interrupt is the one instruction that means "the kernel is
; wanted" from any privilege level.
;
; It is in the PSP because the PSP is the one piece of a program's
; address space the KERNEL lays out, and because CS and DS have the
; same base here - so bytes written as data are reachable as code.
;
; THE TWO BYTES ARE A BARE INT 21h AND CARRY NO FUNCTION NUMBER, and
; that is the whole trick.  An INT 24h handler answers in AL and an
; INT 23h handler may answer in nothing at all: by the time control
; reaches here the registers belong to the HANDLER, and a trampoline
; that loaded a function number into AX would destroy the answer it
; exists to bring back.  So the kernel recognises this call by the EIP
; it arrives from, which is a constant it laid out itself.  DPMI's
; real-mode callback return is the same idiom, arrived at the same way.
;---------------------------------------------------------------------
R3RET_LEN       EQU     2                       ; db 0CDh,21h
R3RET_EIP       EQU     SLOT_PSP + PSP_R3RET + R3RET_LEN


IDT_BASE        EQU     00210000h
IDT_LIMIT       EQU     07FFh                   ; 256 * 8 - 1

KSTACK_TOP      EQU     00214000h               ; kernel ESP after PM switch

;---------------------------------------------------------------------
; The TSS.  Virtual-8086 mode runs at CPL 3, so every fault out of a
; V86 task switches stacks through a TSS - without one the first INT a
; DOS program executes is a triple fault.  We never task-switch; the
; TSS exists for SS0:ESP0 and for the I/O permission bitmap.
;
; That bitmap is 8K of zeros with THREE BITS SET, which grants the V86
; task every port but those three.  In V86 mode IOPL does NOT govern
; IN and OUT - only the bitmap does - so this is what lets a DOS
; program talk to hardware without the monitor having to emulate every
; I/O instruction.  A port whose bit is set raises #GP instead, and
; the monitor emulates that one instruction.
;
; The three are 60h, 64h and 92h, and they are all the same device:
; the A20 gate.  Emulating A20 in the page tables is worth nothing if
; the guest can walk round the back and move the real line, and it has
; two ways to try - the keyboard controller's output port and the PS/2
; system control port.  Left open, a DOS box could put the machine
; into a state where the kernel's own view of memory and the guest's
; disagree, and clearing the real line would unmap PMDOS.SYS from
; underneath itself.  See vio_in / vio_out in VM86.INC.
;---------------------------------------------------------------------
TSS_BASE        EQU     00214000h
TSS_IOBASE      EQU     104                     ; bitmap starts right after
TSS_TOTAL       EQU     8300                    ; 104 + 8192 + FFh, rounded
TSS_LIMIT       EQU     TSS_TOTAL - 1
TSS_ESP0        EQU     04h
TSS_SS0         EQU     08h
TSS_IOMAP       EQU     66h                     ; word: offset of the bitmap

;---------------------------------------------------------------------
; DPMI.  A protected-mode client runs at ring 3 out of an LDT of its
; own, and both of the things it needs live in the gap between the end
; of the TSS (21606Ch) and the page directory at 220000h.
;
; THE RING-0 STACK IS NOT OPTIONAL AND IT IS NOT THE KERNEL'S.  Any
; interrupt taken while ring-3 code is running switches stacks through
; TSS_ESP0 - a DPMI call, a DOS call, and the timer and keyboard IRQs,
; which arrive whether the client asked for them or not.  v86_run
; points ESP0 at its own frame for the duration of a DOS box; a PM
; client needs somewhere that is valid whenever it is running and is
; not somebody else's live stack.
;---------------------------------------------------------------------
DPMI_KSTACK     EQU     00218000h               ; ring-0 ESP for ring-3 entries
DPMI_LDT        EQU     00218000h               ; the client's descriptor table

; And the same thing for a NATIVE ring-3 program, which needs its own
; rather than sharing: the shell is a ring-3 program that stays running
; while a DOS box and a DPMI client inside it come and go, so the two
; stacks are live at the same time.  It sits above the LDT (2K of the
; 32K between DPMI_LDT and the page directory) and grows down.
R3_KSTACK       EQU     0021C000h               ; ring-0 ESP while a native
                                                ; program is running
DPMI_LDT_ENT    EQU     256                     ; descriptors in it
DPMI_LDT_LIMIT  EQU     DPMI_LDT_ENT * 8 - 1

; Selectors are built with RPL 3 and TI 1 - a client's selector always
; points into its own table and always runs unprivileged.
DPMI_SELBITS    EQU     7                       ; TI + RPL 3
DPMI_SELINC     EQU     8                       ; 21h/0003h answers this

; The first four descriptors are handed to the client by the mode
; switch and are the same four every DPMI host gives: its code, its
; data, its stack and its PSP.
DPMI_IDX_CS     EQU     1
DPMI_IDX_DS     EQU     2
DPMI_IDX_SS     EQU     3
DPMI_IDX_PSP    EQU     4

; Three more the host keeps for itself.  They are what a real-mode
; callback is made of: the code segment a callback procedure's IRET
; returns into, a window on the real-mode stack the call was made from,
; and the protected-mode stack the procedure runs on.  A client never
; asks for these and 0000h never hands them out.
DPMI_IDX_CBRET  EQU     5
DPMI_IDX_RMS    EQU     6
DPMI_IDX_CBSS   EQU     7
DPMI_IDX_FIRST  EQU     8                       ; the first one 0000h can give

; Descriptor access bytes.  DPL 3 throughout: there is nothing a
; client is allowed to do at ring 0.
DACC_CODE       EQU     0FAh                    ; present, DPL 3, code, readable
DACC_DATA       EQU     0F2h                    ; present, DPL 3, data, writable
DGRAN_16        EQU     000h                    ; byte granular, 16-bit
DGRAN_32        EQU     040h                    ; byte granular, 32-bit
DGRAN_G         EQU     080h                    ; 4K granular, added to either

; INT 2Fh AX=1687h - what a client asks to find out we are here
DPMI_VER_MAJOR  EQU     0
DPMI_VER_MINOR  EQU     90                      ; DPMI 0.9
DPMI_PRIVATE    EQU     0                       ; paragraphs of DOS memory the
                                                ; host wants for itself: none,
                                                ; because the LDT is above 1MB

;---------------------------------------------------------------------
; The real-mode call structure - 50 bytes at ES:DI on 21h/0300h and
; the family below it.  Unchanged since DPMI 0.9 and worth spelling
; out because the reserved dword at 0Ch is easy to leave out and
; everything after it then lands one field early.
;---------------------------------------------------------------------
RMCS_EDI        EQU     00h
RMCS_ESI        EQU     04h
RMCS_EBP        EQU     08h
RMCS_RSVD       EQU     0Ch
RMCS_EBX        EQU     10h
RMCS_EDX        EQU     14h
RMCS_ECX        EQU     18h
RMCS_EAX        EQU     1Ch
RMCS_FLAGS      EQU     20h                     ; word from here down
RMCS_ES         EQU     22h
RMCS_DS         EQU     24h
RMCS_FS         EQU     26h
RMCS_GS         EQU     28h
RMCS_IP         EQU     2Ah
RMCS_CS         EQU     2Ch
RMCS_SP         EQU     2Eh
RMCS_SS         EQU     30h
RMCS_LEN        EQU     32h

; The scratch real-mode stack a 0300h gets when the caller leaves
; SS:SP zero, which is what the specification says to do and what
; every client actually does.
DPMI_RMSTK      EQU     256                     ; bytes, out of the DOS arena

;---------------------------------------------------------------------
; Real-mode callbacks - 21h/0303h.
;
; A callback is an address real-mode code can FAR CALL that arrives in
; the client as a protected-mode procedure.  It is how a client hooks
; a real-mode interrupt, and how a driver written for real mode calls
; back into a program that no longer lives there.
;
; The addresses come out of a paragraph of low memory that is nothing
; but landing pads: two bytes per callback, a HLT for the monitor to
; stop on and a RETF behind it for the case where nobody claims the
; call.  That is the same idiom as the XMS and DPMI entry points, and
; for the same reason - a HLT is the only instruction a V86 guest can
; execute that means "the kernel is wanted" and nothing else.
;
; Behind the slots are two more bytes that are NOT real-mode code at
; all.  The specification says a callback procedure ends with an IRET,
; so the host has to push a return address that takes control back,
; and DPMICB_R3RET is it: an INT 31h, executed at ring 3 through a
; code descriptor over this same paragraph.  The way back is a trap
; whichever mode you are in.
;---------------------------------------------------------------------
DPMI_MAXCB      EQU     8                       ; live callbacks, at once
DPMICB_SLOT     EQU     2                       ; F4 CB - the HLT and its RETF
DPMICB_R3RET    EQU     DPMI_MAXCB * DPMICB_SLOT        ; CD 31
DPMICB_RETF     EQU     DPMICB_R3RET + 2        ; CB - 0305h's do-nothing
DPMICB_PARAS    EQU     2                       ; paragraphs of all that

; One of these per callback the client holds.
DCB_FLAGS       EQU     0                       ; dd - 0 free, 1 allocated
DCB_PROC        EQU     4                       ; dd - the client's procedure
DCB_PSEL        EQU     8                       ; dd - and the selector it is in
DCB_RMCS        EQU     12                      ; dd - its register structure,
                                                ;      flat, for us to fill in
DCB_RSEL        EQU     16                      ; dd - and as the client named
DCB_ROFF        EQU     20                      ; dd - it, to hand back in ES:DI
DCB_LEN         EQU     24

; The protected-mode stack a callback procedure runs on.  DPMI calls
; it "a locked API stack"; here nothing is pageable, so locking it is
; a matter of never freeing it.
DPMI_CBSTK      EQU     4096

;---------------------------------------------------------------------
; INT 31h error codes.  DPMI 0.9 returns them in AX with CF set.
;---------------------------------------------------------------------
DPE_UNSUPPORTED EQU     8001h
DPE_NOMEM       EQU     8012h                   ; linear memory unavailable
DPE_NODESC      EQU     8011h                   ; descriptor unavailable
DPE_BADVALUE    EQU     8021h
DPE_BADSEL      EQU     8022h
DPE_BADHANDLE   EQU     8023h
DPE_BADADDR     EQU     8025h
DPE_NOCB        EQU     8015h                   ; callback unavailable

; Memory blocks handed out by 21h/0501h.  The handle IS the index of
; one of these, plus one, so that zero is never a valid handle.
DPMI_SEGMAX     EQU     32                      ; 0002h remembers this many
                                                ; segment-to-selector answers
DPMI_MAXMEM     EQU     32
DM_FLAGS        EQU     0                       ; dd - 0 free, 1 taken
DM_ADDR         EQU     4                       ; dd - flat address
DM_SIZE         EQU     8                       ; dd - bytes
DM_OWN          EQU     12                      ; dd - the owner id that took it
DM_LEN          EQU     16

;---------------------------------------------------------------------
; The VDM's conventional memory: everything from the end of the XFER
; buffer to the video RAM, 624K of it.
;
; The 16K below it is not recoverable by moving anything else up - see
; the itemised list at the top of this file.  The IVT and BIOS data
; area are fixed by the hardware; the real-mode stack has to be
; somewhere the 8086 can address; XFER has to be below 1MB because
; that is the only place INT 13h can transfer to; and the stub itself
; is the code that gets into and out of protected mode, so it cannot
; be anywhere a real-mode far jump cannot reach.
;---------------------------------------------------------------------
; THE DOS DATA SEGMENT, AND IT HAS TO BE BELOW THE ARENA.
;
; The list of lists used to be allocated OUT OF the arena it
; describes, five paragraphs above the first MCB, and that is a place
; no real DOS has ever kept it: MS-DOS's data segment sits below the
; transient program area, and the arena begins above it.  The
; difference is not cosmetic.  MS-DOS 6's MEM.EXE sizes the DOS data
; area by SUBTRACTING - "(first MCB) - (the segment 52h answered)" -
; and with the list of lists above the first MCB that subtraction goes
; NEGATIVE.  MEM took the result as an unsigned length, entered a four
; gigabyte block in its table, flooded the screen padding a field to a
; nonsense width, and then took a BOUND fault in its own bounds check.
;
; So the block is a fixed 256 bytes below the arena now, exactly where
; DOS keeps it, and the arena starts one page further up.  What it
; costs is a quarter of a kilobyte of a DOS box's memory; what it buys
; is that every tool computing a size by subtraction gets a positive
; one.
DOSDATA         EQU     00004000h
DOSDATA_SEG     EQU     0400h
DOSDATA_SIZE    EQU     00000100h

V86_TPA         EQU     00004100h
V86_TPA_SEG     EQU     0410h
V86_TPA_TOP     EQU     000A0000h       ; THE CEILING, NOT THE TOP.  Where the
                                        ; arena really ends is what the BIOS
                                        ; says at 40:13 and is decided at run
                                        ; time - see lm_init and lm_top.  640K
                                        ; is the most there can ever be, which
                                        ; is what this is for.

;---------------------------------------------------------------------
; Memory control blocks, the DOS memory arena, unchanged since 1983.
; One 16-byte header in front of every block, chained by size, the
; last one marked 'Z'.  Programs walk this chain - MEM does, and so do
; a surprising number of games - so it is the real thing and not a
; convenient approximation.
;---------------------------------------------------------------------
; RMCB_, not MCB_: ARENA.INC already owns MCB_* for the kernel's own
; 32-bit block header, which is a different thing at a different size.
; These are the real-mode ones a DOS program can see.
RMCB_SIG        EQU     00h                     ; byte 'M', or 'Z' if last
RMCB_OWN        EQU     01h                     ; word PSP segment, 0 = free
RMCB_LEN        EQU     03h                     ; word paragraphs, excl. header
RMCB_NAME       EQU     08h                     ; 8 bytes
RMCB_SIZE       EQU     10h

;---------------------------------------------------------------------
; The "list of lists" a 16-BIT guest gets back from INT 21h AH=52h.
;
; The kernel's own 52h (fn52) answers a flat 32-bit pointer to dos_lol,
; which is right for a D32 program and useless to a DOS box: a guest
; can only hold an address as a segment and an offset, and dos_lol
; lives above 1MB where no real-mode pointer reaches.  So the VDM
; answers this structure instead - the same call, a shape a real-mode
; program can actually follow.
;
; ONE FIELD HERE IS A CONTRACT AND THE REST ARE COURTESY.  The word
; BELOW the returned pointer is the first memory control block, it has
; meant that in every DOS since 2.0, and it is what a memory-mapping
; tool is really after when it makes this call.  Everything else moved
; between versions, so a tool that reads it hedges - which is what
; makes the rest safe to answer honestly rather than convincingly.
;
; WHAT THE POINTERS SAY.  PM-DOS has no DPBs, no SFT chain in DOS's
; shape, no CDS array and no FCB table, so those four answer FFFF:FFFF
; - the sentinel DOS itself uses for "the chain ends here", which a
; walker tests for before it dereferences anything.  Inventing plausible
; structures there would be worse than saying no: a tool would follow
; them.  The device chain is different, because PM-DOS genuinely has
; devices, so it gets one real terminated entry.
;
; Offsets here are from the SEGMENT BASE, not from the returned
; pointer, which sits GLOL_PTR bytes in.  Subtract 2 to read them
; against a DOS reference.
;---------------------------------------------------------------------
GLOL_MCBSEG     EQU     00h             ; dw - first MCB.  [ES:BX-2]
GLOL_PTR        EQU     02h             ; ES:BX is handed out pointing here
GLOL_DPB        EQU     02h             ; dd - +00 first drive parameter block
GLOL_SFT        EQU     06h             ; dd - +04 first system file table
GLOL_CLOCK      EQU     0Ah             ; dd - +08 the CLOCK$ device
GLOL_CON        EQU     0Eh             ; dd - +0C the console device
GLOL_MAXSEC     EQU     12h             ; dw - +10 largest sector of any block
GLOL_BUFFERS    EQU     14h             ; dd - +12 the disk buffer chain
GLOL_CDS        EQU     18h             ; dd - +16 the current directory array
GLOL_FCB        EQU     1Ch             ; dd - +1A the system FCB table
GLOL_PROTFCB    EQU     20h             ; dw - +1E FCBs kept from recycling
GLOL_NBLOCK     EQU     22h             ; db - +20 block devices, filled per call
GLOL_LASTDRV    EQU     23h             ; db - +21 LASTDRIVE
GLOL_NUL        EQU     24h             ; +22 the NUL driver header, and with
                                        ; it the head of the device chain
GLOL_NJOIN      EQU     36h             ; db - +34 JOIN'ed drives, none

; THE TAIL OF THE STRUCTURE, AND WHY IT IS HERE AT ALL.
;
; The list of lists did not stop at the JOIN count in DOS 5, and this
; one did: the device headers began immediately afterwards, on top of
; eight documented fields.  A program reading the boot drive out of
; LoL+43h got the high byte of the CON header's interrupt entry; one
; reading the extended memory size out of LoL+45h got the letters "ON"
; out of the word 'CON     ' - twenty thousand and forty-seven
; kilobytes, from a machine with a hundred and twenty-eight megabytes.
;
; Nothing here was invented for a program to find.  These are fields
; MS-DOS 5.0 and 6.x publish at fixed offsets, which means a memory
; reporter written against them reads SOMETHING whatever we do; the
; only choice is whether it reads an answer or a device name.  That is
; the same argument the FFFF:FFFF pointers above are made of, one step
; further along: say nothing, but say it in the right place.
GLOL_SPECIAL    EQU     37h             ; dw - +35 "special code segment",
                                        ; unused from DOS 5 onwards
GLOL_SETVER     EQU     39h             ; dd - +37 the SETVER table
GLOL_CPFIX      EQU     3Dh             ; dw - +3B Microsoft's copy-protection
                                        ; .COM fixup entry
GLOL_A20PSP     EQU     3Fh             ; dw - +3D PSP of the last program
                                        ; loaded, for disabling A20
GLOL_NBUF       EQU     41h             ; dw - +3F disk buffers
GLOL_NLOOK      EQU     43h             ; dw - +41 lookahead buffers
GLOL_BOOTDRV    EQU     45h             ; db - +43 boot drive, 1 = A
GLOL_DWMOVE     EQU     46h             ; db - +44 386 dword moves available
GLOL_EXTMEM     EQU     47h             ; dw - +45 extended memory, KB
GLOL_TAIL       EQU     49h             ; +47 - and there the structure ends

; Past the list of lists proper, and in the same block for the same
; reason: THESE ARE THE OTHER THINGS A GUEST IS HANDED A POINTER TO.
; 52h was not a special case, it was the first case - every INT 21h
; call that answers an ADDRESS has to answer one a real-mode program
; can hold, and a kernel pointer above 1MB is not one.  Keeping them
; here means the handlers return a flat address inside this block and
; the bridge converts it the same way for all of them.
;
; They live AFTER the documented tail now, which is where anything of
; ours belongs: an offset DOS defines is not ours to spend.
GLOL_RETF       EQU     49h             ; db - a far return, which is what both
                                        ; driver entries below point at: they
                                        ; are FAR CALL targets, and a program
                                        ; that calls one must come back
GLOL_MEDIA      EQU     4Ah             ; db - 1Bh/1Ch hand back DS:BX
                                        ; pointing at the media descriptor
                                        ; byte of the drive just asked about
GLOL_DBCS       EQU     4Ch             ; dw - 63h hands back DS:SI pointing
                                        ; at the DBCS lead-byte table, and a
                                        ; single zero word IS the table: no
                                        ; ranges, which is the truth for
                                        ; every codepage PM-DOS has

;---------------------------------------------------------------------
; THE DEVICE CHAIN, and it is a real one now.
;
; It shipped as a single synthetic NUL header - enough for a walker to
; start and stop, and a fiction about what the machine has.  PM-DOS
; has had four kinds of character device since v0.30 (dev_tab), so it
; can afford the truth: one header each, chained in the order DOS
; chained them, ending where DOS ends it.
;
; NUL IS THE HEAD AND THAT IS NOT ARBITRARY.  The list of lists points
; at it, every device walker starts from it, and DOS itself put NUL
; first so that a loaded driver could be inserted immediately after
; without moving the anchor.  That is the hook DEVICE= will need.
;
; The units are NOT separate headers.  LPT1-3 are one PRN driver with
; three of them and COM1-4 one AUX driver with four, exactly as
; dev_tab already says, because that is what a unit number is for.
; CLOCK$ has no header for the reason dev_tab gives: half a clock
; would be a lie, and 2Ah-2Dh are the whole of the truth.
;---------------------------------------------------------------------
DEVH_NEXT       EQU     00h             ; dd - FFFF:FFFF ends the chain
DEVH_ATTR       EQU     04h             ; dw
DEVH_STRAT      EQU     06h             ; dw - offset of the strategy entry
DEVH_INTR       EQU     08h             ; dw - offset of the interrupt entry
DEVH_NAME       EQU     0Ah             ; 8 bytes, blank padded
DEVH_LEN        EQU     12h

; GLOL_H* are the HEADERS.  GLOL_CON above is the list of lists' own
; pointer AT the console header and is a different thing at a
; different offset - hence the H, which is not decoration.
GLOL_HNUL       EQU     GLOL_NUL        ; the head, already inside the LoL
GLOL_HCON       EQU     4Eh
GLOL_HAUX       EQU     60h
GLOL_HPRN       EQU     72h

; UMB_HEAD, AND IT IS NOT PART OF THE LIST OF LISTS AT ALL.
;
; It is a word in the DOS DATA SEGMENT at a fixed offset - the segment
; 52h answers in ES, not the pointer it answers in BX - and it is
; where UPPER MEMORY BEGINS.  MS-DOS 6's own MEM.EXE reads it as
; literally as this: take ES from 52h, force the offset to 008Ch,
; read the word, and then for every block in the chain ask "is this
; segment below it?"  Below is conventional; at or above is upper.
;
; PM-DOS left those two bytes as whatever was in the paragraph, and
; the consequences were the whole of the report being wrong: the
; ENTIRE arena classified as upper memory, conventional showing not
; one free byte in it, and therefore a largest-executable-program
; size of nothing on a machine with 600K free.  Every absurd figure
; came from that one uninitialised word.
;
; FFFFh is the value DOS itself uses for "no upper memory", and the
; kernel tests for exactly that (alloc.asm: "Q: is umb_head
; initialized").  No segment can be at or above FFFFh, so every block
; is conventional - which was the whole truth until v0.38 and is still
; the answer whenever DOS=UMB is absent.
;
; WHEN THERE ARE UMBs THIS DOES NOT POINT AT ONE.  It points at the
; LAST PARAGRAPH OF CONVENTIONAL MEMORY, which holds the header of a
; system-owned block spanning the display and the ROMs; the first real
; upper block is the one after that.  It reads like an off-by-one and
; it is the only arrangement that works, because a block's header has
; to live at an address something is willing to store sixteen bytes
; at, and the first paragraph above conventional memory is the frame
; buffer.  MS-DOS puts it in exactly the same place for exactly the
; same reason (sysconf.asm, LinkFirstUMB: "cx = ax - 1" where ax is
; the size of memory in paragraphs).  See UMB.INC.
GLOL_UMBHEAD    EQU     8Ch             ; dw - where upper memory starts
GLOL_SIZE       EQU     8Eh
                                        ; and it lives at DOSDATA, below the
                                        ; arena - not in it

; Attribute words.  Bit 15 says character device and the rest say what
; kind of one.
;
; CON DELIBERATELY DOES NOT SET BIT 4.  That bit means "this console
; accepts INT 29h", the fast write DOS uses to put a character on the
; screen without going through the device at all - and PM-DOS reflects
; INT 29h to an IRET, which SWALLOWS the character.  Claiming the bit
; would make well-behaved software choose the one path where its
; output disappears.  It goes back the day INT 29h writes a character.
DEV_ATTR_NUL    EQU     8004h           ; character, and the bit bucket
DEV_ATTR_CON    EQU     8003h           ; character, is stdin, is stdout
DEV_ATTR_AUX    EQU     8000h           ; character, and nothing special
DEV_ATTR_PRN    EQU     8000h
DEV_ATTR_CHAR   EQU     8000h           ; the bit that says which kind
DEV_ATTR_BIG    EQU     0002h           ; block only: understands a 32-bit
                                        ; sector number in a request packet

;---------------------------------------------------------------------
; THE REQUEST PACKET - how a loaded driver is spoken to.
;
; The four devices above are 32-bit code inside the kernel and are
; called as subroutines; a DEVICE= driver is 16-bit real-mode code
; that has never heard of any of that, and the only thing it
; understands is this structure.  DOS builds one, points ES:BX at it,
; far-calls the driver's STRATEGY entry to hand it over, and then
; far-calls the INTERRUPT entry to make it happen.  Two entries for
; one operation is an artefact of a 1981 plan for asynchronous
; drivers that never arrived; every driver ever shipped saves the
; pointer in the first and does the work in the second, and a DOS
; that called only the second would break all of them.
;
; The packet is the caller's memory and the driver writes its answer
; back into it - REQ_STATUS, and for INIT the address where the
; resident part ends.
;---------------------------------------------------------------------
REQ_LEN         EQU     00h             ; db - size of this packet
REQ_UNIT        EQU     01h             ; db - which unit, block drivers only
REQ_CMD         EQU     02h             ; db - DEVC_*
REQ_STATUS      EQU     03h             ; dw - DEVS_*, written by the driver
REQ_RESERVED    EQU     05h             ; 8 bytes DOS keeps its queue in
REQ_HDRLEN      EQU     0Dh

; INIT (command 0).  What goes in is a pointer to the rest of the
; CONFIG.SYS line; what comes back is where the driver's resident part
; ends, and everything above that address is handed straight back.
REQI_UNITS      EQU     0Dh             ; db - units, block drivers only
REQI_ENDOFF     EQU     0Eh             ; dw } the first byte NOT kept
REQI_ENDSEG     EQU     10h             ; dw
REQI_ARGOFF     EQU     12h             ; dw } in: the command line
REQI_ARGSEG     EQU     14h             ; dw   out: the BPB array, block only
REQI_DRIVE      EQU     16h             ; db - first drive letter, block only
REQI_LEN        EQU     17h

; Character read and write (commands 4, 8 and 9).
REQC_MEDIA      EQU     0Dh             ; db - media descriptor, block only
REQC_BUFOFF     EQU     0Eh             ; dw } where the bytes go or come from
REQC_BUFSEG     EQU     10h             ; dw
REQC_COUNT      EQU     12h             ; dw - in: wanted; out: transferred
REQC_SECTOR     EQU     14h             ; dw - block drivers only
REQC_LEN        EQU     16h

; ...and the DOS 4 form of that sector number, for a volume past the
; 65535 sectors a word can name.  FFFFh in the word above says "look
; here instead", and only a driver with DEV_ATTR_BIG set understands
; it - one written earlier reads the word and transfers to sector
; 65535, successfully, which is the worst way to be wrong.
REQC_HUGE       EQU     16h             ; dd - the real sector number
REQC_HUGELEN    EQU     1Ah

; A BLOCK DRIVER'S BPB is not a boot sector and does not have a boot
; sector's offsets: it begins at what a boot sector calls BPB_BPS, so
; every field is 0Bh lower.  Spelled out rather than biased, because a
; bias applied once too often is a number that still looks plausible.
DBPB_BPS        EQU     00h             ; dw - bytes per sector
DBPB_SPC        EQU     02h             ; db - sectors per cluster
DBPB_TOTSEC16   EQU     08h             ; dw - 0 if it needs 32 bits
DBPB_MEDIA      EQU     0Ah             ; db
DBPB_TOTSEC32   EQU     15h             ; dd

; Non-destructive read (command 5) puts the byte it peeked here.
REQP_CHAR       EQU     0Dh             ; db
REQP_LEN        EQU     0Eh

DEVC_INIT       EQU     0
DEVC_MEDIACHK   EQU     1
DEVC_BUILDBPB   EQU     2
DEVC_IOCTLIN    EQU     3
DEVC_READ       EQU     4
DEVC_PEEK       EQU     5               ; non-destructive read
DEVC_INSTAT     EQU     6
DEVC_INFLUSH    EQU     7
DEVC_WRITE      EQU     8
DEVC_WRITEVFY   EQU     9
DEVC_OUTSTAT    EQU     10
DEVC_OUTFLUSH   EQU     11
DEVC_IOCTLOUT   EQU     12
DEVC_OPEN       EQU     13
DEVC_CLOSE      EQU     14
DEVC_REMOVABLE  EQU     15
DEVC_OUTBUSY    EQU     16

DEVS_DONE       EQU     0100h           ; the driver is finished
DEVS_BUSY       EQU     0200h
DEVS_ERROR      EQU     8000h           ; and the low byte says which

; These offsets are a published layout, not an internal one - they are
; what a DOS program compiled in 1989 expects to find - so a later edit
; that inserts a field is a bug the assembler should catch rather than
; something a guest discovers by walking off the end of a chain.  A
; device header is 18 bytes and the JOIN count follows it; the RETF the
; two driver entries point at follows that; and the whole thing has to
; fit in the block vdm_lolinit asks lm_alloc for.
IF GLOL_NJOIN NE GLOL_NUL + DEVH_LEN
        .ERR    <GLOL: the NUL device header is not 18 bytes>
ENDIF
IF GLOL_SPECIAL NE GLOL_NJOIN + 1
        .ERR    <GLOL: the DOS 5 tail does not follow the JOIN count>
ENDIF
IF GLOL_SETVER NE GLOL_SPECIAL + 2
        .ERR    <GLOL: the SETVER pointer is not at LoL+37h>
ENDIF
IF GLOL_NBUF NE GLOL_A20PSP + 2
        .ERR    <GLOL: the buffer count is not at LoL+3Fh>
ENDIF
IF GLOL_BOOTDRV NE GLOL_NLOOK + 2
        .ERR    <GLOL: the boot drive is not at LoL+43h>
ENDIF
IF GLOL_EXTMEM NE GLOL_DWMOVE + 1
        .ERR    <GLOL: the extended memory size is not at LoL+45h>
ENDIF
IF GLOL_TAIL NE GLOL_EXTMEM + 2
        .ERR    <GLOL: the documented structure does not end at LoL+47h>
ENDIF
IF GLOL_RETF LT GLOL_TAIL
        .ERR    <GLOL: ours has been put inside the part DOS defines>
ENDIF
IF GLOL_MEDIA NE GLOL_RETF + 1
        .ERR    <GLOL: the media byte does not follow the driver return>
ENDIF
IF GLOL_HCON LT GLOL_DBCS + 2
        .ERR    <GLOL: the device chain overlaps the DBCS table>
ENDIF
IF GLOL_HAUX NE GLOL_HCON + DEVH_LEN
        .ERR    <GLOL: the CON header is not 18 bytes>
ENDIF
IF GLOL_HPRN NE GLOL_HAUX + DEVH_LEN
        .ERR    <GLOL: the AUX header is not 18 bytes>
ENDIF
IF GLOL_UMBHEAD LT GLOL_HPRN + DEVH_LEN
        .ERR    <GLOL: UMB_HEAD is underneath the PRN device header>
ENDIF
IF GLOL_SIZE NE GLOL_UMBHEAD + 2
        .ERR    <GLOL: GLOL_SIZE does not cover the whole structure>
ENDIF
IF GLOL_SIZE GT DOSDATA_SIZE
        .ERR    <GLOL: the structure does not fit in the DOS data segment>
ENDIF
IF DOSDATA + DOSDATA_SIZE NE V86_TPA
        .ERR    <GLOL: the DOS data segment does not end where the arena begins>
ENDIF

;---------------------------------------------------------------------
; 38h country information, as DOS 3.0 and later lay it out.  34 bytes,
; and PM-DOS answers exactly one country - see fn38.
;---------------------------------------------------------------------
CTRY_DATEFMT    EQU     00h             ; dw  0 = mm/dd/yy, 1 = dd/mm/yy
CTRY_CURRENCY   EQU     02h             ; 5 bytes ASCIZ
CTRY_THOUSANDS  EQU     07h             ; 2 bytes ASCIZ
CTRY_DECIMAL    EQU     09h             ; 2 bytes ASCIZ
CTRY_DATESEP    EQU     0Bh             ; 2 bytes ASCIZ
CTRY_TIMESEP    EQU     0Dh             ; 2 bytes ASCIZ
CTRY_CURRFMT    EQU     0Fh             ; db  bit 0 clear = symbol leads
CTRY_CURRDIGITS EQU     10h             ; db  digits after the point
CTRY_TIMEFMT    EQU     11h             ; db  bit 0 clear = 12-hour
CTRY_CASEMAP    EQU     12h             ; dd  FAR CALL, AL = char to map
CTRY_LISTSEP    EQU     16h             ; 2 bytes ASCIZ
CTRY_RESERVED   EQU     18h             ; 10 bytes
CTRY_SIZE       EQU     22h

CTRY_USA        EQU     1               ; the one country code answered

;---------------------------------------------------------------------
; The real-mode PSP a DOS program is entitled to.  Same 100h bytes
; DOS built; the fields it actually reads are the memory top, the
; saved vectors, the parent, the environment and the command tail.
;---------------------------------------------------------------------
RSP_INT20       EQU     00h                     ; dw 20CDh
RSP_MEMTOP      EQU     02h                     ; dw - first paragraph past us
RSP_DISP        EQU     05h                     ; far call to the DOS dispatcher
RSP_TERM        EQU     0Ah                     ; dd - saved INT 22h
RSP_CTRLC       EQU     0Eh                     ; dd - saved INT 23h
RSP_CRITERR     EQU     12h                     ; dd - saved INT 24h
RSP_PARENT      EQU     16h                     ; dw - parent PSP segment
RSP_JFT         EQU     18h                     ; 20 bytes, FFh = closed
RSP_ENV         EQU     2Ch                     ; dw - environment segment
RSP_JFTLEN      EQU     32h                     ; dw - 20
RSP_JFTPTR      EQU     34h                     ; dd - far pointer to the JFT
RSP_DOSCALL     EQU     50h                     ; INT 21h / RETF
RSP_FCB1        EQU     5Ch
RSP_FCB2        EQU     6Ch
RSP_TAIL        EQU     80h                     ; len, text, 0Dh
RSP_SIZE        EQU     100h

;---------------------------------------------------------------------
; The MZ header, at the front of every DOS .EXE.
;
; Two fields are easy to get wrong.  CBLP is the number of valid bytes
; in the LAST page, and zero means the last page is FULL, not empty.
; MAXALLOC is what the program would LIKE and is 0FFFFh in almost every
; file ever linked, which is why "allocate maxalloc" always fails and
; the loader is really driven by MINALLOC.
;---------------------------------------------------------------------
EXE_SIG         EQU     00h                     ; 'MZ', or 'ZM' from some
EXE_CBLP        EQU     02h                     ; bytes used in the last page
EXE_CP          EQU     04h                     ; pages of 512, last included
EXE_CRLC        EQU     06h                     ; relocation entries
EXE_CPARHDR     EQU     08h                     ; header size in paragraphs
EXE_MINALLOC    EQU     0Ah                     ; paragraphs needed past the image
EXE_MAXALLOC    EQU     0Ch                     ; paragraphs wanted past the image
EXE_SS          EQU     0Eh                     ; stack segment, to be relocated
EXE_SP          EQU     10h
EXE_CSUM        EQU     12h                     ; nobody has ever checked this
EXE_IP          EQU     14h
EXE_CS          EQU     16h                     ; code segment, to be relocated
EXE_LFARLC      EQU     18h                     ; file offset of the fixup table
EXE_OVNO        EQU     1Ah                     ; overlay number, 0 = the program
EXE_HDRLEN      EQU     1Ch                     ; what we read to decide

EXE_MZ          EQU     5A4Dh                   ; 'M','Z' as a little-endian word
EXE_ZM          EQU     4D5Ah

;---------------------------------------------------------------------
; 21h/F1h AL=8 - one block of one memory arena, in a shape that does
; not depend on which arena it came out of.
;
; Both chains are walked by index rather than by following pointers,
; because the caller can no longer follow one: a program's whole view
; of memory is its own slot now.  A walker asks for block 0, then 1,
; and stops when it is told there is no such block.
;---------------------------------------------------------------------
F8_SIG          EQU     0               ; dd - 'M' used, 'Z' free, 'E' end
F8_SIZE         EQU     4               ; dd - payload bytes
F8_OWNER        EQU     8               ; dd - owner id, or PSP segment in
                                        ;      the conventional arena
F8_ADDR         EQU     12              ; dd - where the block is, flat
F8_LEN          EQU     16

F8_ARENA        EQU     0               ; DL: the kernel's, above 2MB
F8_CONV         EQU     1               ; DL: the one a DOS box lives in
F8_UPPER        EQU     2               ; DL: the upper half of that same
                                        ; chain, when DOS=UMB built one

;---------------------------------------------------------------------
; How a V86 DOS call's pointer arguments have to be translated.  The
; guest says DS:DX; the kernel's own handlers want one flat address.
;---------------------------------------------------------------------
P_DX            EQU     01h                     ; DS:DX
P_SI            EQU     02h                     ; DS:SI
P_DI            EQU     04h                     ; ES:DI
P_BX            EQU     08h                     ; ES:BX

;---------------------------------------------------------------------
; EFLAGS bits the monitor cares about
;---------------------------------------------------------------------
EFL_IF          EQU     00000200h
EFL_TF          EQU     00000100h
EFL_OF          EQU     00000800h
EFL_IOPL        EQU     00003000h
EFL_NT          EQU     00004000h
EFL_VM          EQU     00020000h

; The flags a V86 guest may own: CF PF AF ZF SF DF OF.  IF is
; virtualised, IOPL and VM are the monitor's, and TF is deliberately
; left out until there is a debugger that wants it - a guest that sets
; it by accident would otherwise single-step into the monitor forever.
; NT is left out too, and for a sharper reason: NT turns IRET into a
; task switch, and a guest is never allowed to arrange one of those.
V86_UFLAGS      EQU     00000CD5h

; ...but NOT OWNING A FLAG AND NOT SEEING IT ARE DIFFERENT THINGS, and
; the monitor used to confuse them.  IOPL and NT must never reach the
; real EFLAGS a guest runs under - that part above is right - yet a
; program is entitled to read back what it wrote, because that is how
; every 286-versus-386 test ever written works: set bits 12 to 14 with
; POPF, read them back with PUSHF, and conclude 286 if they would not
; stick.  Masking them out of the ANSWER as well made a 486 report
; itself as a 286, which is what Windows for Workgroups 3.11 was
; complaining about when it said it required an 80386.
;
; So they are virtualised exactly as IF already was: kept in a shadow
; the monitor owns, folded back into every flags image the guest is
; shown, and never once written to the flags the CPU runs on.
V86_VFLAGS      EQU     EFL_IOPL OR EFL_NT

;---------------------------------------------------------------------
; V86 register context, passed to IOS_V86RUN in ESI.
;
; The first eight dwords are in PUSHAD order on purpose: the monitor
; saves the guest's registers with one PUSHAD and copies the block
; straight out.  ESP there is the ring-0 ESP and is overwritten with
; the guest's own SP, which lives in the interrupt frame.
;---------------------------------------------------------------------
V86C_EDI        EQU     00h
V86C_ESI        EQU     04h
V86C_EBP        EQU     08h
V86C_ESP        EQU     0Ch
V86C_EBX        EQU     10h
V86C_EDX        EQU     14h
V86C_ECX        EQU     18h
V86C_EAX        EQU     1Ch
V86C_EIP        EQU     20h
V86C_CS         EQU     24h
V86C_FLAGS      EQU     28h
V86C_SS         EQU     2Ch
V86C_ES         EQU     30h
V86C_DS         EQU     34h
V86C_FS         EQU     38h
V86C_GS         EQU     3Ch
V86C_TRAP       EQU     40h                     ; 32-byte bitmap, one bit
                                                ; per vector: set = stop and
                                                ; hand it to the caller,
                                                ; clear = reflect to the IVT
V86C_SIZE       EQU     60h

; Why IOS_V86RUN stopped
V86R_INT        EQU     1                       ; EBX = vector
V86R_HLT        EQU     2
V86R_FAULT      EQU     3                       ; EBX = exception vector
V86R_FLTLOOP    EQU     4                       ; EBX = a vector the guest's own
                                                ; handler keeps taking at one
                                                ; address without getting past
                                                ; it - see vf_exc

;---------------------------------------------------------------------
; The VDM's nesting stack.  A DOS box used to be one program: one
; context, one PSP, one owner id, all of them single variables.  A
; guest that EXECs a child needs two of each and a way back, so the
; live values stay where they were - vdm_ctx, vdm_pspseg and the rest,
; which every other routine still names directly - and this is where
; the PARENT's copies wait while the child runs.
;
; Pushed on the way in and popped on the way out, so the singletons
; always describe the program actually executing.  Nothing else in the
; VDM had to learn about nesting.
;---------------------------------------------------------------------
VDM_MAXNEST     EQU     8                       ; the same depth as EXEC_MAX

VF_CTX          EQU     000h                    ; V86C_SIZE bytes
VF_OWN          EQU     060h                    ; dd - owner id to restore
VF_DTA          EQU     064h                    ; dd - and the DTA
VF_PSPSEG       EQU     068h                    ; dw
VF_ENVSEG       EQU     06Ah                    ; dw
VF_PARAS        EQU     06Ch                    ; dw
VF_CURPSP       EQU     06Eh                    ; dw
VF_PARPSP       EQU     070h                    ; dw
VF_LEN          EQU     080h

;---------------------------------------------------------------------
; 21h/4Bh - the EXEC parameter block, at ES:BX.  Unchanged since 1983,
; and the two FCB pointers are still in it even though almost nothing
; passes anything but a pair of empty ones.
;---------------------------------------------------------------------
EP_ENV          EQU     00h                     ; dw - environment segment,
                                                ; 0 = inherit the parent's
EP_TAIL         EQU     02h                     ; dd - far ptr, command tail
EP_FCB1         EQU     06h                     ; dd - far ptr
EP_FCB2         EQU     0Ah                     ; dd - far ptr

; 21h/4Dh - how the last program ended, in AH
VDX_NORMAL      EQU     0
VDX_CTRLC       EQU     1
VDX_CRITERR     EQU     2
VDX_RESIDENT    EQU     3                       ; it went TSR

XFER_BUF        EQU     00002000h               ; below 1M, for BIOS transfers
XFER_SEG        EQU     0200h                   ; ...as a real-mode segment
XFER_SIZE       EQU     00002000h               ; 8K = 16 sectors

; SP, not a buffer: the stack runs DOWN from here to 0530h, which is
; where the thunk's register frame ends.  It used to be 7C00h, which
; put twenty-five kilobytes of untouched stack in the middle of what a
; DOS program wanted, purely because 7C00h is the number a boot sector
; starts life with and nobody had asked since.
RM_STACK_TOP    EQU     1000h                   ; real-mode SP (SS=0)

;---------------------------------------------------------------------
; Boot-time loader (BOOT\FATLOAD.INC, shared by BOOT2 and PMIO).
;
; A FAT32 volume boot record cannot hold a FAT-walking loader in 512
; bytes, so the hard-disk VBR is only stage 1: it reads S2_SECTORS
; sectors out of the partition's RESERVED area - not a file, so no
; chicken-and-egg - and jumps to them.  Microsoft's own FAT32 boot
; record does the same thing with its sector 2.
;---------------------------------------------------------------------
S2_BASE         EQU     00008000h               ; stage 2 runs at 0000:8000
S2_SECTORS      EQU     8                       ; 4K of it
S2_START16      EQU     1                       ; FAT16: reserved sector 1
S2_START32      EQU     9                       ; FAT32: after FSInfo/backup

; ALL THREE OF THESE LIVE INSIDE THE TRANSIENT PROGRAM AREA, and that
; is deliberate.  They are boot-only: the FAT window and the directory
; sector are how PMIO finds PMDOS.SYS, and the E820 table is collected
; before the switch to protected mode.  Every one of them is finished
; with by the time lm_init lays the first memory control block over
; the top of them, and long before any DOS program is loaded.
;
; The cost of the tidier-looking alternative - a reserved area below
; the TPA - is 2.25K of conventional memory held for the life of the
; machine to store something nothing ever reads again.
;
; They have moved before, and it went badly: at B000h and D000h the
; FAT window ended up exactly where the relocated stub had just been
; put, and overwrote the code reading it on the first cluster of the
; first file.  So: anything here must be checked against the stub, the
; XFER buffer and the boot sector at 7C00h, not just against the TPA.
FL_FATBUF       EQU     00004000h               ; 2-sector FAT window
FL_DIRBUF       EQU     00004400h               ; one directory sector
FL_BPBSEG       EQU     07C0h                   ; the VBR, still resident

MEMMAP          EQU     00004600h               ; E820 entries land here
MEMMAP_MAX      EQU     32                      ; 32 * 24 = 768 bytes

;---------------------------------------------------------------------
; Thunk interface (see RMCALL.INC / IORM.ASM)
;---------------------------------------------------------------------
RMSAVE_ESP      EQU     000004E8h               ; caller's 32-bit ESP
RMRET           EQU     000004F0h               ; dd offset32, dw selector
RMFRAME         EQU     00000500h               ; RMR_* offsets below

;---------------------------------------------------------------------
; The two DOS status flags, and why they live down here in the BIOS
; data area rather than in the kernel's own data with everything else.
;
; 21h/34h hands a program the ADDRESS of the InDOS flag, and the
; program that asks is nearly always a TSR that will read it from
; inside an interrupt to decide whether calling DOS right now would
; re-enter it.  A 16-bit guest can only hold that address as a real-
; mode far pointer, so the flag has to be somewhere a real-mode far
; pointer can reach - and the kernel's data segment is above 1MB.
;
; They go at the tail of the BDA's user area, which the thunk's return
; address already occupies, rather than into the transient program
; area: a paragraph taken out of the TPA would be a paragraph off
; every DOS box AND a stray block in the MCB chain that MEM would
; report and nobody could account for.
;
; The critical-error flag is at InDOS-1 ON PURPOSE.  That adjacency is
; not a coincidence in real DOS either, and a great deal of TSR code
; finds the error flag by subtracting one from what 34h returned
; rather than by calling 5D06h.
;---------------------------------------------------------------------
DOS_ERRFLG      EQU     000004F6h               ; byte - in a critical error
DOS_INDOS       EQU     000004F7h               ; byte - INT 21h nesting depth
RMT_PM2RM       EQU     0010h                   ; entry in the RM stub

; Register frame layout. Deliberately in PUSHAD order so the thunk can
; POPAD straight out of it.
RMR_EDI         EQU     RMFRAME + 00h
RMR_ESI         EQU     RMFRAME + 04h
RMR_EBP         EQU     RMFRAME + 08h
RMR_ESP         EQU     RMFRAME + 0Ch           ; ignored
RMR_EBX         EQU     RMFRAME + 10h
RMR_EDX         EQU     RMFRAME + 14h
RMR_ECX         EQU     RMFRAME + 18h
RMR_EAX         EQU     RMFRAME + 1Ch
RMR_FLAGS       EQU     RMFRAME + 20h
RMR_ES          EQU     RMFRAME + 24h
RMR_DS          EQU     RMFRAME + 26h
RMR_INTNO       EQU     RMFRAME + 28h           ; byte
RMFRAME_LEN     EQU     2Ch

;---------------------------------------------------------------------
; GDT selectors.  The GDT lives inside the real-mode stub at IO_BASE+0C00h
; so it is valid in real mode, 16-bit protected mode and 32-bit flat mode.
;---------------------------------------------------------------------
SEL_NULL        EQU     0000h
SEL_CODE32      EQU     0008h                   ; base 0, limit 4G, 32-bit
SEL_DATA32      EQU     0010h                   ; base 0, limit 4G, 32-bit
SEL_CODE16      EQU     0018h                   ; base IO_BASE, limit 64K
SEL_DATA16      EQU     0020h                   ; base 0, limit 64K
SEL_TSS         EQU     0028h                   ; the one task, for V86
SEL_LDT         EQU     0030h                   ; the DPMI client's own table

; The running program's own two, and the RPL is part of the number: a
; native program is ring-3 code and these selectors are never loaded by
; anything else.  Their base and limit describe whichever slot is
; running and are rewritten by EXEC - one pair, because one program
; runs at a time.  Index 7 and 8, so SEL_PROG_CS is 0038h + RPL 3.
SEL_PROG_CS     EQU     003Bh
SEL_PROG_DS     EQU     0043h
GDT_IDX_PROGCS  EQU     7
GDT_IDX_PROGDS  EQU     8
GDT_ENTRIES     EQU     9

GDT_OFF         EQU     0C00h                   ; offset within the RM stub
GDT_BASE        EQU     IO_BASE + GDT_OFF

;---------------------------------------------------------------------
; Globals published by the real-mode stub, read by everything later.
;---------------------------------------------------------------------
GV_OFF          EQU     0F00h
GV_BASE         EQU     IO_BASE + GV_OFF

GV_BOOTDRV      EQU     GV_BASE + 00h           ; byte  - BIOS drive number
GV_DOSSIZE      EQU     GV_BASE + 04h           ; dword - bytes of PMDOS.SYS
GV_MEMKB        EQU     GV_BASE + 08h           ; dword - KB of extended memory
GV_E820CNT      EQU     GV_BASE + 0Ch           ; dword - E820 entry count
GV_MEMTOP       EQU     GV_BASE + 10h           ; dword - highest usable linear

; CR3, or zero while paging is off.  It lives out here rather than in
; IOPM because the pm2rm thunk is the one piece of code that has to
; know: paging MUST be off before protected mode is left, and back on
; before the caller is resumed.  GVO_* is the same slot addressed as
; an offset in SEL_DATA16, which is what the thunk runs with.
GV_PGDIR        EQU     GV_BASE + 14h           ; dword - CR3, 0 = not paging
GVO_PGDIR       EQU     GV_OFF  + 14h

;---------------------------------------------------------------------
; PMIO service table.  IOPM.BIN begins with a JMP to its entry point,
; then a table of near function pointers at IO_PM_BASE+10h.  PMDOS
; reaches PMIO only through this table, so the two files stay
; independently linkable.
;
; Call them as:  call dword ptr [IOS_xxx]
;---------------------------------------------------------------------
IOSVC           EQU     IO_PM_BASE + 10h

IOS_CONOUT      EQU     IOSVC + 00h             ; AL = char
IOS_CONSTR      EQU     IOSVC + 04h             ; ESI -> ASCIIZ
IOS_CONIN       EQU     IOSVC + 08h             ; -> AL (blocks), AH = scan
IOS_CONSTAT     EQU     IOSVC + 0Ch             ; -> ZF set if no key waiting
IOS_CLS         EQU     IOSVC + 10h
IOS_SETCUR      EQU     IOSVC + 14h             ; DH = row, DL = col
IOS_GETCUR      EQU     IOSVC + 18h             ; -> DH = row, DL = col
IOS_RDBLK       EQU     IOSVC + 1Ch             ; AL=drv EBX=lba CX=cnt EDI=buf
IOS_WRBLK       EQU     IOSVC + 20h             ; ditto
IOS_REALINT     EQU     IOSVC + 24h             ; AL = int number, RMFRAME set
IOS_TICKS       EQU     IOSVC + 28h             ; -> EAX = 18.2Hz tick count
IOS_DRVPARM     EQU     IOSVC + 2Ch             ; AL=drv -> EBX=sectors, CX=bps
IOS_SETVEC      EQU     IOSVC + 30h             ; AL=vec EDI=handler
IOS_PANIC       EQU     IOSVC + 34h             ; ESI -> ASCIIZ, never returns
IOS_V86RUN      EQU     IOSVC + 38h             ; ESI -> V86 context
                                                ; -> EAX = V86R_*, EBX = vector
IOS_V86INT      EQU     IOSVC + 3Ch             ; ESI -> context, AL = vector:
                                                ; reflect it into the guest's
                                                ; own handler, on the CONTEXT
IOS_A20         EQU     IOSVC + 40h             ; AL = 0 wrap / 1 flat
IOS_A20GET      EQU     IOSVC + 44h             ; -> AL = the gate as it is
IOS_DRVGEO      EQU     IOSVC + 48h             ; AL=drv -> CX=sectors/track,
                                                ; DX=heads, EBX=total sectors.
                                                ; The BIOS's CHS view of the
                                                ; disk, which is the one a DOS
                                                ; program's INT 13h expects -
                                                ; IOS_DRVPARM answers with the
                                                ; true capacity instead.
IOS_SCRSIZE     EQU     IOSVC + 4Ch             ; -> DH = rows, DL = columns,
                                                ; as the screen is now and not
                                                ; as it was when we booted
IOS_PM3HOOK     EQU     IOSVC + 50h             ; EDI = what to call when a
                                                ; ring-3 DPMI client faults on
                                                ; something the monitor cannot
                                                ; emulate.  PMIO knows how to
                                                ; catch it; only PMDOS knows
                                                ; there is a DOS box to end
                                                ; NOW, which is not always
                                                ; what XMS last asked for: a
                                                ; guest can move it itself
                                                ; through port 92h or the 8042
IOS_EXCHOOK     EQU     IOSVC + 54h             ; EDI = what to call after a
                                                ; RING-0 fault has been
                                                ; reported.  Same division of
                                                ; labour as PM3HOOK: PMIO
                                                ; catches the fault and says
                                                ; what it was, and only PMDOS
                                                ; knows there is a shell to
                                                ; go back to.  Called with
                                                ; interrupts off and a dead
                                                ; stack; it must not return
IOS_EXCCLEAR    EQU     IOSVC + 58h             ; the DOS side is back at a
                                                ; prompt: arm the recovery
                                                ; again for the next fault
IOS_NULLTRAP    EQU     IOSVC + 5Ch             ; AL = 1 arm the null-pointer
                                                ; guard, 0 disarm it.  See
                                                ; NULLTRAP.INC: it is four debug
                                                ; registers, not a page, and the
                                                ; comment there says why it had
                                                ; to be
IOS_NULLGET     EQU     IOSVC + 60h             ; -> AL = armed as it stands
IOS_KBBOX       EQU     IOSVC + 64h             ; a fresh DOS box: throw away
                                                ; any scan codes queued for the
                                                ; last one.  See kb_boxreset -
                                                ; a key typed at a program that
                                                ; never read it must not be
                                                ; delivered to the next one
IOS_PGMAP       EQU     IOSVC + 68h             ; EAX = linear, EDX = physical,
                                                ; ECX = pages.  Repoints that
                                                ; many page table entries and
                                                ; flushes.  The ONLY departure
                                                ; from the identity map apart
                                                ; from the A20 alias, and the
                                                ; reason UMB.INC can exist
IOS_UMBPROBE    EQU     IOSVC + 6Ch             ; EDI -> UMB_SCAN_PAGES bytes.
                                                ; Fills in one UMBP_* for each
                                                ; 4K page of C0000h-EFFFFh

;---------------------------------------------------------------------
; The three ports the V86 monitor takes away from the guest.  Two of
; them are the 8042 keyboard controller, whose "output port" carries
; the A20 line on bit 1 along with the CPU reset line on bit 0, and
; the third is the PS/2 system control port, which carries the same
; two signals in the same order and was invented to make the 8042
; route unnecessary.  Every A20 routine ever written uses one or both.
;---------------------------------------------------------------------
PORT_KBDATA     EQU     60h                     ; 8042 data
PORT_KBCMD      EQU     64h                     ; 8042 command / status
PORT_SYSCTL     EQU     92h                     ; PS/2 system control port A

KBC_RDOUT       EQU     0D0h                    ; read output port
KBC_WROUT       EQU     0D1h                    ; write output port
KBC_A20OFF      EQU     0DDh                    ; and the two shortcuts some
KBC_A20ON       EQU     0DFh                    ; chipsets answer directly

; The 8042 commands a DOS box may NOT have, and the two whose data
; byte has to be followed to know whose the next write to port 60h is.
; See vio_o64: the keyboard belongs to the kernel and the rest of the
; controller does not, so the line is drawn command by command.
KBC_WRCMD       EQU     060h                    ; write the command byte -
                                                ; one more byte to 60h follows
KBC_WRAUX       EQU     0D4h                    ; write to the AUXILIARY
                                                ; device: likewise, and that
                                                ; byte is the mouse's
KBC_KBDOFF      EQU     0ADh                    ; disable the keyboard...
KBC_KBDON       EQU     0AEh                    ; ...and enable it again
KBC_PULSE       EQU     0F0h                    ; F0h-FFh pulse the output
                                                ; lines, and FEh is the one
                                                ; that resets the CPU
KBS_OBF         EQU     001h                    ; status: a byte is waiting
KBS_AUXB        EQU     020h                    ; ...and it came from the mouse
KBCB_KBDINT     EQU     001h                    ; command byte: IRQ 1 enabled
KBCB_KBDOFF     EQU     010h                    ; ...and the keyboard disabled

;---------------------------------------------------------------------
; 8259A PIC.  Remapped away from the CPU exception range.
;---------------------------------------------------------------------
PIC1_CMD        EQU     20h
PIC1_DATA       EQU     21h
PIC2_CMD        EQU     0A0h
PIC2_DATA       EQU     0A1h
; Keep hardware IRQs clear of both the CPU exceptions (00h-1Fh) AND the
; DOS software interrupts (20h-2Fh) - an IRQ base of 20h would put the
; keyboard IRQ on vector 21h, right where the DOS API gate goes.
IRQ_BASE        EQU     50h                     ; IRQ0..7  -> INT 50h..57h
IRQ_BASE2       EQU     58h                     ; IRQ8..15 -> INT 58h..5Fh

;---------------------------------------------------------------------
; Console
;
; SCR_COLS and SCR_ROWS are the DEFAULTS, not the truth: the console
; takes its geometry from the BIOS data area at boot and again whenever
; it is asked to draw, so a guest that sets 40 columns or 50 lines is
; followed rather than scribbled over.  They are what the driver falls
; back to when the BDA holds something impossible.
;---------------------------------------------------------------------
VGA_TEXT        EQU     000B8000h               ; colour text page 0
VGA_MONO        EQU     000B0000h               ; ...and the MDA's
SCR_COLS        EQU     80
SCR_ROWS        EQU     25
SCR_MAXCOLS     EQU     132                     ; sanity bounds on the BDA
SCR_MAXROWS     EQU     60
SCR_ATTR        EQU     07h                     ; light grey on black
CRTC_IDX        EQU     3D4h
CRTC_DAT        EQU     3D5h

;---------------------------------------------------------------------
; Floppy controller.  The kernel does not drive the FDC - the BIOS
; does - but it must be able to turn the MOTOR off, because the code
; that normally does that is the BIOS timer tick and PM-DOS owns the
; timer.  DOR_OFF is the idle value: controller out of reset, DMA and
; IRQ enabled, drive 0 selected, all four motor bits clear.
;---------------------------------------------------------------------
FDC_DOR         EQU     3F2h                    ; digital output register
FDC_DOR_OFF     EQU     0Ch
TICKS_PER_DAY   EQU     001800B0h               ; 18.2 Hz x 24 hours

;---------------------------------------------------------------------
; BIOS data area.  The console used to keep its cursor to itself, which
; was fine while the kernel was the only thing writing to the screen -
; and stopped being fine the moment a DOS box could call INT 10h, since
; that goes to the real BIOS, which moves the cursor HERE.  These are
; the fields both halves have to agree about.
;---------------------------------------------------------------------
; The three below are maintained by the BIOS's own INT 8, which in
; PM-DOS NEVER RUNS: the kernel owns IRQ 0 in protected mode and the
; real-mode handler is only reached during an excursion.  Everything
; the BIOS timer tick was responsible for is therefore ours, and
; irq_timer does it - see EXCEPT.INC.
BDA_BASE        EQU     00000400h
BDA_MOTORST     EQU     BDA_BASE + 3Fh          ; db - floppy motors running
BDA_MOTORCNT    EQU     BDA_BASE + 40h          ; db - ticks until motor off
BDA_EQUIP       EQU     BDA_BASE + 10h          ; dw - equipment word
BDA_DSKSTAT     EQU     BDA_BASE + 41h          ; db - last INT 13h status
BDA_TICKS       EQU     BDA_BASE + 6Ch          ; dd - 18.2 Hz since midnight
BDA_TICKROLL    EQU     BDA_BASE + 70h          ; db - and its 24-hour flag
BDA_VMODE       EQU     BDA_BASE + 49h          ; db - current video mode
BDA_VCOLS       EQU     BDA_BASE + 4Ah          ; dw - columns on screen
BDA_VPGSZ       EQU     BDA_BASE + 4Ch          ; dw - bytes per video page
BDA_VPGOFF      EQU     BDA_BASE + 4Eh          ; dw - offset of the live page
BDA_CURPOS      EQU     BDA_BASE + 50h          ; 8 words - col,row per page
BDA_CURSHAPE    EQU     BDA_BASE + 60h          ; dw - end,start scan lines
BDA_VPAGE       EQU     BDA_BASE + 62h          ; db - the displayed page
BDA_HDCOUNT     EQU     BDA_BASE + 75h          ; db - hard disks the BIOS found
BDA_VROWS       EQU     BDA_BASE + 84h          ; db - rows on screen, LESS ONE

; THE KEYBOARD BUFFER, and it belongs to the machine for the same
; reason the tick count does.  Owning IRQ 1 means the BIOS's own
; keyboard interrupt never runs, so everything that handler was
; responsible for stops happening - and a 16-bit program asking INT 16h
; for a key is asking the BIOS to look at exactly these bytes.  See
; kb_put.
;
; The two pointers are OFFSETS FROM SEGMENT 40h, not addresses, which
; is why they hold 1Eh and not 41Eh: that is the form every program
; that reads them expects, and the form the ROM writes.  Head is where
; the next key is taken FROM and tail is where the next one is put, so
; equal means empty - the opposite way round from the ring this driver
; used to keep, which is a good enough reason to have deleted that one.
BDA_MEMSIZE     EQU     BDA_BASE + 13h          ; dw - KB of conventional
                                                ; memory, which is NOT always
                                                ; 640: a BIOS with an extended
                                                ; data area takes the top of it
                                                ; and says so here.  lm_init
BDA_KBFLAG      EQU     BDA_BASE + 17h          ; db - shift state
BDA_KBFLAG2     EQU     BDA_BASE + 18h          ; db - ...and the rest of it
BDA_KBHEAD      EQU     BDA_BASE + 1Ah          ; dw - next key to read
BDA_KBTAIL      EQU     BDA_BASE + 1Ch          ; dw - next free slot
BDA_KBBUF       EQU     BDA_BASE + 1Eh          ; 16 words of (ASCII, scan)
BDA_KBSTART     EQU     BDA_BASE + 80h          ; dw - where the ring begins
BDA_KBEND       EQU     BDA_BASE + 82h          ; dw - and one past its end
KB_RINGLO       EQU     1Eh                     ; the two values those hold
KB_RINGHI       EQU     3Eh

; Where a 16-bit program's keyboard handler is found: the slot for
; vector 9 in the real-mode IVT at 0:0, which is a linear address and
; not a segment:offset because the kernel is flat.  Both halves of the
; IRQ 1 story read it - the driver to ask whether a DOS box has hooked
; it, the monitor to find the handler to deliver into.
IVT_INT09       EQU     9 * 4

; The bits of BDA_KBFLAG a program reads.  Only the four this driver
; actually tracks are written; the lock lights are not simulated, so
; claiming their bits would be worse than leaving them alone.
BDAK_RSHIFT     EQU     01h
BDAK_LSHIFT     EQU     02h
BDAK_CTRL       EQU     04h
BDAK_ALT        EQU     08h
BDAK_CAPS       EQU     40h

;---------------------------------------------------------------------
; INT 13h.  Status codes are the BIOS's own, in AH, and the ones below
; are the only ones this kernel ever raises.
;---------------------------------------------------------------------
D13_OK          EQU     00h
D13_BADCMD      EQU     01h                     ; invalid function
D13_BADSEC      EQU     04h                     ; sector not found
D13_BADBUF      EQU     09h                     ; the classic "data boundary
                                                ; error" - here, a buffer
                                                ; outside the DOS box
D13_WRPROT      EQU     03h                     ; write protected
D13_BADDRV      EQU     0Ch                     ; unsupported track/drive
D13_GENERAL     EQU     20h                     ; controller failure
D13_NOTREADY    EQU     0AAh                    ; drive not ready

; AH=15h - what kind of drive DL is
D13_T_NONE      EQU     00h
D13_T_FLOPPY    EQU     01h                     ; no change-line support
D13_T_CHGLINE   EQU     02h                     ; floppy with a change line
D13_T_FIXED     EQU     03h                     ; and a hard disk

; AH=41h-48h - the EDD extensions.  A disk address packet at DS:SI.
DAP_SIZE        EQU     00h                     ; db - packet length, 10h/18h
DAP_COUNT       EQU     02h                     ; dw - sectors to move
DAP_BUF         EQU     04h                     ; dd - far ptr, the buffer
DAP_LBA         EQU     08h                     ; dq - the starting sector
DAP_LEN         EQU     10h

EDD_MAGIC       EQU     55AAh                   ; BX going in ...
EDD_REPLY       EQU     0AA55h                  ; ... and coming back
EDD_VER11       EQU     21h                     ; EDD 1.1
EDD_CAP_EXT     EQU     0001h                   ; extended disk access

; AH=48h - the drive parameter block the caller supplies at DS:SI
DP_SIZE         EQU     00h                     ; dw - buffer length in
DP_FLAGS        EQU     02h                     ; dw - information flags
DP_CYLS         EQU     04h                     ; dd - cylinders
DP_HEADS        EQU     08h                     ; dd - heads
DP_SPT          EQU     0Ch                     ; dd - sectors per track
DP_TOTAL        EQU     10h                     ; dq - total sectors
DP_BPS          EQU     18h                     ; dw - bytes per sector
DP_MINLEN       EQU     1Ah                     ; the v1.x block, all of it

;---------------------------------------------------------------------
; INT 21h error codes (subset of the real DOS list)
;---------------------------------------------------------------------
ERR_FUNC        EQU     1                       ; invalid function
ERR_NOFILE      EQU     2                       ; file not found
ERR_NOPATH      EQU     3                       ; path not found
ERR_NOHANDLES   EQU     4                       ; too many open files
ERR_ACCESS      EQU     5                       ; access denied
ERR_BADHANDLE   EQU     6                       ; invalid handle
ERR_NOMEM       EQU     8                       ; insufficient memory
ERR_BADBLOCK    EQU     9                       ; memory block address invalid
ERR_BADMCB      EQU     7                       ; memory control block destroyed
ERR_EXEFMT      EQU     11                      ; invalid executable format
ERR_BADDRIVE    EQU     15                      ; invalid drive
ERR_BADFUNC     EQU     16h                     ; unknown command
ERR_NOTSAME     EQU     11h                     ; not same device (56h)
ERR_EXISTS      EQU     80                      ; file already exists
ERR_DISKFULL    EQU     70h                     ; (PM-DOS: no free clusters)

;---------------------------------------------------------------------
; INT 24h - the critical-error interface, shared by the kernel that
; raises one and the handler that answers it.
;
; AH carries the flags below, AL the failing drive (0 = A:), DI the
; error code, and ESI the device header - DOS passed BP:SI and a flat
; pointer is the same address, so EBP is left zero.  The handler
; answers in AL with one of the CERR_* values.
;---------------------------------------------------------------------
CE_WRITE        EQU     01h             ; clear = read, set = write
CE_AREA_FAT     EQU     02h             ; bits 2-1: 0 DOS, 1 FAT,
CE_AREA_DIR     EQU     04h             ;   2 root directory, 3 data
CE_AREA_DATA    EQU     06h
CE_AREA         EQU     06h             ; the mask over both bits
CE_FAIL_OK      EQU     08h             ; the handler may answer fail
CE_RETRY_OK     EQU     10h             ; ...retry
CE_IGNORE_OK    EQU     20h             ; ...ignore
CE_CHARDEV      EQU     80h             ; a character device, not a disk

CERR_IGNORE     EQU     0
CERR_RETRY      EQU     1
CERR_ABORT      EQU     2
CERR_FAIL       EQU     3

; Error codes as the handler sees them in DI.
CERR_WRPROT     EQU     00h             ; write protect violation
CERR_UNIT       EQU     01h             ; unknown unit
CERR_NOTREADY   EQU     02h             ; drive not ready
CERR_CMD        EQU     03h             ; unknown command
CERR_DATA       EQU     04h             ; data error (CRC)
CERR_LENGTH     EQU     05h             ; bad request structure length
CERR_SEEK       EQU     06h             ; seek error
CERR_MEDIA      EQU     07h             ; unknown media type
CERR_SECTOR     EQU     08h             ; sector not found
CERR_PAPER      EQU     09h             ; printer out of paper
CERR_WRFAULT    EQU     0Ah             ; write fault
CERR_RDFAULT    EQU     0Bh             ; read fault
CERR_GENERAL    EQU     0Ch             ; general failure
CERR_MAXCODE    EQU     0Ch

;---------------------------------------------------------------------
; 21h/7303h - extended free space.  The caller passes EDX -> a path
; naming the drive, EDI -> a record of this shape and ECX = its size.
;
; It exists because 36h answers in 16-bit registers and therefore
; saturates at 65535 clusters, which had a 40MB volume reporting 32MB
; and is why FORMAT works its own numbers out.  Every field here is
; 32 bits.  With no compression anywhere in PM-DOS the "physical" and
; "allocation unit" fields repeat the cluster ones, which is what a
; system that compresses nothing is supposed to say.
;---------------------------------------------------------------------
F73_LEVEL       EQU     00h             ; dw - structure version, 0
F73_SPC         EQU     02h             ; dd - sectors per cluster
F73_BPS         EQU     06h             ; dd - bytes per sector
F73_FREECL      EQU     0Ah             ; dd - available clusters
F73_TOTCL       EQU     0Eh             ; dd - total clusters
F73_FREESEC     EQU     12h             ; dd - available physical sectors
F73_TOTSEC      EQU     16h             ; dd - total physical sectors
F73_FREEAU      EQU     1Ah             ; dd - available allocation units
F73_TOTAU       EQU     1Eh             ; dd - total allocation units
F73_SIZE        EQU     2Ch             ; ...and eight reserved bytes

;---------------------------------------------------------------------
; The MBR partition table: four entries of sixteen bytes at 1BEh, and
; the same layout again inside every extended boot record.
;
; PE_START and PE_SECS are what anything should actually use.  The CHS
; fields are there because the table has always had them and because a
; 1983 disk fills them in; they cannot describe anything past 8GB and
; on a modern disk they are frozen at their maximum, so a driver that
; believes them is a driver that stops working at 504MB.
;---------------------------------------------------------------------
PE_BOOT         EQU     00h                     ; byte - 80h = active
PE_CHSBEG       EQU     01h                     ; 3 bytes - head, sec, cyl
PE_TYPE         EQU     04h                     ; byte - see PT_* below
PE_CHSEND       EQU     05h                     ; 3 bytes
PE_START        EQU     08h                     ; dd - LBA, relative to the
                                                ; disk for a primary and to
                                                ; the EXTENDED CONTAINER for
                                                ; a logical
PE_SECS         EQU     0Ch                     ; dd - length in sectors
PE_LEN          EQU     10h
PART_OFF        EQU     1BEh                    ; where the table starts
PART_MAX        EQU     4
MBR_SIG         EQU     0AA55h

;---------------------------------------------------------------------
; Partition types.  The ones DOS gave a drive letter to, the two that
; hold a chain of more partitions, and the "hidden" spellings that a
; multi-boot manager sets to make a volume disappear - which is a
; request, and is honoured by not assigning a letter.
;---------------------------------------------------------------------
PT_EMPTY        EQU     00h
PT_FAT12        EQU     01h
PT_FAT16S       EQU     04h                     ; FAT16, under 32MB
PT_EXTENDED     EQU     05h                     ; container, CHS
PT_FAT16B       EQU     06h                     ; FAT16, 32MB and over
PT_FAT32        EQU     0Bh
PT_FAT32L       EQU     0Ch                     ; ...addressed by LBA
PT_FAT16L       EQU     0Eh                     ; FAT16B addressed by LBA
PT_EXTENDEDL    EQU     0Fh                     ; container, LBA
PT_HID_FAT12    EQU     11h
PT_HID_FAT16S   EQU     14h
PT_HID_FAT16B   EQU     16h
PT_HID_FAT32    EQU     1Bh
PT_HID_FAT32L   EQU     1Ch
PT_HID_FAT16L   EQU     1Eh

;---------------------------------------------------------------------
; BPB field offsets, counted from the start of the boot sector.
;
; The first three (JMP, OEM name) are not data but they are how you
; tell a boot sector from a random sector: DOS itself checked for a
; jump instruction before it believed anything else in here.
;
; DOS 2.0 stopped at BPB_MEDIA+2.  DOS 3.0 added SECPERTRK/NUMHEADS
; and a 16-bit HIDDSEC.  DOS 3.31 widened HIDDSEC to 32 bits and added
; TOTSEC32, which is where FAT16B partitions over 32MB came from.
; FAT32 replaced everything from SPF16 onwards.  All four layouts are
; still on disks in the world.
;---------------------------------------------------------------------
BPB_JMP         EQU     00h                     ; EBh xx 90h, or E9h xx xx
BPB_OEM         EQU     03h                     ; 8 bytes
BPB_BPS         EQU     0Bh                     ; dw - bytes per sector
BPB_SPC         EQU     0Dh                     ; db - sectors per cluster
BPB_RSVD        EQU     0Eh                     ; dw - reserved sectors
BPB_NFATS       EQU     10h                     ; db
BPB_ROOTENT     EQU     11h                     ; dw - 0 on FAT32
BPB_TOTSEC16    EQU     13h                     ; dw - 0 if it needs 32 bits
BPB_MEDIA       EQU     15h                     ; db - and it must match FAT[0]
BPB_SPF16       EQU     16h                     ; dw - 0 on FAT32
BPB_SECPERTRK   EQU     18h                     ; dw - DOS 3.0 and later
BPB_NUMHEADS    EQU     1Ah                     ; dw
BPB_HIDDSEC     EQU     1Ch                     ; dd - DOS 3.31 and later
BPB_TOTSEC32    EQU     20h                     ; dd
BPB_SPF32       EQU     24h                     ; dd - FAT32 only
BPB_EXTFLAGS    EQU     28h                     ; dw - FAT32 only
BPB_FSVER       EQU     2Ah                     ; dw
BPB_ROOTCLUS    EQU     2Ch                     ; dd
BPB_FSINFO      EQU     30h                     ; dw
BPB_BKBOOT      EQU     32h                     ; dw

; FAT32 BPB_EXTFLAGS.  Bit 7 clear means every FAT is a mirror of the
; others and a write goes to all of them; set means only ONE is live
; and the low nibble says which.  Writing all of them in that state
; corrupts the copies the volume was told to leave alone.
EXTF_NOMIRROR   EQU     0080h
EXTF_ACTIVE     EQU     000Fh

; Media descriptor bytes.  The one in the BPB has to match FAT[0]'s low
; byte, which is the oldest integrity check in the system and the only
; one a DOS 1.x disk has - those have no BPB at all, and the media byte
; is the entire description of the format.
MEDIA_F8        EQU     0F8h                    ; fixed disk
MEDIA_F0        EQU     0F0h                    ; 1.44M / 2.88M 3.5"
MEDIA_F9        EQU     0F9h                    ; 720K 3.5", 1.2M 5.25"
MEDIA_FC        EQU     0FCh                    ; 180K 5.25", 9 sec 1 side
MEDIA_FD        EQU     0FDh                    ; 360K 5.25", 9 sec 2 side
MEDIA_FE        EQU     0FEh                    ; 160K 5.25", 8 sec 1 side
MEDIA_FF        EQU     0FFh                    ; 320K 5.25", 8 sec 2 side

;---------------------------------------------------------------------
; One drive letter.  A LETTER IS A VOLUME, NOT A DISK - which sounds
; obvious and was not how this kernel worked until 0.26: logical drive
; 2 meant BIOS drive 80h and the first partition on it, so a second
; partition was unreachable and an extended one was invisible.
;---------------------------------------------------------------------
DRV_BIOS        EQU     00h                     ; db - BIOS drive number
DRV_TYPE        EQU     01h                     ; db - PT_*, 0 = whole medium
DRV_FLAGS       EQU     02h                     ; db - DRVF_*
DRV_PIDX        EQU     03h                     ; db - which primary slot it
                                                ; came from, FFh if logical
DRV_START       EQU     04h                     ; dd - absolute first LBA
DRV_SECS        EQU     08h                     ; dd - length, 0 = ask the BPB
DRV_LEN         EQU     0Ch

DRVF_PRESENT    EQU     01h                     ; the slot names something
DRVF_FLOPPY     EQU     02h                     ; removable: re-mount on access
DRVF_DRIVER     EQU     04h                     ; served by a loaded block driver
                                        ; rather than by the block layer -
                                        ; DRV_BIOS then names a dl_units row
                                        ; rather than a BIOS drive.  See
                                        ; DL_UNIT_BASE

;---------------------------------------------------------------------
; How many drive letters exist.  SHARED, and it has to be: COMMAND.COM
; had its own copy of this as a "cmp al,4" with the comment "A: B: C:
; D:", so raising the kernel's limit gave the shell four volumes it
; could reach through the API and would not parse on the command line
; - DIR E: listed E: and headed it with the label and free space of
; the default drive, which is a wrong answer that looks like a right
; one.  21h/0Eh reports this number; nothing should assume it.
;---------------------------------------------------------------------
LASTDRIVE       EQU     8               ; A: to H:

;---------------------------------------------------------------------
; FAT directory attribute bits (also the CX values for 21h/4Eh)
;---------------------------------------------------------------------
ATTR_READONLY   EQU     01h
ATTR_HIDDEN     EQU     02h
ATTR_SYSTEM     EQU     04h
ATTR_VOLUME     EQU     08h
ATTR_DIR        EQU     10h
ATTR_ARCHIVE    EQU     20h
ATTR_LFN        EQU     0Fh             ; RO+HID+SYS+VOL = an LFN fragment

; Long file names (VFAT).  An LFN rides in 32-byte directory entries
; of attribute 0Fh placed directly before the 8.3 entry it names.
LFN_MAX         EQU     260             ; ASCIIZ long name, NUL included
FIND_MAX        EQU     4               ; concurrent 71h find handles
; Internal flag OR'd into [srch_attr] by 4Eh/4Fh only: apply the DOS
; rule that hidden and system files stay out of a directory search
; unless asked for.  Lookups by name (open, exec, path walking) leave
; it clear and see everything.
SRCH_FILTER     EQU     80h

;---------------------------------------------------------------------
; File handle table
;---------------------------------------------------------------------
MAX_HANDLES     EQU     64              ; the table.  FILES= chooses how much
                                        ; of it is handed out; see cfg_files
SFT_HANDLE      EQU     0                       ; per-entry field offsets
SFT_MODE        EQU     4
SFT_DIRSTART    EQU     8                       ; owning directory (LBA or cluster)
SFT_CLUSTER     EQU     12
SFT_SIZE        EQU     16
SFT_POS         EQU     20
SFT_CURCLUS     EQU     24
SFT_DIRENT      EQU     28
SFT_FLAGS       EQU     32
SFT_DRIVE       EQU     36                      ; dd - logical drive (0=A..3=D)
SFT_OWNER       EQU     40                      ; dd - owner id that opened it
SFT_DEVID       EQU     44                      ; dd - DEV_* + unit, if a device
SFT_TIME        EQU     48                      ; dw - the file's stamp, as
SFT_DATE        EQU     50                      ; dw - opened or as 57h set it

; THE LAST TWO FIELDS EXIST FOR THE FCB API AND NOTHING ELSE.  An FCB
; is the CALLER's memory and holds its own file position, so the eight
; reserved bytes in it can only cache which table entry the file is
; open on - and a cache needs a way to know it has gone stale.
;
;   SFT_STAMP is a number never handed out twice.  An FCB carries the
;   stamp of the entry it was opened on, so an entry that has since
;   been closed and reused answers "not yours" to the old FCB instead
;   of handing it somebody else's file.  A bare index cannot: index 7
;   is index 7 whoever is in it.
;
;   SFT_FCBLRU is zero for a handle and counts up for an FCB, so that
;   when the table is full the FCB that has waited longest can be
;   closed to make room.  Real DOS does exactly this (LRUFcb, and the
;   FCBS= statement that sizes it) because a CP/M-era program opens
;   FCBs and never closes them - it is not a leak there, it is the
;   documented way the API is used, and a kernel that refuses the
;   thirty-third one is a kernel those programs do not run on.  What
;   makes eviction safe is that an FCB can always be re-opened from
;   the name it is still carrying; see fcb_bind.
SFT_STAMP       EQU     52                      ; dd - unique claim id
SFT_FCBLRU      EQU     56                      ; dd - 0 = not an FCB's
SFT_LEN         EQU     60

SFTF_INUSE      EQU     0001h
SFTF_DEVICE     EQU     0002h                   ; a character device, not a file
SFTF_DIRTY      EQU     0004h
SFTF_DIRROOT    EQU     0008h                   ; SFT_DIRSTART is the fixed root
SFTF_SETDT      EQU     0010h                   ; 57h set the stamp: keep it
SFTF_RAW        EQU     0020h                   ; 44h/01h put it in binary mode

;---------------------------------------------------------------------
; FILE CONTROL BLOCKS - the pre-1983 file API, and the layout is the
; caller's, not ours.  Every field below is a byte a program written
; for DOS 1.0 fills in by hand and reads back, so this is a
; description of somebody else's structure rather than a definition
; of one of ours.
;
;   +00h  drive (0 = default, 1 = A:)
;   +01h  8 bytes of name, blank padded
;   +09h  3 bytes of extension, blank padded
;   +0Ch  current block (a block is 128 records)
;   +0Eh  record size; an open sets it to 128 and the program may
;         change it afterwards, which is the whole trick of the API
;   +10h  file size in bytes
;   +14h  date, +16h time - of the last write, in FAT form
;   +18h  EIGHT BYTES RESERVED TO DOS.  See below.
;   +20h  record within the current block
;   +21h  random record number (four bytes; the fourth is only read
;         when the record size is under 64, which is a rule from the
;         days when the field was three bytes and nobody wanted to
;         break the programs that knew it)
;
; An EXTENDED FCB is the same thing with a seven-byte hat: FFh, five
; unused bytes, and an attribute byte that lets a search see hidden,
; system, directory and volume-label entries.  Everything here takes
; either kind and works on the plain FCB inside.
;---------------------------------------------------------------------
FCB_DRIVE       EQU     00h
FCB_NAME        EQU     01h
FCB_EXT         EQU     09h
FCB_EXTENT      EQU     0Ch                     ; dw - current block
FCB_RECSIZ      EQU     0Eh                     ; dw
FCB_FILSIZ      EQU     10h                     ; dd
FCB_FDATE       EQU     14h                     ; dw
FCB_FTIME       EQU     16h                     ; dw
FCB_RSV         EQU     18h                     ; 8 bytes, ours
FCB_NR          EQU     20h                     ; db - record in block
FCB_RR          EQU     21h                     ; dd - random record
FCB_LEN         EQU     25h

FCBX_SIG        EQU     0FFh                    ; first byte of an extended FCB
FCBX_ATTR       EQU     06h                     ; its attribute byte
FCBX_LEN        EQU     07h                     ; and how much longer it is

; WHAT WE KEEP IN THE EIGHT RESERVED BYTES, and why it is this and not
; what real DOS keeps there.  MS-DOS stores the file's first cluster
; and the sector and position of its directory entry, so that it can
; rebuild a table entry without touching the disk.  Those fields are
; sixteen bits wide, which is a FAT16 answer to a FAT16 question, and
; PM-DOS mounts FAT32 volumes where none of the three fit.  So we keep
; the one thing that is always enough - which entry it was open on,
; and proof that the entry is still the same one - and rebuild by
; RE-OPENING THE NAME, which the FCB is still carrying and which no
; program is allowed to change while a file is open on it.  That is
; what real DOS's own regeneration path does when its cached fields
; are unusable, so it is the same answer arrived at from a wider disk.
FCBR_SFN        EQU     18h                     ; db - table index, or FCBR_NONE
FCBR_DRV        EQU     19h                     ; db - drive it resolved to, 1-based
FCBR_STAMP      EQU     1Ah                     ; dw - the entry's claim id
FCBR_OWN        EQU     1Ch                     ; dd - the program that opened it
FCBR_NONE       EQU     0FFh                    ; SFN value meaning "not bound"

; The four things an FCB read or write can be, in the two bits that
; distinguish them, plus the block flag that says CX holds a count.
FIOP_READ       EQU     01h
FIOP_RANDOM     EQU     02h
FIOP_BLOCK      EQU     04h

; And the four answers, which are the FCB API's entire error
; vocabulary - there is no error CODE, only these, and a program that
; wants to know why asks 59h afterwards.
FIOR_OK         EQU     0
FIOR_EOF        EQU     1                       ; nothing there / disk full
FIOR_TRIM       EQU     2                       ; would not fit at the DTA
FIOR_PART       EQU     3                       ; partial record, zero filled

;---------------------------------------------------------------------
; Character devices.
;
; Before this there was exactly one, CON, matched by comparing four
; bytes - so NUL, PRN and the rest were ordinary file names, and
; "VER > NUL" wrote a 25-byte file called NUL into the current
; directory.  That is the single most common idiom in DOS batch files
; and it silently littered the disk.
;
; The id is a byte of KIND plus a byte of UNIT, so LPT2 and COM3 are
; the same driver as LPT1 and COM1 with a different port number, which
; is what they are.
;---------------------------------------------------------------------
DEV_CON         EQU     0                       ; keyboard and screen
DEV_NUL         EQU     1                       ; the bit bucket
DEV_PRN         EQU     2                       ; LPT1-3, via INT 17h
DEV_AUX         EQU     3                       ; COM1-4, via INT 14h
DEV_LOADED      EQU     4                       ; anything a DEVICE= line put
                                                ; there; the unit is its row
                                                ; in dl_tab, which is why the
                                                ; unit byte is a byte and not
                                                ; a port number here
DEV_UNIT        EQU     256                     ; multiply the unit by this
DEV_KIND        EQU     0FFh                    ; and mask by this to undo it

;---------------------------------------------------------------------
; The command interpreter, as the kernel holds it.  Buffers rather
; than constants because CONFIG.SYS's SHELL= gets to replace both.
;---------------------------------------------------------------------
SHELLNAME_MAX   EQU     80              ; "X:\PATH\COMMAND.COM",0
SHELLTAIL_MAX   EQU     130             ; length byte + 126 + CR + slack

;---------------------------------------------------------------------
; Boot / kernel version
;
; TWO VERSIONS, and they answer different questions.
;
; PMDOS_* is what PM-DOS is: the build number of this kernel, reported
; by 21h/F100h and by nothing else.  It goes up every release.
;
; DOSVER_* is what PM-DOS ANSWERS WHEN ASKED WHAT DOS IT IS, and it is
; a compatibility target rather than a fact.  6.22 is the last MS-DOS
; that was only DOS, it is the version the software of the period was
; written and tested against, and it is the number every "DOS 3.3 or
; later" check in existence is satisfied by.
;
; It is reported by BOTH 21h/30h and 21h/3306h, and that pairing is
; deliberate.  3306h exists because 30h can be lied to by SETVER, so a
; program that wants the truth asks 3306h - and a program that finds
; 6.22 at 30h and 0.31 at 3306h has caught the system contradicting
; itself, which is worse than either answer alone.  The honest number
; has a call of its own; these two agree.
;---------------------------------------------------------------------
PMDOS_MAJOR     EQU     0
PMDOS_MINOR     EQU     40

DOSVER_MAJOR    EQU     6
DOSVER_MINOR    EQU     22
