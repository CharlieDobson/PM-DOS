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
;           A DISK DRIVER (NDTF_DISK) is told ECX = how many drive
;           letters are left for its units and EBX = the first of them
;           (0 = A:), the rest following in order, and answers in its
;           table: NDT_NUNITS, 1 to ECX, and NDT_UNITS.  The kernel reads
;           them once INIT has come back CF clear and gives each unit
;           its letter (DEVLOAD.INC, dl_ndunits).  A disk driver with no
;           letter to have refuses.
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
NDT_FLAGS       EQU     6               ; dw - NDTF_*: what kind of
                                        ; driver, for whoever looks for
                                        ; one of a kind
NDT_NAME        EQU     8               ; 8 bytes, blank padded: the name
                                        ; the kernel shows for it
NDT_INIT        EQU     16              ; dd
NDT_PARK        EQU     20              ; dd, or 0
NDT_HALT        EQU     24              ; dd, or 0
NDT_CALL        EQU     28              ; dd, or 0
NDT_LEN         EQU     32

; THE REST OF A DISK DRIVER'S TABLE (NDTF_DISK), which says so by its
; version: NDT_DISKVER, so that a kernel from before disk drivers
; refuses the file instead of loading a driver whose units it would
; never give a letter.  Every other driver's table stops at NDT_LEN.
NDT_NUNITS      EQU     32              ; dd - how many units, from INIT
NDT_UNITS       EQU     36              ; dd - -> that many NDU_LEN records,
                                        ; in the driver's own memory
NDT_DLEN        EQU     40

NDT_SIGVAL      EQU     5652444Eh       ; 'N','D','R','V' as a dword
NDT_VERSION     EQU     1
NDT_DISKVER     EQU     2               ; a table with the disk fields

NDTF_NIC        EQU     0001h           ; a network card: CALL answers
                                        ; the functions in INC\NIC.INC
NDTF_STACK      EQU     0002h           ; the TCP/IP stack: CALL answers
                                        ; the socket functions in
                                        ; INC\SOCKET.H, and 21h/F1h AL=26h
                                        ; reaches it without a row number
NDTF_REDIR      EQU     0004h           ; a network redirector (DRV\SMBFS):
                                        ; CALL answers the REDIR_* functions
                                        ; in INC\REDIR.H, and the kernel's
                                        ; net-drive branches reach it by this
                                        ; flag.  It rides on the stack through
                                        ; NDS_SOCKCALL the way the stack rides
                                        ; on the cards through NDS_NICCALL
NDTF_BLOCK      EQU     0008h           ; NOT a loaded driver: a sentinel the
                                        ; WfW bridge VxD sends brg_entry to
                                        ; reach the kernel's own block layer
                                        ; (IOS_BRGBLK -> blk_bridge) for USB-
                                        ; disk sectors while parked.  Handled
                                        ; before the nd_find_flag path
NDTF_DISK       EQU     0010h           ; a block device: its units are drive
                                        ; letters, read and written through
                                        ; NDU_IO.  The table is NDT_DISKVER's

;---------------------------------------------------------------------
; ONE UNIT of a disk driver - one drive letter - as INIT describes it.
; The record is the driver's and stays where it is for life: the
; kernel keeps its address and hands it back with every transfer.
;
;   NDU_IO  the transfer.  AL = 0 read, 1 write; EDX -> this record;
;           EBX = the first sector; ECX = how many, 1 or more; EDI ->
;           the buffer, ECX * 512 bytes, flat.  CF clear: every sector
;           moved.  CF set: AH = what went wrong, in INT 13h's numbers
;           (04h sector not found, 03h write protected, 80h not ready).
;           The kernel has already checked that the sectors are inside
;           NDU_SECS.  Called with DS and ES the kernel's, the direction
;           flag clear and interrupts as the caller had them; it may use
;           every register but the stack, and it may not wait.
;
; A UNIT THAT IS MEMORY (NDUF_MEMORY) still moves its sectors through
; NDU_IO, but says where they are as well.  While the machine is parked
; no driver can be called, and RMDOS moves the sectors itself instead
; (RMDISK.INC, rd_memraw): that is what keeps a RAM disk's letter while
; Windows runs.  The kernel's cache leaves such a unit alone, since
; caching memory in memory buys nothing.
;---------------------------------------------------------------------
NDU_IO          EQU     0               ; dd - the transfer, above
NDU_SECS        EQU     4               ; dd - the unit's size, in sectors
NDU_MEM         EQU     8               ; dd - NDUF_MEMORY: where sector 0
                                        ; is, sector N at NDU_MEM + N * 512
