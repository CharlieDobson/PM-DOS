;=====================================================================
; NDRV.INC - a 32-BIT DRIVER loaded by DEVICE=
;
; PM-DOS's own drivers are 32-bit code inside PMIO.SYS and PMDOS.SYS,
; called as subroutines; a driver named on a DEVICE= line has until
; now been a lump of 16-bit real-mode code run through the V86
; monitor (DEVLOAD.INC).  This is the third kind: a 32-bit driver in
; a file of its own, loaded above the megabyte by DEVICE=, relocated
; to wherever it lands, and called as a subroutine like the kernel's
; own.  It costs no conventional memory and no excursion, and it is
; the shape a network stack and its card drivers take.
;
; THE FILE.  A D32 image (PMDOS.INC) whose header says D32T_DRV,
; linked at offset 0, with a RELOCATION TABLE after the image - the
; offsets of every dword that holds an address - and 128 bytes of
; real-mode device driver in front of everything (TOOLS\DRVSTUB.ASM)
; for the benefit of MS-DOS: a real DOS given the file on a DEVICE=
; line loads it as a block driver, whose INIT prints one sentence and
; reports no units, which is the one answer that makes every DOS from
; 3.0 on drop a driver quietly and free its memory (SYSINIT's
; erase_dev_do, S:\DOS6SRC\bios\sysconf.asm).  PM-DOS knows the file
; by the name in that header, DRVSTUB_NAME, which DOS never reads.
; TOOLS\D32DRV.C makes the file out of two links of the same driver:
; a dword that differs between them by exactly the difference in link
; address is a relocation, and nothing else may differ at all.
;
; WHERE IT RUNS.  Ring 0, flat, DS and ES the kernel's, in a block
; out of the arena owned by the kernel - so it is never freed, and
; park_xmsgrab never hands it to Windows, because that gives away only
; the largest FREE block.  The address is for life: nothing moves a
; driver once it is relocated.
;
; WHAT IT MAY DO.  Its own memory, its own hardware, and the services
; in the table it is given at INIT (NDS_* below) - and nothing else.
; No INT 21h, no console but NDS_PRINT, no waiting.  The rule is the
; bridge's (NOTES: the plan for WfW): a driver that keeps to it runs
; unchanged while Windows has the machine, called through a VxD that
; switches into the parked kernel's context.  A driver that calls DOS
; from an interrupt handler is one that will not survive that.
;
; THE PARK.  A driver's PARK entry is called with AL = 0 as the
; machine is about to be real mode's and AL = 1 when it is back - the
; same two calls usb_park gets (USB.INC).  Nothing else is done for
; it: what to do with its card is the driver's to decide.
;
; Included by PMDOS.INC's includers that need the equates: DOSMAIN.ASM
; (DOS\NDLOAD.INC is the loader), a driver's own source, and the
; test that reads the counts back.
;=====================================================================

;---------------------------------------------------------------------
; The real-mode stub in front of a driver file.  D32_DRVSTUB in
; PMDOS.INC says where the image starts; this is the mark in the stub's
; device header (DEVH_NAME) that says the file is one of ours.
;---------------------------------------------------------------------
; DRVSTUB_NAME: the eight bytes 'PMD32DRV' - spelled out where they are
; used (DRVSTUB.ASM, NDLOAD.INC, D32DRV.C), because wasm emits a text
; equate's angle bracket into a DB

;---------------------------------------------------------------------
; THE DRIVER TABLE - what D32H_ENTRY names in a driver's header.
; Every entry is called with DS and ES the kernel's flat selectors,
; the direction flag clear, and may use every register but the stack
; below ESP.  An entry a driver does not have is 0.
;
;   INIT    ESI -> the services (NDS_*), EDI -> the driver's row in
;           the kernel's table (ND_*), EDX -> the DEVICE= line as the
;           file saw it: the path and whatever followed it, ended by
;           CR, LF and a zero.  CF clear: installed.  CF set: refused,
;           and the driver's memory is given back - so a driver that
;           refuses after hooking anything unhooks it first.
;           INIT runs once, at CONFIG.SYS time, with interrupts on;
;           it is the one entry that may take its time.
;   PARK    AL = 0: the machine is about to be handed to real mode.
;           AL = 1: it has come back.  Preserve nothing; the kernel
;           keeps the registers.
;   HALT    the machine is about to restart through the ROM or switch
;           off: a DMA engine left running would be writing into
;           memory the BIOS is about to use.  Interrupts may be off.
;   CALL    a program's request, through 21h/F1h AL=25h NDF_CALL:
;           EBX = the function, whatever the driver says it is, EDI ->
;           a buffer the program owns and the kernel has checked, ECX
;           = its length (0: no buffer), ESI -> the driver's row.
;           Answers EAX, and CF with AX an error code (ERR_*).
;---------------------------------------------------------------------
NDT_SIG         EQU     0               ; dd - NDT_SIGVAL
NDT_VER         EQU     4               ; dw - NDT_VERSION
NDT_FLAGS       EQU     6               ; dw - none defined; 0
NDT_NAME        EQU     8               ; 8 bytes, blank padded: the name
                                        ; the kernel shows for it