NDU_MEDIA       EQU     12              ; db - the media byte its BPB says
NDU_FLAGS       EQU     13              ; db - NDUF_*
NDU_LETTER      EQU     14              ; db - the letter it got, 0 = A:;
                                        ; written by the kernel, FFh before
                                        ; and for a unit that got none
                                        ; db - spare, 0
NDU_LEN         EQU     16

NDUF_MEMORY     EQU     01h             ; one block of memory, for life

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
;                   interrupt or a tick.  The block is counted as the
;                   calling driver's (NDF_MEM): the call has to come
;                   from code in its own image
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
;   NDS_NICCOUNT    -> EAX = how many loaded drivers are NICs (NDTF_NIC).
;                   For the TCP/IP stack, which rides on the card drivers
;                   rather than touching a card itself
;   NDS_NICCALL     EAX = a NIC index (0..NDS_NICCOUNT-1), EBX = a NIC_*
;                   function (INC\NIC.INC), EDI -> a buffer, ECX = its
;                   length -> EAX = what the card's CALL answered, CF and
;                   AX = ERR_* for a bad index or a card that said no.
;                   The same path 21h/F1h AL=25h NDF_CALL takes, from one
;                   driver to another
;   NDS_SOCKCALL    EBX = a socket function (INC\SOCKET.H), EDI -> a
;                   request buffer, ECX = its length -> EAX = what the one
;                   loaded TCP/IP stack (NDTF_STACK) answered, CF and AX =
;                   ERR_GENERAL when no stack is loaded or ERR_* from the
;                   stack.  For a redirector (NDTF_REDIR) that makes socket
;                   calls from inside the kernel, the path 21h/F1h AL=26h
;                   takes for a program - the buffer is the caller's own
;                   kernel memory, so it is not checked
;   NDS_STAMP       -> EAX = now, as a directory entry is stamped: the
;                   date in the high word and the time in the low, both
;                   packed as DOS packs them (the clock's, through the
;                   BIOS).  For a disk driver that writes a directory of
;                   its own, as RAMDISK.SYS does its label.  INIT and CALL
;                   time only.  Since NDS_VERSION 4
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
NDS_NICCOUNT    EQU     40
NDS_NICCALL     EQU     44
NDS_SOCKCALL    EQU     48
NDS_STAMP       EQU     52
NDS_LEN         EQU     56

NDS_VERSION     EQU     4

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
ND_FLAGS        EQU     20              ; dd - the table's NDT_FLAGS, so
                                        ; that a program finds a card
                                        ; driver by NDTF_NIC without
                                        ; reading the table
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
;   NDF_MEM     DL = a row -> ECX = the bytes that row's driver holds
;               through NDS_ALLOC now (the blocks' payload, headers not
;               counted), EDX = how many blocks.  Its image is ND_SIZE
;               and not in these.  The kernel counts a block against
;               the driver whose code called NDS_ALLOC, and takes it
;               off when that driver gives it back.  CF and AX =
;               ERR_BADDRIVE for a row there is not.  For MEM /D
;---------------------------------------------------------------------
NDF_COUNT       EQU     0
NDF_INFO        EQU     1
NDF_CALL        EQU     2
NDF_MEM         EQU     3