NDT_INIT        EQU     16              ; dd
NDT_PARK        EQU     20              ; dd, or 0
NDT_HALT        EQU     24              ; dd, or 0
NDT_CALL        EQU     28              ; dd, or 0
NDT_LEN         EQU     32

NDT_SIGVAL      EQU     5652444Eh       ; 'N','D','R','V' as a dword
NDT_VERSION     EQU     1

;---------------------------------------------------------------------
; THE SERVICES - ESI at INIT, and the address is good for life.  Each
; entry holds the address of a routine with the register contract of
; the kernel routine behind it; a driver calls one with
; "call dword ptr [esi+NDS_xxx]".  Every one preserves what it does
; not return, as the kernel's own do.
;
;   NDS_VER         dd - low word NDS_VERSION, high word NDS_LEN: a
;                   driver that needs an entry checks the length
;   NDS_ALLOC       EBX = bytes -> EAX = the block, CF if none.  The
;                   kernel's block, whoever is running: it is never
;                   reclaimed.  INIT and CALL time only - not from an
;                   interrupt or a tick
;   NDS_FREE        EDX = a block from NDS_ALLOC
;   NDS_PRINT       ESI -> ASCIIZ, to the console (IOS_CONSTR).  INIT
;                   and CALL time only
;   NDS_TICKS       -> EAX = the 18.2Hz tick count (IOS_TICKS)
;   NDS_BUS         AH = a BUS_* function (PMDOS.INC): the PCI bus,
;                   device memory, the interrupt lines, ISA Plug and
;                   Play (IOS_BUS)
;   NDS_IRQHOOK     AL = a line, EDI = a handler (IOS_IRQHOOK): called
;                   with EBX = the line and interrupts off; CF clear
;                   if its device was asking, which it finds out by
;                   asking the device.  No EOI: the kernel's
;   NDS_IRQUNHOOK   the same two registers
;   NDS_TICKHOOK    EDI = a handler, called on every whole tick of the
;                   clock with interrupts off; it may use any register
;                   (IOS_TICKHOOK).  CF if the table is full
;   NDS_TICKUNHOOK  EDI = the handler again
;---------------------------------------------------------------------
NDS_VER         EQU     0
NDS_ALLOC       EQU     4
NDS_FREE        EQU     8
NDS_PRINT       EQU     12
NDS_TICKS       EQU     16
NDS_BUS         EQU     20
NDS_IRQHOOK     EQU     24
NDS_IRQUNHOOK   EQU     28
NDS_TICKHOOK    EQU     32
NDS_TICKUNHOOK  EQU     36
NDS_LEN         EQU     40

NDS_VERSION     EQU     1

;---------------------------------------------------------------------
; THE KERNEL'S ROW for a loaded driver - EDI at INIT and CALL, and what
; NDF_INFO copies to a program.  A driver reads it; the kernel writes
; it.
;---------------------------------------------------------------------
ND_NAME         EQU     0               ; 8 bytes, from NDT_NAME
ND_BASE         EQU     8               ; dd - where the image is: its
                                        ; D32 header is the first thing
ND_SIZE         EQU     12              ; dd - bytes kept: image and BSS
ND_TABLE        EQU     16              ; dd - its driver table
ND_FLAGS        EQU     20              ; dd - none defined; 0
ND_PARKS        EQU     24              ; dd - PARK calls with AL = 0
ND_UNPARKS      EQU     28              ; dd - ...and with AL = 1
ND_LEN          EQU     32

ND_MAX          EQU     8               ; drivers, in total

;---------------------------------------------------------------------
; 21h/F1h AL=25h - the 32-bit drivers.  Bits 16-23 of EAX say which.
; Native programs only: EDI is a flat pointer.
;
;   NDF_COUNT   -> ECX = how many are loaded, EAX = ND_MAX
;   NDF_INFO    DL = a row, EDI -> ND_LEN bytes -> that row.  CF and
;               AX = ERR_BADDRIVE for a row there is not
;   NDF_CALL    DL = a row, EBX = the driver's function, EDI -> a
;               buffer of ECX bytes (0: none) -> what the driver's CALL
;               entry answers.  CF and AX = ERR_BADDRIVE for a row
;               there is not, ERR_FUNC for a driver with no CALL entry
;---------------------------------------------------------------------
NDF_COUNT       EQU     0
NDF_INFO        EQU     1
NDF_CALL        EQU     2
